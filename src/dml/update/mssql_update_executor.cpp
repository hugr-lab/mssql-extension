#include "dml/update/mssql_update_executor.hpp"
#include <cstdio>
#include <cstdlib>
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_transaction.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "connection/mssql_settings.hpp"
#include "dml/mssql_dml_execute.hpp"
#include "dml/mssql_dml_outcome.hpp"
#include "dml/mssql_rowid_extractor.hpp"
#include "dml/update/mssql_update_statement.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "tds/tds_connection_pool.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetUpdateDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define UPDATE_DEBUG(level, fmt, ...)                                   \
	do {                                                                \
		if (GetUpdateDebugLevel() >= level) {                           \
			fprintf(stderr, "[MSSQL UPDATE] " fmt "\n", ##__VA_ARGS__); \
		}                                                               \
	} while (0)

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLUpdateExecutor Implementation
//===----------------------------------------------------------------------===//

MSSQLUpdateExecutor::MSSQLUpdateExecutor(ClientContext &context, const MSSQLUpdateTarget &target,
										 const MSSQLDMLConfig &config)
	: context_(context), target_(target), config_(config), connection_pool_(nullptr) {
	// Compute effective batch size based on parameters per row
	effective_batch_size_ = config_.EffectiveBatchSize(target_.GetParamsPerRow());
	UPDATE_DEBUG(1, "UpdateExecutor: effective_batch_size=%llu (params_per_row=%llu)",
				 (unsigned long long)effective_batch_size_, (unsigned long long)target_.GetParamsPerRow());

	// Batches go out as the rows arrive, in a transaction too: the scan feeding
	// the UPDATE is materialised at init (spec 080 PR 1 -- UPDATE / DELETE /
	// MERGE count as sinks in MSSQLOptimizer's CollectSinkCatalogs), so the
	// pinned connection is idle by the time the first batch is sent. Until then
	// the statement deferred every batch to Finalize with its rows buffered.
}

MSSQLUpdateExecutor::~MSSQLUpdateExecutor() = default;

tds::ConnectionPool &MSSQLUpdateExecutor::GetConnectionPool() {
	if (connection_pool_) {
		return *connection_pool_;
	}

	// Spec 047: route through DuckDB catalog (per-catalog pool ownership).
	auto &catalog = Catalog::GetCatalog(context_, Identifier(target_.catalog_name));
	auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();
	connection_pool_ = &mssql_catalog.GetConnectionPool();
	return *connection_pool_;
}

idx_t MSSQLUpdateExecutor::Execute(DataChunk &chunk) {
	UPDATE_DEBUG(1, "Execute: chunk_size=%llu", (unsigned long long)chunk.size());

	if (finalized_) {
		throw InternalException("MSSQLUpdateExecutor::Execute called after Finalize");
	}

	// Process each row in the chunk
	for (idx_t row_idx = 0; row_idx < chunk.size(); row_idx++) {
		AccumulateRow(chunk, row_idx);

		if (!config_.defer_to_finalize && pending_pk_values_.size() >= effective_batch_size_) {
			UPDATE_DEBUG(1, "Execute: batch full at row %llu, flushing...", (unsigned long long)row_idx);
			auto result = FlushBatch();
			if (!result.success) {
				throw IOException("%s", result.FormatError("UPDATE"));
			}
		}
	}

	UPDATE_DEBUG(1, "Execute: chunk processed, total_updated=%llu, pending=%llu",
				 (unsigned long long)total_rows_updated_, (unsigned long long)pending_pk_values_.size());

	return total_rows_updated_;
}

MSSQLDMLResult MSSQLUpdateExecutor::Finalize() {
	UPDATE_DEBUG(1, "Finalize: starting, finalized=%d, pending=%llu", finalized_,
				 (unsigned long long)pending_pk_values_.size());

	if (finalized_) {
		return MSSQLDMLResult::Success(total_rows_updated_, batch_count_);
	}

	finalized_ = true;

	// Flush the remaining rows: less than one batch, or every row of a MERGE
	// action (MSSQLDMLConfig::defer_to_finalize)
	while (!pending_pk_values_.empty()) {
		UPDATE_DEBUG(1, "Finalize: flushing batch, pending=%llu", (unsigned long long)pending_pk_values_.size());
		auto result = FlushBatch();
		if (!result.success) {
			return result;
		}
	}

	// Every batch is in: one COMMIT for the statement (a no-op inside a DuckDB
	// transaction, and when no batch was ever sent).
	try {
		auto &catalog = Catalog::GetCatalog(context_, Identifier(target_.catalog_name));
		stmt_conn_.Commit(context_, catalog.Cast<MSSQLCatalog>());
	} catch (const std::exception &e) {
		return MSSQLDMLResult::Failure(string("UPDATE failed: ") + e.what(), 0, batch_count_);
	}

	UPDATE_DEBUG(1, "Finalize: done, total_updated=%llu, batch_count=%llu", (unsigned long long)total_rows_updated_,
				 (unsigned long long)batch_count_);
	return MSSQLDMLResult::Success(total_rows_updated_, batch_count_);
}

void MSSQLUpdateExecutor::AccumulateRow(DataChunk &chunk, idx_t row_idx) {
	// DuckDB UPDATE chunk layout:
	// - Columns 0 to N-1: update expression values
	// - Column N: rowid (added by BindRowIdColumns at the END of projection)
	//
	// The rowid column is the LAST column in the chunk
	idx_t rowid_col_idx = chunk.ColumnCount() - 1;

	// Extract PK values from rowid (last column)
	auto pk_values = ExtractSingleRowPK(chunk.data[rowid_col_idx], row_idx, target_.pk_info);
	pending_pk_values_.push_back(std::move(pk_values));

	// Extract update values (columns 0 to N-1)
	vector<Value> update_values;
	update_values.reserve(target_.update_columns.size());

	for (idx_t col_idx = 0; col_idx < target_.update_columns.size(); col_idx++) {
		auto &update_col = target_.update_columns[col_idx];
		// Update column values are at chunk index as specified in update_col.chunk_index
		auto value = chunk.data[update_col.chunk_index].GetValue(row_idx);
		update_values.push_back(std::move(value));
	}

	pending_update_values_.push_back(std::move(update_values));
}

MSSQLDMLResult MSSQLUpdateExecutor::FlushBatch() {
	if (pending_pk_values_.empty()) {
		return MSSQLDMLResult::Success(0, batch_count_);
	}

	batch_count_++;

	// Extract up to effective_batch_size_ rows for this batch
	idx_t rows_to_process = std::min<idx_t>(pending_pk_values_.size(), effective_batch_size_);

	vector<vector<Value>> batch_pk_values;
	vector<vector<Value>> batch_update_values;
	batch_pk_values.reserve(rows_to_process);
	batch_update_values.reserve(rows_to_process);

	for (idx_t i = 0; i < rows_to_process; i++) {
		batch_pk_values.push_back(std::move(pending_pk_values_[i]));
		batch_update_values.push_back(std::move(pending_update_values_[i]));
	}

	// Remove processed rows from pending
	pending_pk_values_.erase(pending_pk_values_.begin(), pending_pk_values_.begin() + rows_to_process);
	pending_update_values_.erase(pending_update_values_.begin(), pending_update_values_.begin() + rows_to_process);

	UPDATE_DEBUG(1, "FlushBatch: batch %llu with %llu rows (remaining=%llu)", (unsigned long long)batch_count_,
				 (unsigned long long)rows_to_process, (unsigned long long)pending_pk_values_.size());

	// Build the UPDATE statement
	MSSQLUpdateStatement stmt(target_);
	auto batch = stmt.Build(batch_pk_values, batch_update_values, batch_count_);

	if (!batch.IsValid()) {
		return MSSQLDMLResult::Failure("Failed to build UPDATE batch", 0, batch_count_);
	}

	UPDATE_DEBUG(2, "FlushBatch: SQL=\n%s", batch.sql.c_str());

	// Execute the batch
	try {
		auto rows_affected = ExecuteBatch(batch.sql);
		total_rows_updated_ += rows_affected;
		UPDATE_DEBUG(1, "FlushBatch: rows_affected=%llu", (unsigned long long)rows_affected);
		return MSSQLDMLResult::Success(rows_affected, batch_count_);
	} catch (const InterruptException &) {
		throw;	// Ctrl-C: the query is cancelled, not failed
	} catch (const std::exception &e) {
		return MSSQLDMLResult::Failure(e.what(), 0, batch_count_);
	}
}

idx_t MSSQLUpdateExecutor::ExecuteBatch(const string &sql) {
	UPDATE_DEBUG(1, "ExecuteBatch: starting, sql_length=%zu", sql.size());

	// Get the MSSQLCatalog for ConnectionProvider
	auto &catalog = Catalog::GetCatalog(context_, Identifier(target_.catalog_name));
	auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();

	// The statement's one connection -- pinned inside a DuckDB transaction,
	// else a pool connection with the statement's own server transaction begun
	// on it (spec 062 W1c). Throws when none can be had.
	auto connection = stmt_conn_.Acquire(context_, mssql_catalog);
	auto pinned_lock = stmt_conn_.LockPinned(context_, mssql_catalog);
	MSSQLStatementConnection::RequireIdle(*connection, "UPDATE");

	mssql::DmlBatchOptions options;
	options.label = "update statement batch";
	mssql::DmlBatchResponse response;
	try {
		response = mssql::ExecuteDmlBatch(context_, *connection, sql, options);
	} catch (const std::exception &e) {
		stmt_conn_.Fail(context_, mssql_catalog);
		throw IOException("UPDATE execution failed: %s", MSSQLRawMessage(e));
	}
	UPDATE_DEBUG(1, "ExecuteBatch: rows_affected=%llu, error='%s'", (unsigned long long)response.rows_affected,
				 response.error_message.c_str());

	if (response.status == mssql::DmlBatchResponse::Status::INTERRUPTED) {
		stmt_conn_.Fail(context_, mssql_catalog);
		throw InterruptException();
	}
	if (!response.Succeeded()) {
		stmt_conn_.Fail(context_, mssql_catalog);
		// What happened to the rows: the statement's server transaction is
		// rolled back (Fail above), or they sit in the open DuckDB
		// transaction (issue #344, spec 062 W1c). A parse error is a
		// client-side framing failure -- the server ran this batch -- and
		// says so.
		const string outcome = MSSQLRowsBeforeOutcome(stmt_conn_.InOpenTransaction(), total_rows_updated_);
		if (response.status == mssql::DmlBatchResponse::Status::PARSE_ERROR) {
			throw IOException("UPDATE failed: %s; the server executed this batch (batch %llu); %s",
							  response.error_message, (unsigned long long)batch_count_, outcome);
		}
		throw IOException("UPDATE failed: %s; %s", response.error_message, outcome);
	}

	// The connection stays with the statement until Finalize commits.
	return response.rows_affected;
}

}  // namespace duckdb

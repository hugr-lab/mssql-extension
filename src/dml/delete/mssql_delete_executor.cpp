#include "dml/delete/mssql_delete_executor.hpp"
#include <cstdio>
#include <cstdlib>
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_transaction.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "connection/mssql_settings.hpp"
#include "dml/delete/mssql_delete_statement.hpp"
#include "dml/mssql_dml_execute.hpp"
#include "dml/mssql_dml_outcome.hpp"
#include "dml/mssql_rowid_extractor.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "tds/tds_connection_pool.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetDeleteDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define DELETE_DEBUG(level, fmt, ...)                                   \
	do {                                                                \
		if (GetDeleteDebugLevel() >= level) {                           \
			fprintf(stderr, "[MSSQL DELETE] " fmt "\n", ##__VA_ARGS__); \
		}                                                               \
	} while (0)

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLDeleteExecutor Implementation
//===----------------------------------------------------------------------===//

MSSQLDeleteExecutor::MSSQLDeleteExecutor(ClientContext &context, const MSSQLDeleteTarget &target,
										 const MSSQLDMLConfig &config)
	: context_(context), target_(target), config_(config) {
	// Create statement generator
	statement_ = make_uniq<MSSQLDeleteStatement>(target_);

	// Compute effective batch size based on parameters per row (PK columns only for DELETE)
	effective_batch_size_ = config_.EffectiveBatchSize(statement_->GetParametersPerRow());
	DELETE_DEBUG(1, "DeleteExecutor: effective_batch_size=%llu (params_per_row=%llu)",
				 (unsigned long long)effective_batch_size_, (unsigned long long)statement_->GetParametersPerRow());

	// Batches go out as the rows arrive, in a transaction too: the scan feeding
	// the DELETE is materialised at init (spec 080 PR 1, as for UPDATE).
}

MSSQLDeleteExecutor::~MSSQLDeleteExecutor() = default;

idx_t MSSQLDeleteExecutor::Execute(DataChunk &chunk) {
	DELETE_DEBUG(1, "Execute: chunk_size=%llu, column_count=%llu", (unsigned long long)chunk.size(),
				 (unsigned long long)chunk.ColumnCount());

	if (finalized_) {
		throw InternalException("MSSQLDeleteExecutor::Execute called after Finalize");
	}

	// Where DuckDB's row-id expression put the rowid (PlanDelete). Not the last
	// column, as this assumed: a DELETE has no projection of its own, so its
	// child's columns come through, and a NOT EXISTS leaves its mark column
	// after the rowid -- DuckLake's snapshot expiry read that BOOLEAN as the key
	// (issue #337).
	const idx_t rowid_col_idx = target_.rowid_chunk_index;
	DELETE_DEBUG(1, "Execute: rowid at column %llu", (unsigned long long)rowid_col_idx);

	// Extract PK values from the rowid column using target's pk_info
	auto pk_values = ExtractPKFromRowid(chunk.data[rowid_col_idx], chunk.size(), target_.pk_info);

	// Accumulate into pending batch
	for (auto &pk : pk_values) {
		pending_pk_values_.push_back(std::move(pk));

		// Check if we need to flush the batch
		if (!config_.defer_to_finalize && pending_pk_values_.size() >= effective_batch_size_) {
			DELETE_DEBUG(1, "Execute: batch full, flushing %llu rows...",
						 (unsigned long long)pending_pk_values_.size());
			auto result = FlushBatch();
			if (!result.success) {
				throw IOException("%s", result.FormatError("DELETE"));
			}
		}
	}

	DELETE_DEBUG(1, "Execute: chunk processed, total_deleted=%llu, pending=%llu",
				 (unsigned long long)total_rows_deleted_, (unsigned long long)pending_pk_values_.size());

	return total_rows_deleted_;
}

MSSQLDMLResult MSSQLDeleteExecutor::Finalize() {
	DELETE_DEBUG(1, "Finalize: starting, finalized=%d, pending=%llu", finalized_,
				 (unsigned long long)pending_pk_values_.size());

	if (finalized_) {
		return MSSQLDMLResult::Success(total_rows_deleted_, batch_count_);
	}

	finalized_ = true;

	// Flush all remaining rows in batches
	while (!pending_pk_values_.empty()) {
		DELETE_DEBUG(1, "Finalize: flushing batch, pending=%llu", (unsigned long long)pending_pk_values_.size());
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
		return MSSQLDMLResult::Failure(string("DELETE failed: ") + e.what(), 0, batch_count_);
	}

	DELETE_DEBUG(1, "Finalize: done, total_deleted=%llu, batch_count=%llu", (unsigned long long)total_rows_deleted_,
				 (unsigned long long)batch_count_);
	return MSSQLDMLResult::Success(total_rows_deleted_, batch_count_);
}

MSSQLDMLResult MSSQLDeleteExecutor::FlushBatch() {
	if (pending_pk_values_.empty()) {
		return MSSQLDMLResult::Success(0, batch_count_);
	}

	batch_count_++;

	// Extract up to effective_batch_size_ rows for this batch
	vector<vector<Value>> batch_pk_values;
	idx_t rows_to_process = std::min<idx_t>(pending_pk_values_.size(), effective_batch_size_);

	batch_pk_values.reserve(rows_to_process);
	for (idx_t i = 0; i < rows_to_process; i++) {
		batch_pk_values.push_back(std::move(pending_pk_values_[i]));
	}

	// Remove processed rows from pending
	pending_pk_values_.erase(pending_pk_values_.begin(), pending_pk_values_.begin() + rows_to_process);

	DELETE_DEBUG(1, "FlushBatch: batch %llu with %llu rows (remaining=%llu)", (unsigned long long)batch_count_,
				 (unsigned long long)rows_to_process, (unsigned long long)pending_pk_values_.size());

	// Build the DELETE statement
	auto batch = statement_->Build(batch_pk_values);

	if (!batch.IsValid()) {
		return MSSQLDMLResult::Failure("Failed to build DELETE batch", 0, batch_count_);
	}

	DELETE_DEBUG(2, "FlushBatch: SQL preview: %.500s%s", batch.sql.c_str(), batch.sql.size() > 500 ? "..." : "");

	// Execute the batch
	return ExecuteBatch(batch);
}

MSSQLDMLResult MSSQLDeleteExecutor::ExecuteBatch(const MSSQLDMLBatch &batch) {
	DELETE_DEBUG(1, "ExecuteBatch: starting, sql_length=%zu", batch.sql.size());

	// Get the MSSQLCatalog for ConnectionProvider
	auto &catalog = Catalog::GetCatalog(context_, Identifier(target_.catalog_name));
	auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();

	// The statement's one connection -- pinned inside a DuckDB transaction,
	// else a pool connection with the statement's own server transaction begun
	// on it (spec 062 W1c).
	std::shared_ptr<tds::TdsConnection> connection;
	try {
		connection = stmt_conn_.Acquire(context_, mssql_catalog);
	} catch (const std::exception &e) {
		DELETE_DEBUG(1, "ExecuteBatch: failed to acquire connection");
		return MSSQLDMLResult::Failure(string("DELETE execution failed: ") + e.what(), 0, batch_count_);
	}
	auto pinned_lock = stmt_conn_.LockPinned(context_, mssql_catalog);
	MSSQLStatementConnection::RequireIdle(*connection, "DELETE");

	mssql::DmlBatchOptions options;
	options.label = "delete statement batch";
	mssql::DmlBatchResponse response;
	try {
		response = mssql::ExecuteDmlBatch(context_, *connection, batch.sql, options);
	} catch (const std::exception &e) {
		stmt_conn_.Fail(context_, mssql_catalog);
		return MSSQLDMLResult::Failure(string("DELETE execution failed: ") + MSSQLRawMessage(e), 0, batch_count_);
	}
	DELETE_DEBUG(1, "ExecuteBatch: rows_affected=%llu, error='%s'", (unsigned long long)response.rows_affected,
				 response.error_message.c_str());

	if (response.status == mssql::DmlBatchResponse::Status::INTERRUPTED) {
		stmt_conn_.Fail(context_, mssql_catalog);
		throw InterruptException();
	}
	if (!response.Succeeded()) {
		stmt_conn_.Fail(context_, mssql_catalog);
		// What happened to the rows (issue #344, spec 062 W1c) -- see the
		// UPDATE executor.
		const string outcome = MSSQLRowsBeforeOutcome(stmt_conn_.InOpenTransaction(), total_rows_deleted_);
		if (response.status == mssql::DmlBatchResponse::Status::PARSE_ERROR) {
			return MSSQLDMLResult::Failure(
				StringUtil::Format("DELETE failed: %s; the server executed this batch (batch %llu); %s",
								   response.error_message, (unsigned long long)batch_count_, outcome),
				0, batch_count_);
		}
		return MSSQLDMLResult::Failure(StringUtil::Format("DELETE failed: %s; %s", response.error_message, outcome), 0,
									   batch_count_);
	}

	// The connection stays with the statement until Finalize commits.
	total_rows_deleted_ += response.rows_affected;
	return MSSQLDMLResult::Success(response.rows_affected, batch_count_);
}

}  // namespace duckdb

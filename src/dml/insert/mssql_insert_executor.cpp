#include "dml/insert/mssql_insert_executor.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "catalog/mssql_catalog.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "connection/mssql_settings.hpp"
#include "dml/insert/mssql_batch_builder.hpp"
#include "dml/insert/mssql_insert_statement.hpp"
#include "dml/insert/mssql_returning_parser.hpp"
#include "dml/mssql_dml_outcome.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_connection_pool.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetInsertDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define INSERT_DEBUG(level, fmt, ...)                                   \
	do {                                                                \
		if (GetInsertDebugLevel() >= level) {                           \
			fprintf(stderr, "[MSSQL INSERT] " fmt "\n", ##__VA_ARGS__); \
		}                                                               \
	} while (0)

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLInsertException
//===----------------------------------------------------------------------===//

MSSQLInsertException::MSSQLInsertException(const MSSQLInsertError &error)
	: Exception(ExceptionType::IO, error.FormatMessage()), error_(error) {}

//===----------------------------------------------------------------------===//
// Constructor / Destructor
//===----------------------------------------------------------------------===//

MSSQLInsertExecutor::MSSQLInsertExecutor(ClientContext &context, const MSSQLInsertTarget &target,
										 const MSSQLInsertConfig &config)
	: context_(context), target_(target), config_(config), finalized_(false), connection_pool_(nullptr) {}

MSSQLInsertExecutor::~MSSQLInsertExecutor() {
	// Nothing is sent from here. This used to flush pending rows "in case the
	// caller forgets" -- and DuckDB never forgets on success, so the only time
	// it fired was an unwind that had not passed through FailStatement (a row
	// over mssql_insert_max_sql_bytes, another operator failing, an interrupt),
	// where it sent the pending rows and, since W1c, COMMITTED them: the
	// partial application W1c removes, back through the destructor (spec 062
	// self-review). The statement's transaction is rolled back and its
	// connection returned by ~MSSQLStatementConnection.
	if (!finalized_ && batch_builder_ && batch_builder_->HasPendingRows()) {
		INSERT_DEBUG(1, "~MSSQLInsertExecutor: %llu pending row(s) dropped on unwind, transaction rolled back",
					 (unsigned long long)batch_builder_->GetPendingRowCount());
	}
}

//===----------------------------------------------------------------------===//
// Connection Pool Access
//===----------------------------------------------------------------------===//

tds::ConnectionPool &MSSQLInsertExecutor::GetConnectionPool() {
	if (connection_pool_) {
		return *connection_pool_;
	}

	// Spec 047: route through DuckDB catalog (per-catalog pool ownership).
	// The MssqlPoolManager singleton is gone; pool is owned by MSSQLCatalog.
	auto &catalog = Catalog::GetCatalog(context_, Identifier(target_.catalog_name));
	auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();
	connection_pool_ = &mssql_catalog.GetConnectionPool();
	return *connection_pool_;
}

MSSQLCatalog &MSSQLInsertExecutor::GetMSSQLCatalog() {
	auto &catalog = Catalog::GetCatalog(context_, Identifier(target_.catalog_name));
	return catalog.Cast<MSSQLCatalog>();
}

//===----------------------------------------------------------------------===//
// Spec 077 W2: the identity column is in the list
//===----------------------------------------------------------------------===//

void MSSQLInsertExecutor::EnsureIdentityInsert() {
	if (!target_.identity_in_list || stmt_conn_.IdentityInsertOn()) {
		return;
	}
	try {
		stmt_conn_.EnableIdentityInsert(target_.schema_name, target_.table_name);
	} catch (...) {
		FailStatement(GetMSSQLCatalog());
		throw;
	}
}

void MSSQLInsertExecutor::RefuseNullIdentity(DataChunk &chunk) {
	if (!target_.identity_in_list) {
		return;
	}
	// The chunk is full-width in table order (see MSSQLBatchBuilder::SerializeRow).
	auto &vec = chunk.data[target_.identity_column_index];
	UnifiedVectorFormat fmt;
	vec.ToUnifiedFormat(fmt);
	if (fmt.validity.CannotHaveNull()) {
		return;
	}
	for (idx_t r = 0; r < chunk.size(); r++) {
		if (!fmt.validity.RowIsValid(fmt.sel->get_index(r))) {
			const auto &col = target_.columns[target_.identity_column_index];
			throw InvalidInputException(
				"MSSQL: the INSERT names the identity column '%s' of %s.%s but supplies no value for it in row %llu "
				"(DEFAULT and NULL both reach the extension as NULL, and the server assigns an identity value only "
				"when the column is left out of the list). Either leave '%s' out of the column list so the server "
				"assigns every row, or supply a value for every row; a statement mixing the two has to be split.",
				col.name, target_.schema_name, target_.table_name, (unsigned long long)r, col.name);
		}
	}
}

void MSSQLInsertExecutor::FailStatement(MSSQLCatalog &catalog) {
	failed_ = true;
	stmt_conn_.Fail(context_, catalog);
}

void MSSQLInsertExecutor::CommitStatement() {
	auto &catalog = GetMSSQLCatalog();
	try {
		stmt_conn_.Commit(context_, catalog);
	} catch (const std::exception &e) {
		failed_ = true;
		throw IOException("INSERT failed: %s", MSSQLRawMessage(e));
	}
}

//===----------------------------------------------------------------------===//
// Batch Builder Initialization
//===----------------------------------------------------------------------===//

void MSSQLInsertExecutor::EnsureBatchBuilder(bool with_output) {
	if (!batch_builder_) {
		batch_builder_ = make_uniq<MSSQLBatchBuilder>(target_, config_, with_output);
	}
}

//===----------------------------------------------------------------------===//
// Batch Execution
//===----------------------------------------------------------------------===//

idx_t MSSQLInsertExecutor::ExecuteBatch(const MSSQLInsertBatch &batch) {
	const string &sql = batch.sql_statement;
	INSERT_DEBUG(1, "ExecuteBatch: starting, sql_length=%zu", sql.size());
	// Print first 2000 chars of SQL for debugging
	INSERT_DEBUG(1, "ExecuteBatch: SQL preview: %.2000s%s", sql.c_str(), sql.size() > 2000 ? "..." : "");

	// The statement's one connection -- pinned inside a DuckDB transaction,
	// else a pool connection with the statement's own server transaction begun
	// on it (spec 062 W1c). Throws when none can be had.
	auto &mssql_catalog = GetMSSQLCatalog();
	auto connection = stmt_conn_.Acquire(context_, mssql_catalog);
	auto pinned_lock = stmt_conn_.LockPinned(context_, mssql_catalog);
	MSSQLStatementConnection::RequireIdle(*connection, "INSERT");
	EnsureIdentityInsert();

	auto start_time = std::chrono::steady_clock::now();
	mssql::DmlBatchOptions options;
	options.label = "insert statement batch";
	const auto response = RunBatch(*connection, batch, sql, options, mssql_catalog);
	if (!response.Succeeded()) {
		ThrowBatchFailure(batch, response, mssql_catalog);
	}
	const idx_t rows_affected = response.rows_affected;
	INSERT_DEBUG(1, "ExecuteBatch: rows_affected=%llu", (unsigned long long)rows_affected);

	// The connection stays with the statement until CommitStatement.
	auto end_time = std::chrono::steady_clock::now();
	auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
	statistics_.RecordBatch(rows_affected, sql.size(), duration_us);
	return rows_affected;
}

unique_ptr<DataChunk> MSSQLInsertExecutor::ExecuteBatchWithOutput(const MSSQLInsertBatch &batch,
																  const vector<idx_t> &returning_column_ids) {
	auto &mssql_catalog = GetMSSQLCatalog();
	auto connection = stmt_conn_.Acquire(context_, mssql_catalog);
	auto pinned_lock = stmt_conn_.LockPinned(context_, mssql_catalog);
	MSSQLStatementConnection::RequireIdle(*connection, "INSERT");
	EnsureIdentityInsert();

	auto start_time = std::chrono::steady_clock::now();
	MSSQLReturningParser output(target_, returning_column_ids);
	mssql::DmlBatchOptions options;
	options.label = "insert statement batch";
	output.Attach(options);
	auto response = RunBatch(*connection, batch, batch.sql_statement, options, mssql_catalog);
	if (response.status == mssql::DmlBatchResponse::Status::SERVER_ERROR && response.error_number == 334 &&
		target_.output_into_table && !batch_builder_->UsesOutputInto()) {
		// Issue #471: the target has an enabled trigger, beside which a bare
		// OUTPUT is refused. That is a compile error: nothing ran, and the
		// statement's server transaction is as it was. This batch and the rest
		// of the statement go through OUTPUT ... INTO a session table, which
		// costs a table created and dropped per batch -- measured 1.2 s for
		// 10000 rows where the bare form takes 0.09 s, so it is not the default.
		batch_builder_->UseOutputInto();
		const auto into = MSSQLInsertStatement(target_, true, true).Build(batch.row_literals);
		response = RunBatch(*connection, batch, into, options, mssql_catalog);
	}
	if (!response.Succeeded()) {
		ThrowBatchFailure(batch, response, mssql_catalog);
	}

	// Record statistics based on the rows the OUTPUT clause returned
	auto end_time = std::chrono::steady_clock::now();
	auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
	statistics_.RecordBatch(output.GetRowCount(), batch.sql_statement.size(), duration_us);
	return output.TakeResult();
}

mssql::DmlBatchResponse MSSQLInsertExecutor::RunBatch(tds::TdsConnection &connection, const MSSQLInsertBatch &batch,
													  const string &sql, const mssql::DmlBatchOptions &options,
													  MSSQLCatalog &catalog) {
	try {
		return mssql::ExecuteDmlBatch(context_, connection, sql, options);
	} catch (const std::exception &e) {
		// A fault of the client's own while reading the response: an OUTPUT
		// value that does not convert, a malformed packet header.
		MSSQLInsertError error;
		error.statement_index = batch_builder_->GetBatchCount() - 1;
		error.row_offset_start = batch.row_offset_start;
		error.row_offset_end = batch.row_offset_end;
		error.sql_error_number = 0;
		error.sql_error_message = MSSQLRawMessage(e);
		error.rows_applied_before = statistics_.total_rows_inserted;
		error.in_open_transaction = stmt_conn_.InOpenTransaction();
		FailStatement(catalog);
		throw MSSQLInsertException(error);
	}
}

void MSSQLInsertExecutor::ThrowBatchFailure(const MSSQLInsertBatch &batch, const mssql::DmlBatchResponse &response,
											MSSQLCatalog &catalog) {
	MSSQLInsertError error;
	error.statement_index = batch_builder_->GetBatchCount() - 1;
	error.row_offset_start = batch.row_offset_start;
	error.row_offset_end = batch.row_offset_end;
	error.rows_applied_before = statistics_.total_rows_inserted;
	error.in_open_transaction = stmt_conn_.InOpenTransaction();
	FailStatement(catalog);
	if (response.status == mssql::DmlBatchResponse::Status::INTERRUPTED) {
		throw InterruptException();
	}
	error.sql_error_number =
		response.status == mssql::DmlBatchResponse::Status::SERVER_ERROR ? response.error_number : 0;
	error.sql_error_message = response.error_message;
	// A parse error is a client-side framing failure: the server ran this
	// statement (issue #344). The message says so; a SQL error says the server
	// rejected it.
	error.statement_executed = response.status == mssql::DmlBatchResponse::Status::PARSE_ERROR;
	throw MSSQLInsertException(error);
}

//===----------------------------------------------------------------------===//
// Execute (Mode A: Bulk Insert)
//===----------------------------------------------------------------------===//

idx_t MSSQLInsertExecutor::Execute(DataChunk &input_chunk) {
	INSERT_DEBUG(1, "Execute: chunk_size=%llu", (unsigned long long)input_chunk.size());

	if (finalized_) {
		throw InternalException("MSSQLInsertExecutor::Execute called after Finalize");
	}
	if (failed_) {
		throw InternalException("MSSQLInsertExecutor::Execute called after a batch failed");
	}

	EnsureBatchBuilder(false);
	RefuseNullIdentity(input_chunk);

	idx_t total_inserted = 0;

	// Process each row in the chunk
	for (idx_t row_idx = 0; row_idx < input_chunk.size(); row_idx++) {
		// Try to add row to current batch
		if (!batch_builder_->AddRow(input_chunk, row_idx)) {
			// Batch is full, flush it
			INSERT_DEBUG(1, "Execute: batch full at row %llu, flushing...", (unsigned long long)row_idx);
			auto batch = batch_builder_->FlushBatch();
			INSERT_DEBUG(1, "Execute: flushed batch with %llu rows, %llu bytes", (unsigned long long)batch.row_count,
						 (unsigned long long)batch.sql_bytes);
			total_inserted += ExecuteBatch(batch);

			// Now add the row that didn't fit
			if (!batch_builder_->AddRow(input_chunk, row_idx)) {
				throw InternalException("Failed to add row to empty batch");
			}
		}
	}

	INSERT_DEBUG(1, "Execute: chunk processed, total_inserted=%llu, pending=%llu", (unsigned long long)total_inserted,
				 (unsigned long long)batch_builder_->GetPendingRowCount());

	return total_inserted;
}

//===----------------------------------------------------------------------===//
// Execute with RETURNING (Mode B)
//===----------------------------------------------------------------------===//

vector<unique_ptr<DataChunk>> MSSQLInsertExecutor::ExecuteWithReturning(DataChunk &input_chunk,
																		const vector<idx_t> &returning_column_ids) {
	if (finalized_) {
		throw InternalException("MSSQLInsertExecutor::ExecuteWithReturning called after Finalize");
	}
	if (failed_) {
		throw InternalException("MSSQLInsertExecutor::ExecuteWithReturning called after a batch failed");
	}

	EnsureBatchBuilder(true);
	RefuseNullIdentity(input_chunk);

	// Store returning column IDs for later use
	returning_column_ids_ = returning_column_ids;

	// One result chunk per statement this input chunk completes. This used to
	// keep the LAST one only ("for simplicity"), so an INSERT ... RETURNING of
	// more rows than one statement carries silently lost the earlier
	// statements' rows -- above 1000 rows before, above 1000 / columns rows
	// once spec 062 W1b sized statements under the auto-parameterisation line.
	vector<unique_ptr<DataChunk>> results;

	// Process each row in the chunk
	for (idx_t row_idx = 0; row_idx < input_chunk.size(); row_idx++) {
		// Try to add row to current batch
		if (!batch_builder_->AddRow(input_chunk, row_idx)) {
			// Batch is full, flush it with OUTPUT
			auto batch = batch_builder_->FlushBatch();
			auto batch_result = ExecuteBatchWithOutput(batch, returning_column_ids);
			if (batch_result && batch_result->size() > 0) {
				results.push_back(std::move(batch_result));
			}

			// Now add the row that didn't fit
			if (!batch_builder_->AddRow(input_chunk, row_idx)) {
				throw InternalException("Failed to add row to empty batch");
			}
		}
	}

	return results;
}

//===----------------------------------------------------------------------===//
// Finalization
//===----------------------------------------------------------------------===//

void MSSQLInsertExecutor::Finalize() {
	INSERT_DEBUG(1, "Finalize: starting, finalized=%d, has_builder=%d", finalized_, batch_builder_ != nullptr);

	if (finalized_) {
		INSERT_DEBUG(1, "Finalize: already finalized, returning");
		return;
	}
	if (failed_) {
		throw InternalException("MSSQLInsertExecutor::Finalize called after a batch failed");
	}

	finalized_ = true;

	if (batch_builder_ && batch_builder_->HasPendingRows()) {
		INSERT_DEBUG(1, "Finalize: flushing %llu pending rows",
					 (unsigned long long)batch_builder_->GetPendingRowCount());
		auto batch = batch_builder_->FlushBatch();
		INSERT_DEBUG(1, "Finalize: executing final batch with %llu bytes", (unsigned long long)batch.sql_bytes);
		ExecuteBatch(batch);
		INSERT_DEBUG(1, "Finalize: done");
	} else {
		INSERT_DEBUG(1, "Finalize: no pending rows");
	}
	// Every batch is in: one COMMIT for the statement (a no-op inside a DuckDB
	// transaction, and when no batch was ever sent).
	CommitStatement();
}

unique_ptr<DataChunk> MSSQLInsertExecutor::FinalizeWithReturning() {
	if (finalized_) {
		return nullptr;
	}
	if (failed_) {
		throw InternalException("MSSQLInsertExecutor::FinalizeWithReturning called after a batch failed");
	}

	finalized_ = true;

	unique_ptr<DataChunk> result;
	if (batch_builder_ && batch_builder_->HasPendingRows()) {
		auto batch = batch_builder_->FlushBatch();
		result = ExecuteBatchWithOutput(batch, returning_column_ids_);
	}
	CommitStatement();
	return result;
}

//===----------------------------------------------------------------------===//
// Statistics
//===----------------------------------------------------------------------===//

const MSSQLInsertStatistics &MSSQLInsertExecutor::GetStatistics() const {
	return statistics_;
}

idx_t MSSQLInsertExecutor::GetTotalRowsInserted() const {
	return statistics_.total_rows_inserted;
}

}  // namespace duckdb

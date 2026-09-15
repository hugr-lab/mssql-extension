#include "dml/insert/mssql_insert_executor.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "catalog/mssql_catalog.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "connection/mssql_settings.hpp"
#include "dml/insert/mssql_batch_builder.hpp"
#include "dml/insert/mssql_returning_parser.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "tds/encoding/type_converter.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_connection_pool.hpp"
#include "tds/tds_packet.hpp"
#include "tds/tds_token_parser.hpp"

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
		throw IOException("INSERT failed: %s", e.what());
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

	INSERT_DEBUG(2, "ExecuteBatch: connection acquired, state=%d", (int)connection->GetState());

	idx_t rows_affected = 0;
	auto start_time = std::chrono::steady_clock::now();

	try {
		// Get socket for packet-based reading
		auto *socket = connection->GetSocket();
		if (!socket) {
			INSERT_DEBUG(1, "ExecuteBatch: socket is null");
			FailStatement(mssql_catalog);
			throw IOException("Connection socket is null");
		}

		INSERT_DEBUG(2, "ExecuteBatch: socket obtained, connected=%d", socket->IsConnected());

		// Clear any leftover data before starting
		socket->ClearReceiveBuffer();

		// Send the SQL batch
		INSERT_DEBUG(1, "ExecuteBatch: sending SQL batch...");
		if (!connection->ExecuteBatch(sql)) {
			INSERT_DEBUG(1, "ExecuteBatch: ExecuteBatch failed, error=%s", connection->GetLastError().c_str());
			MSSQLInsertError error;
			error.statement_index = batch_builder_->GetBatchCount() - 1;
			error.row_offset_start = batch.row_offset_start;
			error.row_offset_end = batch.row_offset_end;
			error.sql_error_number = 0;
			error.sql_error_message = connection->GetLastError();
			error.rows_applied_before = statistics_.total_rows_inserted;
			error.in_open_transaction = stmt_conn_.IsPinned();

			FailStatement(mssql_catalog);
			throw MSSQLInsertException(error);
		}

		INSERT_DEBUG(1, "ExecuteBatch: SQL sent successfully, waiting for response...");

		// Parse the TDS response to get error info and row counts
		tds::TokenParser parser;
		const int64_t fail_after_tokens = LoadTestFailParseAfterTokens(context_);
		int64_t tokens_seen = 0;
		bool done = false;
		int timeout_ms = 30000;	 // 30 second timeout
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
		string error_message;
		uint32_t error_number = 0;
		int packet_count = 0;

		while (!done) {
			// Check timeout
			auto now = std::chrono::steady_clock::now();
			if (now >= deadline) {
				INSERT_DEBUG(1, "ExecuteBatch: TIMEOUT after 30s, packets_received=%d", packet_count);
				connection->SendAttention();
				connection->WaitForAttentionAck(5000);
				FailStatement(mssql_catalog);
				throw IOException("INSERT execution timeout");
			}

			auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
			int recv_timeout = static_cast<int>(std::min<long long>(remaining_ms, timeout_ms));

			INSERT_DEBUG(2, "ExecuteBatch: calling ReceivePacket, timeout=%d, packets_so_far=%d", recv_timeout,
						 packet_count);

			// Read TDS packet
			tds::TdsPacket packet;
			if (!socket->ReceivePacket(packet, recv_timeout)) {
				// Capture error message BEFORE releasing connection
				string socket_error = socket->GetLastError();
				bool still_connected = socket->IsConnected();
				INSERT_DEBUG(1, "ExecuteBatch: ReceivePacket FAILED, error='%s', connected=%d", socket_error.c_str(),
							 still_connected);
				FailStatement(mssql_catalog);
				throw IOException("Failed to receive TDS packet: %s", socket_error);
			}

			packet_count++;
			INSERT_DEBUG(2, "ExecuteBatch: packet %d received, size=%zu, eom=%d", packet_count,
						 packet.GetPayload().size(), packet.IsEndOfMessage());

			bool is_eom = packet.IsEndOfMessage();

			// Feed packet payload to parser -- but not into a parser already in
			// Error. From there TryParseNext returns at once, so ConsumeBytes and
			// with it CompactBuffer never run, and every fed byte is retained;
			// a desync early in a batched response buffers the whole tail for
			// nothing. The drain to EOM below still runs: that is what leaves the
			// socket clean for the next statement. MSSQLSimpleQuery already does
			// this (issue #323); these four loops are copies of the same loop.
			const auto &payload = packet.GetPayload();
			if (!payload.empty() && parser.GetState() != tds::ParserState::Error) {
				parser.Feed(payload);
			}

			// Parse tokens
			tds::ParsedTokenType token;
			while ((token = parser.TryParseNext()) != tds::ParsedTokenType::NeedMoreData) {
				if (fail_after_tokens > 0 && ++tokens_seen >= fail_after_tokens) {
					// Test lever (mssql_test_fail_parse_after_tokens): drop this token
					// and desync, as a framing error would.
					parser.InjectParseError("injected parse error (mssql_test_fail_parse_after_tokens)");
					break;
				}
				INSERT_DEBUG(2, "ExecuteBatch: parsed token type=%d", (int)token);
				switch (token) {
				case tds::ParsedTokenType::Done: {
					const tds::DoneToken &done_token = parser.GetDone();
					INSERT_DEBUG(
						1, "ExecuteBatch: DONE token - status=0x%04x, row_count=%llu, has_row_count=%d, is_final=%d",
						done_token.status, (unsigned long long)done_token.row_count, done_token.HasRowCount(),
						done_token.IsFinal());
					if (done_token.HasRowCount()) {
						rows_affected = done_token.row_count;
					}
					if (done_token.IsFinal()) {
						done = true;
						// Transition connection back to Idle
						connection->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
					}
					break;
				}
				case tds::ParsedTokenType::Error: {
					const tds::TdsError &tds_error = parser.GetError();
					error_number = tds_error.number;
					error_message = tds_error.message;
					INSERT_DEBUG(1, "ExecuteBatch: ERROR token - number=%u, message='%s'", error_number,
								 error_message.c_str());
					// Continue reading to drain the response
					break;
				}
				default:
					// Skip other tokens
					break;
				}
			}

			// A parser stuck in Error answers NeedMoreData forever, so the token
			// loop above has already exited and the EOM branch below forces done
			// and reports SUCCESS -- dropping every token after the desync,
			// including a SQL Server ERROR token following a stored-procedure
			// call, which is the one a caller most needs. Record it as the
			// failure it is. A SQL error already captured keeps precedence.
			if (parser.GetState() == tds::ParserState::Error && error_message.empty()) {
				error_number = 0;
				error_message = "TDS parse error: " + parser.GetParseError();
				INSERT_DEBUG(1, "ExecuteBatch: %s", error_message.c_str());
			}

			// Handle EOM without done token
			if (is_eom && !done) {
				INSERT_DEBUG(1, "ExecuteBatch: EOM without DONE final, marking done");
				done = true;
				connection->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
			}
		}

		INSERT_DEBUG(1, "ExecuteBatch: response parsed, rows_affected=%llu, error='%s'",
					 (unsigned long long)rows_affected, error_message.c_str());

		// Check for errors
		if (!error_message.empty()) {
			MSSQLInsertError error;
			error.statement_index = batch_builder_->GetBatchCount() - 1;
			error.row_offset_start = batch.row_offset_start;
			error.row_offset_end = batch.row_offset_end;
			error.sql_error_number = error_number;
			error.sql_error_message = error_message;
			error.rows_applied_before = statistics_.total_rows_inserted;
			error.in_open_transaction = stmt_conn_.IsPinned();
			// A parse error is a client-side framing failure: the server ran this
			// statement (issue #344). The message says so; a SQL error says the
			// server rejected it.
			error.statement_executed = (error_number == 0);

			FailStatement(mssql_catalog);
			throw MSSQLInsertException(error);
		}

	} catch (const MSSQLInsertException &) {
		throw;	// Re-throw insert exceptions
	} catch (const IOException &) {
		throw;	// Already failed and worded above (timeout, socket)
	} catch (const std::exception &e) {
		FailStatement(mssql_catalog);
		throw IOException("INSERT execution failed: %s", e.what());
	}

	// The connection stays with the statement until CommitStatement.

	// Record timing
	auto end_time = std::chrono::steady_clock::now();
	auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
	statistics_.RecordBatch(rows_affected, sql.size(), duration_us);

	return rows_affected;
}

unique_ptr<DataChunk> MSSQLInsertExecutor::ExecuteBatchWithOutput(const MSSQLInsertBatch &batch,
																  const vector<idx_t> &returning_column_ids) {
	const string &sql = batch.sql_statement;
	auto &mssql_catalog = GetMSSQLCatalog();
	auto connection = stmt_conn_.Acquire(context_, mssql_catalog);

	auto start_time = std::chrono::steady_clock::now();
	unique_ptr<DataChunk> result_chunk;

	try {
		// Get socket for packet-based reading
		auto *socket = connection->GetSocket();
		if (!socket) {
			FailStatement(mssql_catalog);
			throw IOException("Connection socket is null");
		}

		// Clear any leftover data before starting
		socket->ClearReceiveBuffer();

		// Send the SQL batch (with OUTPUT clause)
		if (!connection->ExecuteBatch(sql)) {
			MSSQLInsertError error;
			error.statement_index = batch_builder_->GetBatchCount() - 1;
			error.row_offset_start = batch.row_offset_start;
			error.row_offset_end = batch.row_offset_end;
			error.sql_error_number = 0;
			error.sql_error_message = connection->GetLastError();
			error.rows_applied_before = statistics_.total_rows_inserted;
			error.in_open_transaction = stmt_conn_.IsPinned();

			FailStatement(mssql_catalog);
			throw MSSQLInsertException(error);
		}

		// Parse the OUTPUT INSERTED results using MSSQLReturningParser
		MSSQLReturningParser parser(target_, returning_column_ids);
		result_chunk = parser.ParseResponse(*connection, 30000, LoadTestFailParseAfterTokens(context_));

		// Check for errors from parser
		if (parser.HasError()) {
			MSSQLInsertError error;
			error.statement_index = batch_builder_->GetBatchCount() - 1;
			error.row_offset_start = batch.row_offset_start;
			error.row_offset_end = batch.row_offset_end;
			error.sql_error_number = parser.GetErrorNumber();
			error.sql_error_message = parser.GetErrorMessage();
			error.rows_applied_before = statistics_.total_rows_inserted;
			error.in_open_transaction = stmt_conn_.IsPinned();
			error.statement_executed = parser.IsParseError();

			FailStatement(mssql_catalog);
			throw MSSQLInsertException(error);
		}

		// Record statistics based on parsed rows
		idx_t rows_inserted = parser.GetRowCount();
		auto end_time = std::chrono::steady_clock::now();
		auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
		statistics_.RecordBatch(rows_inserted, sql.size(), duration_us);

	} catch (const MSSQLInsertException &) {
		throw;	// Re-throw insert exceptions
	} catch (const IOException &) {
		throw;	// Already failed and worded above
	} catch (const std::exception &e) {
		FailStatement(mssql_catalog);
		throw IOException("INSERT with RETURNING execution failed: %s", e.what());
	}

	return result_chunk;
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

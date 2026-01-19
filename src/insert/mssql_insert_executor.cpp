#include "insert/mssql_insert_executor.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "catalog/mssql_catalog.hpp"
#include "connection/mssql_pool_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "insert/mssql_batch_builder.hpp"
#include "insert/mssql_returning_parser.hpp"
#include "tds/encoding/type_converter.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_connection_pool.hpp"
#include "tds/tds_packet.hpp"
#include "tds/tds_token_parser.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetInsertDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
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
	// Ensure we finalize even if caller forgets
	if (!finalized_ && batch_builder_ && batch_builder_->HasPendingRows()) {
		try {
			Finalize();
		} catch (...) {
			// Ignore errors in destructor
		}
	}
}

//===----------------------------------------------------------------------===//
// Connection Pool Access
//===----------------------------------------------------------------------===//

tds::ConnectionPool &MSSQLInsertExecutor::GetConnectionPool() {
	if (connection_pool_) {
		return *connection_pool_;
	}

	// Get the connection pool from the pool manager using the catalog name
	auto *pool = MssqlPoolManager::Instance().GetPool(target_.catalog_name);
	if (!pool) {
		throw IOException("MSSQL connection pool for catalog '%s' not found", target_.catalog_name);
	}

	connection_pool_ = pool;
	return *connection_pool_;
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

idx_t MSSQLInsertExecutor::ExecuteBatch(const string &sql) {
	INSERT_DEBUG(1, "ExecuteBatch: starting, sql_length=%zu", sql.size());
	// Print first 500 chars of SQL for debugging
	INSERT_DEBUG(1, "ExecuteBatch: SQL preview: %.500s%s", sql.c_str(), sql.size() > 500 ? "..." : "");

	auto &pool = GetConnectionPool();
	auto connection = pool.Acquire();
	if (!connection) {
		INSERT_DEBUG(1, "ExecuteBatch: failed to acquire connection");
		throw IOException("Failed to acquire connection for INSERT execution");
	}

	INSERT_DEBUG(2, "ExecuteBatch: connection acquired, state=%d", (int)connection->GetState());

	idx_t rows_affected = 0;
	auto start_time = std::chrono::steady_clock::now();

	try {
		// Get socket for packet-based reading
		auto *socket = connection->GetSocket();
		if (!socket) {
			INSERT_DEBUG(1, "ExecuteBatch: socket is null");
			pool.Release(std::move(connection));
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
			error.statement_index = batch_builder_->GetBatchCount();
			error.row_offset_start = batch_builder_->GetCurrentRowOffset() - batch_builder_->GetPendingRowCount();
			error.row_offset_end = batch_builder_->GetCurrentRowOffset();
			error.sql_error_number = 0;
			error.sql_error_message = connection->GetLastError();

			pool.Release(std::move(connection));
			throw MSSQLInsertException(error);
		}

		INSERT_DEBUG(1, "ExecuteBatch: SQL sent successfully, waiting for response...");

		// Parse the TDS response to get error info and row counts
		tds::TokenParser parser;
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
				pool.Release(std::move(connection));
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
				pool.Release(std::move(connection));
				throw IOException("Failed to receive TDS packet: %s", socket_error);
			}

			packet_count++;
			INSERT_DEBUG(2, "ExecuteBatch: packet %d received, size=%zu, eom=%d", packet_count,
						 packet.GetPayload().size(), packet.IsEndOfMessage());

			bool is_eom = packet.IsEndOfMessage();

			// Feed packet payload to parser
			const auto &payload = packet.GetPayload();
			if (!payload.empty()) {
				parser.Feed(payload);
			}

			// Parse tokens
			tds::ParsedTokenType token;
			while ((token = parser.TryParseNext()) != tds::ParsedTokenType::NeedMoreData) {
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
			error.statement_index = batch_builder_->GetBatchCount();
			error.row_offset_start = batch_builder_->GetCurrentRowOffset() - batch_builder_->GetPendingRowCount();
			error.row_offset_end = batch_builder_->GetCurrentRowOffset();
			error.sql_error_number = error_number;
			error.sql_error_message = error_message;

			pool.Release(std::move(connection));
			throw MSSQLInsertException(error);
		}

	} catch (const MSSQLInsertException &) {
		throw;	// Re-throw insert exceptions
	} catch (const std::exception &e) {
		pool.Release(std::move(connection));
		throw IOException("INSERT execution failed: %s", e.what());
	}

	pool.Release(std::move(connection));

	// Record timing
	auto end_time = std::chrono::steady_clock::now();
	auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
	statistics_.RecordBatch(rows_affected, sql.size(), duration_us);

	return rows_affected;
}

unique_ptr<DataChunk> MSSQLInsertExecutor::ExecuteBatchWithOutput(const string &sql,
																  const vector<idx_t> &returning_column_ids) {
	auto &pool = GetConnectionPool();
	auto connection = pool.Acquire();
	if (!connection) {
		throw IOException("Failed to acquire connection for INSERT execution");
	}

	auto start_time = std::chrono::steady_clock::now();
	unique_ptr<DataChunk> result_chunk;

	try {
		// Get socket for packet-based reading
		auto *socket = connection->GetSocket();
		if (!socket) {
			pool.Release(std::move(connection));
			throw IOException("Connection socket is null");
		}

		// Clear any leftover data before starting
		socket->ClearReceiveBuffer();

		// Send the SQL batch (with OUTPUT clause)
		if (!connection->ExecuteBatch(sql)) {
			MSSQLInsertError error;
			error.statement_index = batch_builder_->GetBatchCount();
			error.row_offset_start = batch_builder_->GetCurrentRowOffset() - batch_builder_->GetPendingRowCount();
			error.row_offset_end = batch_builder_->GetCurrentRowOffset();
			error.sql_error_number = 0;
			error.sql_error_message = connection->GetLastError();

			pool.Release(std::move(connection));
			throw MSSQLInsertException(error);
		}

		// Parse the OUTPUT INSERTED results using MSSQLReturningParser
		MSSQLReturningParser parser(target_, returning_column_ids);
		result_chunk = parser.ParseResponse(*connection, 30000);

		// Check for errors from parser
		if (parser.HasError()) {
			MSSQLInsertError error;
			error.statement_index = batch_builder_->GetBatchCount();
			error.row_offset_start = batch_builder_->GetCurrentRowOffset() - batch_builder_->GetPendingRowCount();
			error.row_offset_end = batch_builder_->GetCurrentRowOffset();
			error.sql_error_number = parser.GetErrorNumber();
			error.sql_error_message = parser.GetErrorMessage();

			pool.Release(std::move(connection));
			throw MSSQLInsertException(error);
		}

		// Record statistics based on parsed rows
		idx_t rows_inserted = parser.GetRowCount();
		auto end_time = std::chrono::steady_clock::now();
		auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
		statistics_.RecordBatch(rows_inserted, sql.size(), duration_us);

	} catch (const MSSQLInsertException &) {
		throw;	// Re-throw insert exceptions
	} catch (const std::exception &e) {
		pool.Release(std::move(connection));
		throw IOException("INSERT with RETURNING execution failed: %s", e.what());
	}

	pool.Release(std::move(connection));
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
			total_inserted += ExecuteBatch(batch.sql_statement);

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

unique_ptr<DataChunk> MSSQLInsertExecutor::ExecuteWithReturning(DataChunk &input_chunk,
																const vector<idx_t> &returning_column_ids) {
	if (finalized_) {
		throw InternalException("MSSQLInsertExecutor::ExecuteWithReturning called after Finalize");
	}

	EnsureBatchBuilder(true);

	// Store returning column IDs for later use
	returning_column_ids_ = returning_column_ids;

	// Accumulate results across batches
	unique_ptr<DataChunk> accumulated_results;

	// Process each row in the chunk
	for (idx_t row_idx = 0; row_idx < input_chunk.size(); row_idx++) {
		// Try to add row to current batch
		if (!batch_builder_->AddRow(input_chunk, row_idx)) {
			// Batch is full, flush it with OUTPUT
			auto batch = batch_builder_->FlushBatch();
			auto batch_result = ExecuteBatchWithOutput(batch.sql_statement, returning_column_ids);

			// Accumulate results
			if (batch_result) {
				if (!accumulated_results) {
					accumulated_results = std::move(batch_result);
				} else {
					// Append batch_result to accumulated_results
					// For simplicity, we'll just return the last batch for now
					// Full accumulation would require a more complex data structure
					accumulated_results = std::move(batch_result);
				}
			}

			// Now add the row that didn't fit
			if (!batch_builder_->AddRow(input_chunk, row_idx)) {
				throw InternalException("Failed to add row to empty batch");
			}
		}
	}

	return accumulated_results;
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

	finalized_ = true;

	if (batch_builder_ && batch_builder_->HasPendingRows()) {
		INSERT_DEBUG(1, "Finalize: flushing %llu pending rows",
					 (unsigned long long)batch_builder_->GetPendingRowCount());
		auto batch = batch_builder_->FlushBatch();
		INSERT_DEBUG(1, "Finalize: executing final batch with %llu bytes", (unsigned long long)batch.sql_bytes);
		ExecuteBatch(batch.sql_statement);
		INSERT_DEBUG(1, "Finalize: done");
	} else {
		INSERT_DEBUG(1, "Finalize: no pending rows");
	}
}

unique_ptr<DataChunk> MSSQLInsertExecutor::FinalizeWithReturning() {
	if (finalized_) {
		return nullptr;
	}

	finalized_ = true;

	if (batch_builder_ && batch_builder_->HasPendingRows()) {
		auto batch = batch_builder_->FlushBatch();
		return ExecuteBatchWithOutput(batch.sql_statement, returning_column_ids_);
	}

	return nullptr;
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

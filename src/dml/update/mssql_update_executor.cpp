#include "dml/update/mssql_update_executor.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_transaction.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "connection/mssql_pool_manager.hpp"
#include "dml/mssql_rowid_extractor.hpp"
#include "dml/update/mssql_update_statement.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "tds/tds_connection_pool.hpp"
#include "tds/tds_packet.hpp"
#include "tds/tds_token_parser.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetUpdateDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
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

	// Check if we need to defer execution until Finalize
	// This is required when in an explicit transaction because:
	// 1. The scan uses the pinned connection to stream rowids
	// 2. UPDATE batches would need the same pinned connection
	// 3. But the connection is in "Executing" state while streaming
	// Solution: Buffer all data during Sink, execute in Finalize after scan completes
	if (!context.transaction.IsAutoCommit()) {
		auto &catalog = Catalog::GetCatalog(context, target_.catalog_name);
		auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();
		if (ConnectionProvider::IsInTransaction(context, mssql_catalog)) {
			defer_execution_ = true;
			UPDATE_DEBUG(1, "UpdateExecutor: defer_execution=true (in transaction)");
		}
	}
}

MSSQLUpdateExecutor::~MSSQLUpdateExecutor() = default;

tds::ConnectionPool &MSSQLUpdateExecutor::GetConnectionPool() {
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

idx_t MSSQLUpdateExecutor::Execute(DataChunk &chunk) {
	UPDATE_DEBUG(1, "Execute: chunk_size=%llu", (unsigned long long)chunk.size());

	if (finalized_) {
		throw InternalException("MSSQLUpdateExecutor::Execute called after Finalize");
	}

	// Process each row in the chunk
	for (idx_t row_idx = 0; row_idx < chunk.size(); row_idx++) {
		AccumulateRow(chunk, row_idx);

		// Check if we need to flush the batch
		// In defer_execution_ mode, we accumulate everything and flush in Finalize
		if (!defer_execution_ && pending_pk_values_.size() >= effective_batch_size_) {
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
	UPDATE_DEBUG(1, "Finalize: starting, finalized=%d, pending=%llu, defer_execution=%d", finalized_,
				 (unsigned long long)pending_pk_values_.size(), defer_execution_);

	if (finalized_) {
		return MSSQLDMLResult::Success(total_rows_updated_, batch_count_);
	}

	finalized_ = true;

	// Flush all remaining rows in batches
	// In defer_execution_ mode, we may have accumulated many batches worth of rows
	while (!pending_pk_values_.empty()) {
		UPDATE_DEBUG(1, "Finalize: flushing batch, pending=%llu", (unsigned long long)pending_pk_values_.size());
		auto result = FlushBatch();
		if (!result.success) {
			return result;
		}
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
	} catch (const std::exception &e) {
		return MSSQLDMLResult::Failure(e.what(), 0, batch_count_);
	}
}

idx_t MSSQLUpdateExecutor::ExecuteBatch(const string &sql) {
	UPDATE_DEBUG(1, "ExecuteBatch: starting, sql_length=%zu", sql.size());

	// Get the MSSQLCatalog for ConnectionProvider
	auto &catalog = Catalog::GetCatalog(context_, target_.catalog_name);
	auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();

	// Acquire connection via ConnectionProvider (handles transaction pinning)
	auto connection = ConnectionProvider::GetConnection(context_, mssql_catalog);
	if (!connection) {
		UPDATE_DEBUG(1, "ExecuteBatch: failed to acquire connection");
		throw IOException("Failed to acquire connection for UPDATE execution");
	}

	UPDATE_DEBUG(2, "ExecuteBatch: connection acquired");

	idx_t rows_affected = 0;

	try {
		// Get socket for packet-based reading
		auto *socket = connection->GetSocket();
		if (!socket) {
			UPDATE_DEBUG(1, "ExecuteBatch: socket is null");
			ConnectionProvider::ReleaseConnection(context_, mssql_catalog, std::move(connection));
			throw IOException("Connection socket is null");
		}

		// Clear any leftover data before starting
		socket->ClearReceiveBuffer();

		// Send the SQL batch
		UPDATE_DEBUG(1, "ExecuteBatch: sending SQL batch...");
		if (!connection->ExecuteBatch(sql)) {
			string error = connection->GetLastError();
			UPDATE_DEBUG(1, "ExecuteBatch: ExecuteBatch failed, error=%s", error.c_str());
			ConnectionProvider::ReleaseConnection(context_, mssql_catalog, std::move(connection));
			throw IOException("UPDATE execution failed: %s", error);
		}

		UPDATE_DEBUG(1, "ExecuteBatch: SQL sent successfully, waiting for response...");

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
				UPDATE_DEBUG(1, "ExecuteBatch: TIMEOUT after 30s, packets_received=%d", packet_count);
				connection->SendAttention();
				connection->WaitForAttentionAck(5000);
				ConnectionProvider::ReleaseConnection(context_, mssql_catalog, std::move(connection));
				throw IOException("UPDATE execution timeout");
			}

			auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
			int recv_timeout = static_cast<int>(std::min<long long>(remaining_ms, timeout_ms));

			// Read TDS packet
			tds::TdsPacket packet;
			if (!socket->ReceivePacket(packet, recv_timeout)) {
				string socket_error = socket->GetLastError();
				UPDATE_DEBUG(1, "ExecuteBatch: ReceivePacket FAILED, error='%s'", socket_error.c_str());
				ConnectionProvider::ReleaseConnection(context_, mssql_catalog, std::move(connection));
				throw IOException("Failed to receive TDS packet: %s", socket_error);
			}

			packet_count++;
			UPDATE_DEBUG(2, "ExecuteBatch: packet %d received, size=%zu, eom=%d", packet_count,
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
				UPDATE_DEBUG(2, "ExecuteBatch: parsed token type=%d", (int)token);
				switch (token) {
				case tds::ParsedTokenType::Done: {
					const tds::DoneToken &done_token = parser.GetDone();
					UPDATE_DEBUG(1, "ExecuteBatch: DONE token - status=0x%04x, row_count=%llu, has_row_count=%d",
								 done_token.status, (unsigned long long)done_token.row_count, done_token.HasRowCount());
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
					UPDATE_DEBUG(1, "ExecuteBatch: ERROR token - number=%u, message='%s'", error_number,
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
				UPDATE_DEBUG(1, "ExecuteBatch: EOM without DONE final, marking done");
				done = true;
				connection->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
			}
		}

		UPDATE_DEBUG(1, "ExecuteBatch: response parsed, rows_affected=%llu, error='%s'",
					 (unsigned long long)rows_affected, error_message.c_str());

		// Check for errors
		if (!error_message.empty()) {
			ConnectionProvider::ReleaseConnection(context_, mssql_catalog, std::move(connection));
			throw IOException("UPDATE failed: %s", error_message);
		}

	} catch (const IOException &) {
		throw;	// Re-throw IO exceptions
	} catch (const std::exception &e) {
		ConnectionProvider::ReleaseConnection(context_, mssql_catalog, std::move(connection));
		throw IOException("UPDATE execution failed: %s", e.what());
	}

	ConnectionProvider::ReleaseConnection(context_, mssql_catalog, std::move(connection));
	return rows_affected;
}

}  // namespace duckdb

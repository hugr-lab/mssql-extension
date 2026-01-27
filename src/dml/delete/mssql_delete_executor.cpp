#include "dml/delete/mssql_delete_executor.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_transaction.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "connection/mssql_pool_manager.hpp"
#include "dml/delete/mssql_delete_statement.hpp"
#include "dml/mssql_rowid_extractor.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "tds/tds_connection_pool.hpp"
#include "tds/tds_packet.hpp"
#include "tds/tds_token_parser.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetDeleteDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
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

	// Check if we need to defer execution until Finalize
	// This is required when in an explicit transaction because:
	// 1. The scan uses the pinned connection to stream rowids
	// 2. DELETE batches would need the same pinned connection
	// 3. But the connection is in "Executing" state while streaming
	// Solution: Buffer all rowids during Sink, execute in Finalize after scan completes
	if (!context.transaction.IsAutoCommit()) {
		auto &catalog = Catalog::GetCatalog(context, target_.catalog_name);
		auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();
		if (ConnectionProvider::IsInTransaction(context, mssql_catalog)) {
			defer_execution_ = true;
			DELETE_DEBUG(1, "DeleteExecutor: defer_execution=true (in transaction)");
		}
	}
}

MSSQLDeleteExecutor::~MSSQLDeleteExecutor() = default;

idx_t MSSQLDeleteExecutor::Execute(DataChunk &chunk) {
	DELETE_DEBUG(1, "Execute: chunk_size=%llu, column_count=%llu", (unsigned long long)chunk.size(),
				 (unsigned long long)chunk.ColumnCount());

	if (finalized_) {
		throw InternalException("MSSQLDeleteExecutor::Execute called after Finalize");
	}

	// DuckDB DELETE chunk layout:
	// - Columns 0 to N-1: filter columns (from WHERE clause)
	// - Column N: rowid (added by BindRowIdColumns at the END of projection)
	// This is the same pattern as UPDATE uses
	idx_t rowid_col_idx = chunk.ColumnCount() - 1;
	DELETE_DEBUG(1, "Execute: rowid at column %llu", (unsigned long long)rowid_col_idx);

	// Extract PK values from the rowid column (last column) using target's pk_info
	auto pk_values = ExtractPKFromRowid(chunk.data[rowid_col_idx], chunk.size(), target_.pk_info);

	// Accumulate into pending batch
	for (auto &pk : pk_values) {
		pending_pk_values_.push_back(std::move(pk));

		// Check if we need to flush the batch
		// In defer_execution_ mode, we accumulate everything and flush in Finalize
		if (!defer_execution_ && pending_pk_values_.size() >= effective_batch_size_) {
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
	DELETE_DEBUG(1, "Finalize: starting, finalized=%d, pending=%llu, defer_execution=%d", finalized_,
				 (unsigned long long)pending_pk_values_.size(), defer_execution_);

	if (finalized_) {
		return MSSQLDMLResult::Success(total_rows_deleted_, batch_count_);
	}

	finalized_ = true;

	// Flush all remaining rows in batches
	// In defer_execution_ mode, we may have accumulated many batches worth of rows
	while (!pending_pk_values_.empty()) {
		DELETE_DEBUG(1, "Finalize: flushing batch, pending=%llu", (unsigned long long)pending_pk_values_.size());
		auto result = FlushBatch();
		if (!result.success) {
			return result;
		}
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
	auto &catalog = Catalog::GetCatalog(context_, target_.catalog_name);
	auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();

	// Acquire connection via ConnectionProvider (handles transaction pinning)
	auto connection = ConnectionProvider::GetConnection(context_, mssql_catalog);
	if (!connection) {
		DELETE_DEBUG(1, "ExecuteBatch: failed to acquire connection");
		return MSSQLDMLResult::Failure("Failed to acquire connection for DELETE execution", 0, batch_count_);
	}

	DELETE_DEBUG(2, "ExecuteBatch: connection acquired");

	idx_t rows_affected = 0;

	try {
		// Get socket for packet-based reading
		auto *socket = connection->GetSocket();
		if (!socket) {
			DELETE_DEBUG(1, "ExecuteBatch: socket is null");
			ConnectionProvider::ReleaseConnection(context_, mssql_catalog, std::move(connection));
			return MSSQLDMLResult::Failure("Connection socket is null", 0, batch_count_);
		}

		// Clear any leftover data before starting
		socket->ClearReceiveBuffer();

		// Send the SQL batch
		DELETE_DEBUG(1, "ExecuteBatch: sending SQL batch...");
		if (!connection->ExecuteBatch(batch.sql)) {
			string error = connection->GetLastError();
			DELETE_DEBUG(1, "ExecuteBatch: ExecuteBatch failed, error=%s", error.c_str());
			ConnectionProvider::ReleaseConnection(context_, mssql_catalog, std::move(connection));
			return MSSQLDMLResult::Failure("DELETE execution failed: " + error, 0, batch_count_);
		}

		DELETE_DEBUG(1, "ExecuteBatch: SQL sent successfully, waiting for response...");

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
				DELETE_DEBUG(1, "ExecuteBatch: TIMEOUT after 30s, packets_received=%d", packet_count);
				connection->SendAttention();
				connection->WaitForAttentionAck(5000);
				ConnectionProvider::ReleaseConnection(context_, mssql_catalog, std::move(connection));
				return MSSQLDMLResult::Failure("DELETE execution timeout", 0, batch_count_);
			}

			auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
			int recv_timeout = static_cast<int>(std::min<long long>(remaining_ms, timeout_ms));

			// Read TDS packet
			tds::TdsPacket packet;
			if (!socket->ReceivePacket(packet, recv_timeout)) {
				string socket_error = socket->GetLastError();
				DELETE_DEBUG(1, "ExecuteBatch: ReceivePacket FAILED, error='%s'", socket_error.c_str());
				ConnectionProvider::ReleaseConnection(context_, mssql_catalog, std::move(connection));
				return MSSQLDMLResult::Failure("Failed to receive TDS packet: " + socket_error, 0, batch_count_);
			}

			packet_count++;
			DELETE_DEBUG(2, "ExecuteBatch: packet %d received, size=%zu, eom=%d", packet_count,
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
				DELETE_DEBUG(2, "ExecuteBatch: parsed token type=%d", (int)token);
				switch (token) {
				case tds::ParsedTokenType::Done: {
					const tds::DoneToken &done_token = parser.GetDone();
					DELETE_DEBUG(1, "ExecuteBatch: DONE token - status=0x%04x, row_count=%llu, has_row_count=%d",
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
					DELETE_DEBUG(1, "ExecuteBatch: ERROR token - number=%u, message='%s'", error_number,
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
				DELETE_DEBUG(1, "ExecuteBatch: EOM without DONE final, marking done");
				done = true;
				connection->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
			}
		}

		DELETE_DEBUG(1, "ExecuteBatch: response parsed, rows_affected=%llu, error='%s'",
					 (unsigned long long)rows_affected, error_message.c_str());

		// Check for errors
		if (!error_message.empty()) {
			ConnectionProvider::ReleaseConnection(context_, mssql_catalog, std::move(connection));
			return MSSQLDMLResult::Failure("DELETE failed: " + error_message, 0, batch_count_);
		}

		ConnectionProvider::ReleaseConnection(context_, mssql_catalog, std::move(connection));
		total_rows_deleted_ += rows_affected;
		return MSSQLDMLResult::Success(rows_affected, batch_count_);

	} catch (const std::exception &e) {
		ConnectionProvider::ReleaseConnection(context_, mssql_catalog, std::move(connection));
		return MSSQLDMLResult::Failure(string("DELETE execution failed: ") + e.what(), 0, batch_count_);
	}
}

}  // namespace duckdb

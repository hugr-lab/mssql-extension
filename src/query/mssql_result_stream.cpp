#include "query/mssql_result_stream.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include "connection/mssql_pool_manager.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "tds/encoding/type_converter.hpp"
#include "tds/tds_packet.hpp"
#include "tds/tds_socket.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
// Set MSSQL_DEBUG=1 to enable, MSSQL_DEBUG=2 for verbose row-level logging
static int GetDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define MSSQL_DEBUG_LOG(level, fmt, ...)                               \
	do {                                                               \
		if (GetDebugLevel() >= level) {                                \
			fprintf(stderr, "[MSSQL DEBUG] " fmt "\n", ##__VA_ARGS__); \
		}                                                              \
	} while (0)

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLResultStream Implementation
//===----------------------------------------------------------------------===//

MSSQLResultStream::MSSQLResultStream(std::shared_ptr<tds::TdsConnection> connection, const string &sql,
									 const string &context_name)
	: connection_(std::move(connection)),
	  context_name_(context_name),
	  sql_(sql),
	  state_(MSSQLResultStreamState::Initializing),
	  is_cancelled_(false),
	  rows_read_(0) {}

MSSQLResultStream::~MSSQLResultStream() {
	// If we're still in streaming state, try to cancel
	if (state_ == MSSQLResultStreamState::Streaming) {
		Cancel();
	}

	// Return connection to the pool
	if (connection_) {
		auto conn_state = connection_->GetState();
		if (conn_state != tds::ConnectionState::Idle && conn_state != tds::ConnectionState::Disconnected) {
			// Connection is in unexpected state - close it to prevent pool corruption
			connection_->Close();
		}

		// Release connection back to pool
		auto *pool = MssqlPoolManager::Instance().GetPool(context_name_);
		if (pool) {
			pool->Release(std::move(connection_));
		}
	}
}

bool MSSQLResultStream::Initialize() {
	if (state_ != MSSQLResultStreamState::Initializing) {
		throw InvalidInputException("MSSQLResultStream already initialized");
	}

	// Send the SQL batch
	if (!connection_->ExecuteBatch(sql_)) {
		throw IOException("Failed to execute SQL batch: " + connection_->GetLastError());
	}

	// Read and parse until we get COLMETADATA
	while (state_ == MSSQLResultStreamState::Initializing) {
		if (!ReadMoreData(read_timeout_ms_)) {
			throw IOException("Connection closed while waiting for COLMETADATA");
		}

		tds::ParsedTokenType token;
		while ((token = parser_.TryParseNext()) != tds::ParsedTokenType::NeedMoreData) {
			switch (token) {
			case tds::ParsedTokenType::ColMetadata: {
				// Got column metadata - transition to streaming
				const auto &parsed_columns = parser_.GetColumnMetadata();
				column_metadata_.clear();
				column_metadata_.reserve(parsed_columns.size());
				for (const auto &col : parsed_columns) {
					column_metadata_.push_back(col);
				}
				column_names_.clear();
				column_types_.clear();

				for (const auto &col : column_metadata_) {
					column_names_.push_back(col.name);
					column_types_.push_back(tds::encoding::TypeConverter::GetDuckDBType(col));
				}

				row_reader_ = make_uniq<tds::RowReader>(parsed_columns);
				state_ = MSSQLResultStreamState::Streaming;
				return true;
			}

			case tds::ParsedTokenType::Error: {
				auto error = parser_.GetError();
				errors_.push_back(error);
				// Fatal errors (severity >= 20) throw immediately
				if (error.IsFatal()) {
					state_ = MSSQLResultStreamState::Error;
					throw IOException("SQL Server fatal error [%d, severity %d]: %s", error.number, error.severity,
									  error.message);
				}
				break;
			}

			case tds::ParsedTokenType::Info:
				info_messages_.push_back(parser_.GetInfo());
				break;

			case tds::ParsedTokenType::Done:
				// Unexpected DONE before COLMETADATA - might be empty result or error
				if (!errors_.empty()) {
					state_ = MSSQLResultStreamState::Error;
					auto &err = errors_[0];
					throw InvalidInputException("SQL Server error [%d, severity %d]: %s", err.number, err.severity,
												err.message);
				}
				// Empty result set (e.g., UPDATE statement)
				state_ = MSSQLResultStreamState::Complete;
				return true;

			case tds::ParsedTokenType::None:
				if (parser_.GetState() == tds::ParserState::Error) {
					throw IOException("TDS parse error: " + parser_.GetParseError());
				}
				break;

			default:
				// Skip other tokens
				break;
			}
		}
	}

	return state_ == MSSQLResultStreamState::Streaming || state_ == MSSQLResultStreamState::Complete;
}

idx_t MSSQLResultStream::FillChunk(DataChunk &chunk) {
	auto chunk_start = std::chrono::steady_clock::now();

	if (state_ == MSSQLResultStreamState::Complete || state_ == MSSQLResultStreamState::Error) {
		return 0;
	}

	if (state_ != MSSQLResultStreamState::Streaming) {
		throw InvalidInputException("MSSQLResultStream not in streaming state");
	}

	// Check for cancellation
	if (is_cancelled_.load(std::memory_order_acquire)) {
		DrainAfterCancel();
		return 0;
	}

	// Reset chunk for new data - DuckDB already initialized it with correct types
	chunk.Reset();

	const idx_t max_rows = STANDARD_VECTOR_SIZE;  // 2048
	idx_t row_count = 0;
	uint64_t parse_time_us = 0;
	uint64_t read_time_us = 0;
	uint64_t process_time_us = 0;

	while (row_count < max_rows && state_ == MSSQLResultStreamState::Streaming) {
		// Check for cancellation periodically
		if (is_cancelled_.load(std::memory_order_acquire)) {
			break;
		}

		auto parse_start = std::chrono::steady_clock::now();
		tds::ParsedTokenType token = parser_.TryParseNext();
		auto parse_end = std::chrono::steady_clock::now();
		parse_time_us += std::chrono::duration_cast<std::chrono::microseconds>(parse_end - parse_start).count();

		switch (token) {
		case tds::ParsedTokenType::Row: {
			auto process_start = std::chrono::steady_clock::now();
			ProcessRow(chunk, row_count++);
			auto process_end = std::chrono::steady_clock::now();
			process_time_us +=
				std::chrono::duration_cast<std::chrono::microseconds>(process_end - process_start).count();
			rows_read_++;
			break;
		}

		case tds::ParsedTokenType::Done: {
			auto done = parser_.GetDone();
			if (done.HasError()) {
				// Error indicated in DONE token
				state_ = MSSQLResultStreamState::Error;
				if (!errors_.empty()) {
					auto &err = errors_[0];
					throw InvalidInputException("SQL Server error [%d, severity %d]: %s", err.number, err.severity,
												err.message);
				}
				throw InvalidInputException("SQL Server returned error status");
			}
			if (done.IsFinal()) {
				state_ = MSSQLResultStreamState::Complete;
				// Transition connection back to Idle
				connection_->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
			}
			break;
		}

		case tds::ParsedTokenType::Error: {
			auto error = parser_.GetError();
			errors_.push_back(error);
			// Fatal errors (severity >= 20) throw immediately
			if (error.IsFatal()) {
				state_ = MSSQLResultStreamState::Error;
				throw IOException("SQL Server fatal error [%d, severity %d]: %s", error.number, error.severity,
								  error.message);
			}
			break;
		}

		case tds::ParsedTokenType::Info:
			info_messages_.push_back(parser_.GetInfo());
			break;

		case tds::ParsedTokenType::NeedMoreData:
			// Check if parser is in terminal state (Complete or Error)
			if (parser_.GetState() == tds::ParserState::Complete) {
				state_ = MSSQLResultStreamState::Complete;
				connection_->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
				goto exit_loop;	 // Exit the while loop
			}
			if (parser_.GetState() == tds::ParserState::Error) {
				state_ = MSSQLResultStreamState::Error;
				throw IOException("TDS parse error: " + parser_.GetParseError());
			}
			{
				auto read_start = std::chrono::steady_clock::now();
				bool read_ok = ReadMoreData(read_timeout_ms_);
				auto read_end = std::chrono::steady_clock::now();
				read_time_us += std::chrono::duration_cast<std::chrono::microseconds>(read_end - read_start).count();
				if (!read_ok) {
					if (row_count > 0) {
						// Return what we have
						goto exit_loop;
					}
					throw IOException("Connection closed unexpectedly");
				}
			}
			break;

		case tds::ParsedTokenType::None:
			if (parser_.GetState() == tds::ParserState::Error) {
				state_ = MSSQLResultStreamState::Error;
				throw IOException("TDS parse error: " + parser_.GetParseError());
			}
			if (parser_.GetState() == tds::ParserState::Complete) {
				state_ = MSSQLResultStreamState::Complete;
				connection_->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
			}
			break;

		default:
			// Skip other tokens
			break;
		}
	}

exit_loop:
	chunk.SetCardinality(row_count);

	// Log timing summary
	auto chunk_end = std::chrono::steady_clock::now();
	auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(chunk_end - chunk_start).count();
	MSSQL_DEBUG_LOG(1, "FillChunk: %llu rows, total=%ldus, parse=%ldus, read=%ldus, process=%ldus",
					(unsigned long long)row_count, (long)total_us, (long)parse_time_us, (long)read_time_us,
					(long)process_time_us);

	return row_count;
}

void MSSQLResultStream::ProcessRow(DataChunk &chunk, idx_t row_idx) {
	const auto &row = parser_.GetRow();

	// Determine how many columns to fill:
	// - If target_vectors_ is set, use those vectors instead of chunk.data
	// - If columns_to_fill_ was explicitly set (e.g., for COUNT(*)), use that
	// - Otherwise, fill up to chunk's column count (but not more than we have data for)
	idx_t cols_to_fill;
	if (!target_vectors_.empty()) {
		// Use target vectors - columns_to_fill_ should match target_vectors_.size()
		cols_to_fill = std::min(target_vectors_.size(), column_metadata_.size());
	} else if (columns_to_fill_ != static_cast<idx_t>(-1)) {
		// Explicitly set - use this value (may be 0 for COUNT(*))
		cols_to_fill = std::min(columns_to_fill_, static_cast<idx_t>(column_metadata_.size()));
	} else {
		// Default behavior - fill columns that exist in both SQL result and chunk
		cols_to_fill = std::min(static_cast<idx_t>(column_metadata_.size()), chunk.ColumnCount());
	}

	// Debug: log column count info on first row
	if (row_idx == 0) {
		MSSQL_DEBUG_LOG(1,
						"ProcessRow: sql_columns=%zu, chunk_columns=%llu, columns_to_fill_=%llu, cols_to_fill=%llu, "
						"target_vectors=%zu",
						column_metadata_.size(), (unsigned long long)chunk.ColumnCount(),
						(unsigned long long)columns_to_fill_, (unsigned long long)cols_to_fill, target_vectors_.size());
	}

	for (idx_t col_idx = 0; col_idx < cols_to_fill; col_idx++) {
		// Get the target vector: either from target_vectors_ or from chunk.data
		Vector *target_vector;
		if (!target_vectors_.empty()) {
			// Write to target vectors (e.g., STRUCT children)
			target_vector = target_vectors_[col_idx];
		} else if (!output_column_mapping_.empty()) {
			// Map SQL column index to output chunk column index
			target_vector = &chunk.data[output_column_mapping_[col_idx]];
		} else {
			// Default: SQL column i goes to output i
			target_vector = &chunk.data[col_idx];
		}

		tds::encoding::TypeConverter::ConvertValue(row.values[col_idx], row.null_mask[col_idx],
												   column_metadata_[col_idx], *target_vector, row_idx);
	}
}

bool MSSQLResultStream::ReadMoreData(int timeout_ms) {
	// Read TDS packet from connection (packet includes 8-byte header)
	// We use the socket's ReceivePacket method to properly parse the header
	auto *socket = connection_->GetSocket();
	if (!socket) {
		return false;
	}

	tds::TdsPacket packet;
	if (!socket->ReceivePacket(packet, timeout_ms)) {
		return false;
	}

	// Feed only the payload to the parser (not the header)
	const auto &payload = packet.GetPayload();
	if (!payload.empty()) {
		parser_.Feed(payload);
	}

	return true;
}

void MSSQLResultStream::Cancel() {
	if (is_cancelled_.load(std::memory_order_acquire)) {
		MSSQL_DEBUG_LOG(1, "Cancel: already cancelled, skipping");
		return;	 // Already cancelled
	}

	is_cancelled_.store(true, std::memory_order_release);

	// Send ATTENTION signal if we're in streaming state
	if (state_ == MSSQLResultStreamState::Streaming) {
		MSSQL_DEBUG_LOG(1, "Cancel: sending ATTENTION (state=Streaming, rows_read=%llu)",
						(unsigned long long)rows_read_);
		state_ = MSSQLResultStreamState::Draining;

		if (connection_->SendAttention()) {
			MSSQL_DEBUG_LOG(1, "Cancel: ATTENTION sent, starting drain");
			// Wait for attention acknowledgment
			DrainAfterCancel();
		} else {
			MSSQL_DEBUG_LOG(1, "Cancel: SendAttention FAILED");
		}
	} else {
		MSSQL_DEBUG_LOG(1, "Cancel: not in Streaming state (state=%d)", (int)state_);
	}
}

void MSSQLResultStream::DrainAfterCancel() {
	// After sending ATTENTION:
	// 1. Read data from socket as fast as possible (data may already be buffered)
	// 2. Cancel timeout runs in parallel
	// 3. Race: DONE+ATTN arrives first → reuse connection
	//          Timeout fires first → close connection
	//
	// Use very short per-read timeout (10ms) to quickly consume any buffered data
	// without blocking. Overall timeout determines when we give up.
	auto start = std::chrono::steady_clock::now();
	auto timeout = std::chrono::milliseconds(cancel_timeout_ms_);

	// NOTE: Don't reset parser! We need column metadata to skip ROW tokens.
	// SQL Server may have data in TCP buffer that arrives after ATTENTION.
	// Enable skip mode - ROW tokens skipped without parsing values (much faster)
	parser_.SetSkipMode(true);

	int read_count = 0;
	int token_count = 0;

	while (state_ == MSSQLResultStreamState::Draining) {
		auto elapsed = std::chrono::steady_clock::now() - start;
		if (elapsed > timeout) {
			// Overall timeout - close connection, don't try to reuse
			MSSQL_DEBUG_LOG(1, "DrainAfterCancel: TIMEOUT after %ldms (reads=%d, tokens=%d), closing connection",
							(long)std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), read_count,
							token_count);
			connection_->Close();
			state_ = MSSQLResultStreamState::Error;
			return;
		}

		// Try to read with short timeout (poll for data)
		if (!ReadMoreData(cancel_read_timeout_ms_)) {
			// No data available, keep trying until overall timeout
			continue;
		}
		read_count++;

		tds::ParsedTokenType token;
		while ((token = parser_.TryParseNext()) != tds::ParsedTokenType::NeedMoreData) {
			token_count++;

			if (token == tds::ParsedTokenType::Done) {
				auto done = parser_.GetDone();
				MSSQL_DEBUG_LOG(1, "DrainAfterCancel: DONE token - status=0x%04x, IsFinal=%d, IsAttentionAck=%d",
								done.status, done.IsFinal(), done.IsAttentionAck());
				if (done.IsAttentionAck()) {
					// Got ATTENTION acknowledgment - connection is clean
					MSSQL_DEBUG_LOG(1, "DrainAfterCancel: SUCCESS - got ATTN ack in %ldms, connection reusable",
									(long)std::chrono::duration_cast<std::chrono::milliseconds>(
										std::chrono::steady_clock::now() - start)
										.count());
					state_ = MSSQLResultStreamState::Complete;
					connection_->TransitionState(tds::ConnectionState::Cancelling, tds::ConnectionState::Idle);
					return;
				}
				// Got DONE but not ATTN - parser may have set state to Complete
				// Reset to WaitingForToken to keep parsing until ATTN
				parser_.ResetState();
			}
			// Discard other tokens while draining
		}
	}
}

void MSSQLResultStream::SurfaceWarnings(ClientContext &context) {
	// Surface INFO messages as warnings to DuckDB
	// DuckDB doesn't have a built-in warning API, but we can log to the client context
	for (const auto &info : info_messages_) {
		if (!info.message.empty()) {
			// Log INFO messages at info level (severity 1-10 are informational in SQL Server)
			// Use DuckDB's context to add a message
			// For now, we store them for the caller to retrieve
			(void)context;	// Context can be used for logging if needed
		}
	}
	// Note: info_messages_ can be retrieved via GetInfoMessages() for caller inspection
}

}  // namespace duckdb

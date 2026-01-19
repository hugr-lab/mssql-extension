#include "insert/mssql_returning_parser.hpp"
#include "tds/tds_packet.hpp"
#include "tds/encoding/type_converter.hpp"
#include "duckdb/common/exception.hpp"
#include <chrono>

namespace duckdb {

//===----------------------------------------------------------------------===//
// Constructor
//===----------------------------------------------------------------------===//

MSSQLReturningParser::MSSQLReturningParser(const MSSQLInsertTarget &target,
                                           const vector<idx_t> &returning_column_ids)
    : target_(target), returning_column_ids_(returning_column_ids) {

	// Build result types from returning column indices
	for (auto col_idx : returning_column_ids_) {
		if (col_idx < target_.columns.size()) {
			result_types_.push_back(target_.columns[col_idx].duckdb_type);
		}
	}
}

//===----------------------------------------------------------------------===//
// Result Chunk Initialization
//===----------------------------------------------------------------------===//

unique_ptr<DataChunk> MSSQLReturningParser::InitializeResultChunk() {
	auto chunk = make_uniq<DataChunk>();
	chunk->Initialize(Allocator::DefaultAllocator(), result_types_);
	return chunk;
}

//===----------------------------------------------------------------------===//
// Row Processing
//===----------------------------------------------------------------------===//

void MSSQLReturningParser::ProcessRow(const tds::RowData &row,
                                       const std::vector<tds::ColumnMetadata> &columns,
                                       DataChunk &chunk,
                                       idx_t row_idx) {
	// The OUTPUT INSERTED columns should match our returning_column_ids in order
	// Process each column from the TDS response
	for (idx_t i = 0; i < columns.size() && i < result_types_.size(); i++) {
		bool is_null = (i < row.null_mask.size()) ? row.null_mask[i] : false;
		const auto &value = (i < row.values.size()) ? row.values[i] : std::vector<uint8_t>{};

		// Use TypeConverter to convert TDS value to DuckDB format
		tds::encoding::TypeConverter::ConvertValue(value, is_null, columns[i],
		                                           chunk.data[i], row_idx);
	}
}

//===----------------------------------------------------------------------===//
// Main Parse Method (with existing parser)
//===----------------------------------------------------------------------===//

unique_ptr<DataChunk> MSSQLReturningParser::Parse(tds::TdsConnection &connection,
                                                   tds::TokenParser &parser,
                                                   tds::TdsSocket &socket,
                                                   int timeout_ms) {
	// Initialize result chunk
	auto chunk = InitializeResultChunk();
	row_count_ = 0;
	error_message_.clear();
	error_number_ = 0;

	// Calculate deadline
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

	std::vector<tds::ColumnMetadata> columns;
	bool done = false;

	while (!done) {
		// Check timeout
		auto now = std::chrono::steady_clock::now();
		if (now >= deadline) {
			error_message_ = "Timeout waiting for OUTPUT INSERTED results";
			connection.SendAttention();
			connection.WaitForAttentionAck(5000);
			return nullptr;
		}

		auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
		int recv_timeout = static_cast<int>(std::min(remaining_ms, static_cast<long long>(timeout_ms)));

		// Try to parse tokens from existing buffer
		tds::ParsedTokenType token;
		while ((token = parser.TryParseNext()) != tds::ParsedTokenType::NeedMoreData) {
			switch (token) {
			case tds::ParsedTokenType::ColMetadata:
				columns = parser.GetColumnMetadata();
				break;

			case tds::ParsedTokenType::Row: {
				const tds::RowData &row = parser.GetRow();

				// Ensure we have space in the chunk
				if (row_count_ >= STANDARD_VECTOR_SIZE) {
					// Chunk is full - this shouldn't happen for typical INSERT batches
					// but handle gracefully
					chunk->SetCardinality(row_count_);
					return chunk;
				}

				// Process the row
				ProcessRow(row, columns, *chunk, row_count_);
				row_count_++;
				break;
			}

			case tds::ParsedTokenType::Done: {
				const tds::DoneToken &done_token = parser.GetDone();
				if (done_token.IsFinal()) {
					done = true;
					connection.TransitionState(tds::ConnectionState::Executing,
					                           tds::ConnectionState::Idle);
				}
				break;
			}

			case tds::ParsedTokenType::Error: {
				const tds::TdsError &tds_error = parser.GetError();
				error_number_ = tds_error.number;
				error_message_ = tds_error.message;
				// Continue reading to drain response
				break;
			}

			default:
				// Skip other tokens (Info, EnvChange, etc.)
				break;
			}
		}

		// If we need more data and not done, read from socket
		if (!done) {
			tds::TdsPacket packet;
			if (!socket.ReceivePacket(packet, recv_timeout)) {
				error_message_ = "Failed to receive TDS packet: " + socket.GetLastError();
				return nullptr;
			}

			bool is_eom = packet.IsEndOfMessage();
			const auto &payload = packet.GetPayload();
			if (!payload.empty()) {
				parser.Feed(payload);
			}

			// Handle EOM without explicit done
			if (is_eom && parser.TryParseNext() == tds::ParsedTokenType::NeedMoreData) {
				done = true;
				connection.TransitionState(tds::ConnectionState::Executing,
				                           tds::ConnectionState::Idle);
			}
		}
	}

	// Set final cardinality and return
	chunk->SetCardinality(row_count_);

	// Return nullptr if no rows (shouldn't happen for INSERT but be safe)
	if (row_count_ == 0) {
		return nullptr;
	}

	return chunk;
}

//===----------------------------------------------------------------------===//
// Parse from Fresh Connection
//===----------------------------------------------------------------------===//

unique_ptr<DataChunk> MSSQLReturningParser::ParseResponse(tds::TdsConnection &connection,
                                                           int timeout_ms) {
	auto *socket = connection.GetSocket();
	if (!socket) {
		error_message_ = "Connection socket is null";
		return nullptr;
	}

	// Create fresh parser
	tds::TokenParser parser;

	// Parse the response
	return Parse(connection, parser, *socket, timeout_ms);
}

}  // namespace duckdb

#include "query/mssql_simple_query.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "tds/encoding/utf16.hpp"
#include "tds/tds_packet.hpp"
#include "tds/tds_socket.hpp"
#include "tds/tds_token_parser.hpp"
#include "tds/tds_types.hpp"

// Debug logging
static int GetSimpleQueryDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define SIMPLE_QUERY_DEBUG(lvl, fmt, ...)                                     \
	do {                                                                      \
		if (GetSimpleQueryDebugLevel() >= lvl)                                \
			fprintf(stderr, "[MSSQL SIMPLE_QUERY] " fmt "\n", ##__VA_ARGS__); \
	} while (0)

namespace duckdb {

//===----------------------------------------------------------------------===//
// Value Conversion Helper
//===----------------------------------------------------------------------===//

static std::string ConvertValueToString(const std::vector<uint8_t> &value, const tds::ColumnMetadata &column) {
	if (value.empty()) {
		return "";
	}

	uint8_t type_id = column.type_id;

	// Unicode string types (NVARCHAR, NCHAR, NTEXT)
	if (type_id == tds::TDS_TYPE_NVARCHAR || type_id == tds::TDS_TYPE_NCHAR || type_id == tds::TDS_TYPE_NTEXT) {
		return tds::encoding::Utf16LEDecode(value.data(), value.size());
	}

	// ASCII/single-byte string types (VARCHAR, CHAR, TEXT)
	if (type_id == tds::TDS_TYPE_BIGVARCHAR || type_id == tds::TDS_TYPE_BIGCHAR || type_id == tds::TDS_TYPE_TEXT) {
		return std::string(reinterpret_cast<const char *>(value.data()), value.size());
	}

	// Integer types
	if (type_id == tds::TDS_TYPE_INTN || type_id == tds::TDS_TYPE_TINYINT || type_id == tds::TDS_TYPE_SMALLINT ||
		type_id == tds::TDS_TYPE_INT || type_id == tds::TDS_TYPE_BIGINT) {
		if (value.size() == 1) {
			return std::to_string(static_cast<int32_t>(value[0]));
		} else if (value.size() == 2) {
			int16_t v;
			std::memcpy(&v, value.data(), 2);
			return std::to_string(v);
		} else if (value.size() == 4) {
			int32_t v;
			std::memcpy(&v, value.data(), 4);
			return std::to_string(v);
		} else if (value.size() == 8) {
			int64_t v;
			std::memcpy(&v, value.data(), 8);
			return std::to_string(v);
		}
	}

	// BIT type
	if (type_id == tds::TDS_TYPE_BIT || type_id == tds::TDS_TYPE_BITN) {
		return (value.size() > 0 && value[0] != 0) ? "1" : "0";
	}

	// Float types
	if (type_id == tds::TDS_TYPE_FLOATN || type_id == tds::TDS_TYPE_REAL || type_id == tds::TDS_TYPE_FLOAT) {
		if (value.size() == 4) {
			float v;
			std::memcpy(&v, value.data(), 4);
			return std::to_string(v);
		} else if (value.size() == 8) {
			double v;
			std::memcpy(&v, value.data(), 8);
			return std::to_string(v);
		}
	}

	// For other types, return empty (can be extended as needed)
	return "";
}

//===----------------------------------------------------------------------===//
// MSSQLSimpleQuery Implementation
//===----------------------------------------------------------------------===//

SimpleQueryResult MSSQLSimpleQuery::Execute(tds::TdsConnection &connection, const std::string &sql, int timeout_ms) {
	SimpleQueryResult result;

	// Use callback version to collect all rows
	auto collect_result = ExecuteWithCallback(
		connection, sql,
		[&result](const std::vector<std::string> &row) {
			result.rows.push_back(row);
			return true;  // continue
		},
		timeout_ms);

	// Copy error info, column names, and rows_affected
	result.success = collect_result.success;
	result.error_message = collect_result.error_message;
	result.error_number = collect_result.error_number;
	result.column_names = collect_result.column_names;
	result.rows_affected = collect_result.rows_affected;

	return result;
}

std::string MSSQLSimpleQuery::ExecuteScalar(tds::TdsConnection &connection, const std::string &sql, int timeout_ms) {
	SimpleQueryResult result = Execute(connection, sql, timeout_ms);
	if (result.HasError() || result.rows.empty() || result.rows[0].empty()) {
		return "";
	}
	return result.rows[0][0];
}

SimpleQueryResult MSSQLSimpleQuery::ExecuteWithCallback(tds::TdsConnection &connection, const std::string &sql,
														RowCallback callback, int timeout_ms) {
	SimpleQueryResult result;

	SIMPLE_QUERY_DEBUG(2, "ExecuteWithCallback: starting, timeout=%dms", timeout_ms);

	// Check connection state
	if (connection.GetState() != tds::ConnectionState::Idle) {
		result.success = false;
		result.error_message = "Connection is not in idle state";
		return result;
	}

	// Get socket for packet-based reading
	auto *socket = connection.GetSocket();
	if (!socket) {
		result.success = false;
		result.error_message = "Connection socket is null";
		return result;
	}

	// Clear any leftover data in receive buffer before starting new query
	// This ensures we don't try to parse stale data from a previous operation
	socket->ClearReceiveBuffer();

	// Send the SQL batch
	if (!connection.ExecuteBatch(sql)) {
		result.success = false;
		result.error_message = "Failed to send SQL batch: " + connection.GetLastError();
		return result;
	}

	// Create parser for reading response
	tds::TokenParser parser;
	std::vector<tds::ColumnMetadata> columns;

	// Calculate deadline
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

	// Read and parse response using proper TDS packet framing
	bool done = false;
	while (!done) {
		// Check timeout
		auto now = std::chrono::steady_clock::now();
		if (now >= deadline) {
			result.success = false;
			result.error_message = "Query timeout";
			connection.SendAttention();
			connection.WaitForAttentionAck(5000);
			// State is reset by WaitForAttentionAck on success, or we should mark it disconnected
			if (connection.GetState() == tds::ConnectionState::Executing) {
				connection.TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Disconnected);
			}
			return result;
		}

		// Calculate remaining time
		auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
		int recv_timeout = static_cast<int>(std::min<long long>(remaining_ms, timeout_ms));

		// Read TDS packet (properly framed with 8-byte header)
		tds::TdsPacket packet;
		if (!socket->ReceivePacket(packet, recv_timeout)) {
			result.success = false;
			result.error_message = "Failed to receive TDS packet: " + socket->GetLastError();
			// Mark connection as disconnected since we can't trust it after a receive error
			connection.TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Disconnected);
			return result;
		}

		bool is_eom = packet.IsEndOfMessage();

		// Feed packet payload to parser (without header)
		const auto &payload = packet.GetPayload();
		if (!payload.empty()) {
			parser.Feed(payload);
		}

		// Parse tokens
		tds::ParsedTokenType token;
		while ((token = parser.TryParseNext()) != tds::ParsedTokenType::NeedMoreData) {
			switch (token) {
			case tds::ParsedTokenType::ColMetadata:
				// Store column metadata
				columns = parser.GetColumnMetadata();
				result.column_names.clear();
				for (const auto &col : columns) {
					result.column_names.push_back(col.name);
				}
				break;

			case tds::ParsedTokenType::Row: {
				// Get row data from parser
				const tds::RowData &row = parser.GetRow();

				// Convert values to strings
				std::vector<std::string> row_values;
				row_values.reserve(row.values.size());

				for (size_t i = 0; i < row.values.size(); i++) {
					if (i < row.null_mask.size() && row.null_mask[i]) {
						// NULL value
						row_values.push_back("");
					} else if (i < columns.size()) {
						// Convert to string based on column type
						row_values.push_back(ConvertValueToString(row.values[i], columns[i]));
					} else {
						row_values.push_back("");
					}
				}

				// Call callback
				if (!callback(row_values)) {
					// Callback requested stop - cancel query
					connection.SendAttention();
					connection.WaitForAttentionAck(5000);
					// Ensure connection is in proper state
					if (connection.GetState() == tds::ConnectionState::Executing) {
						connection.TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Disconnected);
					}
					return result;
				}
				break;
			}

			case tds::ParsedTokenType::Done: {
				const tds::DoneToken &done_token = parser.GetDone();
				// Capture rows_affected from DONE token (for DML operations)
				if (done_token.HasRowCount()) {
					result.rows_affected = static_cast<int64_t>(done_token.row_count);
					SIMPLE_QUERY_DEBUG(2, "ExecuteWithCallback: DONE token rows_affected=%lld",
									   (long long)result.rows_affected);
				}
				if (done_token.IsFinal()) {
					done = true;
					// Transition connection back to Idle
					connection.TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
				}
				break;
			}

			case tds::ParsedTokenType::Error: {
				const tds::TdsError &error = parser.GetError();
				result.success = false;
				result.error_number = error.number;
				result.error_message = error.message;
				// Continue reading to drain the response
				break;
			}

			case tds::ParsedTokenType::Info:
				// Ignore informational messages
				break;

			default:
				// Skip other tokens (EnvChange, etc.)
				break;
			}
		}

		// If EOM was set and we're not done, there's no more data coming
		// This handles the case where the parser needs more data but EOM indicates complete response
		if (is_eom && !done) {
			// Force done - the server has sent all data it's going to send
			done = true;
			connection.TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
		}
	}

	return result;
}

}  // namespace duckdb

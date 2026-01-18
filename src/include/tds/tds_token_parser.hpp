#pragma once

#include "tds_types.hpp"
#include "tds_column_metadata.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace duckdb {
namespace tds {

//===----------------------------------------------------------------------===//
// TdsError - Error information from ERROR token
//===----------------------------------------------------------------------===//

struct TdsError {
	uint32_t number;          // SQL Server error number
	uint8_t state;            // Error state
	uint8_t severity;         // Error severity (0-25)
	std::string message;      // Error message text
	std::string server_name;  // Server name
	std::string proc_name;    // Procedure name (if applicable)
	uint32_t line_number;     // Line number in batch

	// Severity helpers
	bool IsFatal() const { return severity >= 20; }
	bool IsUserError() const { return severity >= 11 && severity <= 16; }
	bool IsInfo() const { return severity <= 10; }
};

//===----------------------------------------------------------------------===//
// TdsInfo - Informational message from INFO token
//===----------------------------------------------------------------------===//

struct TdsInfo {
	uint32_t number;          // Message number
	uint8_t state;            // Message state
	uint8_t severity;         // Message class
	std::string message;      // Message text
	std::string server_name;  // Server name
	std::string proc_name;    // Procedure name (if applicable)
	uint32_t line_number;     // Line number in batch
};

//===----------------------------------------------------------------------===//
// DoneToken - Information from DONE/DONEPROC/DONEINPROC tokens
//===----------------------------------------------------------------------===//

struct DoneToken {
	uint16_t status;          // Status flags
	uint16_t cur_cmd;         // Current command
	uint64_t row_count;       // Row count (if DONE_COUNT set)

	bool IsFinal() const { return (status & static_cast<uint16_t>(DoneStatus::DONE_MORE)) == 0; }
	bool HasError() const { return (status & static_cast<uint16_t>(DoneStatus::DONE_ERROR)) != 0; }
	bool HasRowCount() const { return (status & static_cast<uint16_t>(DoneStatus::DONE_COUNT)) != 0; }
	bool IsAttentionAck() const { return (status & static_cast<uint16_t>(DoneStatus::DONE_ATTN)) != 0; }
};

//===----------------------------------------------------------------------===//
// RowData - Raw row values from ROW token
//===----------------------------------------------------------------------===//

struct RowData {
	std::vector<std::vector<uint8_t>> values;  // Raw value data per column
	std::vector<bool> null_mask;               // NULL indicators

	// Clear values but preserve allocated capacity
	void Clear() {
		for (auto& v : values) {
			v.clear();  // Clears content but preserves capacity
		}
		// Just reset null_mask values, don't deallocate
		std::fill(null_mask.begin(), null_mask.end(), false);
	}

	// Prepare for a specific number of columns (pre-allocate if needed)
	void Prepare(size_t num_columns) {
		if (values.size() != num_columns) {
			values.resize(num_columns);
			null_mask.resize(num_columns, false);
			// Pre-reserve capacity for common value sizes
			for (auto& v : values) {
				v.reserve(32);  // Most values are < 32 bytes
			}
		} else {
			Clear();
		}
	}
};

//===----------------------------------------------------------------------===//
// TokenParser - Incremental parser for TDS token stream
//===----------------------------------------------------------------------===//

enum class ParserState : uint8_t {
	WaitingForToken,     // Expecting token type byte
	ParsingColMetadata,  // Reading COLMETADATA
	ParsingRow,          // Reading ROW data
	ParsingDone,         // Reading DONE token
	ParsingError,        // Reading ERROR token
	ParsingInfo,         // Reading INFO token
	Complete,            // Final DONE received
	Error                // Parse error occurred
};

enum class ParsedTokenType : uint8_t {
	None,           // No token ready
	ColMetadata,    // COLMETADATA parsed (columns available)
	Row,            // ROW parsed (row data available)
	Done,           // DONE/DONEPROC/DONEINPROC parsed
	Error,          // ERROR token parsed
	Info,           // INFO token parsed
	EnvChange,      // ENVCHANGE consumed (no data exposed)
	NeedMoreData    // Incomplete token, need more data
};

class TokenParser {
public:
	TokenParser();
	~TokenParser() = default;

	// Feed data into the parser buffer
	void Feed(const uint8_t* data, size_t length);
	void Feed(const std::vector<uint8_t>& data);

	// Try to parse the next token from buffer
	// Returns the type of token parsed (or NeedMoreData if incomplete)
	ParsedTokenType TryParseNext();

	// Get parsed data (valid after TryParseNext returns appropriate type)
	const std::vector<ColumnMetadata>& GetColumnMetadata() const { return columns_; }
	const RowData& GetRow() const { return current_row_; }
	const TdsError& GetError() const { return current_error_; }
	const TdsInfo& GetInfo() const { return current_info_; }
	const DoneToken& GetDone() const { return current_done_; }

	// State queries
	ParserState GetState() const { return state_; }
	bool IsComplete() const { return state_ == ParserState::Complete; }
	bool HasError() const { return state_ == ParserState::Error; }
	const std::string& GetParseError() const { return parse_error_; }

	// Reset parser state (clears everything)
	void Reset();

	// Reset just the parsing state (keeps buffer and column metadata)
	void ResetState() { state_ = ParserState::WaitingForToken; }

	// Enable skip mode - ROW tokens are skipped without parsing values
	// Use this during drain to avoid wasting time parsing data we don't need
	void SetSkipMode(bool skip) { skip_rows_ = skip; }

	// Check if we have column metadata
	bool HasColumnMetadata() const { return !columns_.empty(); }

private:
	// Parse specific token types
	bool ParseColMetadata();
	bool ParseRow();
	bool ParseNBCRow();  // Null Bitmap Compressed Row
	bool ParseDone();
	bool ParseError();
	bool ParseInfo();
	bool ParseEnvChange();

	// Buffer management
	void ConsumeBytes(size_t count);
	size_t Available() const { return buffer_.size() - buffer_pos_; }
	const uint8_t* Current() const { return buffer_.data() + buffer_pos_; }

	// Compact buffer if needed
	void CompactBuffer();

	// State
	ParserState state_;
	std::string parse_error_;
	bool skip_rows_ = false;  // Skip ROW content during drain

	// Buffer
	std::vector<uint8_t> buffer_;
	size_t buffer_pos_;

	// Parsed data
	std::vector<ColumnMetadata> columns_;
	RowData current_row_;
	TdsError current_error_;
	TdsInfo current_info_;
	DoneToken current_done_;
};

}  // namespace tds
}  // namespace duckdb

#include "tds/tds_token_parser.hpp"
#include "tds/tds_row_reader.hpp"
#include "tds/encoding/utf16.hpp"
#include <cstring>
#include <stdexcept>
#include <cstdlib>
#include <cstdio>
#include <sstream>
#include <iomanip>

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetParserDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char* env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define TDS_PARSER_DEBUG(level, fmt, ...) \
	do { if (GetParserDebugLevel() >= level) { \
		fprintf(stderr, "[TDS PARSER] " fmt "\n", ##__VA_ARGS__); \
	} } while(0)

namespace duckdb {
namespace tds {

//===----------------------------------------------------------------------===//
// TokenParser Implementation
//===----------------------------------------------------------------------===//

TokenParser::TokenParser()
    : state_(ParserState::WaitingForToken),
      buffer_pos_(0) {
}

void TokenParser::Reset() {
	state_ = ParserState::WaitingForToken;
	parse_error_.clear();
	buffer_.clear();
	buffer_pos_ = 0;
	columns_.clear();
	current_row_.Clear();
	current_error_ = TdsError();
	current_info_ = TdsInfo();
	current_done_ = DoneToken();
}

void TokenParser::Feed(const uint8_t* data, size_t length) {
	buffer_.insert(buffer_.end(), data, data + length);
}

void TokenParser::Feed(const std::vector<uint8_t>& data) {
	buffer_.insert(buffer_.end(), data.begin(), data.end());
}

void TokenParser::ConsumeBytes(size_t count) {
	buffer_pos_ += count;
	CompactBuffer();
}

void TokenParser::CompactBuffer() {
	// Compact buffer if we've consumed more than half
	if (buffer_pos_ > buffer_.size() / 2 && buffer_pos_ > 4096) {
		buffer_.erase(buffer_.begin(), buffer_.begin() + buffer_pos_);
		buffer_pos_ = 0;
	}
}

ParsedTokenType TokenParser::TryParseNext() {
	if (state_ == ParserState::Complete || state_ == ParserState::Error) {
		// Return NeedMoreData to break out of all parsing loops
		return ParsedTokenType::NeedMoreData;
	}

	// Need at least 1 byte for token type
	if (Available() < 1) {
		return ParsedTokenType::NeedMoreData;
	}

	uint8_t token_type = Current()[0];

	switch (static_cast<TokenType>(token_type)) {
	case TokenType::COLMETADATA:
		if (ParseColMetadata()) {
			TDS_PARSER_DEBUG(1, "TryParseNext: returning ColMetadata (1)");
			return ParsedTokenType::ColMetadata;
		}
		return ParsedTokenType::NeedMoreData;

	case TokenType::ROW:
		if (ParseRow()) {
			return ParsedTokenType::Row;
		}
		return ParsedTokenType::NeedMoreData;

	case TokenType::NBCROW:
		if (ParseNBCRow()) {
			return ParsedTokenType::Row;
		}
		return ParsedTokenType::NeedMoreData;

	case TokenType::DONE:
	case TokenType::DONEPROC:
	case TokenType::DONEINPROC:
		if (ParseDone()) {
			return ParsedTokenType::Done;
		}
		return ParsedTokenType::NeedMoreData;

	case TokenType::ERROR_TOKEN:
		if (ParseError()) {
			return ParsedTokenType::Error;
		}
		return ParsedTokenType::NeedMoreData;

	case TokenType::INFO:
		if (ParseInfo()) {
			return ParsedTokenType::Info;
		}
		return ParsedTokenType::NeedMoreData;

	case TokenType::ENVCHANGE:
		if (ParseEnvChange()) {
			return ParsedTokenType::EnvChange;
		}
		return ParsedTokenType::NeedMoreData;

	case TokenType::ORDER:
	case TokenType::RETURNSTATUS:
	case TokenType::RETURNVALUE:
	case TokenType::LOGINACK:
	case TokenType::TABNAME:
	case TokenType::COLINFO:
		// Skip these tokens - they have a 2-byte length
		if (Available() < 3) {
			return ParsedTokenType::NeedMoreData;
		}
		{
			uint16_t token_length = static_cast<uint16_t>(Current()[1]) |
			                        (static_cast<uint16_t>(Current()[2]) << 8);
			if (Available() < 3 + token_length) {
				return ParsedTokenType::NeedMoreData;
			}
			ConsumeBytes(3 + token_length);
		}
		return TryParseNext();  // Try next token

	default:
		// Unknown token - dump buffer for debugging
		{
			std::ostringstream hex_dump;
			size_t dump_len = std::min(Available(), static_cast<size_t>(32));
			for (size_t i = 0; i < dump_len; i++) {
				hex_dump << std::hex << std::setw(2) << std::setfill('0')
				         << static_cast<int>(Current()[i]) << " ";
			}
			TDS_PARSER_DEBUG(1, "Unknown token 0x%02x at pos=%zu, buffer_size=%zu, available=%zu, hex: %s",
			                 token_type, buffer_pos_, buffer_.size(), Available(), hex_dump.str().c_str());
		}
		parse_error_ = "Unknown token type: 0x" + std::to_string(token_type);
		state_ = ParserState::Error;
		return ParsedTokenType::None;
	}
}

bool TokenParser::ParseColMetadata() {
	TDS_PARSER_DEBUG(2, "ParseColMetadata: entry, available=%zu, existing_columns=%zu", Available(), columns_.size());

	// Token type + at least 2 bytes for count
	if (Available() < 3) {
		TDS_PARSER_DEBUG(2, "ParseColMetadata: need more data (have %zu, need 3)", Available());
		return false;
	}

	const uint8_t* data = Current() + 1;  // Skip token type
	size_t length = Available() - 1;
	size_t bytes_consumed = 0;

	try {
		if (!ColumnMetadataParser::Parse(data, length, bytes_consumed, columns_)) {
			TDS_PARSER_DEBUG(2, "ParseColMetadata: ColumnMetadataParser needs more data");
			return false;  // Need more data
		}
	} catch (const std::exception& e) {
		parse_error_ = std::string("COLMETADATA parse error: ") + e.what();
		state_ = ParserState::Error;
		TDS_PARSER_DEBUG(1, "ParseColMetadata: exception: %s", e.what());
		return false;
	}

	TDS_PARSER_DEBUG(1, "ParseColMetadata: parsed %zu columns, consumed %zu bytes", columns_.size(), bytes_consumed);
	ConsumeBytes(1 + bytes_consumed);  // token type + parsed data
	state_ = ParserState::ParsingRow;
	return true;
}

bool TokenParser::ParseRow() {
	if (columns_.empty()) {
		parse_error_ = "ROW token before COLMETADATA";
		state_ = ParserState::Error;
		return false;
	}

	// Token type + variable row data
	if (Available() < 1) {
		return false;
	}

	RowReader reader(columns_);
	const uint8_t* data = Current() + 1;  // Skip token type
	size_t length = Available() - 1;
	size_t bytes_consumed = 0;

	// Fast path: skip mode (for drain) - just count bytes, don't parse values
	if (skip_rows_) {
		if (!reader.SkipRow(data, length, bytes_consumed)) {
			return false;  // Need more data
		}
		ConsumeBytes(1 + bytes_consumed);
		return true;
	}

	// Normal path: full parsing
	current_row_.Clear();
	try {
		if (!reader.ReadRow(data, length, bytes_consumed, current_row_)) {
			return false;  // Need more data
		}
	} catch (const std::exception& e) {
		parse_error_ = std::string("ROW parse error: ") + e.what();
		state_ = ParserState::Error;
		return false;
	}

	ConsumeBytes(1 + bytes_consumed);
	return true;
}

bool TokenParser::ParseNBCRow() {
	if (columns_.empty()) {
		parse_error_ = "NBCROW token before COLMETADATA";
		state_ = ParserState::Error;
		return false;
	}

	// Token type + null bitmap + row data for non-NULL columns
	if (Available() < 1) {
		return false;
	}

	RowReader reader(columns_);
	const uint8_t* data = Current() + 1;  // Skip token type
	size_t length = Available() - 1;
	size_t bytes_consumed = 0;

	// Fast path: skip mode (for drain)
	if (skip_rows_) {
		if (!reader.SkipNBCRow(data, length, bytes_consumed)) {
			return false;  // Need more data
		}
		ConsumeBytes(1 + bytes_consumed);
		return true;
	}

	// Normal path: full parsing
	current_row_.Clear();
	try {
		if (!reader.ReadNBCRow(data, length, bytes_consumed, current_row_)) {
			return false;  // Need more data
		}
	} catch (const std::exception& e) {
		parse_error_ = std::string("NBCROW parse error: ") + e.what();
		state_ = ParserState::Error;
		return false;
	}

	ConsumeBytes(1 + bytes_consumed);
	return true;
}

bool TokenParser::ParseDone() {
	// DONE token: 1 type + 2 status + 2 curCmd + 8 rowCount = 13 bytes
	if (Available() < 13) {
		return false;
	}

	const uint8_t* data = Current();

	current_done_.status = static_cast<uint16_t>(data[1]) |
	                       (static_cast<uint16_t>(data[2]) << 8);
	current_done_.cur_cmd = static_cast<uint16_t>(data[3]) |
	                        (static_cast<uint16_t>(data[4]) << 8);
	current_done_.row_count = static_cast<uint64_t>(data[5]) |
	                          (static_cast<uint64_t>(data[6]) << 8) |
	                          (static_cast<uint64_t>(data[7]) << 16) |
	                          (static_cast<uint64_t>(data[8]) << 24) |
	                          (static_cast<uint64_t>(data[9]) << 32) |
	                          (static_cast<uint64_t>(data[10]) << 40) |
	                          (static_cast<uint64_t>(data[11]) << 48) |
	                          (static_cast<uint64_t>(data[12]) << 56);

	ConsumeBytes(13);

	// Check if this is the final DONE
	if (current_done_.IsFinal()) {
		state_ = ParserState::Complete;
	}

	return true;
}

// Helper to parse US_VARCHAR (2-byte length + UTF-16LE)
static bool ParseUSVarchar(const uint8_t* data, size_t length, size_t& offset, std::string& result) {
	if (offset + 2 > length) return false;
	uint16_t char_count = static_cast<uint16_t>(data[offset]) |
	                      (static_cast<uint16_t>(data[offset + 1]) << 8);
	offset += 2;
	size_t byte_length = char_count * 2;
	if (offset + byte_length > length) return false;
	result = encoding::Utf16LEDecode(data + offset, byte_length);
	offset += byte_length;
	return true;
}

// Helper to parse B_VARCHAR (1-byte length + UTF-16LE)
static bool ParseBVarchar(const uint8_t* data, size_t length, size_t& offset, std::string& result) {
	if (offset >= length) return false;
	uint8_t char_count = data[offset++];
	size_t byte_length = char_count * 2;
	if (offset + byte_length > length) return false;
	result = encoding::Utf16LEDecode(data + offset, byte_length);
	offset += byte_length;
	return true;
}

bool TokenParser::ParseError() {
	// ERROR token: 1 type + 2 length + error data
	if (Available() < 3) {
		return false;
	}

	const uint8_t* data = Current();
	uint16_t token_length = static_cast<uint16_t>(data[1]) |
	                        (static_cast<uint16_t>(data[2]) << 8);

	if (Available() < 3 + token_length) {
		return false;
	}

	size_t offset = 3;  // Skip type and length

	// Error number (4 bytes)
	current_error_.number = static_cast<uint32_t>(data[offset]) |
	                        (static_cast<uint32_t>(data[offset + 1]) << 8) |
	                        (static_cast<uint32_t>(data[offset + 2]) << 16) |
	                        (static_cast<uint32_t>(data[offset + 3]) << 24);
	offset += 4;

	// State (1 byte)
	current_error_.state = data[offset++];

	// Severity (1 byte)
	current_error_.severity = data[offset++];

	// Message text (US_VARCHAR)
	if (!ParseUSVarchar(data, 3 + token_length, offset, current_error_.message)) {
		return false;
	}

	// Server name (B_VARCHAR)
	if (!ParseBVarchar(data, 3 + token_length, offset, current_error_.server_name)) {
		return false;
	}

	// Proc name (B_VARCHAR)
	if (!ParseBVarchar(data, 3 + token_length, offset, current_error_.proc_name)) {
		return false;
	}

	// Line number (4 bytes)
	if (offset + 4 > 3 + token_length) {
		return false;
	}
	current_error_.line_number = static_cast<uint32_t>(data[offset]) |
	                             (static_cast<uint32_t>(data[offset + 1]) << 8) |
	                             (static_cast<uint32_t>(data[offset + 2]) << 16) |
	                             (static_cast<uint32_t>(data[offset + 3]) << 24);

	ConsumeBytes(3 + token_length);
	return true;
}

bool TokenParser::ParseInfo() {
	// INFO token has same structure as ERROR
	if (Available() < 3) {
		return false;
	}

	const uint8_t* data = Current();
	uint16_t token_length = static_cast<uint16_t>(data[1]) |
	                        (static_cast<uint16_t>(data[2]) << 8);

	if (Available() < 3 + token_length) {
		return false;
	}

	size_t offset = 3;

	// Info number (4 bytes)
	current_info_.number = static_cast<uint32_t>(data[offset]) |
	                       (static_cast<uint32_t>(data[offset + 1]) << 8) |
	                       (static_cast<uint32_t>(data[offset + 2]) << 16) |
	                       (static_cast<uint32_t>(data[offset + 3]) << 24);
	offset += 4;

	// State (1 byte)
	current_info_.state = data[offset++];

	// Severity (1 byte)
	current_info_.severity = data[offset++];

	// Message text (US_VARCHAR)
	if (!ParseUSVarchar(data, 3 + token_length, offset, current_info_.message)) {
		return false;
	}

	// Server name (B_VARCHAR)
	if (!ParseBVarchar(data, 3 + token_length, offset, current_info_.server_name)) {
		return false;
	}

	// Proc name (B_VARCHAR)
	if (!ParseBVarchar(data, 3 + token_length, offset, current_info_.proc_name)) {
		return false;
	}

	// Line number (4 bytes)
	if (offset + 4 > 3 + token_length) {
		return false;
	}
	current_info_.line_number = static_cast<uint32_t>(data[offset]) |
	                            (static_cast<uint32_t>(data[offset + 1]) << 8) |
	                            (static_cast<uint32_t>(data[offset + 2]) << 16) |
	                            (static_cast<uint32_t>(data[offset + 3]) << 24);

	ConsumeBytes(3 + token_length);
	return true;
}

bool TokenParser::ParseEnvChange() {
	// ENVCHANGE: 1 type + 2 length + data
	if (Available() < 3) {
		return false;
	}

	const uint8_t* data = Current();
	uint16_t token_length = static_cast<uint16_t>(data[1]) |
	                        (static_cast<uint16_t>(data[2]) << 8);

	if (Available() < 3 + token_length) {
		return false;
	}

	// Just consume the token - we don't need to process environment changes
	// for query execution (database context is already set)
	ConsumeBytes(3 + token_length);
	return true;
}

}  // namespace tds
}  // namespace duckdb

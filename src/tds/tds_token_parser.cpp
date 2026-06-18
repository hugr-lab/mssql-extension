#include "tds/tds_token_parser.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include "tds/encoding/utf16.hpp"
#include "tds/tds_row_reader.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetParserDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define TDS_PARSER_DEBUG(level, fmt, ...)                             \
	do {                                                              \
		if (GetParserDebugLevel() >= level) {                         \
			fprintf(stderr, "[TDS PARSER] " fmt "\n", ##__VA_ARGS__); \
		}                                                             \
	} while (0)

namespace duckdb {
namespace tds {

//===----------------------------------------------------------------------===//
// TokenParser Implementation
//===----------------------------------------------------------------------===//

TokenParser::TokenParser() : state_(ParserState::WaitingForToken), buffer_pos_(0) {}

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

void TokenParser::Feed(const uint8_t *data, size_t length) {
	buffer_.insert(buffer_.end(), data, data + length);
}

void TokenParser::Feed(const std::vector<uint8_t> &data) {
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
	TDS_PARSER_DEBUG(2, "TryParseNext: token_type=0x%02X, available=%zu", token_type, Available());

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
			uint16_t token_length = static_cast<uint16_t>(Current()[1]) | (static_cast<uint16_t>(Current()[2]) << 8);
			if (Available() < 3 + token_length) {
				return ParsedTokenType::NeedMoreData;
			}
			ConsumeBytes(3 + token_length);
		}
		return TryParseNext();	// Try next token

	case TokenType::SSPI:
		// SSPI (0xED) - Integrated Auth continuation token (Spec 042; [MS-TDS] 2.2.7.21)
		// Standard 1+2+data layout: TokenType(1) + USHORT length(2) + Data(variable)
		// In normal query streams this should never appear; only during LOGIN it does,
		// and the LOGIN response parser handles it explicitly. Skip here defensively.
		if (Available() < 3) {
			return ParsedTokenType::NeedMoreData;
		}
		{
			uint16_t token_length = static_cast<uint16_t>(Current()[1]) | (static_cast<uint16_t>(Current()[2]) << 8);
			if (Available() < 3 + token_length) {
				return ParsedTokenType::NeedMoreData;
			}
			TDS_PARSER_DEBUG(1, "Skipping SSPI token (unexpected outside LOGIN); length=%u", token_length);
			ConsumeBytes(3 + token_length);
		}
		return TryParseNext();

	case TokenType::FEDAUTHINFO:
		// FEDAUTHINFO (0xEE) - Azure AD authentication info from server (T019)
		// This token has a 4-byte length (DWORD) unlike most other tokens
		// MS-TDS 2.2.7.16: FEDAUTHINFO = TokenType(1) + TokenLength(4) + Data(variable)
		// We just skip this token - the authentication info is not needed after successful login
		TDS_PARSER_DEBUG(1, "Skipping FEDAUTHINFO token");
		if (Available() < 5) {
			return ParsedTokenType::NeedMoreData;
		}
		{
			// FEDAUTHINFO has 4-byte length (little-endian DWORD)
			uint32_t token_length = static_cast<uint32_t>(Current()[1]) | (static_cast<uint32_t>(Current()[2]) << 8) |
									(static_cast<uint32_t>(Current()[3]) << 16) |
									(static_cast<uint32_t>(Current()[4]) << 24);
			if (Available() < 5 + token_length) {
				return ParsedTokenType::NeedMoreData;
			}
			TDS_PARSER_DEBUG(1, "FEDAUTHINFO token: length=%u", token_length);
			ConsumeBytes(5 + token_length);
		}
		return TryParseNext();	// Try next token

	default:
		// Unknown token - dump buffer for debugging
		{
			std::ostringstream hex_dump;
			size_t dump_len = std::min(Available(), static_cast<size_t>(32));
			for (size_t i = 0; i < dump_len; i++) {
				hex_dump << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(Current()[i]) << " ";
			}
			TDS_PARSER_DEBUG(1, "Unknown token 0x%02x at pos=%zu, buffer_size=%zu, available=%zu, hex: %s", token_type,
							 buffer_pos_, buffer_.size(), Available(), hex_dump.str().c_str());
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

	const uint8_t *data = Current() + 1;  // Skip token type
	size_t length = Available() - 1;
	size_t bytes_consumed = 0;

	try {
		if (!ColumnMetadataParser::Parse(data, length, bytes_consumed, columns_)) {
			TDS_PARSER_DEBUG(2, "ParseColMetadata: ColumnMetadataParser needs more data");
			return false;  // Need more data
		}
	} catch (const std::exception &e) {
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
	const uint8_t *data = Current() + 1;  // Skip token type
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
	} catch (const std::exception &e) {
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
	const uint8_t *data = Current() + 1;  // Skip token type
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
	} catch (const std::exception &e) {
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

	const uint8_t *data = Current();

	current_done_.status = static_cast<uint16_t>(data[1]) | (static_cast<uint16_t>(data[2]) << 8);
	current_done_.cur_cmd = static_cast<uint16_t>(data[3]) | (static_cast<uint16_t>(data[4]) << 8);
	current_done_.row_count = static_cast<uint64_t>(data[5]) | (static_cast<uint64_t>(data[6]) << 8) |
							  (static_cast<uint64_t>(data[7]) << 16) | (static_cast<uint64_t>(data[8]) << 24) |
							  (static_cast<uint64_t>(data[9]) << 32) | (static_cast<uint64_t>(data[10]) << 40) |
							  (static_cast<uint64_t>(data[11]) << 48) | (static_cast<uint64_t>(data[12]) << 56);

	ConsumeBytes(13);

	// Check if this is the final DONE
	if (current_done_.IsFinal()) {
		state_ = ParserState::Complete;
	}

	return true;
}

// Helper to parse US_VARCHAR (2-byte length + UTF-16LE)
static bool ParseUSVarchar(const uint8_t *data, size_t length, size_t &offset, std::string &result) {
	if (offset + 2 > length)
		return false;
	uint16_t char_count = static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
	offset += 2;
	size_t byte_length = char_count * 2;
	if (offset + byte_length > length)
		return false;
	result = encoding::Utf16LEDecode(data + offset, byte_length);
	offset += byte_length;
	return true;
}

// Helper to parse B_VARCHAR (1-byte length + UTF-16LE)
static bool ParseBVarchar(const uint8_t *data, size_t length, size_t &offset, std::string &result) {
	if (offset >= length)
		return false;
	uint8_t char_count = data[offset++];
	size_t byte_length = char_count * 2;
	if (offset + byte_length > length)
		return false;
	result = encoding::Utf16LEDecode(data + offset, byte_length);
	offset += byte_length;
	return true;
}

bool TokenParser::ParseError() {
	// ERROR token: 1 type + 2 length + error data
	if (Available() < 3) {
		return false;
	}

	const uint8_t *data = Current();
	uint16_t token_length = static_cast<uint16_t>(data[1]) | (static_cast<uint16_t>(data[2]) << 8);

	if (Available() < 3 + token_length) {
		return false;
	}

	// The fixed prefix (number[4] + state[1] + severity[1]) must fit within the
	// declared token length. The check above only guarantees the buffer holds
	// `3 + token_length` bytes, so a (malicious) server under-declaring
	// token_length < 6 would make the fixed reads below run past the buffer.
	// Found by fuzzing: an ERROR token "AA 00 00" (token_length == 0).
	if (token_length < 6) {
		return false;
	}

	size_t offset = 3;	// Skip type and length

	// Error number (4 bytes)
	current_error_.number = static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
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
	current_error_.line_number = static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
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

	const uint8_t *data = Current();
	uint16_t token_length = static_cast<uint16_t>(data[1]) | (static_cast<uint16_t>(data[2]) << 8);

	if (Available() < 3 + token_length) {
		return false;
	}

	// Same under-declared-length guard as ParseError: the fixed prefix
	// (number[4] + state[1] + severity[1]) must fit within token_length, else
	// the fixed reads below run past the buffer (e.g. INFO token "AB 00 00").
	if (token_length < 6) {
		return false;
	}

	size_t offset = 3;

	// Info number (4 bytes)
	current_info_.number = static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
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
	current_info_.line_number = static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
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

	const uint8_t *data = Current();
	uint16_t token_length = static_cast<uint16_t>(data[1]) | (static_cast<uint16_t>(data[2]) << 8);

	if (Available() < 3 + token_length) {
		return false;
	}

	// Just consume the token - we don't need to process environment changes
	// for query execution (database context is already set)
	ConsumeBytes(3 + token_length);
	return true;
}

bool FindBeginTxnDescriptor(const uint8_t *data, size_t len, uint8_t out_descriptor[8]) {
	// Hardened replacement for the hand-rolled loop that used to live inline in
	// the connection provider (the `offset += token_len - 1` shape). Here `offset`
	// only ever moves forward and every read is bounded by `len`, so a malicious
	// server cannot drive an out-of-bounds read or a non-terminating scan no
	// matter what token lengths it advertises.
	size_t offset = 0;
	while (offset < len) {
		const uint8_t token_type = data[offset++];

		if (token_type == 0xE3) {  // ENVCHANGE
			if (offset + 2 > len) {
				break;
			}
			const uint16_t token_len =
				static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
			offset += 2;
			// End of this token's data, clamped to the buffer — we never trust
			// token_len to keep us inside `data`.
			size_t token_end = offset + token_len;
			if (token_end > len) {
				token_end = len;
			}

			if (offset < len && data[offset] == 0x08) {	 // BEGIN_TRANS env_type
				const size_t new_len_pos = offset + 1;
				if (new_len_pos < len && data[new_len_pos] == 8 && new_len_pos + 1 + 8 <= len) {
					for (int i = 0; i < 8; ++i) {
						out_descriptor[i] = data[new_len_pos + 1 + i];
					}
					return true;
				}
			}
			offset = token_end;	 // always forward, never past `len`
		} else if (token_type == 0xFD || token_type == 0xFE || token_type == 0xFF) {
			// DONE / DONEPROC / DONEINPROC: 12 bytes after the type byte.
			offset += 12;
		} else {
			break;	// unknown token — stop (simplified scan)
		}
	}
	return false;
}

}  // namespace tds
}  // namespace duckdb

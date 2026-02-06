//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// jwt_parser.cpp
//
// JWT parsing for Azure AD access tokens - claim extraction only
// Spec 032: FEDAUTH Token Provider Enhancements
//===----------------------------------------------------------------------===//

#include "azure/jwt_parser.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <sstream>
#include <stdexcept>

namespace duckdb {
namespace mssql {
namespace azure {

//===----------------------------------------------------------------------===//
// Base64URL Decoding
//===----------------------------------------------------------------------===//

// Base64URL alphabet (RFC 4648 Section 5)
// Standard base64 uses '+' and '/', base64url uses '-' and '_'
static const std::string BASE64_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int Base64CharToValue(char c) {
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	}
	if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	}
	if (c == '+' || c == '-') {
		return 62;	// '-' is base64url variant of '+'
	}
	if (c == '/' || c == '_') {
		return 63;	// '_' is base64url variant of '/'
	}
	return -1;
}

static std::string Base64UrlDecode(const std::string &input) {
	std::string result;
	result.reserve((input.size() * 3) / 4);

	int val = 0;
	int bits = -8;

	for (size_t i = 0; i < input.size(); ++i) {
		char c = input[i];
		if (c == '=') {
			break;	// Padding
		}

		int char_val = Base64CharToValue(c);
		if (char_val < 0) {
			continue;  // Skip invalid characters
		}

		val = (val << 6) + char_val;
		bits += 6;

		if (bits >= 0) {
			result.push_back(static_cast<char>((val >> bits) & 0xFF));
			bits -= 8;
		}
	}

	return result;
}

//===----------------------------------------------------------------------===//
// Simple JSON Parsing (for JWT payload)
//===----------------------------------------------------------------------===//

// Parse a JSON string value: "key": "value" or "key":"value"
static std::string ParseJsonString(const std::string &json, const std::string &key) {
	std::string search_key = "\"" + key + "\"";
	size_t key_pos = json.find(search_key);
	if (key_pos == std::string::npos) {
		return "";
	}

	// Find the colon after the key
	size_t colon_pos = json.find(':', key_pos + search_key.length());
	if (colon_pos == std::string::npos) {
		return "";
	}

	// Find the opening quote of the value
	size_t value_start = json.find('"', colon_pos + 1);
	if (value_start == std::string::npos) {
		return "";
	}

	// Find the closing quote of the value
	size_t value_end = json.find('"', value_start + 1);
	if (value_end == std::string::npos) {
		return "";
	}

	return json.substr(value_start + 1, value_end - value_start - 1);
}

// Parse a JSON integer value: "key": 123 or "key":123
static int64_t ParseJsonInt(const std::string &json, const std::string &key) {
	std::string search_key = "\"" + key + "\"";
	size_t key_pos = json.find(search_key);
	if (key_pos == std::string::npos) {
		return 0;
	}

	// Find the colon after the key
	size_t colon_pos = json.find(':', key_pos + search_key.length());
	if (colon_pos == std::string::npos) {
		return 0;
	}

	// Skip whitespace after colon
	size_t value_start = colon_pos + 1;
	while (value_start < json.size() && (json[value_start] == ' ' || json[value_start] == '\t')) {
		++value_start;
	}

	// Parse the integer
	int64_t result = 0;
	bool negative = false;
	if (value_start < json.size() && json[value_start] == '-') {
		negative = true;
		++value_start;
	}

	while (value_start < json.size() && json[value_start] >= '0' && json[value_start] <= '9') {
		result = result * 10 + (json[value_start] - '0');
		++value_start;
	}

	return negative ? -result : result;
}

//===----------------------------------------------------------------------===//
// JWT Parsing
//===----------------------------------------------------------------------===//

JwtClaims ParseJwtClaims(const std::string &token) {
	JwtClaims claims;

	// JWT format: header.payload.signature (base64url encoded)
	// Find the two dots separating the parts
	size_t first_dot = token.find('.');
	if (first_dot == std::string::npos) {
		claims.error = "Invalid JWT format: missing first separator";
		return claims;
	}

	size_t second_dot = token.find('.', first_dot + 1);
	if (second_dot == std::string::npos) {
		claims.error = "Invalid JWT format: missing second separator";
		return claims;
	}

	// Extract and decode the payload (middle part)
	std::string payload_b64 = token.substr(first_dot + 1, second_dot - first_dot - 1);
	if (payload_b64.empty()) {
		claims.error = "Invalid JWT format: empty payload";
		return claims;
	}

	std::string payload_json;
	try {
		payload_json = Base64UrlDecode(payload_b64);
	} catch (const std::exception &e) {
		claims.error = std::string("Failed to decode JWT payload: ") + e.what();
		return claims;
	}

	if (payload_json.empty()) {
		claims.error = "Invalid JWT format: payload decode resulted in empty string";
		return claims;
	}

	// Parse required claims
	claims.exp = ParseJsonInt(payload_json, "exp");
	if (claims.exp == 0) {
		claims.error = "Invalid JWT: missing or invalid 'exp' claim";
		return claims;
	}

	claims.aud = ParseJsonString(payload_json, "aud");
	if (claims.aud.empty()) {
		claims.error = "Invalid JWT: missing 'aud' claim";
		return claims;
	}

	// Parse optional claims (for logging/debugging)
	claims.oid = ParseJsonString(payload_json, "oid");
	claims.tid = ParseJsonString(payload_json, "tid");

	claims.valid = true;
	return claims;
}

//===----------------------------------------------------------------------===//
// Utility Functions
//===----------------------------------------------------------------------===//

std::string FormatTimestamp(int64_t unix_timestamp) {
	std::time_t time = static_cast<std::time_t>(unix_timestamp);
	std::tm *utc_tm = std::gmtime(&time);
	if (!utc_tm) {
		return "invalid timestamp";
	}

	char buffer[32];
	std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S UTC", utc_tm);
	return std::string(buffer);
}

bool IsTokenExpired(int64_t exp_timestamp, int64_t margin_seconds) {
	auto now = std::chrono::system_clock::now();
	auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
	return now_seconds >= (exp_timestamp - margin_seconds);
}

}  // namespace azure
}  // namespace mssql
}  // namespace duckdb

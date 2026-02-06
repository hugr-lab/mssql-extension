// test/cpp/test_jwt_parser.cpp
// Unit tests for JWT parsing functionality
//
// Spec 032: FEDAUTH Token Provider Enhancements - User Story 1
//
// These tests do NOT require a running SQL Server instance or Azure AD tokens.
// They test JWT parsing logic in isolation using synthetic tokens.
//
// Run:
//   ./build/release/test/unittest "*jwt*"

#include <cassert>
#include <iostream>
#include <string>

#include "azure/jwt_parser.hpp"

using namespace duckdb::mssql::azure;

//==============================================================================
// Helper macros for assertions with messages
//==============================================================================
#define ASSERT_EQ(actual, expected)                                                          \
	do {                                                                                     \
		if ((actual) != (expected)) {                                                        \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Expected: " << (expected) << std::endl;                          \
			std::cerr << "  Actual:   " << (actual) << std::endl;                            \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

#define ASSERT_TRUE(cond)                                                                    \
	do {                                                                                     \
		if (!(cond)) {                                                                       \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Condition is false: " #cond << std::endl;                        \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

#define ASSERT_FALSE(cond)                                                                   \
	do {                                                                                     \
		if ((cond)) {                                                                        \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Condition is true: " #cond << std::endl;                         \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

//==============================================================================
// Test Helper: Create a synthetic JWT with specific claims
//==============================================================================
// Base64url encode a string (simplified - works for ASCII payload)
static std::string Base64UrlEncode(const std::string &input) {
	static const char *const BASE64_CHARS =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

	std::string result;
	result.reserve(((input.size() + 2) / 3) * 4);

	int val = 0;
	int bits = -6;

	for (unsigned char c : input) {
		val = (val << 8) + c;
		bits += 8;
		while (bits >= 0) {
			result.push_back(BASE64_CHARS[(val >> bits) & 0x3F]);
			bits -= 6;
		}
	}

	if (bits > -6) {
		result.push_back(BASE64_CHARS[((val << 8) >> (bits + 8)) & 0x3F]);
	}

	// No padding for base64url
	return result;
}

// Create a synthetic JWT with the given payload JSON
static std::string CreateSyntheticJwt(const std::string &payload_json) {
	// Header: {"alg":"none","typ":"JWT"}
	std::string header = "{\"alg\":\"none\",\"typ\":\"JWT\"}";
	std::string signature = "test_signature";

	return Base64UrlEncode(header) + "." + Base64UrlEncode(payload_json) + "." + Base64UrlEncode(signature);
}

//==============================================================================
// T009/T010: Test valid JWT parsing (exp, aud extraction)
//==============================================================================
void test_parse_valid_jwt_basic() {
	std::cout << "\n=== Test: Parse Valid JWT - Basic Claims ===" << std::endl;

	// Create a JWT with standard Azure AD claims
	std::string payload = R"({"exp":1738857600,"aud":"https://database.windows.net/","oid":"12345","tid":"tenant-id"})";
	std::string token = CreateSyntheticJwt(payload);

	JwtClaims claims = ParseJwtClaims(token);

	ASSERT_TRUE(claims.valid);
	ASSERT_EQ(claims.exp, 1738857600);
	ASSERT_EQ(claims.aud, std::string("https://database.windows.net/"));
	ASSERT_EQ(claims.oid, std::string("12345"));
	ASSERT_EQ(claims.tid, std::string("tenant-id"));
	ASSERT_TRUE(claims.error.empty());

	std::cout << "PASSED!" << std::endl;
}

void test_parse_valid_jwt_minimal() {
	std::cout << "\n=== Test: Parse Valid JWT - Minimal Claims ===" << std::endl;

	// Only required claims: exp and aud
	std::string payload = R"({"exp":1738857600,"aud":"https://database.windows.net/"})";
	std::string token = CreateSyntheticJwt(payload);

	JwtClaims claims = ParseJwtClaims(token);

	ASSERT_TRUE(claims.valid);
	ASSERT_EQ(claims.exp, 1738857600);
	ASSERT_EQ(claims.aud, std::string("https://database.windows.net/"));
	// Optional claims should be empty but not cause failure
	ASSERT_TRUE(claims.oid.empty());
	ASSERT_TRUE(claims.tid.empty());

	std::cout << "PASSED!" << std::endl;
}

void test_parse_valid_jwt_large_exp() {
	std::cout << "\n=== Test: Parse Valid JWT - Large Expiration Timestamp ===" << std::endl;

	// Year 2100 timestamp
	std::string payload = R"({"exp":4102444800,"aud":"https://database.windows.net/"})";
	std::string token = CreateSyntheticJwt(payload);

	JwtClaims claims = ParseJwtClaims(token);

	ASSERT_TRUE(claims.valid);
	ASSERT_EQ(claims.exp, 4102444800);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// T011: Test malformed JWT error handling
//==============================================================================
void test_parse_malformed_no_dots() {
	std::cout << "\n=== Test: Parse Malformed JWT - No Dots ===" << std::endl;

	JwtClaims claims = ParseJwtClaims("notavalidtoken");

	ASSERT_FALSE(claims.valid);
	ASSERT_TRUE(claims.error.find("missing first separator") != std::string::npos);

	std::cout << "PASSED!" << std::endl;
}

void test_parse_malformed_one_dot() {
	std::cout << "\n=== Test: Parse Malformed JWT - One Dot ===" << std::endl;

	JwtClaims claims = ParseJwtClaims("header.payload");

	ASSERT_FALSE(claims.valid);
	ASSERT_TRUE(claims.error.find("missing second separator") != std::string::npos);

	std::cout << "PASSED!" << std::endl;
}

void test_parse_malformed_empty_payload() {
	std::cout << "\n=== Test: Parse Malformed JWT - Empty Payload ===" << std::endl;

	JwtClaims claims = ParseJwtClaims("header..signature");

	ASSERT_FALSE(claims.valid);
	ASSERT_TRUE(claims.error.find("empty payload") != std::string::npos);

	std::cout << "PASSED!" << std::endl;
}

void test_parse_malformed_missing_exp() {
	std::cout << "\n=== Test: Parse Malformed JWT - Missing exp Claim ===" << std::endl;

	std::string payload = R"({"aud":"https://database.windows.net/"})";
	std::string token = CreateSyntheticJwt(payload);

	JwtClaims claims = ParseJwtClaims(token);

	ASSERT_FALSE(claims.valid);
	ASSERT_TRUE(claims.error.find("exp") != std::string::npos);

	std::cout << "PASSED!" << std::endl;
}

void test_parse_malformed_missing_aud() {
	std::cout << "\n=== Test: Parse Malformed JWT - Missing aud Claim ===" << std::endl;

	std::string payload = R"({"exp":1738857600})";
	std::string token = CreateSyntheticJwt(payload);

	JwtClaims claims = ParseJwtClaims(token);

	ASSERT_FALSE(claims.valid);
	ASSERT_TRUE(claims.error.find("aud") != std::string::npos);

	std::cout << "PASSED!" << std::endl;
}

void test_parse_malformed_empty_string() {
	std::cout << "\n=== Test: Parse Malformed JWT - Empty String ===" << std::endl;

	JwtClaims claims = ParseJwtClaims("");

	ASSERT_FALSE(claims.valid);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// T012: Test expired token detection
//==============================================================================
void test_is_token_expired_past() {
	std::cout << "\n=== Test: IsTokenExpired - Past Timestamp ===" << std::endl;

	// Timestamp in the past (year 2020)
	int64_t past_exp = 1577836800;  // 2020-01-01 00:00:00 UTC

	ASSERT_TRUE(IsTokenExpired(past_exp, 300));

	std::cout << "PASSED!" << std::endl;
}

void test_is_token_expired_far_future() {
	std::cout << "\n=== Test: IsTokenExpired - Far Future Timestamp ===" << std::endl;

	// Timestamp in the far future (year 2100)
	int64_t future_exp = 4102444800;  // 2100-01-01 00:00:00 UTC

	ASSERT_FALSE(IsTokenExpired(future_exp, 300));

	std::cout << "PASSED!" << std::endl;
}

void test_is_token_expired_with_margin() {
	std::cout << "\n=== Test: IsTokenExpired - Within Margin ===" << std::endl;

	// Get current time and add just under 5 minutes
	auto now = std::chrono::system_clock::now();
	auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

	// Token expires in 4 minutes (240 seconds) - within 5-minute margin
	int64_t exp_in_margin = now_seconds + 240;
	ASSERT_TRUE(IsTokenExpired(exp_in_margin, 300));

	// Token expires in 10 minutes (600 seconds) - outside 5-minute margin
	int64_t exp_outside_margin = now_seconds + 600;
	ASSERT_FALSE(IsTokenExpired(exp_outside_margin, 300));

	std::cout << "PASSED!" << std::endl;
}

void test_is_token_expired_zero_margin() {
	std::cout << "\n=== Test: IsTokenExpired - Zero Margin ===" << std::endl;

	auto now = std::chrono::system_clock::now();
	auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

	// Token expires in 1 second - not expired with 0 margin
	int64_t exp_future = now_seconds + 1;
	ASSERT_FALSE(IsTokenExpired(exp_future, 0));

	// Token expired 1 second ago - expired with 0 margin
	int64_t exp_past = now_seconds - 1;
	ASSERT_TRUE(IsTokenExpired(exp_past, 0));

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// T037/T038: Test timestamp formatting and expiration message format
//==============================================================================
void test_format_timestamp_basic() {
	std::cout << "\n=== Test: FormatTimestamp - Basic ===" << std::endl;

	// 2024-02-06 14:30:00 UTC
	int64_t timestamp = 1707228600;
	std::string formatted = FormatTimestamp(timestamp);

	ASSERT_TRUE(formatted.find("2024") != std::string::npos);
	ASSERT_TRUE(formatted.find("UTC") != std::string::npos);

	std::cout << "PASSED! Formatted: " << formatted << std::endl;
}

void test_format_timestamp_epoch() {
	std::cout << "\n=== Test: FormatTimestamp - Unix Epoch ===" << std::endl;

	std::string formatted = FormatTimestamp(0);

	ASSERT_TRUE(formatted.find("1970") != std::string::npos);
	ASSERT_TRUE(formatted.find("UTC") != std::string::npos);

	std::cout << "PASSED! Formatted: " << formatted << std::endl;
}

//==============================================================================
// Test AZURE_SQL_AUDIENCE constant
//==============================================================================
void test_azure_sql_audience_constant() {
	std::cout << "\n=== Test: AZURE_SQL_AUDIENCE Constant ===" << std::endl;

	ASSERT_EQ(std::string(AZURE_SQL_AUDIENCE), std::string("https://database.windows.net/"));

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Main
//==============================================================================
int main() {
	std::cout << "\n========================================" << std::endl;
	std::cout << "JWT Parser Unit Tests (Spec 032)" << std::endl;
	std::cout << "========================================" << std::endl;

	// T009/T010: Valid JWT parsing
	test_parse_valid_jwt_basic();
	test_parse_valid_jwt_minimal();
	test_parse_valid_jwt_large_exp();

	// T011: Malformed JWT error handling
	test_parse_malformed_no_dots();
	test_parse_malformed_one_dot();
	test_parse_malformed_empty_payload();
	test_parse_malformed_missing_exp();
	test_parse_malformed_missing_aud();
	test_parse_malformed_empty_string();

	// T012: Expired token detection
	test_is_token_expired_past();
	test_is_token_expired_far_future();
	test_is_token_expired_with_margin();
	test_is_token_expired_zero_margin();

	// T037/T038: Timestamp formatting
	test_format_timestamp_basic();
	test_format_timestamp_epoch();

	// Constants
	test_azure_sql_audience_constant();

	std::cout << "\n========================================" << std::endl;
	std::cout << "ALL JWT PARSER TESTS PASSED!" << std::endl;
	std::cout << "========================================\n" << std::endl;

	return 0;
}

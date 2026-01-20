// test/cpp/test_value_serializer.cpp
// Unit tests for MSSQLValueSerializer
//
// These tests do NOT require a running SQL Server instance.
// They test the T-SQL literal generation logic in isolation.
//
// Tests cover:
// - Unicode string serialization (N'...' prefix)
// - SQL injection prevention (quote escaping)
// - Special character handling
// - Various data type serialization
//
// Compile:
//   See Makefile or CMakeLists.txt
//
// Run:
//   ./test_value_serializer

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/value.hpp"
#include "insert/mssql_value_serializer.hpp"

using namespace duckdb;

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

#define ASSERT_CONTAINS(str, substr)                                                         \
	do {                                                                                     \
		if ((str).find(substr) == std::string::npos) {                                       \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  String does not contain: " << (substr) << std::endl;             \
			std::cerr << "  Actual string: " << (str) << std::endl;                          \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

#define ASSERT_STARTS_WITH(str, prefix)                                                      \
	do {                                                                                     \
		if ((str).find(prefix) != 0) {                                                       \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  String does not start with: " << (prefix) << std::endl;          \
			std::cerr << "  Actual string: " << (str) << std::endl;                          \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

#define ASSERT_NOT_CONTAINS(str, substr)                                                     \
	do {                                                                                     \
		if ((str).find(substr) != std::string::npos) {                                       \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  String unexpectedly contains: " << (substr) << std::endl;        \
			std::cerr << "  Actual string: " << (str) << std::endl;                          \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

//==============================================================================
// Test: EscapeIdentifier - Basic identifiers
//==============================================================================
void test_escape_identifier_basic() {
	std::cout << "\n=== Test: EscapeIdentifier - Basic ===" << std::endl;

	// Simple identifier
	ASSERT_EQ(MSSQLValueSerializer::EscapeIdentifier("foo"), "[foo]");

	// With spaces
	ASSERT_EQ(MSSQLValueSerializer::EscapeIdentifier("my table"), "[my table]");

	// With numbers
	ASSERT_EQ(MSSQLValueSerializer::EscapeIdentifier("table123"), "[table123]");

	// Empty identifier
	ASSERT_EQ(MSSQLValueSerializer::EscapeIdentifier(""), "[]");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: EscapeIdentifier - Bracket escaping
//==============================================================================
void test_escape_identifier_brackets() {
	std::cout << "\n=== Test: EscapeIdentifier - Bracket Escaping ===" << std::endl;

	// Contains closing bracket - must be escaped as ]]
	ASSERT_EQ(MSSQLValueSerializer::EscapeIdentifier("foo]bar"), "[foo]]bar]");

	// Multiple closing brackets
	ASSERT_EQ(MSSQLValueSerializer::EscapeIdentifier("a]b]c"), "[a]]b]]c]");

	// Closing bracket at start
	ASSERT_EQ(MSSQLValueSerializer::EscapeIdentifier("]foo"), "[]]foo]");

	// Closing bracket at end
	ASSERT_EQ(MSSQLValueSerializer::EscapeIdentifier("foo]"), "[foo]]]");

	// Opening bracket (no escaping needed)
	ASSERT_EQ(MSSQLValueSerializer::EscapeIdentifier("foo[bar"), "[foo[bar]");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: EscapeString - Basic string escaping
//==============================================================================
void test_escape_string_basic() {
	std::cout << "\n=== Test: EscapeString - Basic ===" << std::endl;

	// Simple string - no escaping needed
	ASSERT_EQ(MSSQLValueSerializer::EscapeString("hello"), "hello");

	// Empty string
	ASSERT_EQ(MSSQLValueSerializer::EscapeString(""), "");

	// Single quote must be doubled
	ASSERT_EQ(MSSQLValueSerializer::EscapeString("it's"), "it''s");

	// Multiple single quotes
	ASSERT_EQ(MSSQLValueSerializer::EscapeString("'hello'"), "''hello''");

	// Single quotes adjacent
	ASSERT_EQ(MSSQLValueSerializer::EscapeString("a''b"), "a''''b");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: SerializeString - Unicode prefix (N'...')
//==============================================================================
void test_serialize_string_unicode_prefix() {
	std::cout << "\n=== Test: SerializeString - Unicode Prefix (N'...') ===" << std::endl;

	// Simple ASCII string should use N'' prefix
	Value val = Value("hello");
	auto result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_STARTS_WITH(result, "N'");
	ASSERT_EQ(result, "N'hello'");

	// Empty string
	val = Value("");
	result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_EQ(result, "N''");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: SerializeString - Unicode characters (T059)
//==============================================================================
void test_serialize_string_unicode_characters() {
	std::cout << "\n=== Test: SerializeString - Unicode Characters ===" << std::endl;

	// Chinese characters
	Value val = Value("你好世界");	// "Hello World" in Chinese
	auto result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_STARTS_WITH(result, "N'");
	ASSERT_CONTAINS(result, "你好世界");
	ASSERT_EQ(result, "N'你好世界'");

	// Japanese characters (Hiragana)
	val = Value("こんにちは");	// "Hello" in Japanese
	result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_STARTS_WITH(result, "N'");
	ASSERT_CONTAINS(result, "こんにちは");

	// Korean characters
	val = Value("안녕하세요");	// "Hello" in Korean
	result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_STARTS_WITH(result, "N'");
	ASSERT_CONTAINS(result, "안녕하세요");

	// Arabic characters
	val = Value("مرحبا");  // "Hello" in Arabic
	result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_STARTS_WITH(result, "N'");
	ASSERT_CONTAINS(result, "مرحبا");

	// Cyrillic characters
	val = Value("Привет");	// "Hello" in Russian
	result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_STARTS_WITH(result, "N'");
	ASSERT_CONTAINS(result, "Привет");

	// Emoji characters
	val = Value("Hello 😀🎉🚀");
	result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_STARTS_WITH(result, "N'");
	ASSERT_CONTAINS(result, "😀");
	ASSERT_CONTAINS(result, "🎉");
	ASSERT_CONTAINS(result, "🚀");

	// Mixed Unicode and ASCII
	val = Value("Hello 世界 مرحبا 🌍");
	result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_STARTS_WITH(result, "N'");
	ASSERT_CONTAINS(result, "Hello");
	ASSERT_CONTAINS(result, "世界");
	ASSERT_CONTAINS(result, "مرحبا");
	ASSERT_CONTAINS(result, "🌍");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: SerializeString - SQL Injection Prevention (T060)
//==============================================================================
void test_serialize_string_sql_injection() {
	std::cout << "\n=== Test: SerializeString - SQL Injection Prevention ===" << std::endl;

	// Classic SQL injection with single quote
	Value val = Value("'; DROP TABLE users; --");
	auto result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_STARTS_WITH(result, "N'");
	// Single quote should be doubled
	ASSERT_CONTAINS(result, "''");
	// Should NOT contain unescaped single quote followed by semicolon
	ASSERT_NOT_CONTAINS(result, "';");
	std::cout << "  Injection attempt 1: " << result << std::endl;

	// SQL injection with comment
	val = Value("admin'--");
	result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_CONTAINS(result, "''--");
	ASSERT_NOT_CONTAINS(result, "admin'--'");  // Quote should be escaped
	std::cout << "  Injection attempt 2: " << result << std::endl;

	// Multiple quotes in injection
	val = Value("' OR ''='");
	result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	// All quotes should be doubled
	ASSERT_NOT_CONTAINS(result, "' OR");  // Opening quote should be escaped
	std::cout << "  Injection attempt 3: " << result << std::endl;

	// UNION-based injection
	val = Value("' UNION SELECT * FROM passwords --");
	result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_CONTAINS(result, "''");
	std::cout << "  Injection attempt 4: " << result << std::endl;

	// Stacked queries injection
	val = Value("'; INSERT INTO users VALUES('hacker'); --");
	result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_CONTAINS(result, "''");
	// Make sure the semicolon is inside the string, not breaking out
	ASSERT_NOT_CONTAINS(result, "';");
	std::cout << "  Injection attempt 5: " << result << std::endl;

	// Unicode-based injection attempts
	val = Value("' OR 1=1 --你好");
	result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_STARTS_WITH(result, "N'");
	ASSERT_CONTAINS(result, "''");
	std::cout << "  Injection attempt 6 (Unicode): " << result << std::endl;

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: SerializeString - Special Characters
//==============================================================================
void test_serialize_string_special_chars() {
	std::cout << "\n=== Test: SerializeString - Special Characters ===" << std::endl;

	// Backslash (should NOT be escaped in T-SQL)
	Value val = Value("path\\to\\file");
	auto result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_CONTAINS(result, "path\\to\\file");

	// Newline characters
	val = Value("line1\nline2");
	result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_CONTAINS(result, "\n");

	// Tab characters
	val = Value("col1\tcol2");
	result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_CONTAINS(result, "\t");

	// Carriage return
	val = Value("line1\r\nline2");
	result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_CONTAINS(result, "\r\n");

	// Null character (embedded in string)
	std::string with_null = "before";
	with_null += '\0';
	with_null += "after";
	val = Value(with_null);
	result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	// Should preserve null character
	ASSERT_STARTS_WITH(result, "N'");

	// Percent and underscore (LIKE wildcards - should NOT be escaped)
	val = Value("50% off_sale");
	result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_CONTAINS(result, "%");
	ASSERT_CONTAINS(result, "_");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: SerializeBoolean
//==============================================================================
void test_serialize_boolean() {
	std::cout << "\n=== Test: SerializeBoolean ===" << std::endl;

	Value val = Value::BOOLEAN(true);
	auto result = MSSQLValueSerializer::Serialize(val, LogicalType::BOOLEAN);
	ASSERT_EQ(result, "1");

	val = Value::BOOLEAN(false);
	result = MSSQLValueSerializer::Serialize(val, LogicalType::BOOLEAN);
	ASSERT_EQ(result, "0");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: SerializeInteger
//==============================================================================
void test_serialize_integer() {
	std::cout << "\n=== Test: SerializeInteger ===" << std::endl;

	// Positive integer
	Value val = Value::INTEGER(42);
	auto result = MSSQLValueSerializer::Serialize(val, LogicalType::INTEGER);
	ASSERT_EQ(result, "42");

	// Negative integer
	val = Value::INTEGER(-123);
	result = MSSQLValueSerializer::Serialize(val, LogicalType::INTEGER);
	ASSERT_EQ(result, "-123");

	// Zero
	val = Value::INTEGER(0);
	result = MSSQLValueSerializer::Serialize(val, LogicalType::INTEGER);
	ASSERT_EQ(result, "0");

	// BIGINT max
	val = Value::BIGINT(9223372036854775807LL);
	result = MSSQLValueSerializer::Serialize(val, LogicalType::BIGINT);
	ASSERT_EQ(result, "9223372036854775807");

	// BIGINT min
	val = Value::BIGINT(-9223372036854775807LL - 1);
	result = MSSQLValueSerializer::Serialize(val, LogicalType::BIGINT);
	ASSERT_EQ(result, "-9223372036854775808");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: SerializeFloat
//==============================================================================
void test_serialize_float() {
	std::cout << "\n=== Test: SerializeFloat ===" << std::endl;

	// Simple float
	Value val = Value::FLOAT(3.14f);
	auto result = MSSQLValueSerializer::Serialize(val, LogicalType::FLOAT);
	ASSERT_CONTAINS(result, "3.14");

	// Zero
	val = Value::FLOAT(0.0f);
	result = MSSQLValueSerializer::Serialize(val, LogicalType::FLOAT);
	ASSERT_CONTAINS(result, "0");

	// Negative
	val = Value::FLOAT(-2.5f);
	result = MSSQLValueSerializer::Serialize(val, LogicalType::FLOAT);
	ASSERT_CONTAINS(result, "-2.5");

	// Double precision
	val = Value::DOUBLE(3.141592653589793);
	result = MSSQLValueSerializer::Serialize(val, LogicalType::DOUBLE);
	ASSERT_CONTAINS(result, "3.14159");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: SerializeNull
//==============================================================================
void test_serialize_null() {
	std::cout << "\n=== Test: SerializeNull ===" << std::endl;

	// NULL VARCHAR
	Value val = Value(LogicalType::VARCHAR);
	auto result = MSSQLValueSerializer::Serialize(val, LogicalType::VARCHAR);
	ASSERT_EQ(result, "NULL");

	// NULL INTEGER
	val = Value(LogicalType::INTEGER);
	result = MSSQLValueSerializer::Serialize(val, LogicalType::INTEGER);
	ASSERT_EQ(result, "NULL");

	// NULL BOOLEAN
	val = Value(LogicalType::BOOLEAN);
	result = MSSQLValueSerializer::Serialize(val, LogicalType::BOOLEAN);
	ASSERT_EQ(result, "NULL");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: SerializeBlob
//==============================================================================
void test_serialize_blob() {
	std::cout << "\n=== Test: SerializeBlob ===" << std::endl;

	// Simple blob
	Value val = Value::BLOB("\x00\x01\x02\x03", 4);
	auto result = MSSQLValueSerializer::Serialize(val, LogicalType::BLOB);
	ASSERT_STARTS_WITH(result, "0x");
	ASSERT_CONTAINS(result, "00010203");

	// Empty blob
	val = Value::BLOB("", 0);
	result = MSSQLValueSerializer::Serialize(val, LogicalType::BLOB);
	ASSERT_EQ(result, "0x");

	// Blob with all byte values
	std::string all_bytes;
	all_bytes += '\xFF';
	all_bytes += '\x00';
	all_bytes += '\xAB';
	val = Value::BLOB(all_bytes);
	result = MSSQLValueSerializer::Serialize(val, LogicalType::BLOB);
	ASSERT_STARTS_WITH(result, "0x");
	ASSERT_CONTAINS(result, "FF00AB");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: SerializeDate
//==============================================================================
void test_serialize_date() {
	std::cout << "\n=== Test: SerializeDate ===" << std::endl;

	// 2024-01-15
	Value val = Value::DATE(Date::FromDate(2024, 1, 15));
	auto result = MSSQLValueSerializer::Serialize(val, LogicalType::DATE);
	ASSERT_CONTAINS(result, "2024");
	ASSERT_CONTAINS(result, "01");
	ASSERT_CONTAINS(result, "15");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: SerializeTime
//==============================================================================
void test_serialize_time() {
	std::cout << "\n=== Test: SerializeTime ===" << std::endl;

	// 14:30:00
	Value val = Value::TIME(Time::FromTime(14, 30, 0, 0));
	auto result = MSSQLValueSerializer::Serialize(val, LogicalType::TIME);
	ASSERT_CONTAINS(result, "14");
	ASSERT_CONTAINS(result, "30");
	ASSERT_CONTAINS(result, "00");

	// Midnight
	val = Value::TIME(Time::FromTime(0, 0, 0, 0));
	result = MSSQLValueSerializer::Serialize(val, LogicalType::TIME);
	ASSERT_CONTAINS(result, "00");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: SerializeTimestamp
//==============================================================================
void test_serialize_timestamp() {
	std::cout << "\n=== Test: SerializeTimestamp ===" << std::endl;

	// 2024-01-15 14:30:00
	Value val = Value::TIMESTAMP(Timestamp::FromDatetime(Date::FromDate(2024, 1, 15), Time::FromTime(14, 30, 0, 0)));
	auto result = MSSQLValueSerializer::Serialize(val, LogicalType::TIMESTAMP);
	ASSERT_CONTAINS(result, "2024");
	ASSERT_CONTAINS(result, "01");
	ASSERT_CONTAINS(result, "15");
	ASSERT_CONTAINS(result, "14");
	ASSERT_CONTAINS(result, "30");
	// Should use CAST to datetime2
	ASSERT_CONTAINS(result, "CAST");
	ASSERT_CONTAINS(result, "datetime2");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: SerializeDecimal
//==============================================================================
void test_serialize_decimal() {
	std::cout << "\n=== Test: SerializeDecimal ===" << std::endl;

	// Simple decimal
	Value val = Value::DECIMAL(12345, 5, 2);  // 123.45
	auto result = MSSQLValueSerializer::Serialize(val, LogicalType::DECIMAL(5, 2));
	ASSERT_CONTAINS(result, "123");
	ASSERT_CONTAINS(result, "45");

	// Zero decimal
	val = Value::DECIMAL(0, 5, 2);
	result = MSSQLValueSerializer::Serialize(val, LogicalType::DECIMAL(5, 2));
	ASSERT_CONTAINS(result, "0");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: SerializeUUID
//==============================================================================
void test_serialize_uuid() {
	std::cout << "\n=== Test: SerializeUUID ===" << std::endl;

	// Standard UUID format
	hugeint_t uuid_value;
	UUID::FromString("550e8400-e29b-41d4-a716-446655440000", uuid_value);
	Value val = Value::UUID(uuid_value);
	auto result = MSSQLValueSerializer::Serialize(val, LogicalType::UUID);
	// Should be quoted string format
	ASSERT_CONTAINS(result, "550e8400");
	ASSERT_CONTAINS(result, "446655440000");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: SerializeUBigInt
//==============================================================================
void test_serialize_ubigint() {
	std::cout << "\n=== Test: SerializeUBigInt ===" << std::endl;

	// Large unsigned value within BIGINT range
	Value val = Value::UBIGINT(9223372036854775807ULL);
	auto result = MSSQLValueSerializer::Serialize(val, LogicalType::UBIGINT);
	ASSERT_CONTAINS(result, "9223372036854775807");

	// Value exceeding BIGINT max (requires CAST to DECIMAL)
	val = Value::UBIGINT(18446744073709551615ULL);	// UBIGINT max
	result = MSSQLValueSerializer::Serialize(val, LogicalType::UBIGINT);
	// Should use CAST to DECIMAL for large values
	ASSERT_CONTAINS(result, "CAST");
	ASSERT_CONTAINS(result, "DECIMAL");
	ASSERT_CONTAINS(result, "18446744073709551615");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: SerializeTinyInt and SmallInt
//==============================================================================
void test_serialize_small_integers() {
	std::cout << "\n=== Test: SerializeTinyInt and SmallInt ===" << std::endl;

	// TINYINT
	Value val = Value::TINYINT(255);
	auto result = MSSQLValueSerializer::Serialize(val, LogicalType::TINYINT);
	ASSERT_EQ(result, "255");

	val = Value::TINYINT(0);
	result = MSSQLValueSerializer::Serialize(val, LogicalType::TINYINT);
	ASSERT_EQ(result, "0");

	// SMALLINT
	val = Value::SMALLINT(32767);
	result = MSSQLValueSerializer::Serialize(val, LogicalType::SMALLINT);
	ASSERT_EQ(result, "32767");

	val = Value::SMALLINT(-32768);
	result = MSSQLValueSerializer::Serialize(val, LogicalType::SMALLINT);
	ASSERT_EQ(result, "-32768");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Main
//==============================================================================
int main() {
	std::cout << "==========================================" << std::endl;
	std::cout << "MSSQLValueSerializer Unit Tests" << std::endl;
	std::cout << "==========================================" << std::endl;

	try {
		// Identifier escaping tests
		test_escape_identifier_basic();
		test_escape_identifier_brackets();

		// String escaping tests
		test_escape_string_basic();

		// Unicode prefix tests (T058)
		test_serialize_string_unicode_prefix();

		// Unicode character tests (T059)
		test_serialize_string_unicode_characters();

		// SQL injection prevention tests (T060)
		test_serialize_string_sql_injection();

		// Special character tests
		test_serialize_string_special_chars();

		// Other type tests
		test_serialize_boolean();
		test_serialize_integer();
		test_serialize_small_integers();
		test_serialize_float();
		test_serialize_null();
		test_serialize_blob();
		test_serialize_date();
		test_serialize_time();
		test_serialize_timestamp();
		test_serialize_decimal();
		test_serialize_uuid();
		test_serialize_ubigint();

		std::cout << "\n==========================================" << std::endl;
		std::cout << "ALL TESTS PASSED!" << std::endl;
		std::cout << "==========================================" << std::endl;
		return 0;

	} catch (const std::exception &e) {
		std::cerr << "\n==========================================" << std::endl;
		std::cerr << "TEST FAILED WITH EXCEPTION: " << e.what() << std::endl;
		std::cerr << "==========================================" << std::endl;
		return 1;
	}
}

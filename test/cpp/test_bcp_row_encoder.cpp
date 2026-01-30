// test/cpp/test_bcp_row_encoder.cpp
// Unit tests for BCPRowEncoder - binary type encoding for TDS BulkLoadBCP
//
// These tests do NOT require a running SQL Server instance.
// They test the binary encoding logic in isolation against the TDS wire format.
//
// Tests cover:
// - Integer types (INTNTYPE 0x26)
// - Bit type (BITNTYPE 0x68)
// - Float types (FLTNTYPE 0x6D)
// - Decimal type (DECIMALNTYPE 0x6A)
// - Unicode string (NVARCHARTYPE 0xE7)
// - Binary data (BIGVARBINARYTYPE 0xA5)
// - GUID (GUIDTYPE 0x24) - mixed-endian encoding
// - Date/Time types (DATE, TIME, DATETIME2, DATETIMEOFFSET)
// - NULL encoding (fixed and variable length)

#include <cassert>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "tds/encoding/bcp_row_encoder.hpp"

using namespace duckdb;
using namespace duckdb::tds::encoding;

//==============================================================================
// Helper Functions
//==============================================================================

std::string BytesToHex(const std::vector<uint8_t> &bytes) {
	std::stringstream ss;
	for (size_t i = 0; i < bytes.size(); i++) {
		if (i > 0)
			ss << " ";
		ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(bytes[i]);
	}
	return ss.str();
}

#define ASSERT_BYTES_EQ(actual, expected)                                                        \
	do {                                                                                         \
		if ((actual) != (expected)) {                                                            \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl;     \
			std::cerr << "  Expected bytes: " << BytesToHex(expected) << std::endl;              \
			std::cerr << "  Actual bytes:   " << BytesToHex(actual) << std::endl;                \
			assert(false);                                                                       \
		}                                                                                        \
	} while (0)

#define ASSERT_EQ(actual, expected)                                                              \
	do {                                                                                         \
		if ((actual) != (expected)) {                                                            \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl;     \
			std::cerr << "  Expected: " << (expected) << std::endl;                              \
			std::cerr << "  Actual:   " << (actual) << std::endl;                                \
			assert(false);                                                                       \
		}                                                                                        \
	} while (0)

//==============================================================================
// Test: Integer Encoding - Int8
//==============================================================================
void test_encode_int8() {
	std::cout << "\n=== Test: EncodeInt8 ===" << std::endl;

	// Positive value: 42
	std::vector<uint8_t> buffer;
	BCPRowEncoder::EncodeInt8(buffer, 42);
	// Expected: [01] (length) [2A] (42)
	std::vector<uint8_t> expected = {0x01, 0x2A};
	ASSERT_BYTES_EQ(buffer, expected);

	// Zero
	buffer.clear();
	BCPRowEncoder::EncodeInt8(buffer, 0);
	expected = {0x01, 0x00};
	ASSERT_BYTES_EQ(buffer, expected);

	// Negative: -1
	buffer.clear();
	BCPRowEncoder::EncodeInt8(buffer, -1);
	expected = {0x01, 0xFF};  // Two's complement
	ASSERT_BYTES_EQ(buffer, expected);

	// Min value: -128
	buffer.clear();
	BCPRowEncoder::EncodeInt8(buffer, -128);
	expected = {0x01, 0x80};
	ASSERT_BYTES_EQ(buffer, expected);

	// Max value: 127
	buffer.clear();
	BCPRowEncoder::EncodeInt8(buffer, 127);
	expected = {0x01, 0x7F};
	ASSERT_BYTES_EQ(buffer, expected);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Integer Encoding - Int16
//==============================================================================
void test_encode_int16() {
	std::cout << "\n=== Test: EncodeInt16 ===" << std::endl;

	// Value: 1000 (0x03E8)
	std::vector<uint8_t> buffer;
	BCPRowEncoder::EncodeInt16(buffer, 1000);
	// Expected: [02] (length) [E8 03] (little-endian)
	std::vector<uint8_t> expected = {0x02, 0xE8, 0x03};
	ASSERT_BYTES_EQ(buffer, expected);

	// Negative: -1000
	buffer.clear();
	BCPRowEncoder::EncodeInt16(buffer, -1000);
	// -1000 = 0xFC18 in two's complement
	expected = {0x02, 0x18, 0xFC};
	ASSERT_BYTES_EQ(buffer, expected);

	// Max: 32767
	buffer.clear();
	BCPRowEncoder::EncodeInt16(buffer, 32767);
	expected = {0x02, 0xFF, 0x7F};
	ASSERT_BYTES_EQ(buffer, expected);

	// Min: -32768
	buffer.clear();
	BCPRowEncoder::EncodeInt16(buffer, -32768);
	expected = {0x02, 0x00, 0x80};
	ASSERT_BYTES_EQ(buffer, expected);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Integer Encoding - Int32
//==============================================================================
void test_encode_int32() {
	std::cout << "\n=== Test: EncodeInt32 ===" << std::endl;

	// Value: 123456 (0x0001E240)
	std::vector<uint8_t> buffer;
	BCPRowEncoder::EncodeInt32(buffer, 123456);
	// Expected: [04] [40 E2 01 00] (little-endian)
	std::vector<uint8_t> expected = {0x04, 0x40, 0xE2, 0x01, 0x00};
	ASSERT_BYTES_EQ(buffer, expected);

	// Negative: -123456
	buffer.clear();
	BCPRowEncoder::EncodeInt32(buffer, -123456);
	// -123456 = 0xFFFE1DC0
	expected = {0x04, 0xC0, 0x1D, 0xFE, 0xFF};
	ASSERT_BYTES_EQ(buffer, expected);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Integer Encoding - Int64
//==============================================================================
void test_encode_int64() {
	std::cout << "\n=== Test: EncodeInt64 ===" << std::endl;

	// Value: 1234567890123 (0x0000011F71FB04CB)
	std::vector<uint8_t> buffer;
	BCPRowEncoder::EncodeInt64(buffer, 1234567890123LL);
	// Expected: [08] [CB 04 FB 71 1F 01 00 00] (little-endian)
	std::vector<uint8_t> expected = {0x08, 0xCB, 0x04, 0xFB, 0x71, 0x1F, 0x01, 0x00, 0x00};
	ASSERT_BYTES_EQ(buffer, expected);

	// Negative
	buffer.clear();
	BCPRowEncoder::EncodeInt64(buffer, -1LL);
	expected = {0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	ASSERT_BYTES_EQ(buffer, expected);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Unsigned Int8 Encoding
//==============================================================================
void test_encode_uint8() {
	std::cout << "\n=== Test: EncodeUInt8 ===" << std::endl;

	// Value: 255
	std::vector<uint8_t> buffer;
	BCPRowEncoder::EncodeUInt8(buffer, 255);
	std::vector<uint8_t> expected = {0x01, 0xFF};
	ASSERT_BYTES_EQ(buffer, expected);

	// Value: 0
	buffer.clear();
	BCPRowEncoder::EncodeUInt8(buffer, 0);
	expected = {0x01, 0x00};
	ASSERT_BYTES_EQ(buffer, expected);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Bit Encoding
//==============================================================================
void test_encode_bit() {
	std::cout << "\n=== Test: EncodeBit ===" << std::endl;

	// True
	std::vector<uint8_t> buffer;
	BCPRowEncoder::EncodeBit(buffer, true);
	std::vector<uint8_t> expected = {0x01, 0x01};
	ASSERT_BYTES_EQ(buffer, expected);

	// False
	buffer.clear();
	BCPRowEncoder::EncodeBit(buffer, false);
	expected = {0x01, 0x00};
	ASSERT_BYTES_EQ(buffer, expected);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Float Encoding
//==============================================================================
void test_encode_float() {
	std::cout << "\n=== Test: EncodeFloat ===" << std::endl;

	// Value: 3.14f (IEEE 754: 0x4048F5C3)
	std::vector<uint8_t> buffer;
	BCPRowEncoder::EncodeFloat(buffer, 3.14f);
	// Expected: [04] [C3 F5 48 40] (little-endian)
	ASSERT_EQ(buffer.size(), 5u);
	ASSERT_EQ(buffer[0], 0x04);  // length

	// Verify by decoding back
	uint32_t encoded;
	memcpy(&encoded, &buffer[1], 4);
	float decoded;
	memcpy(&decoded, &encoded, 4);
	assert(std::abs(decoded - 3.14f) < 0.001f);

	// Zero
	buffer.clear();
	BCPRowEncoder::EncodeFloat(buffer, 0.0f);
	std::vector<uint8_t> expected = {0x04, 0x00, 0x00, 0x00, 0x00};
	ASSERT_BYTES_EQ(buffer, expected);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Double Encoding
//==============================================================================
void test_encode_double() {
	std::cout << "\n=== Test: EncodeDouble ===" << std::endl;

	// Value: 3.141592653589793 (IEEE 754: 0x400921FB54442D18)
	std::vector<uint8_t> buffer;
	BCPRowEncoder::EncodeDouble(buffer, 3.141592653589793);
	ASSERT_EQ(buffer.size(), 9u);
	ASSERT_EQ(buffer[0], 0x08);  // length

	// Verify by decoding back
	uint64_t encoded;
	memcpy(&encoded, &buffer[1], 8);
	double decoded;
	memcpy(&decoded, &encoded, 8);
	assert(std::abs(decoded - 3.141592653589793) < 1e-15);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Decimal Encoding
//==============================================================================
void test_encode_decimal() {
	std::cout << "\n=== Test: EncodeDecimal ===" << std::endl;

	// Value: 12345 with precision 5, scale 2 -> represents 123.45
	// The internal value is stored as the scaled integer
	std::vector<uint8_t> buffer;
	hugeint_t value;
	value.upper = 0;
	value.lower = 12345;
	BCPRowEncoder::EncodeDecimal(buffer, value, 5, 2);

	// Precision 5 -> byte_size = 5 (1 sign + 4 mantissa)
	// Expected: [05] [01 (positive)] [39 30 00 00] (12345 LE in 4 bytes)
	std::vector<uint8_t> expected = {0x05, 0x01, 0x39, 0x30, 0x00, 0x00};
	ASSERT_BYTES_EQ(buffer, expected);

	// Negative value: -12345
	buffer.clear();
	value.upper = -1;
	value.lower = static_cast<uint64_t>(-12345);  // Two's complement hugeint
	// Actually need to use proper negative hugeint
	value = hugeint_t(-12345);
	BCPRowEncoder::EncodeDecimal(buffer, value, 5, 2);

	// Expected: [05] [00 (negative)] [39 30 00 00]
	expected = {0x05, 0x00, 0x39, 0x30, 0x00, 0x00};
	ASSERT_BYTES_EQ(buffer, expected);

	// Zero
	buffer.clear();
	value = hugeint_t(0);
	BCPRowEncoder::EncodeDecimal(buffer, value, 5, 2);
	// Sign for zero is 0x01 (non-negative)
	expected = {0x05, 0x01, 0x00, 0x00, 0x00, 0x00};
	ASSERT_BYTES_EQ(buffer, expected);

	// Larger precision (10-19 uses 9 bytes)
	buffer.clear();
	value = hugeint_t(9999999999LL);
	BCPRowEncoder::EncodeDecimal(buffer, value, 15, 2);
	ASSERT_EQ(buffer.size(), 10u);  // 1 length + 1 sign + 8 mantissa
	ASSERT_EQ(buffer[0], 0x09);     // length = 9

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: NVARCHAR Encoding (UTF-16LE)
//==============================================================================
void test_encode_nvarchar() {
	std::cout << "\n=== Test: EncodeNVarchar ===" << std::endl;

	// ASCII string "hello"
	std::vector<uint8_t> buffer;
	BCPRowEncoder::EncodeNVarchar(buffer, string_t("hello"));

	// "hello" in UTF-16LE: h=0x0068, e=0x0065, l=0x006C, l=0x006C, o=0x006F
	// Length: 10 bytes (5 chars * 2)
	// Expected: [0A 00] [68 00 65 00 6C 00 6C 00 6F 00]
	std::vector<uint8_t> expected = {0x0A, 0x00, 0x68, 0x00, 0x65, 0x00, 0x6C, 0x00, 0x6C, 0x00, 0x6F, 0x00};
	ASSERT_BYTES_EQ(buffer, expected);

	// Empty string
	buffer.clear();
	BCPRowEncoder::EncodeNVarchar(buffer, string_t(""));
	expected = {0x00, 0x00};  // Length 0
	ASSERT_BYTES_EQ(buffer, expected);

	// Unicode string: "你好" (Chinese)
	buffer.clear();
	BCPRowEncoder::EncodeNVarchar(buffer, string_t("你好"));
	// 你 = U+4F60 -> 0x4F60 in UTF-16LE: 60 4F
	// 好 = U+597D -> 0x597D in UTF-16LE: 7D 59
	// Length: 4 bytes
	expected = {0x04, 0x00, 0x60, 0x4F, 0x7D, 0x59};
	ASSERT_BYTES_EQ(buffer, expected);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Binary Encoding
//==============================================================================
void test_encode_binary() {
	std::cout << "\n=== Test: EncodeBinary ===" << std::endl;

	// Simple bytes
	std::vector<uint8_t> buffer;
	std::string data = "\x01\x02\x03\x04";
	BCPRowEncoder::EncodeBinary(buffer, string_t(data));

	// Expected: [04 00] (length) [01 02 03 04]
	std::vector<uint8_t> expected = {0x04, 0x00, 0x01, 0x02, 0x03, 0x04};
	ASSERT_BYTES_EQ(buffer, expected);

	// Empty binary
	buffer.clear();
	BCPRowEncoder::EncodeBinary(buffer, string_t(""));
	expected = {0x00, 0x00};
	ASSERT_BYTES_EQ(buffer, expected);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: GUID Encoding (Mixed-Endian)
//==============================================================================
void test_encode_guid() {
	std::cout << "\n=== Test: EncodeGUID ===" << std::endl;

	// UUID: "550e8400-e29b-41d4-a716-446655440000"
	// Standard hex: 550e8400e29b41d4a716446655440000
	//
	// DuckDB UUID has high bit flipped for sortability.
	// We need to create the internal representation.

	hugeint_t uuid;
	UUID::FromString("550e8400-e29b-41d4-a716-446655440000", uuid);

	std::vector<uint8_t> buffer;
	BCPRowEncoder::EncodeGUID(buffer, uuid);

	// Expected wire format (mixed-endian):
	// Length: 16 (0x10)
	// Data1 (550e8400) as LE: 00 84 0e 55
	// Data2 (e29b) as LE: 9b e2
	// Data3 (41d4) as LE: d4 41
	// Data4 (a716446655440000) as BE: a7 16 44 66 55 44 00 00
	std::vector<uint8_t> expected = {0x10, 0x00, 0x84, 0x0E, 0x55, 0x9B, 0xE2, 0xD4,
	                                 0x41, 0xA7, 0x16, 0x44, 0x66, 0x55, 0x44, 0x00, 0x00};
	ASSERT_BYTES_EQ(buffer, expected);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Date Encoding
//==============================================================================
void test_encode_date() {
	std::cout << "\n=== Test: EncodeDate ===" << std::endl;

	// Date: 2024-01-15
	// Days since 1970-01-01 for this date
	date_t date = Date::FromDate(2024, 1, 15);

	std::vector<uint8_t> buffer;
	BCPRowEncoder::EncodeDate(buffer, date);

	// Days since 0001-01-01 = 719162 (epoch offset) + date.days
	// 2024-01-15 is day 19737 since Unix epoch
	// Total: 719162 + 19737 = 738899 = 0x0B4633
	ASSERT_EQ(buffer.size(), 4u);
	ASSERT_EQ(buffer[0], 0x03);  // length = 3

	// Decode the date value to verify
	uint32_t encoded_days = buffer[1] | (buffer[2] << 8) | (buffer[3] << 16);
	ASSERT_EQ(encoded_days, 738899u);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Time Encoding
//==============================================================================
void test_encode_time() {
	std::cout << "\n=== Test: EncodeTime ===" << std::endl;

	// Time: 14:30:00.000000 (scale 6)
	// 14:30:00 = 52200 seconds = 52200000000 microseconds
	dtime_t time = Time::FromTime(14, 30, 0, 0);

	std::vector<uint8_t> buffer;
	BCPRowEncoder::EncodeTime(buffer, time, 6);

	// Scale 6 -> 5 bytes for time value
	// Value: 52200000000 (microseconds)
	ASSERT_EQ(buffer.size(), 6u);
	ASSERT_EQ(buffer[0], 0x05);  // length = 5

	// Scale 0 -> value = 52200 (seconds), 3 bytes
	buffer.clear();
	BCPRowEncoder::EncodeTime(buffer, time, 0);
	ASSERT_EQ(buffer.size(), 4u);
	ASSERT_EQ(buffer[0], 0x03);

	// Decode to verify: should be 52200
	uint32_t encoded_time = buffer[1] | (buffer[2] << 8) | (buffer[3] << 16);
	ASSERT_EQ(encoded_time, 52200u);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: DateTime2 Encoding
//==============================================================================
void test_encode_datetime2() {
	std::cout << "\n=== Test: EncodeDatetime2 ===" << std::endl;

	// Timestamp: 2024-01-15 14:30:00
	timestamp_t ts = Timestamp::FromDatetime(Date::FromDate(2024, 1, 15), Time::FromTime(14, 30, 0, 0));

	std::vector<uint8_t> buffer;
	BCPRowEncoder::EncodeDatetime2(buffer, ts, 6);

	// Scale 6: time (5 bytes) + date (3 bytes) = 8 bytes total
	ASSERT_EQ(buffer.size(), 9u);
	ASSERT_EQ(buffer[0], 0x08);  // length = 8

	// Verify date portion (last 3 bytes)
	uint32_t encoded_date = buffer[6] | (buffer[7] << 8) | (buffer[8] << 16);
	ASSERT_EQ(encoded_date, 738899u);  // Same as date test

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: DateTimeOffset Encoding
//==============================================================================
void test_encode_datetimeoffset() {
	std::cout << "\n=== Test: EncodeDatetimeOffset ===" << std::endl;

	// Timestamp: 2024-01-15 14:30:00 with offset +05:30 (330 minutes)
	timestamp_t ts = Timestamp::FromDatetime(Date::FromDate(2024, 1, 15), Time::FromTime(14, 30, 0, 0));

	std::vector<uint8_t> buffer;
	BCPRowEncoder::EncodeDatetimeOffset(buffer, ts, 330, 6);

	// Scale 6: time (5 bytes) + date (3 bytes) + offset (2 bytes) = 10 bytes
	ASSERT_EQ(buffer.size(), 11u);
	ASSERT_EQ(buffer[0], 0x0A);  // length = 10

	// Verify offset (last 2 bytes): 330 = 0x014A
	int16_t encoded_offset = static_cast<int16_t>(buffer[9] | (buffer[10] << 8));
	ASSERT_EQ(encoded_offset, 330);

	// Test negative offset: -05:00 (-300 minutes)
	buffer.clear();
	BCPRowEncoder::EncodeDatetimeOffset(buffer, ts, -300, 6);
	encoded_offset = static_cast<int16_t>(buffer[9] | (buffer[10] << 8));
	ASSERT_EQ(encoded_offset, -300);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: NULL Encoding
//==============================================================================
void test_encode_null() {
	std::cout << "\n=== Test: NULL Encoding ===" << std::endl;

	// Fixed-length NULL
	std::vector<uint8_t> buffer;
	BCPRowEncoder::EncodeNullFixed(buffer);
	std::vector<uint8_t> expected = {0x00};
	ASSERT_BYTES_EQ(buffer, expected);

	// Variable-length NULL (USHORTLEN)
	buffer.clear();
	BCPRowEncoder::EncodeNullVariable(buffer);
	expected = {0xFF, 0xFF};
	ASSERT_BYTES_EQ(buffer, expected);

	// GUID NULL
	buffer.clear();
	BCPRowEncoder::EncodeNullGUID(buffer);
	expected = {0x00};
	ASSERT_BYTES_EQ(buffer, expected);

	// DateTime NULL
	buffer.clear();
	BCPRowEncoder::EncodeNullDateTime(buffer);
	expected = {0x00};
	ASSERT_BYTES_EQ(buffer, expected);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Multiple Values in Sequence (simulating a row)
//==============================================================================
void test_encode_multiple_values() {
	std::cout << "\n=== Test: Multiple Values in Sequence ===" << std::endl;

	std::vector<uint8_t> buffer;

	// Encode: int32(42), bool(true), varchar("hi"), NULL (fixed)
	BCPRowEncoder::EncodeInt32(buffer, 42);
	BCPRowEncoder::EncodeBit(buffer, true);
	BCPRowEncoder::EncodeNVarchar(buffer, string_t("hi"));
	BCPRowEncoder::EncodeNullFixed(buffer);

	// Expected sequence:
	// Int32: [04] [2A 00 00 00]
	// Bit:   [01] [01]
	// NVC:   [04 00] [68 00 69 00] ("hi" in UTF-16LE)
	// NULL:  [00]
	std::vector<uint8_t> expected = {
	    0x04, 0x2A, 0x00, 0x00, 0x00,  // int32(42)
	    0x01, 0x01,                    // bit(true)
	    0x04, 0x00, 0x68, 0x00, 0x69, 0x00,  // nvarchar("hi")
	    0x00                           // NULL
	};
	ASSERT_BYTES_EQ(buffer, expected);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Main
//==============================================================================
int main() {
	std::cout << "==========================================" << std::endl;
	std::cout << "BCPRowEncoder Unit Tests" << std::endl;
	std::cout << "==========================================" << std::endl;

	try {
		// Integer types
		test_encode_int8();
		test_encode_int16();
		test_encode_int32();
		test_encode_int64();
		test_encode_uint8();

		// Bit type
		test_encode_bit();

		// Float types
		test_encode_float();
		test_encode_double();

		// Decimal type
		test_encode_decimal();

		// String types
		test_encode_nvarchar();
		test_encode_binary();

		// GUID (mixed-endian)
		test_encode_guid();

		// Date/Time types
		test_encode_date();
		test_encode_time();
		test_encode_datetime2();
		test_encode_datetimeoffset();

		// NULL encoding
		test_encode_null();

		// Multiple values
		test_encode_multiple_values();

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

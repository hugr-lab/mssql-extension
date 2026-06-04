// test/cpp/codec/test_type_converter_fallback.cpp
// Unit test for the VARCHAR fallback in TypeConverter::ConvertValue
// (issue #89 fix — spec 045 Phase 6 sub-phase 3).
//
// Does NOT require a running SQL Server instance.
//
// Background — issue #89:
//   SQL Server VIEWs can project a column at a different type than
//   sys.columns reports (typically via CAST/CONVERT inside the view
//   definition). When that happens:
//     - The catalog allocates the result vector based on sys.columns
//       (VARCHAR, say).
//     - TDS returns the column data in the actual projected type
//       (DECIMAL, say).
//     - Pre-spec-045 the dispatcher crashed inside
//       FlatVector::GetData<hugeint_t>(varchar_vector) with the
//       assertion "Expected vector of type INT128, but found vector
//       of type VARCHAR".
//   Post-spec-045 the dispatcher detects the mismatch and renders
//   the value as a string via per-family RenderAsString helpers.
//
// Build & run:
//   GEN=ninja make debug
//   make test-codec-type_converter_fallback

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tds/encoding/type_converter.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_types.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using duckdb::LogicalType;
using duckdb::tds::ColumnMetadata;
using duckdb::tds::encoding::TypeConverter;

namespace {

int failures = 0;

#define CHECK_EQ(actual, expected)                                                                       \
	do {                                                                                                 \
		const auto &_a = (actual);                                                                       \
		const auto &_e = (expected);                                                                     \
		if (!(_a == _e)) {                                                                               \
			++failures;                                                                                  \
			std::cerr << "FAIL [" << __LINE__ << "] " #actual " == " #expected << "\n  actual:   " << _a \
					  << "\n  expected: " << _e << "\n";                                                 \
		}                                                                                                \
	} while (0)

ColumnMetadata MakeColumn(uint8_t type_id, uint8_t precision = 0, uint8_t scale = 0) {
	ColumnMetadata col{};
	col.name = "test_col";
	col.type_id = type_id;
	col.max_length = 0;
	col.precision = precision;
	col.scale = scale;
	col.collation = 0;
	col.flags = 0;
	return col;
}

std::string ReadVectorString(duckdb::Vector &vec, duckdb::idx_t row) {
	auto data = duckdb::FlatVector::GetData<duckdb::string_t>(vec);
	return data[row].GetString();
}

void TestDecimalIntoVarcharVector() {
	std::cout << "Test: DECIMAL TDS bytes into VARCHAR vector — no crash, renders fixed-point\n";

	// DECIMAL(19,4) wire layout for value 10.0000 (stored as hugeint 100000).
	// Sign byte 0x01 + LE mantissa 100000 (8 bytes).
	std::vector<uint8_t> wire = {0x01, 0xa0, 0x86, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_DECIMAL, /*precision*/ 19, /*scale*/ 4);

	duckdb::Vector vec(LogicalType::VARCHAR, 1);
	TypeConverter::ConvertValue(wire, /*is_null*/ false, col, vec, 0);

	CHECK_EQ(ReadVectorString(vec, 0), std::string("10.0000"));
}

void TestNumericIntoVarcharVector() {
	std::cout << "Test: NUMERIC TDS bytes into VARCHAR vector — same path as DECIMAL\n";

	std::vector<uint8_t> wire = {0x00, 0xa0, 0x86, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};	 // -10.0000
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_NUMERIC, /*precision*/ 19, /*scale*/ 4);

	duckdb::Vector vec(LogicalType::VARCHAR, 1);
	TypeConverter::ConvertValue(wire, /*is_null*/ false, col, vec, 0);

	CHECK_EQ(ReadVectorString(vec, 0), std::string("-10.0000"));
}

void TestIntegerIntoVarcharVector() {
	std::cout << "Test: INT TDS bytes into VARCHAR vector — renders bare digits\n";

	// INT (4 bytes LE) — value 12345.
	std::vector<uint8_t> wire = {0x39, 0x30, 0x00, 0x00};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_INTN);

	duckdb::Vector vec(LogicalType::VARCHAR, 1);
	TypeConverter::ConvertValue(wire, /*is_null*/ false, col, vec, 0);

	CHECK_EQ(ReadVectorString(vec, 0), std::string("12345"));
}

void TestBigIntIntoVarcharVector() {
	std::cout << "Test: BIGINT TDS bytes into VARCHAR vector\n";

	// BIGINT (8 bytes LE) — value 9876543210.
	int64_t v = 9876543210LL;
	std::vector<uint8_t> wire(8);
	std::memcpy(wire.data(), &v, 8);
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_BIGINT);

	duckdb::Vector vec(LogicalType::VARCHAR, 1);
	TypeConverter::ConvertValue(wire, /*is_null*/ false, col, vec, 0);

	CHECK_EQ(ReadVectorString(vec, 0), std::string("9876543210"));
}

void TestFloatIntoVarcharVector() {
	std::cout << "Test: REAL (float) TDS bytes into VARCHAR vector\n";

	float f = 3.14f;
	std::vector<uint8_t> wire(4);
	std::memcpy(wire.data(), &f, 4);
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_REAL);

	duckdb::Vector vec(LogicalType::VARCHAR, 1);
	TypeConverter::ConvertValue(wire, /*is_null*/ false, col, vec, 0);

	// setprecision(9) renders 3.14f as "3.1400001" (closest representable).
	auto rendered = ReadVectorString(vec, 0);
	if (rendered.substr(0, 3) != std::string("3.1")) {
		++failures;
		std::cerr << "FAIL: float fallback rendered '" << rendered << "', expected to start with '3.1'\n";
	}
}

void TestMoneyIntoVarcharVector() {
	std::cout << "Test: MONEY (8 bytes) TDS bytes into VARCHAR vector\n";

	// MONEY layout: high int32 (bytes 0-3), low int32 (bytes 4-7), value = high<<32 | low,
	// scaled by 10000. For value 1.5000 (= scaled 15000), high=0, low=15000.
	std::vector<uint8_t> wire = {0x00, 0x00, 0x00, 0x00, 0x98, 0x3a, 0x00, 0x00};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_MONEY);

	duckdb::Vector vec(LogicalType::VARCHAR, 1);
	TypeConverter::ConvertValue(wire, /*is_null*/ false, col, vec, 0);

	CHECK_EQ(ReadVectorString(vec, 0), std::string("1.5000"));
}

void TestBitIntoVarcharVector() {
	std::cout << "Test: BIT (boolean) TDS bytes into VARCHAR vector\n";

	std::vector<uint8_t> wire_t = {0x01};
	std::vector<uint8_t> wire_f = {0x00};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_BIT);

	duckdb::Vector vec(LogicalType::VARCHAR, 2);
	TypeConverter::ConvertValue(wire_t, /*is_null*/ false, col, vec, 0);
	TypeConverter::ConvertValue(wire_f, /*is_null*/ false, col, vec, 1);

	CHECK_EQ(ReadVectorString(vec, 0), std::string("1"));
	CHECK_EQ(ReadVectorString(vec, 1), std::string("0"));
}

void TestNullIntoVarcharVector() {
	std::cout << "Test: NULL TDS into VARCHAR vector — short-circuits to SetNull, no fallback\n";

	auto col = MakeColumn(duckdb::tds::TDS_TYPE_DECIMAL, 19, 4);
	std::vector<uint8_t> empty;
	duckdb::Vector vec(LogicalType::VARCHAR, 1);
	TypeConverter::ConvertValue(empty, /*is_null*/ true, col, vec, 0);

	if (!duckdb::FlatVector::IsNull(vec, 0)) {
		++failures;
		std::cerr << "FAIL: expected NULL bit set on vector after is_null=true\n";
	}
}

void TestUuidIntoVarcharVector() {
	std::cout << "Test: UNIQUEIDENTIFIER TDS bytes into VARCHAR vector — renders canonical lowercase form\n";

	// Wire (TDS mixed-endian) for 6ba7b810-9dad-11d1-80b4-00c04fd430c8 (RFC 4122 §3 example).
	std::vector<uint8_t> wire = {0x10, 0xB8, 0xA7, 0x6B, 0xAD, 0x9D, 0xD1, 0x11,
								 0x80, 0xB4, 0x00, 0xC0, 0x4F, 0xD4, 0x30, 0xC8};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_UNIQUEIDENTIFIER);

	duckdb::Vector vec(LogicalType::VARCHAR, 1);
	TypeConverter::ConvertValue(wire, /*is_null*/ false, col, vec, 0);

	CHECK_EQ(ReadVectorString(vec, 0), std::string("6ba7b810-9dad-11d1-80b4-00c04fd430c8"));
}

void TestStringIntoVarcharVectorTakesNormalPath() {
	std::cout << "Test: NVARCHAR TDS into VARCHAR vector — uses codec::string (not fallback)\n";

	// NVARCHAR wire: UTF-16LE bytes for "hi" (0x68, 0x00, 0x69, 0x00).
	std::vector<uint8_t> wire = {0x68, 0x00, 0x69, 0x00};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_NVARCHAR);

	duckdb::Vector vec(LogicalType::VARCHAR, 1);
	TypeConverter::ConvertValue(wire, /*is_null*/ false, col, vec, 0);

	CHECK_EQ(ReadVectorString(vec, 0), std::string("hi"));
}

}  // namespace

int main() {
	TestDecimalIntoVarcharVector();
	TestNumericIntoVarcharVector();
	TestIntegerIntoVarcharVector();
	TestBigIntIntoVarcharVector();
	TestFloatIntoVarcharVector();
	TestMoneyIntoVarcharVector();
	TestBitIntoVarcharVector();
	TestNullIntoVarcharVector();
	TestUuidIntoVarcharVector();
	TestStringIntoVarcharVectorTakesNormalPath();

	if (failures > 0) {
		std::cerr << "\n" << failures << " assertion(s) failed.\n";
		return 1;
	}
	std::cout << "\nAll TypeConverter VARCHAR-fallback assertions passed.\n";
	return 0;
}

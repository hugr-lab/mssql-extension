// test/cpp/codec/test_money_codec.cpp
// Unit tests for codec::money (spec 045, US3 sub-phase 4 — Phase 6).
//
// Does NOT require a running SQL Server instance.
//
// Money is scan-decode-only by design — only DecodeFromTds is exercised.
// The other 3 ops (EncodeToBcp, FormatSqlLiteral, FormatDdlTypeName) are
// declared in the header but intentionally undefined; values that
// originated as SQL Server MONEY/SMALLMONEY route through the Decimal
// codec for encode/literal/DDL because they live in DuckDB-land as
// DECIMAL(19,4) / DECIMAL(10,4).
//
// Covers:
//   - DecodeFromTds correctness for MONEY (8-byte hugeint storage):
//     zero, small positive, small negative (sign extension), 64-bit
//     extremes (INT64_MAX / INT64_MIN scaled), realistic million-row
//     values.
//   - DecodeFromTds correctness for SMALLMONEY (4-byte int64 storage):
//     zero, positive, negative, INT32_MAX / INT32_MIN scaled.
//   - Invalid wire length (other than 8 or 4) throws InvalidInputException.
//
// Build & run:
//   GEN=ninja make debug
//   make test-codec-money

#include "codec/money_codec.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_types.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using duckdb::hugeint_t;
using duckdb::LogicalType;
using duckdb::tds::ColumnMetadata;

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

#define CHECK_TRUE(expr)                                          \
	do {                                                          \
		if (!(expr)) {                                            \
			++failures;                                           \
			std::cerr << "FAIL [" << __LINE__ << "] " #expr "\n"; \
		}                                                         \
	} while (0)

ColumnMetadata MakeColumn(uint8_t type_id, uint8_t max_length) {
	ColumnMetadata col{};
	col.name = "money_col";
	col.type_id = type_id;
	col.max_length = max_length;
	col.precision = 0;
	col.scale = 4;
	col.collation = 0;
	col.flags = 0;
	return col;
}

hugeint_t ReadHugeint(duckdb::Vector &vec, duckdb::idx_t row) {
	return duckdb::FlatVector::GetData<hugeint_t>(vec)[row];
}

int64_t ReadInt64(duckdb::Vector &vec, duckdb::idx_t row) {
	return duckdb::FlatVector::GetData<int64_t>(vec)[row];
}

void TestDecodeMoneyZero() {
	std::cout << "Test: MONEY 0.0000 — 8 bytes of zero → hugeint 0\n";

	std::vector<uint8_t> wire = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_MONEY, 8);

	duckdb::Vector vec(LogicalType::DECIMAL(19, 4), 1);
	duckdb::mssql::codec::money::DecodeFromTds(wire, col, vec, 0);

	auto v = ReadHugeint(vec, 0);
	CHECK_EQ(v.upper, 0);
	CHECK_EQ(v.lower, 0u);
}

void TestDecodeMoneySmallPositive() {
	std::cout << "Test: MONEY 1.5000 — high=0, low=15000 → hugeint 15000\n";

	// 1.5000 scaled by 10000 = 15000 = 0x00003a98.
	std::vector<uint8_t> wire = {0x00, 0x00, 0x00, 0x00, 0x98, 0x3a, 0x00, 0x00};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_MONEY, 8);

	duckdb::Vector vec(LogicalType::DECIMAL(19, 4), 1);
	duckdb::mssql::codec::money::DecodeFromTds(wire, col, vec, 0);

	auto v = ReadHugeint(vec, 0);
	CHECK_EQ(v.upper, 0);
	CHECK_EQ(v.lower, 15000u);
}

void TestDecodeMoneyMillion() {
	std::cout << "Test: MONEY 100.0000 — high=0, low=1000000 → hugeint 1000000\n";

	std::vector<uint8_t> wire = {0x00, 0x00, 0x00, 0x00, 0x40, 0x42, 0x0f, 0x00};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_MONEY, 8);

	duckdb::Vector vec(LogicalType::DECIMAL(19, 4), 1);
	duckdb::mssql::codec::money::DecodeFromTds(wire, col, vec, 0);

	auto v = ReadHugeint(vec, 0);
	CHECK_EQ(v.upper, 0);
	CHECK_EQ(v.lower, 1000000u);
}

void TestDecodeMoneySmallNegative() {
	std::cout << "Test: MONEY -1.5000 — sign-extended; high=-1, low=0xFFFFC568 → hugeint -15000\n";

	// -15000 in int64 = 0xFFFFFFFFFFFFC568. Wire layout high then low (LE each):
	//   high = 0xFFFFFFFF → bytes ff ff ff ff
	//   low  = 0xFFFFC568 → bytes 68 c5 ff ff
	std::vector<uint8_t> wire = {0xff, 0xff, 0xff, 0xff, 0x68, 0xc5, 0xff, 0xff};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_MONEY, 8);

	duckdb::Vector vec(LogicalType::DECIMAL(19, 4), 1);
	duckdb::mssql::codec::money::DecodeFromTds(wire, col, vec, 0);

	auto v = ReadHugeint(vec, 0);
	// hugeint_t(-15000) constructed from int64 sign-extends to upper.
	hugeint_t expected(static_cast<int64_t>(-15000));
	CHECK_EQ(v.upper, expected.upper);
	CHECK_EQ(v.lower, expected.lower);
}

void TestDecodeMoneyInt64Max() {
	std::cout << "Test: MONEY INT64_MAX scaled — high=0x7FFFFFFF, low=0xFFFFFFFF\n";

	std::vector<uint8_t> wire = {0xff, 0xff, 0xff, 0x7f, 0xff, 0xff, 0xff, 0xff};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_MONEY, 8);

	duckdb::Vector vec(LogicalType::DECIMAL(19, 4), 1);
	duckdb::mssql::codec::money::DecodeFromTds(wire, col, vec, 0);

	auto v = ReadHugeint(vec, 0);
	hugeint_t expected(static_cast<int64_t>(INT64_MAX));
	CHECK_EQ(v.upper, expected.upper);
	CHECK_EQ(v.lower, expected.lower);
}

void TestDecodeMoneyInt64Min() {
	std::cout << "Test: MONEY INT64_MIN scaled — high=0x80000000, low=0\n";

	std::vector<uint8_t> wire = {0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_MONEY, 8);

	duckdb::Vector vec(LogicalType::DECIMAL(19, 4), 1);
	duckdb::mssql::codec::money::DecodeFromTds(wire, col, vec, 0);

	auto v = ReadHugeint(vec, 0);
	hugeint_t expected(static_cast<int64_t>(INT64_MIN));
	CHECK_EQ(v.upper, expected.upper);
	CHECK_EQ(v.lower, expected.lower);
}

void TestDecodeSmallMoneyZero() {
	std::cout << "Test: SMALLMONEY 0.0000 — 4 bytes of zero → int64 0\n";

	std::vector<uint8_t> wire = {0x00, 0x00, 0x00, 0x00};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_SMALLMONEY, 4);

	duckdb::Vector vec(LogicalType::DECIMAL(10, 4), 1);
	duckdb::mssql::codec::money::DecodeFromTds(wire, col, vec, 0);

	CHECK_EQ(ReadInt64(vec, 0), int64_t(0));
}

void TestDecodeSmallMoneyPositive() {
	std::cout << "Test: SMALLMONEY 1.5000 — int32 15000 LE → int64 15000\n";

	std::vector<uint8_t> wire = {0x98, 0x3a, 0x00, 0x00};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_SMALLMONEY, 4);

	duckdb::Vector vec(LogicalType::DECIMAL(10, 4), 1);
	duckdb::mssql::codec::money::DecodeFromTds(wire, col, vec, 0);

	CHECK_EQ(ReadInt64(vec, 0), int64_t(15000));
}

void TestDecodeSmallMoneyNegative() {
	std::cout << "Test: SMALLMONEY -1.5000 — int32 -15000 LE → int64 -15000\n";

	// -15000 as int32 LE = 0xFFFFC568 → bytes 68 c5 ff ff.
	std::vector<uint8_t> wire = {0x68, 0xc5, 0xff, 0xff};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_SMALLMONEY, 4);

	duckdb::Vector vec(LogicalType::DECIMAL(10, 4), 1);
	duckdb::mssql::codec::money::DecodeFromTds(wire, col, vec, 0);

	CHECK_EQ(ReadInt64(vec, 0), int64_t(-15000));
}

void TestDecodeSmallMoneyInt32Max() {
	std::cout << "Test: SMALLMONEY INT32_MAX scaled → int64 2147483647\n";

	std::vector<uint8_t> wire = {0xff, 0xff, 0xff, 0x7f};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_SMALLMONEY, 4);

	duckdb::Vector vec(LogicalType::DECIMAL(10, 4), 1);
	duckdb::mssql::codec::money::DecodeFromTds(wire, col, vec, 0);

	CHECK_EQ(ReadInt64(vec, 0), int64_t(INT32_MAX));
}

void TestDecodeSmallMoneyInt32Min() {
	std::cout << "Test: SMALLMONEY INT32_MIN scaled → int64 -2147483648\n";

	std::vector<uint8_t> wire = {0x00, 0x00, 0x00, 0x80};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_SMALLMONEY, 4);

	duckdb::Vector vec(LogicalType::DECIMAL(10, 4), 1);
	duckdb::mssql::codec::money::DecodeFromTds(wire, col, vec, 0);

	CHECK_EQ(ReadInt64(vec, 0), int64_t(INT32_MIN));
}

void TestDecodeMoneyNVariant8() {
	std::cout << "Test: MONEYN with 8-byte payload — same path as MONEY\n";

	std::vector<uint8_t> wire = {0x00, 0x00, 0x00, 0x00, 0x98, 0x3a, 0x00, 0x00};
	// MONEYN dispatch is on bytes.size(); column.max_length lets the caller
	// allocate the right vector type but the decoder itself only cares about
	// the byte count.
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_MONEYN, 8);

	duckdb::Vector vec(LogicalType::DECIMAL(19, 4), 1);
	duckdb::mssql::codec::money::DecodeFromTds(wire, col, vec, 0);

	auto v = ReadHugeint(vec, 0);
	CHECK_EQ(v.upper, 0);
	CHECK_EQ(v.lower, 15000u);
}

void TestDecodeMoneyNVariant4() {
	std::cout << "Test: MONEYN with 4-byte payload — same path as SMALLMONEY\n";

	std::vector<uint8_t> wire = {0x98, 0x3a, 0x00, 0x00};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_MONEYN, 4);

	duckdb::Vector vec(LogicalType::DECIMAL(10, 4), 1);
	duckdb::mssql::codec::money::DecodeFromTds(wire, col, vec, 0);

	CHECK_EQ(ReadInt64(vec, 0), int64_t(15000));
}

void TestDecodeMoneyInvalidLength() {
	std::cout << "Test: invalid wire length (5 bytes) throws InvalidInputException\n";

	std::vector<uint8_t> wire = {0x01, 0x02, 0x03, 0x04, 0x05};
	auto col = MakeColumn(duckdb::tds::TDS_TYPE_MONEY, 8);

	duckdb::Vector vec(LogicalType::DECIMAL(19, 4), 1);
	bool threw = false;
	try {
		duckdb::mssql::codec::money::DecodeFromTds(wire, col, vec, 0);
	} catch (const duckdb::InvalidInputException &) {
		threw = true;
	}
	CHECK_TRUE(threw);
}

}  // namespace

int main() {
	TestDecodeMoneyZero();
	TestDecodeMoneySmallPositive();
	TestDecodeMoneyMillion();
	TestDecodeMoneySmallNegative();
	TestDecodeMoneyInt64Max();
	TestDecodeMoneyInt64Min();
	TestDecodeSmallMoneyZero();
	TestDecodeSmallMoneyPositive();
	TestDecodeSmallMoneyNegative();
	TestDecodeSmallMoneyInt32Max();
	TestDecodeSmallMoneyInt32Min();
	TestDecodeMoneyNVariant8();
	TestDecodeMoneyNVariant4();
	TestDecodeMoneyInvalidLength();

	if (failures > 0) {
		std::cerr << "\n" << failures << " assertion(s) failed.\n";
		return 1;
	}
	std::cout << "\nAll Money codec assertions passed.\n";
	return 0;
}

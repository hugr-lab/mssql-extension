// test/cpp/codec/test_integer_codec.cpp
// Unit tests for codec::integer (spec 045, US1 Integer MVP — Phase 3).
//
// Does NOT require a running SQL Server instance.
//
// Covers:
//   - FormatSqlLiteral byte-identity across LiteralContext::Filter and
//     LiteralContext::InsertValues for all 9 Integer-family types (FR-020 (b)).
//   - FormatDdlTypeName byte-identity across DdlContext::CreateTable and
//     DdlContext::CtasCreateTable (FR-025 / FR-028 — DDL unification).
//   - EstimateLiteralSize sanity: returned upper bound is at least as large
//     as a worst-case rendered literal.
//   - EncodeToBcp HUGEINT/UHUGEINT as DECIMAL(38,0) — issue #177: round-trip
//     through the wire layout ([17][sign][16-byte LE mantissa]) for 0, ±1,
//     int64 edges, ±(10^38-1); InvalidInputException for 39-digit values
//     (10^38 and above) in both the Vector and Value overloads.
//
// Build & run:
//   GEN=ninja make debug
//   make test-codec-integer

#include "codec/integer_codec.hpp"
#include "codec/literal_context.hpp"
#include "codec/type_family.hpp"
#include "copy/target_resolver.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

using duckdb::hugeint_t;
using duckdb::HugeIntValue;
using duckdb::LogicalType;
using duckdb::uhugeint_t;
using duckdb::Value;
using duckdb::mssql::codec::DdlContext;
using duckdb::mssql::codec::LiteralContext;

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

void TestFormatSqlLiteralByteIdentity() {
	std::cout << "Test: FormatSqlLiteral Filter == InsertValues byte-identity\n";

	const std::pair<Value, LogicalType> samples[] = {
		{Value::TINYINT(-128), LogicalType::TINYINT},
		{Value::TINYINT(127), LogicalType::TINYINT},
		{Value::SMALLINT(-32768), LogicalType::SMALLINT},
		{Value::SMALLINT(32767), LogicalType::SMALLINT},
		{Value::INTEGER(-2147483648), LogicalType::INTEGER},
		{Value::INTEGER(2147483647), LogicalType::INTEGER},
		{Value::BIGINT(std::numeric_limits<int64_t>::min()), LogicalType::BIGINT},
		{Value::BIGINT(std::numeric_limits<int64_t>::max()), LogicalType::BIGINT},
		{Value::UTINYINT(255), LogicalType::UTINYINT},
		{Value::USMALLINT(65535), LogicalType::USMALLINT},
		{Value::UINTEGER(4294967295u), LogicalType::UINTEGER},
		{Value::UBIGINT(0), LogicalType::UBIGINT},
		{Value::UBIGINT(std::numeric_limits<uint64_t>::max()), LogicalType::UBIGINT},
		{Value::HUGEINT(hugeint_t(0, 1)), LogicalType::HUGEINT},
		{Value::HUGEINT(hugeint_t(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL)), LogicalType::HUGEINT},
	};

	for (const auto &sample : samples) {
		auto filter =
			duckdb::mssql::codec::integer::FormatSqlLiteral(sample.first, sample.second, LiteralContext::Filter);
		auto insert =
			duckdb::mssql::codec::integer::FormatSqlLiteral(sample.first, sample.second, LiteralContext::InsertValues);
		CHECK_EQ(filter, insert);
	}
}

void TestFormatSqlLiteralHugeIntFix() {
	std::cout << "Test: HUGEINT literal renders as decimal digits (FR-020 (b))\n";

	auto small = duckdb::mssql::codec::integer::FormatSqlLiteral(Value::HUGEINT(hugeint_t(0, 42)), LogicalType::HUGEINT,
																 LiteralContext::Filter);
	CHECK_EQ(small, std::string("42"));

	auto neg = duckdb::mssql::codec::integer::FormatSqlLiteral(
		Value::HUGEINT(duckdb::Hugeint::Negate(hugeint_t(0, 100))), LogicalType::HUGEINT, LiteralContext::Filter);
	CHECK_EQ(neg, std::string("-100"));
}

void TestFormatSqlLiteralUBigInt() {
	std::cout << "Test: UBIGINT literal uses CAST(... AS DECIMAL(20,0)) when > INT64_MAX\n";

	auto small = duckdb::mssql::codec::integer::FormatSqlLiteral(Value::UBIGINT(100), LogicalType::UBIGINT,
																 LiteralContext::Filter);
	CHECK_EQ(small, std::string("100"));

	auto large = duckdb::mssql::codec::integer::FormatSqlLiteral(Value::UBIGINT(static_cast<uint64_t>(INT64_MAX) + 1),
																 LogicalType::UBIGINT, LiteralContext::InsertValues);
	CHECK_EQ(large, std::string("CAST(9223372036854775808 AS DECIMAL(20,0))"));
}

void TestFormatDdlTypeNameByteIdentity() {
	std::cout << "Test: FormatDdlTypeName CreateTable == CtasCreateTable byte-identity (FR-025/FR-028)\n";

	const LogicalType types[] = {
		LogicalType::TINYINT,  LogicalType::SMALLINT,  LogicalType::INTEGER,  LogicalType::BIGINT,
		LogicalType::UTINYINT, LogicalType::USMALLINT, LogicalType::UINTEGER, LogicalType::UBIGINT,
		LogicalType::HUGEINT,  LogicalType::UHUGEINT,
	};

	duckdb::mssql::CTASConfig cfg;
	for (const auto &t : types) {
		auto create = duckdb::mssql::codec::integer::FormatDdlTypeName(t, cfg, DdlContext::CreateTable);
		auto ctas = duckdb::mssql::codec::integer::FormatDdlTypeName(t, cfg, DdlContext::CtasCreateTable);
		CHECK_EQ(create, ctas);
	}
}

void TestFormatDdlTypeNameExpectedShapes() {
	std::cout << "Test: FormatDdlTypeName returns SQL Server-canonical type names\n";

	duckdb::mssql::CTASConfig cfg;
	CHECK_EQ(duckdb::mssql::codec::integer::FormatDdlTypeName(LogicalType::TINYINT, cfg, DdlContext::CreateTable),
			 std::string("TINYINT"));
	CHECK_EQ(duckdb::mssql::codec::integer::FormatDdlTypeName(LogicalType::UTINYINT, cfg, DdlContext::CreateTable),
			 std::string("TINYINT"));
	CHECK_EQ(duckdb::mssql::codec::integer::FormatDdlTypeName(LogicalType::SMALLINT, cfg, DdlContext::CtasCreateTable),
			 std::string("SMALLINT"));
	CHECK_EQ(duckdb::mssql::codec::integer::FormatDdlTypeName(LogicalType::USMALLINT, cfg, DdlContext::CtasCreateTable),
			 std::string("INT"));
	CHECK_EQ(duckdb::mssql::codec::integer::FormatDdlTypeName(LogicalType::INTEGER, cfg, DdlContext::CreateTable),
			 std::string("INT"));
	CHECK_EQ(duckdb::mssql::codec::integer::FormatDdlTypeName(LogicalType::UINTEGER, cfg, DdlContext::CreateTable),
			 std::string("BIGINT"));
	CHECK_EQ(duckdb::mssql::codec::integer::FormatDdlTypeName(LogicalType::BIGINT, cfg, DdlContext::CtasCreateTable),
			 std::string("BIGINT"));
	CHECK_EQ(duckdb::mssql::codec::integer::FormatDdlTypeName(LogicalType::UBIGINT, cfg, DdlContext::CreateTable),
			 std::string("DECIMAL(20,0)"));
	CHECK_EQ(duckdb::mssql::codec::integer::FormatDdlTypeName(LogicalType::HUGEINT, cfg, DdlContext::CreateTable),
			 std::string("DECIMAL(38,0)"));
	CHECK_EQ(duckdb::mssql::codec::integer::FormatDdlTypeName(LogicalType::HUGEINT, cfg, DdlContext::CtasCreateTable),
			 std::string("DECIMAL(38,0)"));
	CHECK_EQ(duckdb::mssql::codec::integer::FormatDdlTypeName(LogicalType::UHUGEINT, cfg, DdlContext::CtasCreateTable),
			 std::string("DECIMAL(38,0)"));
}

void TestEstimateLiteralSizeUpperBound() {
	std::cout << "Test: EstimateLiteralSize is a true upper bound on rendered length\n";

	const std::pair<Value, LogicalType> samples[] = {
		{Value::TINYINT(-128), LogicalType::TINYINT},
		{Value::SMALLINT(-32768), LogicalType::SMALLINT},
		{Value::INTEGER(-2147483648), LogicalType::INTEGER},
		{Value::BIGINT(std::numeric_limits<int64_t>::min()), LogicalType::BIGINT},
		{Value::UBIGINT(std::numeric_limits<uint64_t>::max()), LogicalType::UBIGINT},
		{Value::HUGEINT(hugeint_t(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL)), LogicalType::HUGEINT},
	};

	for (const auto &s : samples) {
		auto literal = duckdb::mssql::codec::integer::FormatSqlLiteral(s.first, s.second, LiteralContext::InsertValues);
		auto bound = duckdb::mssql::codec::integer::EstimateLiteralSize(s.second);
		if (literal.size() > bound) {
			++failures;
			std::cerr << "FAIL upper bound: type=" << s.second.ToString() << " literal=" << literal
					  << " size=" << literal.size() << " bound=" << bound << "\n";
		}
	}
}

void TestNullLiteral() {
	std::cout << "Test: NULL Value renders as \"NULL\"\n";

	auto null_lit = duckdb::mssql::codec::integer::FormatSqlLiteral(Value(LogicalType::INTEGER), LogicalType::INTEGER,
																	LiteralContext::Filter);
	CHECK_EQ(null_lit, std::string("NULL"));
}

//===----------------------------------------------------------------------===//
// EncodeToBcp HUGEINT/UHUGEINT — issue #177
//===----------------------------------------------------------------------===//

#define CHECK_TRUE(expr)                                          \
	do {                                                          \
		if (!(expr)) {                                            \
			++failures;                                           \
			std::cerr << "FAIL [" << __LINE__ << "] " #expr "\n"; \
		}                                                         \
	} while (0)

duckdb::mssql::BCPColumnMetadata MakeDecimal38Col(const LogicalType &type) {
	duckdb::mssql::BCPColumnMetadata col;
	col.name = "total";
	col.duckdb_type = type;
	col.precision = 38;
	col.scale = 0;
	col.max_length = 17;  // DECIMAL(38,0) storage — mirrors GenerateColumnMetadata
	return col;
}

// Wire layout produced by BCPRowEncoder::EncodeDecimal for precision 38:
// [byte_size=17][sign: 0x01 non-negative / 0x00 negative][16-byte LE mantissa].
bool DecodeDecimal38Wire(const duckdb::vector<uint8_t> &buf, hugeint_t &out_abs, bool &out_negative) {
	if (buf.size() != 18 || buf[0] != 17) {
		return false;
	}
	out_negative = buf[1] == 0x00;
	uint64_t lower = 0, upper = 0;
	for (int i = 0; i < 8; ++i) {
		lower |= static_cast<uint64_t>(buf[2 + i]) << (8 * i);
		upper |= static_cast<uint64_t>(buf[10 + i]) << (8 * i);
	}
	out_abs = hugeint_t(static_cast<int64_t>(upper), lower);
	return true;
}

void TestEncodeToBcpHugeintRoundTrip() {
	std::cout << "Test: EncodeToBcp HUGEINT -> DECIMAL(38,0) wire round-trip (#177)\n";

	const hugeint_t max38 = duckdb::Hugeint::POWERS_OF_TEN[38] - 1;
	const hugeint_t samples[] = {
		hugeint_t(0),
		hugeint_t(1),
		hugeint_t(-1),
		hugeint_t(std::numeric_limits<int64_t>::max()),
		hugeint_t(std::numeric_limits<int64_t>::min()),
		max38,
		duckdb::Hugeint::Negate(max38),
	};
	auto col = MakeDecimal38Col(LogicalType::HUGEINT);

	for (const auto &v : samples) {
		bool expect_negative = v.upper < 0;
		hugeint_t expect_abs = expect_negative ? duckdb::Hugeint::Negate(v) : v;

		// Value overload.
		duckdb::vector<uint8_t> buf;
		duckdb::mssql::codec::integer::EncodeToBcp(Value::HUGEINT(v), col, buf);
		hugeint_t abs;
		bool negative;
		CHECK_TRUE(DecodeDecimal38Wire(buf, abs, negative));
		CHECK_EQ(duckdb::Hugeint::ToString(abs), duckdb::Hugeint::ToString(expect_abs));
		// Zero encodes with the non-negative sign byte.
		CHECK_EQ(negative, expect_negative && !(v == hugeint_t(0)));

		// Vector overload must produce identical bytes.
		duckdb::Vector vec(LogicalType::HUGEINT);
		vec.SetValue(0, Value::HUGEINT(v));
		duckdb::vector<uint8_t> vec_buf;
		duckdb::mssql::codec::integer::EncodeToBcp(vec, 0, col, vec_buf);
		CHECK_TRUE(vec_buf == buf);
	}
}

void TestEncodeToBcpHugeintRangeGuard() {
	std::cout << "Test: EncodeToBcp HUGEINT 39-digit values raise InvalidInputException (#177)\n";

	const hugeint_t pow38 = duckdb::Hugeint::POWERS_OF_TEN[38];	 // 10^38 — first 39-digit value
	const hugeint_t int128_max(std::numeric_limits<int64_t>::max(), std::numeric_limits<uint64_t>::max());
	const hugeint_t out_of_range[] = {pow38, duckdb::Hugeint::Negate(pow38), int128_max,
									  duckdb::Hugeint::Negate(int128_max)};
	auto col = MakeDecimal38Col(LogicalType::HUGEINT);

	for (const auto &v : out_of_range) {
		duckdb::vector<uint8_t> buf;
		bool threw = false;
		try {
			duckdb::mssql::codec::integer::EncodeToBcp(Value::HUGEINT(v), col, buf);
		} catch (const duckdb::InvalidInputException &ex) {
			threw = true;
			std::string msg(ex.what());
			CHECK_TRUE(msg.find("out of range for DECIMAL(38,0)") != std::string::npos);
			CHECK_TRUE(msg.find("total") != std::string::npos);	 // names the column
		}
		CHECK_TRUE(threw);
		CHECK_TRUE(buf.empty());  // nothing written before the guard fired
	}
}

void TestEncodeToBcpUhugeint() {
	std::cout << "Test: EncodeToBcp UHUGEINT edges (#177)\n";

	auto col = MakeDecimal38Col(LogicalType::UHUGEINT);
	const uhugeint_t max38 = duckdb::Uhugeint::POWERS_OF_TEN[38] - 1;

	// In-range: 0, 1, 10^38-1 round-trip with the non-negative sign byte.
	const uhugeint_t ok[] = {uhugeint_t(0), uhugeint_t(1), max38};
	for (const auto &v : ok) {
		duckdb::vector<uint8_t> buf;
		duckdb::mssql::codec::integer::EncodeToBcp(Value::UHUGEINT(v), col, buf);
		hugeint_t abs;
		bool negative;
		CHECK_TRUE(DecodeDecimal38Wire(buf, abs, negative));
		CHECK_EQ(duckdb::Hugeint::ToString(abs), duckdb::Uhugeint::ToString(v));
		CHECK_TRUE(!negative);

		duckdb::Vector vec(LogicalType::UHUGEINT);
		vec.SetValue(0, Value::UHUGEINT(v));
		duckdb::vector<uint8_t> vec_buf;
		duckdb::mssql::codec::integer::EncodeToBcp(vec, 0, col, vec_buf);
		CHECK_TRUE(vec_buf == buf);
	}

	// Out of range: 10^38 and uint128 max.
	const uhugeint_t bad[] = {duckdb::Uhugeint::POWERS_OF_TEN[38],
							  uhugeint_t(std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max())};
	for (const auto &v : bad) {
		duckdb::vector<uint8_t> buf;
		bool threw = false;
		try {
			duckdb::mssql::codec::integer::EncodeToBcp(Value::UHUGEINT(v), col, buf);
		} catch (const duckdb::InvalidInputException &ex) {
			threw = true;
			CHECK_TRUE(std::string(ex.what()).find("out of range for DECIMAL(38,0)") != std::string::npos);
		}
		CHECK_TRUE(threw);
	}
}

}  // namespace

int main() {
	TestFormatSqlLiteralByteIdentity();
	TestFormatSqlLiteralHugeIntFix();
	TestFormatSqlLiteralUBigInt();
	TestFormatDdlTypeNameByteIdentity();
	TestFormatDdlTypeNameExpectedShapes();
	TestEstimateLiteralSizeUpperBound();
	TestNullLiteral();
	TestEncodeToBcpHugeintRoundTrip();
	TestEncodeToBcpHugeintRangeGuard();
	TestEncodeToBcpUhugeint();

	if (failures > 0) {
		std::cerr << "\n" << failures << " assertion(s) failed.\n";
		return 1;
	}
	std::cout << "\nAll codec::integer assertions passed.\n";
	return 0;
}

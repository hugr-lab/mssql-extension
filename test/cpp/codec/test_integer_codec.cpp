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
//
// Build & run:
//   GEN=ninja make debug
//   make test-codec-integer

#include "codec/integer_codec.hpp"
#include "codec/literal_context.hpp"
#include "codec/type_family.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/value.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

using duckdb::hugeint_t;
using duckdb::HugeIntValue;
using duckdb::LogicalType;
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

}  // namespace

int main() {
	TestFormatSqlLiteralByteIdentity();
	TestFormatSqlLiteralHugeIntFix();
	TestFormatSqlLiteralUBigInt();
	TestFormatDdlTypeNameByteIdentity();
	TestFormatDdlTypeNameExpectedShapes();
	TestEstimateLiteralSizeUpperBound();
	TestNullLiteral();

	if (failures > 0) {
		std::cerr << "\n" << failures << " assertion(s) failed.\n";
		return 1;
	}
	std::cout << "\nAll codec::integer assertions passed.\n";
	return 0;
}

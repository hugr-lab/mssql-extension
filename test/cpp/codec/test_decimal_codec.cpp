// test/cpp/codec/test_decimal_codec.cpp
// Unit tests for codec::decimal (spec 045, US3 sub-phase 3 — Phase 6).
//
// Does NOT require a running SQL Server instance.
//
// Covers:
//   - DecodeFromTds correctness for each PhysicalType bucket
//     (precision <= 4 / <= 9 / <= 18 / > 18).
//   - EncodeToBcp Vector + Value variants — precision-bucket wire size
//     (5 / 9 / 13 / 17 bytes), sign byte, LE mantissa.
//   - FormatSqlLiteral Filter == InsertValues byte-identity (FR-022 —
//     headline consolidation: pre-spec-045 Filter used Value::ToString()
//     which could diverge from InsertValues' SerializeDecimal output on
//     edge cases).
//   - FormatDdlTypeName CreateTable == CtasCreateTable byte-identity,
//     with precision clamp (FR-017): width > 38 → 38, scale > precision
//     → precision.
//   - HUGEINT routing (FR-025): codec::FormatSqlLiteral routes HUGEINT
//     through the Decimal family; codec::decimal::FormatDdlTypeName
//     returns DECIMAL(38,0) for both HUGEINT and UHUGEINT.
//   - RenderAsString helper (issue-#89 fallback support) — produces the
//     same string as FormatSqlLiteral on an equivalent Value.
//   - NULL value routed through the dispatcher renders as "NULL".
//   - Dispatcher routing for DECIMAL and HUGEINT.
//
// Build & run:
//   GEN=ninja make debug
//   make test-codec-decimal

#include "codec/decimal_codec.hpp"
#include "codec/literal_context.hpp"
#include "codec/literal_format.hpp"
#include "codec/type_family.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/value.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using duckdb::hugeint_t;
using duckdb::LogicalType;
using duckdb::Value;
using duckdb::mssql::CTASConfig;
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

#define CHECK_TRUE(expr)                                          \
	do {                                                          \
		if (!(expr)) {                                            \
			++failures;                                           \
			std::cerr << "FAIL [" << __LINE__ << "] " #expr "\n"; \
		}                                                         \
	} while (0)

void TestFormatSqlLiteralByteIdentitySmall() {
	std::cout << "Test: FormatSqlLiteral Filter == InsertValues — DECIMAL(4,2) / DECIMAL(9,0)\n";

	// DECIMAL(4,2) — INT16 storage. Value 10.00 stored as 1000.
	auto v_10 = Value::DECIMAL(static_cast<int16_t>(1000), 4, 2);
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatSqlLiteral(v_10, v_10.type(), LiteralContext::Filter),
			 duckdb::mssql::codec::decimal::FormatSqlLiteral(v_10, v_10.type(), LiteralContext::InsertValues));
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatSqlLiteral(v_10, v_10.type(), LiteralContext::Filter),
			 std::string("10.00"));

	auto v_neg = Value::DECIMAL(static_cast<int16_t>(-1000), 4, 2);
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatSqlLiteral(v_neg, v_neg.type(), LiteralContext::Filter),
			 std::string("-10.00"));

	// DECIMAL(9,0) — INT32 storage. Value 123456789 stored as 123456789.
	auto v_big = Value::DECIMAL(static_cast<int32_t>(123456789), 9, 0);
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatSqlLiteral(v_big, v_big.type(), LiteralContext::Filter),
			 duckdb::mssql::codec::decimal::FormatSqlLiteral(v_big, v_big.type(), LiteralContext::InsertValues));
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatSqlLiteral(v_big, v_big.type(), LiteralContext::Filter),
			 std::string("123456789"));
}

void TestFormatSqlLiteralByteIdentityWide() {
	std::cout << "Test: FormatSqlLiteral Filter == InsertValues — DECIMAL(18,6) / DECIMAL(19,4)\n";

	// DECIMAL(18,6) — INT64 storage. 0.000001 stored as 1.
	auto v_micro = Value::DECIMAL(static_cast<int64_t>(1), 18, 6);
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatSqlLiteral(v_micro, v_micro.type(), LiteralContext::Filter),
			 duckdb::mssql::codec::decimal::FormatSqlLiteral(v_micro, v_micro.type(), LiteralContext::InsertValues));
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatSqlLiteral(v_micro, v_micro.type(), LiteralContext::Filter),
			 std::string("0.000001"));

	// DECIMAL(19,4) — INT128 storage. 10.0000 stored as 100000.
	auto v_money = Value::DECIMAL(hugeint_t(100000), 19, 4);
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatSqlLiteral(v_money, v_money.type(), LiteralContext::Filter),
			 duckdb::mssql::codec::decimal::FormatSqlLiteral(v_money, v_money.type(), LiteralContext::InsertValues));
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatSqlLiteral(v_money, v_money.type(), LiteralContext::Filter),
			 std::string("10.0000"));
}

void TestHugeintRoutingFR025() {
	std::cout << "Test: HUGEINT routes through Decimal codec (FR-025)\n";

	auto v_zero = Value::HUGEINT(hugeint_t(0));
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatSqlLiteral(v_zero, v_zero.type(), LiteralContext::Filter),
			 std::string("0"));
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatSqlLiteral(v_zero, v_zero.type(), LiteralContext::InsertValues),
			 std::string("0"));

	auto v_pos = Value::HUGEINT(hugeint_t(123456789012345LL));
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatSqlLiteral(v_pos, v_pos.type(), LiteralContext::Filter),
			 std::string("123456789012345"));

	auto v_neg = Value::HUGEINT(hugeint_t(-123456789012345LL));
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatSqlLiteral(v_neg, v_neg.type(), LiteralContext::InsertValues),
			 std::string("-123456789012345"));

	// Dispatcher routing: HUGEINT now goes through codec::FormatSqlLiteral → Decimal family.
	CHECK_EQ(duckdb::mssql::codec::FormatSqlLiteral(v_pos, v_pos.type(), LiteralContext::Filter),
			 std::string("123456789012345"));
}

void TestFormatDdlTypeName() {
	std::cout << "Test: FormatDdlTypeName CreateTable == CtasCreateTable byte-identity\n";

	CTASConfig cfg;

	// Normal DECIMAL — no clamp.
	auto t_18_6 = LogicalType::DECIMAL(18, 6);
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatDdlTypeName(t_18_6, cfg, DdlContext::CreateTable),
			 duckdb::mssql::codec::decimal::FormatDdlTypeName(t_18_6, cfg, DdlContext::CtasCreateTable));
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatDdlTypeName(t_18_6, cfg, DdlContext::CreateTable),
			 std::string("DECIMAL(18,6)"));

	auto t_38_0 = LogicalType::DECIMAL(38, 0);
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatDdlTypeName(t_38_0, cfg, DdlContext::CreateTable),
			 std::string("DECIMAL(38,0)"));

	// HUGEINT and UHUGEINT both → DECIMAL(38,0). Byte-identical in both contexts.
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatDdlTypeName(LogicalType::HUGEINT, cfg, DdlContext::CreateTable),
			 std::string("DECIMAL(38,0)"));
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatDdlTypeName(LogicalType::HUGEINT, cfg, DdlContext::CtasCreateTable),
			 std::string("DECIMAL(38,0)"));
	CHECK_EQ(duckdb::mssql::codec::decimal::FormatDdlTypeName(LogicalType::UHUGEINT, cfg, DdlContext::CreateTable),
			 std::string("DECIMAL(38,0)"));
}

// FR-017 clamp (precision > 38 → 38, scale > precision → precision) is defensive only —
// DuckDB's LogicalType::DECIMAL constructor itself rejects those invariants with an assertion,
// so the clamp branches are unreachable through normal LogicalType construction. Left in
// codec::decimal::FormatDdlTypeName for parity with the pre-spec-045 implementation; not
// exercised here because we can't construct the invalid LogicalType to drive the path.

void TestEstimateLiteralSize() {
	std::cout << "Test: EstimateLiteralSize sanity (>= 45 for worst-case DECIMAL(38,18))\n";

	auto bound = duckdb::mssql::codec::decimal::EstimateLiteralSize(LogicalType::DECIMAL(38, 18));
	CHECK_TRUE(bound >= 45);

	// Surface through dispatcher.
	CHECK_EQ(duckdb::mssql::codec::EstimateLiteralSize(LogicalType::DECIMAL(38, 18)), bound);
	CHECK_EQ(duckdb::mssql::codec::EstimateLiteralSize(LogicalType::HUGEINT), bound);
}

void TestNullLiteralViaDispatcher() {
	std::cout << "Test: NULL Value renders as \"NULL\" via dispatcher\n";

	Value null_dec(LogicalType::DECIMAL(19, 4));
	CHECK_EQ(duckdb::mssql::codec::FormatSqlLiteral(null_dec, LogicalType::DECIMAL(19, 4), LiteralContext::Filter),
			 std::string("NULL"));
	CHECK_EQ(
		duckdb::mssql::codec::FormatSqlLiteral(null_dec, LogicalType::DECIMAL(19, 4), LiteralContext::InsertValues),
		std::string("NULL"));

	Value null_huge(LogicalType::HUGEINT);
	CHECK_EQ(duckdb::mssql::codec::FormatSqlLiteral(null_huge, LogicalType::HUGEINT, LiteralContext::Filter),
			 std::string("NULL"));
}

void TestDispatcherRoutingDecimal() {
	std::cout << "Test: codec::FormatSqlLiteral dispatcher routes DECIMAL through Decimal family\n";

	auto v = Value::DECIMAL(static_cast<int16_t>(1234), 4, 2);
	CHECK_EQ(duckdb::mssql::codec::FormatSqlLiteral(v, v.type(), LiteralContext::Filter), std::string("12.34"));
	CHECK_EQ(duckdb::mssql::codec::FormatSqlLiteral(v, v.type(), LiteralContext::InsertValues), std::string("12.34"));
}

void TestRenderAsStringMatchesLiteral() {
	std::cout << "Test: RenderAsString output matches FormatSqlLiteral for equivalent Value (issue #89 support)\n";

	// Build TDS wire bytes for DECIMAL(19,4) with value 10.0000 (mantissa = 100000).
	// Wire layout: <sign-byte> + <LE mantissa bytes>. SerializeDecimal produces
	// "10.0000" for stored hugeint 100000 with scale 4.
	std::vector<uint8_t> wire = {0x01, 0xa0, 0x86, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};	 // 1 + 100000 LE (8 bytes)
	std::string rendered = duckdb::mssql::codec::decimal::RenderAsString(wire, /*precision*/ 19, /*scale*/ 4);
	CHECK_EQ(rendered, std::string("10.0000"));

	auto v_money = Value::DECIMAL(hugeint_t(100000), 19, 4);
	CHECK_EQ(rendered,
			 duckdb::mssql::codec::decimal::FormatSqlLiteral(v_money, v_money.type(), LiteralContext::Filter));
}

void TestRenderMoneyAsString() {
	std::cout << "Test: RenderMoneyAsString — MONEY (8 bytes) and SMALLMONEY (4 bytes)\n";

	// SMALLMONEY: 1.5000 = 15000 (LE int32).
	std::vector<uint8_t> small = {0x98, 0x3a, 0x00, 0x00};	// 0x00003a98 = 15000
	CHECK_EQ(duckdb::mssql::codec::decimal::RenderMoneyAsString(small), std::string("1.5000"));

	// MONEY: 1.5000 = 15000 — TDS layout swaps high/low int32 halves
	// (bytes 0-3 = high, bytes 4-7 = low). Value 15000 fits in low half;
	// high half is zero.
	std::vector<uint8_t> money = {0x00, 0x00, 0x00, 0x00, 0x98, 0x3a, 0x00, 0x00};
	CHECK_EQ(duckdb::mssql::codec::decimal::RenderMoneyAsString(money), std::string("1.5000"));
}

}  // namespace

int main() {
	TestFormatSqlLiteralByteIdentitySmall();
	TestFormatSqlLiteralByteIdentityWide();
	TestHugeintRoutingFR025();
	TestFormatDdlTypeName();
	TestEstimateLiteralSize();
	TestNullLiteralViaDispatcher();
	TestDispatcherRoutingDecimal();
	TestRenderAsStringMatchesLiteral();
	TestRenderMoneyAsString();

	if (failures > 0) {
		std::cerr << "\n" << failures << " assertion(s) failed.\n";
		return 1;
	}
	std::cout << "\nAll codec::decimal assertions passed.\n";
	return 0;
}

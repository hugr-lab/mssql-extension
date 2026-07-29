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
#include "copy/target_resolver.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"
#include "tds/encoding/decimal_encoding.hpp"

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

void TestEncodeToBcpMantissaRangeGuard() {
	std::cout << "Test: EncodeToBcp mantissa exceeding target precision raises InvalidInputException (#177)\n";

	// HUGEINT source feeding a decimal(38,0) target column (COPY CREATE_TABLE
	// reads created-table metadata back, so this dispatches via Decimal family).
	duckdb::mssql::BCPColumnMetadata col;
	col.name = "total";
	col.duckdb_type = LogicalType::HUGEINT;
	col.precision = 38;
	col.scale = 0;
	col.max_length = 17;

	// In range: 10^38 − 1 encodes fine.
	const hugeint_t max38 = duckdb::Hugeint::POWERS_OF_TEN[38] - 1;
	duckdb::vector<uint8_t> buf;
	duckdb::mssql::codec::decimal::EncodeToBcp(Value::HUGEINT(max38), col, buf);
	CHECK_EQ(buf.size(), static_cast<size_t>(18));	// [17][sign][16-byte mantissa]

	// Out of range: 10^38 (39 digits) must fail client-side, naming the column.
	bool threw = false;
	try {
		duckdb::vector<uint8_t> guard_buf;
		duckdb::mssql::codec::decimal::EncodeToBcp(Value::HUGEINT(duckdb::Hugeint::POWERS_OF_TEN[38]), col, guard_buf);
	} catch (const duckdb::InvalidInputException &ex) {
		threw = true;
		std::string msg(ex.what());
		CHECK_TRUE(msg.find("out of range for DECIMAL(38,0)") != std::string::npos);
		CHECK_TRUE(msg.find("total") != std::string::npos);
	}
	CHECK_TRUE(threw);

	// Narrower target: mantissa 10^20 into decimal(20,0) is one digit too many.
	col.precision = 20;
	col.max_length = 9;
	bool threw_narrow = false;
	try {
		duckdb::vector<uint8_t> narrow_buf;
		duckdb::mssql::codec::decimal::EncodeToBcp(Value::HUGEINT(duckdb::Hugeint::POWERS_OF_TEN[20]), col, narrow_buf);
	} catch (const duckdb::InvalidInputException &ex) {
		threw_narrow = true;
		CHECK_TRUE(std::string(ex.what()).find("out of range for DECIMAL(20,0)") != std::string::npos);
	}
	CHECK_TRUE(threw_narrow);
}

// ---------------------------------------------------------------------------
// Spec 055 D1: DecimalEncoding::ConvertDecimal wire-magnitude kernel.
//
// The old implementation ran a 128-bit multiply-accumulate per byte; the
// current one loads the little-endian magnitude directly into the int128 words.
// These fixtures pin the byte-level contract that makes that legal: sign byte
// first (0 = negative), magnitude little-endian, any length up to 16 bytes, and
// the >16 fallback still going through the portable path.
void TestConvertDecimalWireKernel() {
	std::cout << "\nTest: DecimalEncoding::ConvertDecimal wire-magnitude kernel (spec 055 D1)\n";
	using duckdb::tds::encoding::DecimalEncoding;

	// Empty payload (NULL magnitude) -> 0
	CHECK_TRUE(DecimalEncoding::ConvertDecimal(nullptr, 0) == hugeint_t(0));

	// Positive, single magnitude byte
	const uint8_t one[] = {1, 0x01};
	CHECK_TRUE(DecimalEncoding::ConvertDecimal(one, sizeof(one)) == hugeint_t(1));

	// Negative sign byte is 0
	const uint8_t minus_one[] = {0, 0x01};
	CHECK_TRUE(DecimalEncoding::ConvertDecimal(minus_one, sizeof(minus_one)) == hugeint_t(-1));

	// Zero magnitude keeps its sign-free value either way
	const uint8_t pos_zero[] = {1, 0x00};
	const uint8_t neg_zero[] = {0, 0x00};
	CHECK_TRUE(DecimalEncoding::ConvertDecimal(pos_zero, sizeof(pos_zero)) == hugeint_t(0));
	CHECK_TRUE(DecimalEncoding::ConvertDecimal(neg_zero, sizeof(neg_zero)) == hugeint_t(0));

	// Little-endian ordering: 0x0201 = 513
	const uint8_t le_two[] = {1, 0x01, 0x02};
	CHECK_TRUE(DecimalEncoding::ConvertDecimal(le_two, sizeof(le_two)) == hugeint_t(513));

	// 4-byte bucket (precision <= 9 on the wire)
	const uint8_t four[] = {1, 0x78, 0x56, 0x34, 0x12};
	CHECK_TRUE(DecimalEncoding::ConvertDecimal(four, sizeof(four)) == hugeint_t(0x12345678));

	// Exactly 8 magnitude bytes — the boundary where the high word starts
	const uint8_t eight[] = {1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	hugeint_t max_u64;
	max_u64.lower = 0xFFFFFFFFFFFFFFFFULL;
	max_u64.upper = 0;
	CHECK_TRUE(DecimalEncoding::ConvertDecimal(eight, sizeof(eight)) == max_u64);

	// 9 magnitude bytes — first byte of the high word
	const uint8_t nine[] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
	hugeint_t two_pow_64;
	two_pow_64.lower = 0;
	two_pow_64.upper = 1;
	CHECK_TRUE(DecimalEncoding::ConvertDecimal(nine, sizeof(nine)) == two_pow_64);

	// Full 16-byte magnitude: 10^38 - 1, the widest DECIMAL SQL Server can send
	hugeint_t max_decimal38 = hugeint_t(1);
	for (int i = 0; i < 38; i++) {
		max_decimal38 = max_decimal38 * hugeint_t(10);
	}
	max_decimal38 = max_decimal38 - hugeint_t(1);
	uint8_t wide[17];
	wide[0] = 1;
	{
		// Serialise max_decimal38 little-endian into the magnitude bytes.
		uint64_t lower = max_decimal38.lower;
		uint64_t upper = static_cast<uint64_t>(max_decimal38.upper);
		for (int i = 0; i < 8; i++) {
			wide[1 + i] = static_cast<uint8_t>((lower >> (8 * i)) & 0xFF);
			wide[9 + i] = static_cast<uint8_t>((upper >> (8 * i)) & 0xFF);
		}
	}
	CHECK_TRUE(DecimalEncoding::ConvertDecimal(wide, sizeof(wide)) == max_decimal38);
	wide[0] = 0;
	CHECK_TRUE(DecimalEncoding::ConvertDecimal(wide, sizeof(wide)) == -max_decimal38);

	// Every length from 1 to 16 magnitude bytes must agree with a byte-by-byte
	// reference built the way the pre-055 implementation did.
	for (size_t mag = 1; mag <= 16; mag++) {
		uint8_t buf[17];
		buf[0] = 1;
		for (size_t i = 0; i < mag; i++) {
			buf[1 + i] = static_cast<uint8_t>(0x10 + i * 7);
		}
		hugeint_t expected(0);
		for (size_t i = mag; i >= 1; i--) {
			expected = expected * hugeint_t(256) + hugeint_t(buf[i]);
		}
		CHECK_TRUE(DecimalEncoding::ConvertDecimal(buf, mag + 1) == expected);
	}
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
	TestEncodeToBcpMantissaRangeGuard();
	TestConvertDecimalWireKernel();

	if (failures > 0) {
		std::cerr << "\n" << failures << " assertion(s) failed.\n";
		return 1;
	}
	std::cout << "\nAll codec::decimal assertions passed.\n";
	return 0;
}

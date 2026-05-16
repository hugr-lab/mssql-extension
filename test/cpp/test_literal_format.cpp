// test/cpp/test_literal_format.cpp
// Unit tests for codec::FormatSqlLiteral dispatcher (spec 045, US2 — Phase 4).
//
// Does NOT require a running SQL Server instance.
//
// Covers:
//   - NULL handling at the dispatcher level — both LiteralContext values
//     produce "NULL" regardless of family.
//   - Integer-family routing — codec::FormatSqlLiteral(v, type, ctx) for
//     Integer types delegates to codec::integer::FormatSqlLiteral. Output
//     is byte-identical between LiteralContext::Filter and
//     LiteralContext::InsertValues for TINYINT..UBIGINT (FR-020 (b) —
//     HUGEINT correctness fix).
//   - HUGEINT divergence-case fix — Filter context renders "<digits>"
//     (bare integer) instead of the pre-spec-045 N'<digits>' string
//     literal. Same result in InsertValues.
//   - EstimateLiteralSize routing — dispatcher returns the per-family
//     upper bound for Integer types.
//   - String family (Phase 5 / US5) — VARCHAR samples route through the
//     dispatcher and produce N'<escaped>' for both contexts. NULL VARCHAR
//     short-circuits to "NULL". INTERVAL also routes through String.
//   - Unmigrated families (Uuid) currently throw NotImplementedException.
//     The dispatch sites (filter_encoder.cpp, mssql_value_serializer.cpp)
//     don't route Uuid through the dispatcher yet — they will once the
//     Phase 6 Uuid sub-phase migration lands.
//
// Build & run:
//   GEN=ninja make debug
//   make test-literal-format

#include "codec/integer_codec.hpp"
#include "codec/literal_context.hpp"
#include "codec/literal_format.hpp"
#include "codec/type_family.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

using duckdb::hugeint_t;
using duckdb::LogicalType;
using duckdb::NotImplementedException;
using duckdb::Value;
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

#define CHECK_THROWS_NOT_IMPLEMENTED(expr)                                                              \
	do {                                                                                                \
		bool _threw = false;                                                                            \
		try {                                                                                           \
			(void)(expr);                                                                               \
		} catch (const NotImplementedException &) {                                                     \
			_threw = true;                                                                              \
		} catch (...) {                                                                                 \
			++failures;                                                                                 \
			std::cerr << "FAIL [" << __LINE__ << "] " #expr " threw wrong exception type\n";            \
			break;                                                                                      \
		}                                                                                               \
		if (!_threw) {                                                                                  \
			++failures;                                                                                 \
			std::cerr << "FAIL [" << __LINE__ << "] " #expr " did NOT throw NotImplementedException\n"; \
		}                                                                                               \
	} while (0)

void TestNullRoutingViaDispatcher() {
	std::cout << "Test: NULL dispatches as \"NULL\" in both contexts (any family)\n";

	// Integer (migrated) — NULL handled by dispatcher early-return.
	auto null_int_filter = duckdb::mssql::codec::FormatSqlLiteral(Value(LogicalType::INTEGER), LogicalType::INTEGER,
																  LiteralContext::Filter);
	auto null_int_insert = duckdb::mssql::codec::FormatSqlLiteral(Value(LogicalType::INTEGER), LogicalType::INTEGER,
																  LiteralContext::InsertValues);
	CHECK_EQ(null_int_filter, std::string("NULL"));
	CHECK_EQ(null_int_insert, std::string("NULL"));

	// Boolean (not yet migrated) — dispatcher's NULL fast-path still
	// short-circuits before the throw-stub.
	auto null_bool_filter = duckdb::mssql::codec::FormatSqlLiteral(Value(LogicalType::BOOLEAN), LogicalType::BOOLEAN,
																   LiteralContext::Filter);
	CHECK_EQ(null_bool_filter, std::string("NULL"));
}

void TestIntegerFamilyDispatcherParity() {
	std::cout << "Test: Integer dispatcher routes to codec::integer (Filter == InsertValues parity)\n";

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
	};

	for (const auto &sample : samples) {
		auto filter = duckdb::mssql::codec::FormatSqlLiteral(sample.first, sample.second, LiteralContext::Filter);
		auto insert = duckdb::mssql::codec::FormatSqlLiteral(sample.first, sample.second, LiteralContext::InsertValues);
		auto direct_filter =
			duckdb::mssql::codec::integer::FormatSqlLiteral(sample.first, sample.second, LiteralContext::Filter);
		auto direct_insert =
			duckdb::mssql::codec::integer::FormatSqlLiteral(sample.first, sample.second, LiteralContext::InsertValues);
		CHECK_EQ(filter, insert);
		CHECK_EQ(filter, direct_filter);
		CHECK_EQ(insert, direct_insert);
	}
}

void TestDispatcherHugeIntFix() {
	std::cout << "Test: HUGEINT through dispatcher renders bare digits (FR-020 (b))\n";

	auto small = duckdb::mssql::codec::FormatSqlLiteral(Value::HUGEINT(hugeint_t(0, 42)), LogicalType::HUGEINT,
														LiteralContext::Filter);
	CHECK_EQ(small, std::string("42"));

	auto neg = duckdb::mssql::codec::FormatSqlLiteral(Value::HUGEINT(duckdb::Hugeint::Negate(hugeint_t(0, 100))),
													  LogicalType::HUGEINT, LiteralContext::Filter);
	CHECK_EQ(neg, std::string("-100"));

	// Both contexts produce identical output — pre-spec-045 the Filter
	// context fell into a N'<digits>' default arm.
	auto filter = duckdb::mssql::codec::FormatSqlLiteral(Value::HUGEINT(hugeint_t(0, 9876543210ULL)),
														 LogicalType::HUGEINT, LiteralContext::Filter);
	auto insert = duckdb::mssql::codec::FormatSqlLiteral(Value::HUGEINT(hugeint_t(0, 9876543210ULL)),
														 LogicalType::HUGEINT, LiteralContext::InsertValues);
	CHECK_EQ(filter, insert);
}

void TestDispatcherEstimateLiteralSize() {
	std::cout << "Test: dispatcher EstimateLiteralSize routes to per-family bound\n";

	CHECK_EQ(duckdb::mssql::codec::EstimateLiteralSize(LogicalType::TINYINT), static_cast<size_t>(4));
	CHECK_EQ(duckdb::mssql::codec::EstimateLiteralSize(LogicalType::SMALLINT), static_cast<size_t>(6));
	CHECK_EQ(duckdb::mssql::codec::EstimateLiteralSize(LogicalType::INTEGER), static_cast<size_t>(11));
	CHECK_EQ(duckdb::mssql::codec::EstimateLiteralSize(LogicalType::BIGINT), static_cast<size_t>(20));
	CHECK_EQ(duckdb::mssql::codec::EstimateLiteralSize(LogicalType::UBIGINT), static_cast<size_t>(50));
	CHECK_EQ(duckdb::mssql::codec::EstimateLiteralSize(LogicalType::HUGEINT), static_cast<size_t>(45));
}

void TestStringFamilyDispatcherWired() {
	std::cout << "Test: String dispatcher arm routes to codec::string (Phase 5 / US5)\n";

	auto filter = duckdb::mssql::codec::FormatSqlLiteral(Value("hello"), LogicalType::VARCHAR, LiteralContext::Filter);
	auto insert =
		duckdb::mssql::codec::FormatSqlLiteral(Value("hello"), LogicalType::VARCHAR, LiteralContext::InsertValues);
	CHECK_EQ(filter, std::string("N'hello'"));
	CHECK_EQ(filter, insert);

	// EstimateLiteralSize routes to codec::string::EstimateLiteralSize which
	// returns the wrapper overhead constant.
	auto bound = duckdb::mssql::codec::EstimateLiteralSize(LogicalType::VARCHAR);
	if (bound == 0) {
		++failures;
		std::cerr << "FAIL: dispatcher String EstimateLiteralSize returned 0\n";
	}
}

void TestBooleanFamilyDispatcherWired() {
	std::cout << "Test: Boolean dispatcher arm routes to codec::boolean (Phase 6 sub-phase 1)\n";

	auto filter_t =
		duckdb::mssql::codec::FormatSqlLiteral(Value::BOOLEAN(true), LogicalType::BOOLEAN, LiteralContext::Filter);
	auto insert_t = duckdb::mssql::codec::FormatSqlLiteral(Value::BOOLEAN(true), LogicalType::BOOLEAN,
														   LiteralContext::InsertValues);
	CHECK_EQ(filter_t, std::string("1"));
	CHECK_EQ(filter_t, insert_t);

	auto filter_f =
		duckdb::mssql::codec::FormatSqlLiteral(Value::BOOLEAN(false), LogicalType::BOOLEAN, LiteralContext::Filter);
	CHECK_EQ(filter_f, std::string("0"));

	CHECK_EQ(duckdb::mssql::codec::EstimateLiteralSize(LogicalType::BOOLEAN), static_cast<size_t>(1));
}

void TestDecimalFamilyDispatcherWired() {
	std::cout << "Test: Decimal dispatcher arm routes to codec::decimal (Phase 6 sub-phase 3)\n";

	auto v = Value::DECIMAL(static_cast<int16_t>(1234), 4, 2);
	auto filter = duckdb::mssql::codec::FormatSqlLiteral(v, v.type(), LiteralContext::Filter);
	auto insert = duckdb::mssql::codec::FormatSqlLiteral(v, v.type(), LiteralContext::InsertValues);
	CHECK_EQ(filter, std::string("12.34"));
	CHECK_EQ(filter, insert);

	// HUGEINT also routes through Decimal codec (FR-025).
	auto huge_filter = duckdb::mssql::codec::FormatSqlLiteral(Value::HUGEINT(hugeint_t(0, 42)), LogicalType::HUGEINT,
															  LiteralContext::Filter);
	auto huge_insert = duckdb::mssql::codec::FormatSqlLiteral(Value::HUGEINT(hugeint_t(0, 42)), LogicalType::HUGEINT,
															  LiteralContext::InsertValues);
	CHECK_EQ(huge_filter, std::string("42"));
	CHECK_EQ(huge_filter, huge_insert);

	auto bound = duckdb::mssql::codec::EstimateLiteralSize(LogicalType::DECIMAL(19, 4));
	if (bound == 0) {
		++failures;
		std::cerr << "FAIL: Decimal EstimateLiteralSize returned 0\n";
	}
}

void TestFloatFamilyDispatcherWired() {
	std::cout << "Test: Float dispatcher arm routes to codec::float_family (Phase 6 sub-phase 2)\n";

	auto via_dispatch_f =
		duckdb::mssql::codec::FormatSqlLiteral(Value::FLOAT(1.5f), LogicalType::FLOAT, LiteralContext::Filter);
	auto via_dispatch_f_i =
		duckdb::mssql::codec::FormatSqlLiteral(Value::FLOAT(1.5f), LogicalType::FLOAT, LiteralContext::InsertValues);
	CHECK_EQ(via_dispatch_f, std::string("1.5"));
	CHECK_EQ(via_dispatch_f, via_dispatch_f_i);

	auto via_dispatch_d =
		duckdb::mssql::codec::FormatSqlLiteral(Value::DOUBLE(2.5), LogicalType::DOUBLE, LiteralContext::Filter);
	auto via_dispatch_d_i =
		duckdb::mssql::codec::FormatSqlLiteral(Value::DOUBLE(2.5), LogicalType::DOUBLE, LiteralContext::InsertValues);
	CHECK_EQ(via_dispatch_d, std::string("2.5"));
	CHECK_EQ(via_dispatch_d, via_dispatch_d_i);

	// EstimateLiteralSize routes through the dispatcher for FLOAT and DOUBLE.
	auto bound_f = duckdb::mssql::codec::EstimateLiteralSize(LogicalType::FLOAT);
	auto bound_d = duckdb::mssql::codec::EstimateLiteralSize(LogicalType::DOUBLE);
	if (bound_f == 0 || bound_d == 0) {
		++failures;
		std::cerr << "FAIL: Float/Double EstimateLiteralSize returned 0\n";
	}
}

void TestBinaryFamilyDispatcherWired() {
	std::cout << "Test: Binary dispatcher arm routes to codec::binary (Phase 6 sub-phase 5)\n";
	std::string payload(1, (char)0xff);
	auto value = Value::BLOB_RAW(payload);
	auto filter =
		duckdb::mssql::codec::FormatSqlLiteral(value, LogicalType::BLOB, duckdb::mssql::codec::LiteralContext::Filter);
	CHECK_EQ(filter, std::string("0xFF"));
	// GEOMETRY also routes through the Binary family (same TypeFamily mapping).
	auto geo_value = Value::BLOB_RAW(payload);
	auto geo_out = duckdb::mssql::codec::FormatSqlLiteral(geo_value, LogicalType::GEOMETRY(),
														  duckdb::mssql::codec::LiteralContext::InsertValues);
	CHECK_EQ(geo_out, std::string("0xFF"));
}

void TestDateTimeFamilyDispatcherWired() {
	std::cout << "Test: DateTime dispatcher arm routes to codec::datetime (Phase 6 sub-phase 6)\n";
	// DATE / TIME / TIMESTAMP / TIMESTAMP_TZ all surface through the same
	// dispatcher arm; spot-check a few wire-distinct cases.
	auto date_v = Value::DATE(duckdb::Date::FromDate(2024, 1, 15));
	auto date_filter = duckdb::mssql::codec::FormatSqlLiteral(date_v, LogicalType::DATE, LiteralContext::Filter);
	auto date_insert = duckdb::mssql::codec::FormatSqlLiteral(date_v, LogicalType::DATE, LiteralContext::InsertValues);
	CHECK_EQ(date_filter, std::string("'2024-01-15'"));
	CHECK_EQ(date_filter, date_insert);

	auto ts =
		duckdb::Timestamp::FromDatetime(duckdb::Date::FromDate(2024, 1, 15), duckdb::Time::FromTime(14, 30, 0, 0));
	auto ts_v = Value::TIMESTAMP(ts);
	auto ts_filter = duckdb::mssql::codec::FormatSqlLiteral(ts_v, LogicalType::TIMESTAMP, LiteralContext::Filter);
	auto ts_insert = duckdb::mssql::codec::FormatSqlLiteral(ts_v, LogicalType::TIMESTAMP, LiteralContext::InsertValues);
	CHECK_EQ(ts_filter, ts_insert);

	// EstimateLiteralSize routes through the dispatcher.
	if (duckdb::mssql::codec::EstimateLiteralSize(LogicalType::DATE) == 0 ||
		duckdb::mssql::codec::EstimateLiteralSize(LogicalType::TIMESTAMP_TZ) == 0) {
		++failures;
		std::cerr << "FAIL: DateTime EstimateLiteralSize returned 0\n";
	}
}

void TestUnmigratedFamiliesThrow() {
	std::cout << "Test: unmigrated family arms still throw NotImplementedException\n";

	// Sanity check: families NOT yet migrated still throw. Only Uuid remains
	// after Phase-6 sub-phase 6 (DateTime).
	CHECK_THROWS_NOT_IMPLEMENTED(duckdb::mssql::codec::EstimateLiteralSize(LogicalType::UUID));

	// MONEY (DuckDB has no MONEY type; codec::money is decode-only fence —
	// FormatSqlLiteral / EstimateLiteralSize remain undefined). Confirm the
	// dispatcher still throws for Money family. There's no LogicalType that
	// maps to TypeFamily::Money so this assertion is implicit — when MONEY
	// values arrive on the wire they decode into DECIMAL(19,4) vectors and
	// surface through the Decimal family.
}

}  // namespace

int main() {
	TestNullRoutingViaDispatcher();
	TestIntegerFamilyDispatcherParity();
	TestDispatcherHugeIntFix();
	TestDispatcherEstimateLiteralSize();
	TestStringFamilyDispatcherWired();
	TestBooleanFamilyDispatcherWired();
	TestFloatFamilyDispatcherWired();
	TestDecimalFamilyDispatcherWired();
	TestBinaryFamilyDispatcherWired();
	TestDateTimeFamilyDispatcherWired();
	TestUnmigratedFamiliesThrow();

	if (failures > 0) {
		std::cerr << "\n" << failures << " assertion(s) failed.\n";
		return 1;
	}
	std::cout << "\nAll literal_format dispatcher assertions passed.\n";
	return 0;
}

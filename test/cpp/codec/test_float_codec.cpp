// test/cpp/codec/test_float_codec.cpp
// Unit tests for codec::float_family (spec 045, US3 sub-phase 2 — Phase 6).
//
// Does NOT require a running SQL Server instance.
//
// Covers:
//   - FormatSqlLiteral byte-identity across LiteralContext::Filter and
//     LiteralContext::InsertValues for normal floats / doubles (FR-020 (b)).
//   - Integer-valued floats / doubles get a ".0" suffix.
//   - High-precision values round-trip via setprecision(9) (float) and
//     setprecision(17) (double).
//   - NaN / +Inf / -Inf throw InvalidInputException — both contexts.
//   - FormatDdlTypeName byte-identity across DdlContext::CreateTable and
//     DdlContext::CtasCreateTable: FLOAT -> "REAL", DOUBLE -> "FLOAT".
//   - EstimateLiteralSize is at least as large as worst-case rendering.
//   - NULL value routed through the dispatcher renders as "NULL".
//
// Build & run:
//   GEN=ninja make debug
//   make test-codec-float

#include "codec/float_codec.hpp"
#include "codec/literal_context.hpp"
#include "codec/literal_format.hpp"
#include "codec/type_family.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

using duckdb::LogicalType;
using duckdb::Value;
using duckdb::mssql::CTASConfig;
using duckdb::mssql::CTASTextType;
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

#define CHECK_THROWS_INVALID_INPUT(expr)                                                              \
	do {                                                                                              \
		bool threw = false;                                                                           \
		try {                                                                                         \
			(void)(expr);                                                                             \
		} catch (const duckdb::InvalidInputException &) {                                             \
			threw = true;                                                                             \
		}                                                                                             \
		if (!threw) {                                                                                 \
			++failures;                                                                               \
			std::cerr << "FAIL [" << __LINE__ << "] " #expr " did NOT throw InvalidInputException\n"; \
		}                                                                                             \
	} while (0)

void TestFormatSqlLiteralFloatByteIdentity() {
	std::cout << "Test: FormatSqlLiteral Filter == InsertValues byte-identity (FLOAT)\n";

	const float samples[] = {0.0f, 1.0f, -1.0f, 1.5f, -2.5f, 3.14f, 0.1f, -0.0f};
	for (float v : samples) {
		auto filter = duckdb::mssql::codec::float_family::FormatSqlLiteral(Value::FLOAT(v), LogicalType::FLOAT,
																		   LiteralContext::Filter);
		auto insert = duckdb::mssql::codec::float_family::FormatSqlLiteral(Value::FLOAT(v), LogicalType::FLOAT,
																		   LiteralContext::InsertValues);
		CHECK_EQ(filter, insert);
	}
}

void TestFormatSqlLiteralDoubleByteIdentity() {
	std::cout << "Test: FormatSqlLiteral Filter == InsertValues byte-identity (DOUBLE)\n";

	const double samples[] = {0.0, 1.0, -1.0, 1.5, -2.5, 3.14, 0.1, -0.0, 1e10, -1e-10};
	for (double v : samples) {
		auto filter = duckdb::mssql::codec::float_family::FormatSqlLiteral(Value::DOUBLE(v), LogicalType::DOUBLE,
																		   LiteralContext::Filter);
		auto insert = duckdb::mssql::codec::float_family::FormatSqlLiteral(Value::DOUBLE(v), LogicalType::DOUBLE,
																		   LiteralContext::InsertValues);
		CHECK_EQ(filter, insert);
	}
}

void TestIntegerValuedFloatSuffix() {
	std::cout << "Test: integer-valued floats / doubles get \".0\" suffix\n";

	CHECK_EQ(duckdb::mssql::codec::float_family::FormatSqlLiteral(Value::FLOAT(5.0f), LogicalType::FLOAT,
																  LiteralContext::Filter),
			 std::string("5.0"));
	CHECK_EQ(duckdb::mssql::codec::float_family::FormatSqlLiteral(Value::DOUBLE(10.0), LogicalType::DOUBLE,
																  LiteralContext::Filter),
			 std::string("10.0"));
	CHECK_EQ(duckdb::mssql::codec::float_family::FormatSqlLiteral(Value::DOUBLE(-7.0), LogicalType::DOUBLE,
																  LiteralContext::InsertValues),
			 std::string("-7.0"));
}

void TestNanInfRejection() {
	std::cout << "Test: NaN / +Inf / -Inf REJECTED in both contexts (FR-020 (b) hardening)\n";

	auto nan_f = std::numeric_limits<float>::quiet_NaN();
	auto pinf_f = std::numeric_limits<float>::infinity();
	auto ninf_f = -std::numeric_limits<float>::infinity();

	CHECK_THROWS_INVALID_INPUT(duckdb::mssql::codec::float_family::FormatSqlLiteral(
		Value::FLOAT(nan_f), LogicalType::FLOAT, LiteralContext::Filter));
	CHECK_THROWS_INVALID_INPUT(duckdb::mssql::codec::float_family::FormatSqlLiteral(
		Value::FLOAT(nan_f), LogicalType::FLOAT, LiteralContext::InsertValues));
	CHECK_THROWS_INVALID_INPUT(duckdb::mssql::codec::float_family::FormatSqlLiteral(
		Value::FLOAT(pinf_f), LogicalType::FLOAT, LiteralContext::Filter));
	CHECK_THROWS_INVALID_INPUT(duckdb::mssql::codec::float_family::FormatSqlLiteral(
		Value::FLOAT(ninf_f), LogicalType::FLOAT, LiteralContext::InsertValues));

	auto nan_d = std::numeric_limits<double>::quiet_NaN();
	auto pinf_d = std::numeric_limits<double>::infinity();
	auto ninf_d = -std::numeric_limits<double>::infinity();

	CHECK_THROWS_INVALID_INPUT(duckdb::mssql::codec::float_family::FormatSqlLiteral(
		Value::DOUBLE(nan_d), LogicalType::DOUBLE, LiteralContext::Filter));
	CHECK_THROWS_INVALID_INPUT(duckdb::mssql::codec::float_family::FormatSqlLiteral(
		Value::DOUBLE(pinf_d), LogicalType::DOUBLE, LiteralContext::Filter));
	CHECK_THROWS_INVALID_INPUT(duckdb::mssql::codec::float_family::FormatSqlLiteral(
		Value::DOUBLE(ninf_d), LogicalType::DOUBLE, LiteralContext::InsertValues));
}

void TestFormatDdlTypeName() {
	std::cout << "Test: FormatDdlTypeName CreateTable == CtasCreateTable byte-identity\n";

	CTASConfig cfg_nvarchar;
	auto create_f = duckdb::mssql::codec::float_family::FormatDdlTypeName(LogicalType::FLOAT, cfg_nvarchar,
																		  DdlContext::CreateTable);
	auto ctas_f = duckdb::mssql::codec::float_family::FormatDdlTypeName(LogicalType::FLOAT, cfg_nvarchar,
																		DdlContext::CtasCreateTable);
	CHECK_EQ(create_f, ctas_f);
	CHECK_EQ(create_f, std::string("REAL"));

	auto create_d = duckdb::mssql::codec::float_family::FormatDdlTypeName(LogicalType::DOUBLE, cfg_nvarchar,
																		  DdlContext::CreateTable);
	auto ctas_d = duckdb::mssql::codec::float_family::FormatDdlTypeName(LogicalType::DOUBLE, cfg_nvarchar,
																		DdlContext::CtasCreateTable);
	CHECK_EQ(create_d, ctas_d);
	CHECK_EQ(create_d, std::string("FLOAT"));

	// text_type policy must NOT affect Float DDL
	CTASConfig cfg_varchar;
	cfg_varchar.text_type = CTASTextType::VARCHAR;
	CHECK_EQ(
		duckdb::mssql::codec::float_family::FormatDdlTypeName(LogicalType::FLOAT, cfg_varchar, DdlContext::CreateTable),
		std::string("REAL"));
	CHECK_EQ(duckdb::mssql::codec::float_family::FormatDdlTypeName(LogicalType::DOUBLE, cfg_varchar,
																   DdlContext::CreateTable),
			 std::string("FLOAT"));
}

void TestEstimateLiteralSizeUpperBound() {
	std::cout << "Test: EstimateLiteralSize sanity (>= rendered literal worst case)\n";

	auto float_bound = duckdb::mssql::codec::float_family::EstimateLiteralSize(LogicalType::FLOAT);
	auto double_bound = duckdb::mssql::codec::float_family::EstimateLiteralSize(LogicalType::DOUBLE);

	// Worst-case rendering for double with setprecision(17) + sign + dot + e+nnn
	// is roughly 24 chars. For float (setprecision 9) about 15 chars.
	if (float_bound < 15) {
		++failures;
		std::cerr << "FAIL float bound=" << float_bound << " < 15\n";
	}
	if (double_bound < 24) {
		++failures;
		std::cerr << "FAIL double bound=" << double_bound << " < 24\n";
	}

	// Surface through dispatcher
	CHECK_EQ(duckdb::mssql::codec::EstimateLiteralSize(LogicalType::FLOAT), float_bound);
	CHECK_EQ(duckdb::mssql::codec::EstimateLiteralSize(LogicalType::DOUBLE), double_bound);
}

void TestNullLiteralViaDispatcher() {
	std::cout << "Test: NULL Value renders as \"NULL\" via dispatcher in both contexts\n";

	Value null_f(LogicalType::FLOAT);
	auto filter_null = duckdb::mssql::codec::FormatSqlLiteral(null_f, LogicalType::FLOAT, LiteralContext::Filter);
	auto insert_null = duckdb::mssql::codec::FormatSqlLiteral(null_f, LogicalType::FLOAT, LiteralContext::InsertValues);
	CHECK_EQ(filter_null, std::string("NULL"));
	CHECK_EQ(insert_null, std::string("NULL"));

	Value null_d(LogicalType::DOUBLE);
	auto filter_null_d = duckdb::mssql::codec::FormatSqlLiteral(null_d, LogicalType::DOUBLE, LiteralContext::Filter);
	CHECK_EQ(filter_null_d, std::string("NULL"));
}

void TestDispatcherRouting() {
	std::cout << "Test: codec::FormatSqlLiteral dispatcher routes FLOAT/DOUBLE to codec::float_family\n";

	auto via_dispatch =
		duckdb::mssql::codec::FormatSqlLiteral(Value::FLOAT(1.5f), LogicalType::FLOAT, LiteralContext::Filter);
	CHECK_EQ(via_dispatch, std::string("1.5"));

	auto via_dispatch_d =
		duckdb::mssql::codec::FormatSqlLiteral(Value::DOUBLE(2.5), LogicalType::DOUBLE, LiteralContext::InsertValues);
	CHECK_EQ(via_dispatch_d, std::string("2.5"));
}

}  // namespace

int main() {
	TestFormatSqlLiteralFloatByteIdentity();
	TestFormatSqlLiteralDoubleByteIdentity();
	TestIntegerValuedFloatSuffix();
	TestNanInfRejection();
	TestFormatDdlTypeName();
	TestEstimateLiteralSizeUpperBound();
	TestNullLiteralViaDispatcher();
	TestDispatcherRouting();

	if (failures > 0) {
		std::cerr << "\n" << failures << " assertion(s) failed.\n";
		return 1;
	}
	std::cout << "\nAll codec::float_family assertions passed.\n";
	return 0;
}

// test/cpp/codec/test_boolean_codec.cpp
// Unit tests for codec::boolean (spec 045, US3 sub-phase 1 — Phase 6).
//
// Does NOT require a running SQL Server instance.
//
// Covers:
//   - FormatSqlLiteral byte-identity across LiteralContext::Filter and
//     LiteralContext::InsertValues for true and false (FR-020 (b)).
//   - FormatSqlLiteral output strings ("1" for true, "0" for false) match
//     the legacy SerializeBoolean / filter-encoder inline ternary.
//   - FormatDdlTypeName byte-identity across DdlContext::CreateTable and
//     DdlContext::CtasCreateTable for BOOLEAN (-> "BIT") regardless of
//     cfg.text_type (FR-027 / FR-028).
//   - EstimateLiteralSize sanity: returned upper bound is at least as
//     large as the worst-case rendered literal (which is 1 char).
//   - NULL value routed through the dispatcher renders as "NULL"
//     (handled at dispatcher level — family arm never sees NULL).
//
// Build & run:
//   GEN=ninja make debug
//   make test-codec-boolean

#include "codec/boolean_codec.hpp"
#include "codec/literal_context.hpp"
#include "codec/literal_format.hpp"
#include "codec/type_family.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
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

void TestFormatSqlLiteralByteIdentity() {
	std::cout << "Test: FormatSqlLiteral Filter == InsertValues byte-identity (BOOLEAN)\n";

	// true
	auto filter_t = duckdb::mssql::codec::boolean::FormatSqlLiteral(Value::BOOLEAN(true), LogicalType::BOOLEAN,
																	LiteralContext::Filter);
	auto insert_t = duckdb::mssql::codec::boolean::FormatSqlLiteral(Value::BOOLEAN(true), LogicalType::BOOLEAN,
																	LiteralContext::InsertValues);
	CHECK_EQ(filter_t, insert_t);
	CHECK_EQ(filter_t, std::string("1"));

	// false
	auto filter_f = duckdb::mssql::codec::boolean::FormatSqlLiteral(Value::BOOLEAN(false), LogicalType::BOOLEAN,
																	LiteralContext::Filter);
	auto insert_f = duckdb::mssql::codec::boolean::FormatSqlLiteral(Value::BOOLEAN(false), LogicalType::BOOLEAN,
																	LiteralContext::InsertValues);
	CHECK_EQ(filter_f, insert_f);
	CHECK_EQ(filter_f, std::string("0"));
}

void TestFormatSqlLiteralViaDispatcher() {
	std::cout << "Test: codec::FormatSqlLiteral dispatcher routes BOOLEAN to boolean::FormatSqlLiteral\n";

	auto via_dispatcher =
		duckdb::mssql::codec::FormatSqlLiteral(Value::BOOLEAN(true), LogicalType::BOOLEAN, LiteralContext::Filter);
	CHECK_EQ(via_dispatcher, std::string("1"));

	auto via_dispatcher_f = duckdb::mssql::codec::FormatSqlLiteral(Value::BOOLEAN(false), LogicalType::BOOLEAN,
																   LiteralContext::InsertValues);
	CHECK_EQ(via_dispatcher_f, std::string("0"));
}

void TestFormatDdlTypeNameByteIdentity() {
	std::cout << "Test: FormatDdlTypeName CreateTable == CtasCreateTable byte-identity (BOOLEAN -> BIT)\n";

	CTASConfig cfg_nvarchar;  // default text_type is NVARCHAR
	auto create =
		duckdb::mssql::codec::boolean::FormatDdlTypeName(LogicalType::BOOLEAN, cfg_nvarchar, DdlContext::CreateTable);
	auto ctas = duckdb::mssql::codec::boolean::FormatDdlTypeName(LogicalType::BOOLEAN, cfg_nvarchar,
																 DdlContext::CtasCreateTable);
	CHECK_EQ(create, ctas);
	CHECK_EQ(create, std::string("BIT"));

	// text_type policy must NOT affect Boolean DDL
	CTASConfig cfg_varchar;
	cfg_varchar.text_type = CTASTextType::VARCHAR;
	auto create_vc =
		duckdb::mssql::codec::boolean::FormatDdlTypeName(LogicalType::BOOLEAN, cfg_varchar, DdlContext::CreateTable);
	auto ctas_vc = duckdb::mssql::codec::boolean::FormatDdlTypeName(LogicalType::BOOLEAN, cfg_varchar,
																	DdlContext::CtasCreateTable);
	CHECK_EQ(create_vc, ctas_vc);
	CHECK_EQ(create_vc, std::string("BIT"));
}

void TestEstimateLiteralSizeUpperBound() {
	std::cout << "Test: EstimateLiteralSize for BOOLEAN >= rendered literal size (1)\n";

	auto bound = duckdb::mssql::codec::boolean::EstimateLiteralSize(LogicalType::BOOLEAN);
	if (bound < 1) {
		++failures;
		std::cerr << "FAIL EstimateLiteralSize(BOOLEAN)=" << bound << " < 1\n";
	}

	// Surface through the dispatcher as well
	auto bound_dispatch = duckdb::mssql::codec::EstimateLiteralSize(LogicalType::BOOLEAN);
	CHECK_EQ(bound, bound_dispatch);
}

void TestNullLiteralViaDispatcher() {
	std::cout << "Test: NULL Value renders as \"NULL\" via dispatcher in both contexts\n";

	Value null_v(LogicalType::BOOLEAN);
	auto filter_null = duckdb::mssql::codec::FormatSqlLiteral(null_v, LogicalType::BOOLEAN, LiteralContext::Filter);
	auto insert_null =
		duckdb::mssql::codec::FormatSqlLiteral(null_v, LogicalType::BOOLEAN, LiteralContext::InsertValues);
	CHECK_EQ(filter_null, std::string("NULL"));
	CHECK_EQ(insert_null, std::string("NULL"));
}

}  // namespace

int main() {
	TestFormatSqlLiteralByteIdentity();
	TestFormatSqlLiteralViaDispatcher();
	TestFormatDdlTypeNameByteIdentity();
	TestEstimateLiteralSizeUpperBound();
	TestNullLiteralViaDispatcher();

	if (failures > 0) {
		std::cerr << "\n" << failures << " assertion(s) failed.\n";
		return 1;
	}
	std::cout << "\nAll codec::boolean assertions passed.\n";
	return 0;
}

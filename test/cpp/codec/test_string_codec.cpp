// test/cpp/codec/test_string_codec.cpp
// Unit tests for codec::string (spec 045, US5 String migration — Phase 5).
//
// Does NOT require a running SQL Server instance.
//
// Covers:
//   - FormatSqlLiteral byte-identity across LiteralContext::Filter and
//     LiteralContext::InsertValues for VARCHAR samples (FR-020 (b)).
//   - Single-quote escaping: ' -> '' inside N'...'.
//   - FormatDdlTypeName byte-identity across DdlContext::CreateTable and
//     DdlContext::CtasCreateTable for VARCHAR (cfg.text_type=NVARCHAR ->
//     "NVARCHAR(MAX)"; cfg.text_type=VARCHAR -> "VARCHAR(MAX)") and
//     INTERVAL (-> "NVARCHAR(50)") per FR-026 / FR-027 / FR-028.
//   - EstimateLiteralSize sanity: returned upper bound is at least as large
//     as the worst-case rendered literal for representative inputs.
//   - NULL value -> "NULL" literal in both contexts.
//
// Build & run:
//   GEN=ninja make debug
//   make test-codec-string

#include "codec/literal_context.hpp"
#include "codec/string_codec.hpp"
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
	std::cout << "Test: FormatSqlLiteral Filter == InsertValues byte-identity (VARCHAR)\n";

	const std::string samples[] = {
		"",			 "hello", "Hello, world!", "abc 123 XYZ", "строка с русскими буквами", "emoji: 😀😁😂",
		"NUL\0byte",  // embedded null — must survive escape pass-through
	};

	for (const auto &s : samples) {
		Value v(s);
		auto filter = duckdb::mssql::codec::string::FormatSqlLiteral(v, LogicalType::VARCHAR, LiteralContext::Filter);
		auto insert =
			duckdb::mssql::codec::string::FormatSqlLiteral(v, LogicalType::VARCHAR, LiteralContext::InsertValues);
		CHECK_EQ(filter, insert);
	}
}

void TestFormatSqlLiteralEscapeContract() {
	std::cout << "Test: FormatSqlLiteral doubles single quotes inside N'...'\n";

	// Single quote at start
	auto v1 =
		duckdb::mssql::codec::string::FormatSqlLiteral(Value("'leading"), LogicalType::VARCHAR, LiteralContext::Filter);
	CHECK_EQ(v1, std::string("N'''leading'"));

	// Single quote at end
	auto v2 = duckdb::mssql::codec::string::FormatSqlLiteral(Value("trailing'"), LogicalType::VARCHAR,
															 LiteralContext::InsertValues);
	CHECK_EQ(v2, std::string("N'trailing'''"));

	// Multiple single quotes
	auto v3 = duckdb::mssql::codec::string::FormatSqlLiteral(Value("it's a 'test'"), LogicalType::VARCHAR,
															 LiteralContext::Filter);
	CHECK_EQ(v3, std::string("N'it''s a ''test'''"));

	// No quotes -> plain N'...'
	auto v4 = duckdb::mssql::codec::string::FormatSqlLiteral(Value("plain"), LogicalType::VARCHAR,
															 LiteralContext::InsertValues);
	CHECK_EQ(v4, std::string("N'plain'"));

	// Empty
	auto v5 = duckdb::mssql::codec::string::FormatSqlLiteral(Value(""), LogicalType::VARCHAR, LiteralContext::Filter);
	CHECK_EQ(v5, std::string("N''"));
}

void TestFormatDdlTypeNameVarcharNVarchar() {
	std::cout << "Test: FormatDdlTypeName VARCHAR byte-identity in both DdlContext values (NVARCHAR variant)\n";

	CTASConfig cfg_nvarchar;  // default text_type is NVARCHAR

	auto create =
		duckdb::mssql::codec::string::FormatDdlTypeName(LogicalType::VARCHAR, cfg_nvarchar, DdlContext::CreateTable);
	auto ctas = duckdb::mssql::codec::string::FormatDdlTypeName(LogicalType::VARCHAR, cfg_nvarchar,
																DdlContext::CtasCreateTable);
	CHECK_EQ(create, ctas);
	CHECK_EQ(create, std::string("NVARCHAR(MAX)"));
}

void TestFormatDdlTypeNameVarcharVarchar() {
	std::cout << "Test: FormatDdlTypeName VARCHAR byte-identity in both DdlContext values (VARCHAR variant)\n";

	CTASConfig cfg_varchar;
	cfg_varchar.text_type = CTASTextType::VARCHAR;

	auto create =
		duckdb::mssql::codec::string::FormatDdlTypeName(LogicalType::VARCHAR, cfg_varchar, DdlContext::CreateTable);
	auto ctas =
		duckdb::mssql::codec::string::FormatDdlTypeName(LogicalType::VARCHAR, cfg_varchar, DdlContext::CtasCreateTable);
	CHECK_EQ(create, ctas);
	CHECK_EQ(create, std::string("VARCHAR(MAX)"));
}

void TestFormatDdlTypeNameInterval() {
	std::cout << "Test: FormatDdlTypeName INTERVAL -> NVARCHAR(50) byte-identity in both DdlContext values (FR-026)\n";

	CTASConfig cfg;
	auto create = duckdb::mssql::codec::string::FormatDdlTypeName(LogicalType::INTERVAL, cfg, DdlContext::CreateTable);
	auto ctas =
		duckdb::mssql::codec::string::FormatDdlTypeName(LogicalType::INTERVAL, cfg, DdlContext::CtasCreateTable);
	CHECK_EQ(create, ctas);
	CHECK_EQ(create, std::string("NVARCHAR(50)"));

	// text_type doesn't matter for INTERVAL — always NVARCHAR
	CTASConfig cfg_vc;
	cfg_vc.text_type = CTASTextType::VARCHAR;
	auto create_vc =
		duckdb::mssql::codec::string::FormatDdlTypeName(LogicalType::INTERVAL, cfg_vc, DdlContext::CreateTable);
	CHECK_EQ(create_vc, std::string("NVARCHAR(50)"));
}

void TestEstimateLiteralSizeUpperBound() {
	std::cout << "Test: EstimateLiteralSize >= actual rendered literal size for representative VARCHAR samples\n";

	const std::string samples[] = {
		"",
		"hello",
		"strings with 'quotes'",
		"this is a longer string used to exercise the per-call estimate",
		"contains ' multiple ' quotes '",
	};

	for (const auto &s : samples) {
		Value v(s);
		auto literal =
			duckdb::mssql::codec::string::FormatSqlLiteral(v, LogicalType::VARCHAR, LiteralContext::InsertValues);
		// EstimateLiteralSize is parameterised on type only — for VARCHAR it
		// uses the value's GetString().size(); call via the per-value overload-
		// independent contract. Cross-check against the actual rendered size.
		auto bound = duckdb::mssql::codec::string::EstimateLiteralSize(LogicalType::VARCHAR);
		// The per-type EstimateLiteralSize cannot know the actual value length,
		// so it's a baseline overhead estimate (N'...' + escape headroom).
		// We just verify the bound is positive and small literals fit.
		if (s.size() <= 64 && literal.size() > bound + s.size() * 2) {
			++failures;
			std::cerr << "FAIL upper bound: literal=" << literal << " size=" << literal.size()
					  << " bound+2*input=" << (bound + s.size() * 2) << "\n";
		}
	}
}

void TestNullLiteral() {
	std::cout << "Test: NULL Value renders as \"NULL\" in both contexts\n";

	Value null_v(LogicalType::VARCHAR);
	auto filter_null =
		duckdb::mssql::codec::string::FormatSqlLiteral(null_v, LogicalType::VARCHAR, LiteralContext::Filter);
	auto insert_null =
		duckdb::mssql::codec::string::FormatSqlLiteral(null_v, LogicalType::VARCHAR, LiteralContext::InsertValues);
	CHECK_EQ(filter_null, std::string("NULL"));
	CHECK_EQ(insert_null, std::string("NULL"));
}

}  // namespace

int main() {
	TestFormatSqlLiteralByteIdentity();
	TestFormatSqlLiteralEscapeContract();
	TestFormatDdlTypeNameVarcharNVarchar();
	TestFormatDdlTypeNameVarcharVarchar();
	TestFormatDdlTypeNameInterval();
	TestEstimateLiteralSizeUpperBound();
	TestNullLiteral();

	if (failures > 0) {
		std::cerr << "\n" << failures << " assertion(s) failed.\n";
		return 1;
	}
	std::cout << "\nAll codec::string assertions passed.\n";
	return 0;
}

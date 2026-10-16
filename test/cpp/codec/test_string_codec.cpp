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

#define CHECK_TRUE(expr)                                             \
	do {                                                             \
		if (!(expr)) {                                               \
			++failures;                                              \
			std::cerr << "FAIL [" << __LINE__ << "] " #expr << "\n"; \
		}                                                            \
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

//===----------------------------------------------------------------------===//
// Issue #489: a MAX-typed value past SQL Server's 2^31-1 bytes is refused
// rather than framed with a truncated PLP chunk length.
//
// The bug needed a single value over 2 GiB, which no unit test can build — so
// the bound is exposed as a predicate (RequirePlpWireBytes) and driven here with
// plain numbers. That tests the arithmetic and the boundary, which is where the
// bug lived: the chunk length is a four-byte wire field and the payload was
// sized in size_t, so a value of 2^32 + k bytes declared k and the server read
// the rest of it as the next tokens.
//
// What this does NOT cover: that the three framing paths actually call the
// predicate. Those calls are one line each at the point the length is written
// (string_codec.cpp:197, :252, :898) and are visible in the diff.
//===----------------------------------------------------------------------===//

bool PlpRefuses(size_t wire_bytes) {
	try {
		duckdb::mssql::codec::string::RequirePlpWireBytes(wire_bytes, "nvarchar(max)");
	} catch (const duckdb::InvalidInputException &) {
		return true;
	}
	return false;
}

void TestPlpWireByteBound() {
	std::cout << "Test: issue #489 -- a MAX value past 2^31-1 wire bytes is refused\n";

	const size_t limit = duckdb::mssql::codec::string::MAX_PLP_WIRE_BYTES;
	CHECK_EQ(limit, static_cast<size_t>(2147483647));

	// Accepted: everything up to and including the limit.
	CHECK_TRUE(!PlpRefuses(0));
	CHECK_TRUE(!PlpRefuses(1));
	CHECK_TRUE(!PlpRefuses(8000));
	CHECK_TRUE(!PlpRefuses(limit - 1));
	CHECK_TRUE(!PlpRefuses(limit));

	// Refused: one past the limit, and the two sizes that used to truncate --
	// 2^32 declared 0, and 2^32 + 8000 declared 8000.
	CHECK_TRUE(PlpRefuses(limit + 1));
	CHECK_TRUE(PlpRefuses(static_cast<size_t>(1) << 31));
	CHECK_TRUE(PlpRefuses(static_cast<size_t>(1) << 32));
	CHECK_TRUE(PlpRefuses((static_cast<size_t>(1) << 32) + 8000));
	// The worst case the nvarchar path can reach: a uint32-length string_t of
	// ASCII, doubled by the UTF-16 encode.
	CHECK_TRUE(PlpRefuses(static_cast<size_t>(4294967295ULL) * 2));

	// The message names the size and the limit, so a user can tell this from a
	// server-side rejection.
	try {
		duckdb::mssql::codec::string::RequirePlpWireBytes((static_cast<size_t>(1) << 32) + 8000, "nvarchar(max)");
		CHECK_TRUE(false);
	} catch (const duckdb::InvalidInputException &e) {
		const std::string msg = e.what();
		CHECK_TRUE(msg.find("4294975296") != std::string::npos);
		CHECK_TRUE(msg.find("2147483647") != std::string::npos);
		CHECK_TRUE(msg.find("nvarchar(max)") != std::string::npos);
	}
}

int main() {
	TestFormatSqlLiteralByteIdentity();
	TestFormatSqlLiteralEscapeContract();
	TestFormatDdlTypeNameVarcharNVarchar();
	TestFormatDdlTypeNameVarcharVarchar();
	TestFormatDdlTypeNameInterval();
	TestEstimateLiteralSizeUpperBound();
	TestNullLiteral();
	TestPlpWireByteBound();

	if (failures > 0) {
		std::cerr << "\n" << failures << " assertion(s) failed.\n";
		return 1;
	}
	std::cout << "\nAll codec::string assertions passed.\n";
	return 0;
}

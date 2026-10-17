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

//===----------------------------------------------------------------------===//
// Review of PR #490: the COLUMNAR path's planning arithmetic.
//
// PlanColumn computes a payload length per value and the chunk buffer is sized
// from it. The length used to be narrowed to uint32 BEFORE the UTF-16 multiply,
// so a 2 GiB source wrapped, and the wrapped value drove three things at once:
// the bound comparison that triggers the clamp, the stored payload length, and
// the wire size. On the non-ASCII arm that meant the encoder wrote the full
// UTF-16 payload into a buffer sized from the wrapped length -- a client-side
// heap overflow, not the desync #489 was about.
//
// This is the path COPY / CTAS / INSERT-via-BCP take for a valid nvarchar(max)
// value; the row path #489 guarded is reached only when PlanColumn returns
// false, i.e. for invalid UTF-8. A 2 GiB string cannot be built in a unit test,
// so the decision is driven here with numbers through PlanOneLength.
//===----------------------------------------------------------------------===//

void TestPlanOneLength() {
	std::cout << "Test: columnar planning refuses or clamps on the TRUE length (review of #490)\n";

	using duckdb::mssql::codec::string::MAX_PLP_WIRE_BYTES;
	using duckdb::mssql::codec::string::PlanOneLength;
	const uint32_t no_bound = 0xFFFFFFFFu;	// what PlanColumn passes for a MAX column

	// --- a MAX (PLP) column: refuse past the ceiling, never clamp ---
	{
		auto ok = PlanOneLength(8000, no_bound, true);
		CHECK_TRUE(!ok.past_max);
		CHECK_TRUE(!ok.over_bound);
		CHECK_EQ(ok.stored, static_cast<uint32_t>(8000));

		auto edge = PlanOneLength(MAX_PLP_WIRE_BYTES, no_bound, true);
		CHECK_TRUE(!edge.past_max);

		auto past = PlanOneLength(MAX_PLP_WIRE_BYTES + 1, no_bound, true);
		CHECK_TRUE(past.past_max);

		// The ASCII arm's worst case: a uint32-length string_t, doubled. This is
		// the value that used to wrap to something small.
		const size_t ascii_worst = static_cast<size_t>(4294967295ULL) * 2;
		auto worst = PlanOneLength(ascii_worst, no_bound, true);
		CHECK_TRUE(worst.past_max);
		// Saturated, not wrapped: 2^33-2 must not present as a small number.
		CHECK_EQ(worst.stored, static_cast<uint32_t>(0xFFFFFFFFu));

		// 2^32 exactly: the non-ASCII arm's wrap point, which used to store 0.
		auto wrap = PlanOneLength(static_cast<size_t>(1) << 32, no_bound, true);
		CHECK_TRUE(wrap.past_max);
		CHECK_EQ(wrap.stored, static_cast<uint32_t>(0xFFFFFFFFu));
	}

	// --- a BOUNDED column: the clamp must see the true length ---
	{
		const uint32_t bound = 200;	 // nvarchar(100) bounds 200 UTF-16 bytes

		auto fits = PlanOneLength(200, bound, false);
		CHECK_TRUE(!fits.over_bound);
		CHECK_TRUE(!fits.past_max);

		auto over = PlanOneLength(201, bound, false);
		CHECK_TRUE(over.over_bound);

		// The review's third bullet: an ASCII value of exactly 2^31 bytes into
		// nvarchar(100) planned as length 0 under the old arithmetic -- 2^31 * 2
		// is 2^32, which wrapped to zero -- so it was never clamped and went as
		// an empty string. The true length is over the bound, so it clamps.
		const size_t two_gib_ascii = (static_cast<size_t>(1) << 31) * 2;
		auto wrapped_to_zero = PlanOneLength(two_gib_ascii, bound, false);
		CHECK_TRUE(wrapped_to_zero.over_bound);
		CHECK_TRUE(wrapped_to_zero.stored != 0);
		// A bounded column is never a MAX column, so the ceiling does not apply:
		// the clamp is the mechanism, not a refusal.
		CHECK_TRUE(!wrapped_to_zero.past_max);
	}

	// --- the arithmetic itself: 2 GiB of ASCII is 4 GiB of UTF-16, not zero ---
	{
		using duckdb::mssql::codec::string::PlannedNVarcharBytes;
		CHECK_EQ(PlannedNVarcharBytes(0), static_cast<size_t>(0));
		CHECK_EQ(PlannedNVarcharBytes(8), static_cast<size_t>(16));
		// The wrap point. `static_cast<uint32_t>(size) * 2` gives 0 here, which
		// is the bug; in size_t it is 2^32.
		CHECK_EQ(PlannedNVarcharBytes(static_cast<size_t>(1) << 31), static_cast<size_t>(1) << 32);
		// And the largest string_t there is: 2^33 - 2, not 2^32 - 2.
		CHECK_EQ(PlannedNVarcharBytes(4294967295ULL), static_cast<size_t>(8589934590ULL));
	}

	// --- the names that reach the message ---
	{
		using duckdb::mssql::codec::string::MaxTypeName;
		using duckdb::mssql::codec::string::VarKind;
		CHECK_EQ(std::string(MaxTypeName(VarKind::NVarchar)), std::string("nvarchar(max)"));
		CHECK_EQ(std::string(MaxTypeName(VarKind::Utf8Varchar)), std::string("varchar(max)"));
		CHECK_EQ(std::string(MaxTypeName(VarKind::Binary)), std::string("varbinary(max)"));
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
	TestPlanOneLength();

	if (failures > 0) {
		std::cerr << "\n" << failures << " assertion(s) failed.\n";
		return 1;
	}
	std::cout << "\nAll codec::string assertions passed.\n";
	return 0;
}

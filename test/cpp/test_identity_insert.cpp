// test/cpp/test_identity_insert.cpp
//
// Unit tests for dml/mssql_identity_insert.hpp (spec 077 W2): the statement the
// bracket sends, and which server refusals of it are recognised and explained.
//
// No SQL Server, no linking: the header is self-contained. What this pins is
// which error numbers are recognised — 1088 (ALTER), 8106 (stale catalog), 8107
// (another table) — and that anything else passes through untouched. A live
// test of 1088 would need a second login and a GRANT, and the mapping is the
// part that can silently rot.
//
// Run:
//   make test-identity-insert

#include <iostream>
#include <string>

#include "dml/mssql_identity_insert.hpp"

using namespace duckdb::mssql;

static int g_failures = 0;

#define CHECK(cond, what)                                                                              \
	do {                                                                                               \
		if (!(cond)) {                                                                                 \
			std::cerr << "  FAIL: " << what << " (" << #cond << ") at line " << __LINE__ << std::endl; \
			g_failures++;                                                                              \
		}                                                                                              \
	} while (0)

static bool Has(const std::string &s, const std::string &needle) {
	return s.find(needle) != std::string::npos;
}

int main() {
	std::cout << "=== IDENTITY_INSERT bracket (spec 077 W2) ===" << std::endl;

	// --- the statement, bracketed, with ] doubled
	CHECK(IdentityInsertSql("dbo", "T", true) == "SET IDENTITY_INSERT [dbo].[T] ON", "ON");
	CHECK(IdentityInsertSql("dbo", "T", false) == "SET IDENTITY_INSERT [dbo].[T] OFF", "OFF");
	CHECK(IdentityInsertSql("s]x", "t]y", true) == "SET IDENTITY_INSERT [s]]x].[t]]y] ON", "] is doubled");

	// --- 1088: needs ALTER; names the permission, the table, and both ways out
	{
		auto m = ExplainIdentityInsertError(1088, "Cannot find the object \"T\"", "dbo", "T");
		CHECK(!m.empty(), "1088 recognised");
		CHECK(Has(m, "ALTER"), "names ALTER");
		CHECK(Has(m, "dbo.T"), "names the table");
		CHECK(Has(m, "leave the identity column out"), "names the other way out");
		CHECK(Has(m, "Cannot find the object"), "carries the server's text as the cause");
	}
	// --- 8106: stale is_identity; names the cache and the function that fixes it
	{
		auto m = ExplainIdentityInsertError(8106, "Table 'T' does not have the identity property", "dbo", "T");
		CHECK(!m.empty(), "8106 recognised");
		CHECK(Has(m, "mssql_invalidate_cache()"), "names the fix");
		CHECK(Has(m, "stale"), "says the cache is stale");
	}
	// --- 8107: another table is ON; names both possible sources, accuses neither
	{
		auto m = ExplainIdentityInsertError(8107, "IDENTITY_INSERT is already ON for table 'dbo.Other'", "dbo", "T");
		CHECK(!m.empty(), "8107 recognised");
		CHECK(Has(m, "mssql_exec"), "names the user's own SET as one source");
		CHECK(Has(m, "did not turn it off"), "names a leaked ON of ours as the other");
		CHECK(Has(m, "dbo.Other"), "carries the table the server named");
	}
	// --- anything else passes through: the caller reports the server's text
	CHECK(ExplainIdentityInsertError(544, "x", "dbo", "T").empty(), "544 is not ours to explain");
	CHECK(ExplainIdentityInsertError(0, "", "dbo", "T").empty(), "0 (no server error) passes through");

	if (g_failures) {
		std::cout << "FAIL: " << g_failures << " check(s)" << std::endl;
		return 1;
	}
	std::cout << "PASS: bracket text and every explained refusal pinned" << std::endl;
	return 0;
}

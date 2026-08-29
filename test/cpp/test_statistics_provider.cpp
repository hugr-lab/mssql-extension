// test/cpp/test_statistics_provider.cpp
// Unit tests for MSSQLStatisticsProvider — cache lifetime and TTL ownership.
//
// CONVERTED FROM CATCH AND WIRED INTO `make test-cpp` (job 1216). This file used
// Catch and was built by NOTHING — not CMake, not the Makefile, not CI — so every
// assertion in it was documentation that looked like a guarantee, which is the
// same class the Makefile's own STANDALONE_TEST_SOURCES comment describes. Adding
// the TTL coverage here without also building it would have repeated that.
//
// What it exists to pin now: the provider is ONE object per catalog, while
// `mssql_statistics_cache_ttl_seconds` is a SESSION setting. Every read must
// therefore take the TTL as an argument and must NOT mutate the provider's own
// field, or one session's SET governs every other session's lookups. That
// regression shipped twice (jobs 1203, 1216) before anything tested it.

#include <cassert>
#include <iostream>

#include "catalog/mssql_statistics.hpp"

using namespace duckdb;

static int g_failures = 0;

static void Check(const char *what, bool ok, const char *why) {
	if (!ok) {
		std::cerr << "FAIL: " << what << " (" << why << ")\n";
		++g_failures;
	} else {
		std::cout << "ok: " << what << " (" << why << ")\n";
	}
}

static void TestConstruction() {
	MSSQLStatisticsProvider provider(300);
	Check("constructed TTL is honoured", provider.GetCacheTTL() == 300, "300 in, 300 out");
}

static void TestInvalidationDoesNotCrash() {
	MSSQLStatisticsProvider provider(300);
	provider.PreloadRowCount("dbo", "t", 42);
	provider.InvalidateTable("dbo", "t");
	idx_t out = 0;
	Check("InvalidateTable drops the entry", !provider.TryGetCachedRowCount("dbo", "t", 300, out),
		  "point invalidation");

	provider.PreloadRowCount("dbo", "t", 42);
	provider.InvalidateSchema("dbo");
	Check("InvalidateSchema drops the entry", !provider.TryGetCachedRowCount("dbo", "t", 300, out),
		  "the path COPY/CTAS/DDL take (job 1193)");

	provider.PreloadRowCount("dbo", "t", 42);
	provider.InvalidateAll();
	Check("InvalidateAll drops the entry", !provider.TryGetCachedRowCount("dbo", "t", 300, out), "whole-catalog");
}

// The invariant the TTL fix exists to hold.
static void TestCallerSuppliedTTLGovernsAndDoesNotLeak() {
	MSSQLStatisticsProvider provider(300);
	provider.PreloadRowCount("dbo", "t", 42);

	idx_t out = 0;
	Check("the read returns the preloaded count", provider.TryGetCachedRowCount("dbo", "t", 300, out) && out == 42,
		  "fresh entry, generous TTL");

	Check("the passed TTL did NOT mutate shared state", provider.GetCacheTTL() == 300,
		  "one session's setting must not reach another's lookups (jobs 1203, 1216)");

	// THE DISCRIMINATING ASSERTION (job 1230). By DEFAULT the caller's TTL
	// governs, exemption or not — so a TTL of 0 refuses even a preloaded entry.
	// Revert IsCacheValid to reading the provider's own field and this fails,
	// which is the whole point: the earlier version of this test passed against
	// the reverted code because the exemption short-circuited before the argument
	// was read.
	out = 0;
	Check("a passed TTL of 0 refuses the entry by default", !provider.TryGetCachedRowCount("dbo", "t", 0, out),
		  "the caller's session disabled caching; the provider's 300 must not win");

	// The exemption is OPT-IN, and only the table-listing path asks for it:
	// ageing a catalog-sourced count out costs a connection + DMV query per table.
	out = 0;
	Check("...but survives when the caller opts into the catalog exemption",
		  provider.TryGetCachedRowCount("dbo", "t", 0, out, /*exempt_catalog_sourced=*/true) && out == 42,
		  "SHOW ALL TABLES must not become N connections per listing (job 1217)");

	provider.InvalidateTable("dbo", "t");
	out = 0;
	Check("invalidation still clears an exempt entry",
		  !provider.TryGetCachedRowCount("dbo", "t", 0, out, /*exempt_catalog_sourced=*/true),
		  "the exemption is from ageing, not from invalidation");

	MSSQLStatisticsProvider strict(0);
	strict.PreloadRowCount("dbo", "t", 7);
	out = 0;
	Check("a 0-TTL provider still serves a caller-supplied read",
		  strict.TryGetCachedRowCount("dbo", "t", 3600, out) && out == 7,
		  "the read is governed by the argument, not the field");
	Check("and still did not mutate it", strict.GetCacheTTL() == 0, "no write-back");
}

int main() {
	std::cout << "== MSSQLStatisticsProvider unit tests ==\n";
	TestConstruction();
	TestInvalidationDoesNotCrash();
	TestCallerSuppliedTTLGovernsAndDoesNotLeak();
	if (g_failures == 0) {
		std::cout << "\nAll statistics-provider tests passed.\n";
		return 0;
	}
	std::cerr << "\n" << g_failures << " statistics-provider test(s) failed.\n";
	return 1;
}

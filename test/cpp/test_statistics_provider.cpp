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

static void TestConstructionAndExplicitSet() {
	MSSQLStatisticsProvider provider(300);
	Check("constructed TTL is honoured", provider.GetCacheTTL() == 300, "300 in, 300 out");
	provider.SetCacheTTL(600);
	Check("SetCacheTTL still works as the catalog-wide knob", provider.GetCacheTTL() == 600,
		  "explicit, not per-session");
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
	Check("3-arg read uses the provider's TTL", provider.TryGetCachedRowCount("dbo", "t", out) && out == 42,
		  "300 s provider, fresh entry");

	out = 0;
	Check("a passed TTL of 0 refuses the same entry", !provider.TryGetCachedRowCount("dbo", "t", 0, out),
		  "the caller's session disabled caching; the provider's 300 must not win");

	Check("the passed TTL did NOT mutate shared state", provider.GetCacheTTL() == 300,
		  "one session's setting must not reach another's lookups (jobs 1203, 1216)");

	// And the reverse direction: a provider that caches nothing must still honour
	// a caller who wants caching.
	MSSQLStatisticsProvider strict(0);
	strict.PreloadRowCount("dbo", "t", 7);
	out = 0;
	Check("a passed TTL of 3600 accepts on a 0-TTL provider",
		  strict.TryGetCachedRowCount("dbo", "t", 3600, out) && out == 7,
		  "the read is governed by the argument, not the field");
	Check("and still did not mutate it", strict.GetCacheTTL() == 0, "no write-back");
}

int main() {
	std::cout << "== MSSQLStatisticsProvider unit tests ==\n";
	TestConstructionAndExplicitSet();
	TestInvalidationDoesNotCrash();
	TestCallerSuppliedTTLGovernsAndDoesNotLeak();
	if (g_failures == 0) {
		std::cout << "\nAll statistics-provider tests passed.\n";
		return 0;
	}
	std::cerr << "\n" << g_failures << " statistics-provider test(s) failed.\n";
	return 1;
}

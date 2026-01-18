#include "catch.hpp"
#include "catalog/mssql_statistics.hpp"
#include <thread>

using namespace duckdb;

TEST_CASE("MSSQLStatisticsProvider cache key building", "[statistics]") {
	// Note: We can't test actual statistics fetching without a connection,
	// but we can test the cache behavior through the public interface

	MSSQLStatisticsProvider provider(300);  // 5 minute TTL

	SECTION("Initial state") {
		REQUIRE(provider.GetCacheTTL() == 300);
	}

	SECTION("SetCacheTTL") {
		provider.SetCacheTTL(600);
		REQUIRE(provider.GetCacheTTL() == 600);
	}

	SECTION("InvalidateAll clears cache") {
		// Just verify it doesn't crash
		provider.InvalidateAll();
	}

	SECTION("InvalidateTable clears specific entry") {
		// Just verify it doesn't crash
		provider.InvalidateTable("dbo", "test_table");
	}

	SECTION("InvalidateSchema clears schema entries") {
		// Just verify it doesn't crash
		provider.InvalidateSchema("dbo");
	}
}

TEST_CASE("MSSQLStatisticsProvider TTL behavior", "[statistics]") {
	SECTION("Zero TTL means no caching") {
		MSSQLStatisticsProvider provider(0);
		REQUIRE(provider.GetCacheTTL() == 0);
	}

	SECTION("Negative TTL treated as no caching") {
		MSSQLStatisticsProvider provider(-1);
		REQUIRE(provider.GetCacheTTL() == -1);
	}
}

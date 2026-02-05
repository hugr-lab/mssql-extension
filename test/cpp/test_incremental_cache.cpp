// test/cpp/test_incremental_cache.cpp
// Unit tests for incremental catalog cache (CacheLoadState transitions and invalidation)
//
// These tests verify the cache state machine and invalidation logic in isolation.
// Full integration testing is done in test/sql/catalog/lazy_loading.test
//
// Note: Many cache operations require a TdsConnection, so only state inspection
// and invalidation methods can be tested without mocking.

#include <cassert>
#include <iostream>

#include "catalog/mssql_metadata_cache.hpp"

using namespace duckdb;

//==============================================================================
// Helper macros for assertions with messages
//==============================================================================
#define ASSERT_EQ(actual, expected)                                                          \
	do {                                                                                     \
		if ((actual) != (expected)) {                                                        \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Expected: " << (expected) << std::endl;                          \
			std::cerr << "  Actual:   " << (actual) << std::endl;                            \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

#define ASSERT_TRUE(cond)                                                                    \
	do {                                                                                     \
		if (!(cond)) {                                                                       \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Condition was false: " #cond << std::endl;                       \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

#define ASSERT_FALSE(cond)                                                                   \
	do {                                                                                     \
		if ((cond)) {                                                                        \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Condition was true: " #cond << std::endl;                        \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

//==============================================================================
// Test: CacheLoadState enum values
//==============================================================================
void test_cache_load_state_values() {
	std::cout << "\n=== Test: CacheLoadState enum values ===" << std::endl;

	// Verify enum values are as expected
	ASSERT_EQ(static_cast<uint8_t>(CacheLoadState::NOT_LOADED), 0);
	ASSERT_EQ(static_cast<uint8_t>(CacheLoadState::LOADING), 1);
	ASSERT_EQ(static_cast<uint8_t>(CacheLoadState::LOADED), 2);
	ASSERT_EQ(static_cast<uint8_t>(CacheLoadState::STALE), 3);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: MSSQLMetadataCache initial state
//==============================================================================
void test_cache_initial_state() {
	std::cout << "\n=== Test: MSSQLMetadataCache initial state ===" << std::endl;

	MSSQLMetadataCache cache;

	// Initial schema state should be NOT_LOADED
	ASSERT_EQ(cache.GetSchemasState(), CacheLoadState::NOT_LOADED);

	// Global state should be EMPTY
	ASSERT_EQ(cache.GetState(), MSSQLCacheState::EMPTY);

	// Should need refresh
	ASSERT_TRUE(cache.NeedsRefresh());

	// Should not be expired (TTL = 0 by default means never auto-expires)
	ASSERT_FALSE(cache.IsExpired());

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: TTL configuration
//==============================================================================
void test_ttl_configuration() {
	std::cout << "\n=== Test: TTL configuration ===" << std::endl;

	MSSQLMetadataCache cache;

	// Default TTL should be 0 (manual refresh only)
	ASSERT_EQ(cache.GetTTL(), 0);

	// Set TTL
	cache.SetTTL(60);
	ASSERT_EQ(cache.GetTTL(), 60);

	// Set TTL to 0 (disable)
	cache.SetTTL(0);
	ASSERT_EQ(cache.GetTTL(), 0);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: InvalidateAll resets all states
//==============================================================================
void test_invalidate_all() {
	std::cout << "\n=== Test: InvalidateAll ===" << std::endl;

	MSSQLMetadataCache cache;

	// InvalidateAll should set global state to INVALID and schemas to NOT_LOADED
	cache.InvalidateAll();

	ASSERT_EQ(cache.GetSchemasState(), CacheLoadState::NOT_LOADED);
	ASSERT_EQ(cache.GetState(), MSSQLCacheState::INVALID);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Invalidate delegates to InvalidateAll
//==============================================================================
void test_invalidate() {
	std::cout << "\n=== Test: Invalidate ===" << std::endl;

	MSSQLMetadataCache cache;

	// Invalidate should call InvalidateAll
	cache.Invalidate();

	ASSERT_EQ(cache.GetSchemasState(), CacheLoadState::NOT_LOADED);
	ASSERT_EQ(cache.GetState(), MSSQLCacheState::INVALID);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: HasSchema/HasTable without loading
//==============================================================================
void test_has_schema_without_loading() {
	std::cout << "\n=== Test: HasSchema/HasTable without loading ===" << std::endl;

	MSSQLMetadataCache cache;

	// HasSchema should return false and NOT trigger loading
	ASSERT_FALSE(cache.HasSchema("dbo"));
	ASSERT_EQ(cache.GetSchemasState(), CacheLoadState::NOT_LOADED);

	// HasTable should return false and NOT trigger loading
	ASSERT_FALSE(cache.HasTable("dbo", "test_table"));
	ASSERT_EQ(cache.GetSchemasState(), CacheLoadState::NOT_LOADED);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: GetTablesState for non-existent schema
//==============================================================================
void test_get_tables_state_nonexistent() {
	std::cout << "\n=== Test: GetTablesState for non-existent schema ===" << std::endl;

	MSSQLMetadataCache cache;

	// Should return NOT_LOADED for non-existent schema
	ASSERT_EQ(cache.GetTablesState("nonexistent"), CacheLoadState::NOT_LOADED);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: GetColumnsState for non-existent table
//==============================================================================
void test_get_columns_state_nonexistent() {
	std::cout << "\n=== Test: GetColumnsState for non-existent table ===" << std::endl;

	MSSQLMetadataCache cache;

	// Should return NOT_LOADED for non-existent schema/table
	ASSERT_EQ(cache.GetColumnsState("dbo", "nonexistent"), CacheLoadState::NOT_LOADED);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: MSSQLTableMetadata move semantics
//==============================================================================
void test_table_metadata_move() {
	std::cout << "\n=== Test: MSSQLTableMetadata move semantics ===" << std::endl;

	MSSQLTableMetadata table1;
	table1.name = "test_table";
	table1.object_type = MSSQLObjectType::TABLE;
	table1.approx_row_count = 1000;
	table1.columns_load_state = CacheLoadState::LOADED;

	// Move construct
	MSSQLTableMetadata table2(std::move(table1));
	ASSERT_EQ(table2.name, "test_table");
	ASSERT_EQ(table2.object_type, MSSQLObjectType::TABLE);
	ASSERT_EQ(table2.approx_row_count, static_cast<idx_t>(1000));
	ASSERT_EQ(table2.columns_load_state, CacheLoadState::LOADED);

	// Move assign
	MSSQLTableMetadata table3;
	table3 = std::move(table2);
	ASSERT_EQ(table3.name, "test_table");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: MSSQLSchemaMetadata move semantics
//==============================================================================
void test_schema_metadata_move() {
	std::cout << "\n=== Test: MSSQLSchemaMetadata move semantics ===" << std::endl;

	MSSQLSchemaMetadata schema1("dbo");
	schema1.tables_load_state = CacheLoadState::LOADED;

	// Move construct
	MSSQLSchemaMetadata schema2(std::move(schema1));
	ASSERT_EQ(schema2.name, "dbo");
	ASSERT_EQ(schema2.tables_load_state, CacheLoadState::LOADED);

	// Move assign
	MSSQLSchemaMetadata schema3;
	schema3 = std::move(schema2);
	ASSERT_EQ(schema3.name, "dbo");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Database collation
//==============================================================================
void test_database_collation() {
	std::cout << "\n=== Test: Database collation ===" << std::endl;

	MSSQLMetadataCache cache;

	// Default collation should be empty
	ASSERT_TRUE(cache.GetDatabaseCollation().empty());

	// Set collation
	cache.SetDatabaseCollation("Latin1_General_CI_AS");
	ASSERT_EQ(cache.GetDatabaseCollation(), "Latin1_General_CI_AS");

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Main entry point
//==============================================================================
int main() {
	std::cout << "========================================" << std::endl;
	std::cout << " Incremental Catalog Cache Unit Tests  " << std::endl;
	std::cout << "========================================" << std::endl;

	// CacheLoadState tests
	test_cache_load_state_values();

	// MSSQLMetadataCache tests
	test_cache_initial_state();
	test_ttl_configuration();
	test_invalidate_all();
	test_invalidate();
	test_has_schema_without_loading();
	test_get_tables_state_nonexistent();
	test_get_columns_state_nonexistent();
	test_database_collation();

	// Move semantics tests
	test_table_metadata_move();
	test_schema_metadata_move();

	std::cout << "\n========================================" << std::endl;
	std::cout << "         ALL TESTS PASSED!              " << std::endl;
	std::cout << "========================================" << std::endl;

	return 0;
}

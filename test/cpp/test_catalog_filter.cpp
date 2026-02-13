// test/cpp/test_catalog_filter.cpp
// Unit tests for MSSQLCatalogFilter (regex-based object visibility filtering)

#include <cassert>
#include <iostream>

#include "catalog/mssql_catalog_filter.hpp"

using namespace duckdb;

//==============================================================================
// Helper macros
//==============================================================================
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
// Test: Default state (no filters)
//==============================================================================
static void TestDefaultState() {
	std::cout << "  TestDefaultState..." << std::endl;
	MSSQLCatalogFilter filter;

	ASSERT_FALSE(filter.HasSchemaFilter());
	ASSERT_FALSE(filter.HasTableFilter());
	ASSERT_FALSE(filter.HasFilters());

	// No filter = match all
	ASSERT_TRUE(filter.MatchesSchema("dbo"));
	ASSERT_TRUE(filter.MatchesSchema("sales"));
	ASSERT_TRUE(filter.MatchesTable("Orders"));
	ASSERT_TRUE(filter.MatchesTable("anything"));

	ASSERT_TRUE(filter.GetSchemaPattern().empty());
	ASSERT_TRUE(filter.GetTablePattern().empty());

	std::cout << "  TestDefaultState PASSED" << std::endl;
}

//==============================================================================
// Test: Schema filter - partial match (regex_search)
//==============================================================================
static void TestSchemaFilterPartialMatch() {
	std::cout << "  TestSchemaFilterPartialMatch..." << std::endl;
	MSSQLCatalogFilter filter;
	filter.SetSchemaFilter("dbo");

	ASSERT_TRUE(filter.HasSchemaFilter());
	ASSERT_TRUE(filter.HasFilters());

	ASSERT_TRUE(filter.MatchesSchema("dbo"));
	ASSERT_TRUE(filter.MatchesSchema("dbo_archive"));  // partial match
	ASSERT_FALSE(filter.MatchesSchema("sales"));
	ASSERT_FALSE(filter.MatchesSchema("production"));

	std::cout << "  TestSchemaFilterPartialMatch PASSED" << std::endl;
}

//==============================================================================
// Test: Schema filter - exact match with anchors
//==============================================================================
static void TestSchemaFilterExactMatch() {
	std::cout << "  TestSchemaFilterExactMatch..." << std::endl;
	MSSQLCatalogFilter filter;
	filter.SetSchemaFilter("^dbo$");

	ASSERT_TRUE(filter.MatchesSchema("dbo"));
	ASSERT_FALSE(filter.MatchesSchema("dbo_archive"));
	ASSERT_FALSE(filter.MatchesSchema("sales"));

	std::cout << "  TestSchemaFilterExactMatch PASSED" << std::endl;
}

//==============================================================================
// Test: Case insensitivity
//==============================================================================
static void TestCaseInsensitivity() {
	std::cout << "  TestCaseInsensitivity..." << std::endl;
	MSSQLCatalogFilter filter;
	filter.SetSchemaFilter("^dbo$");
	filter.SetTableFilter("^Orders$");

	ASSERT_TRUE(filter.MatchesSchema("dbo"));
	ASSERT_TRUE(filter.MatchesSchema("DBO"));
	ASSERT_TRUE(filter.MatchesSchema("Dbo"));

	ASSERT_TRUE(filter.MatchesTable("Orders"));
	ASSERT_TRUE(filter.MatchesTable("ORDERS"));
	ASSERT_TRUE(filter.MatchesTable("orders"));

	std::cout << "  TestCaseInsensitivity PASSED" << std::endl;
}

//==============================================================================
// Test: Table filter with alternatives
//==============================================================================
static void TestTableFilterAlternatives() {
	std::cout << "  TestTableFilterAlternatives..." << std::endl;
	MSSQLCatalogFilter filter;
	filter.SetTableFilter("^(Orders|Products|Customers)$");

	ASSERT_TRUE(filter.MatchesTable("Orders"));
	ASSERT_TRUE(filter.MatchesTable("Products"));
	ASSERT_TRUE(filter.MatchesTable("Customers"));
	ASSERT_FALSE(filter.MatchesTable("Invoices"));
	ASSERT_FALSE(filter.MatchesTable("OrdersHistory"));

	std::cout << "  TestTableFilterAlternatives PASSED" << std::endl;
}

//==============================================================================
// Test: Prefix/suffix patterns
//==============================================================================
static void TestPrefixPattern() {
	std::cout << "  TestPrefixPattern..." << std::endl;
	MSSQLCatalogFilter filter;
	filter.SetTableFilter("^Sales");

	ASSERT_TRUE(filter.MatchesTable("SalesOrders"));
	ASSERT_TRUE(filter.MatchesTable("SalesInvoices"));
	ASSERT_TRUE(filter.MatchesTable("Sales"));
	ASSERT_FALSE(filter.MatchesTable("ProductSales"));
	ASSERT_FALSE(filter.MatchesTable("Orders"));

	std::cout << "  TestPrefixPattern PASSED" << std::endl;
}

//==============================================================================
// Test: Invalid regex throws
//==============================================================================
static void TestInvalidRegexThrows() {
	std::cout << "  TestInvalidRegexThrows..." << std::endl;

	// ValidatePattern returns error message
	string error = MSSQLCatalogFilter::ValidatePattern("[invalid");
	ASSERT_FALSE(error.empty());

	// Empty pattern is valid
	error = MSSQLCatalogFilter::ValidatePattern("");
	ASSERT_TRUE(error.empty());

	// Valid pattern
	error = MSSQLCatalogFilter::ValidatePattern("^dbo$");
	ASSERT_TRUE(error.empty());

	// SetSchemaFilter with invalid regex should throw
	MSSQLCatalogFilter filter;
	bool threw = false;
	try {
		filter.SetSchemaFilter("[invalid");
	} catch (...) {
		threw = true;
	}
	ASSERT_TRUE(threw);

	// SetTableFilter with invalid regex should throw
	threw = false;
	try {
		filter.SetTableFilter("[invalid");
	} catch (...) {
		threw = true;
	}
	ASSERT_TRUE(threw);

	std::cout << "  TestInvalidRegexThrows PASSED" << std::endl;
}

//==============================================================================
// Test: Empty pattern clears filter
//==============================================================================
static void TestEmptyPatternClearsFilter() {
	std::cout << "  TestEmptyPatternClearsFilter..." << std::endl;
	MSSQLCatalogFilter filter;

	filter.SetSchemaFilter("^dbo$");
	ASSERT_TRUE(filter.HasSchemaFilter());

	filter.SetSchemaFilter("");
	ASSERT_FALSE(filter.HasSchemaFilter());
	ASSERT_TRUE(filter.MatchesSchema("anything"));

	std::cout << "  TestEmptyPatternClearsFilter PASSED" << std::endl;
}

//==============================================================================
// Test: Schema and table filters independent
//==============================================================================
static void TestIndependentFilters() {
	std::cout << "  TestIndependentFilters..." << std::endl;
	MSSQLCatalogFilter filter;
	filter.SetSchemaFilter("^dbo$");

	ASSERT_TRUE(filter.HasSchemaFilter());
	ASSERT_FALSE(filter.HasTableFilter());
	ASSERT_TRUE(filter.HasFilters());

	// Table filter not set, so all tables match
	ASSERT_TRUE(filter.MatchesTable("AnyTable"));

	filter.SetTableFilter("^Orders$");
	ASSERT_TRUE(filter.HasTableFilter());

	// Schema filter unchanged
	ASSERT_TRUE(filter.MatchesSchema("dbo"));
	ASSERT_FALSE(filter.MatchesSchema("sales"));

	std::cout << "  TestIndependentFilters PASSED" << std::endl;
}

//==============================================================================
// Test: Multi-schema pattern with pipe
//==============================================================================
static void TestMultiSchemaPattern() {
	std::cout << "  TestMultiSchemaPattern..." << std::endl;
	MSSQLCatalogFilter filter;
	filter.SetSchemaFilter("dbo|sales");

	ASSERT_TRUE(filter.MatchesSchema("dbo"));
	ASSERT_TRUE(filter.MatchesSchema("sales"));
	ASSERT_FALSE(filter.MatchesSchema("production"));

	std::cout << "  TestMultiSchemaPattern PASSED" << std::endl;
}

//==============================================================================
// Main
//==============================================================================
int main() {
	std::cout << "Running MSSQLCatalogFilter unit tests..." << std::endl;

	TestDefaultState();
	TestSchemaFilterPartialMatch();
	TestSchemaFilterExactMatch();
	TestCaseInsensitivity();
	TestTableFilterAlternatives();
	TestPrefixPattern();
	TestInvalidRegexThrows();
	TestEmptyPatternClearsFilter();
	TestIndependentFilters();
	TestMultiSchemaPattern();

	std::cout << "All MSSQLCatalogFilter tests PASSED!" << std::endl;
	return 0;
}

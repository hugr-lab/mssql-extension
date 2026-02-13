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
// Helper for SQL conversion tests
//==============================================================================
#define ASSERT_EQ(actual, expected)                                                               \
	do {                                                                                         \
		auto _a = (actual);                                                                      \
		auto _e = (expected);                                                                    \
		if (_a != _e) {                                                                          \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl;     \
			std::cerr << "  Expected: " << _e << std::endl;                                      \
			std::cerr << "  Actual:   " << _a << std::endl;                                      \
			assert(false);                                                                       \
		}                                                                                        \
	} while (0)

//==============================================================================
// Test: TryRegexToSQLLike — single patterns (baseline)
//==============================================================================
static void TestSQLLikeSinglePatterns() {
	std::cout << "  TestSQLLikeSinglePatterns..." << std::endl;

	// Exact match: ^dbo$ → col LIKE 'dbo'
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^dbo$", "s.name"),
			  "s.name LIKE 'dbo'");

	// Prefix: ^tbl_ → col LIKE 'tbl_%'
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^tbl_", "o.name"),
			  "o.name LIKE 'tbl_%'");

	// Unanchored substring: order → col LIKE '%order%'
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("order", "o.name"),
			  "o.name LIKE '%order%'");

	// Wildcard: ^tbl_.* → col LIKE 'tbl_%'
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^tbl_.*", "o.name"),
			  "o.name LIKE 'tbl_%'");

	// Empty returns empty
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("", "col"), string(""));

	std::cout << "  TestSQLLikeSinglePatterns PASSED" << std::endl;
}

//==============================================================================
// Test: TryRegexToSQLLike — alternation → IN clause
//==============================================================================
static void TestSQLLikeAlternationIN() {
	std::cout << "  TestSQLLikeAlternationIN..." << std::endl;

	// ^(a|b|c)$ → col IN ('a', 'b', 'c')
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^(dbo|sales|prod)$", "s.name"),
			  "s.name IN ('dbo', 'sales', 'prod')");

	// ^(orders|products)$ → col IN ('orders', 'products')
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^(orders|products)$", "o.name"),
			  "o.name IN ('orders', 'products')");

	// Top-level: ^a$|^b$|^c$ → col IN ('a', 'b', 'c')
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^dbo$|^sales$|^prod$", "s.name"),
			  "s.name IN ('dbo', 'sales', 'prod')");

	// Two alternatives
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^(dbo|staging)$", "s.name"),
			  "s.name IN ('dbo', 'staging')");

	std::cout << "  TestSQLLikeAlternationIN PASSED" << std::endl;
}

//==============================================================================
// Test: TryRegexToSQLLike — alternation → OR clauses
//==============================================================================
static void TestSQLLikeAlternationOR() {
	std::cout << "  TestSQLLikeAlternationOR..." << std::endl;

	// ^(tbl_.*|fact_.*) → prefix alternation with OR
	string result = MSSQLCatalogFilter::TryRegexToSQLLike("^(tbl_.*|fact_.*)$", "o.name");
	ASSERT_EQ(result, "(o.name LIKE 'tbl_%' OR o.name LIKE 'fact_%')");

	// Unanchored alternation: top-level a|b
	result = MSSQLCatalogFilter::TryRegexToSQLLike("orders|products", "o.name");
	ASSERT_EQ(result, "(o.name LIKE '%orders%' OR o.name LIKE '%products%')");

	// Prefix only (no $): ^(tbl_|fact_)
	result = MSSQLCatalogFilter::TryRegexToSQLLike("^(tbl_|fact_)", "o.name");
	ASSERT_EQ(result, "(o.name LIKE 'tbl_%' OR o.name LIKE 'fact_%')");

	std::cout << "  TestSQLLikeAlternationOR PASSED" << std::endl;
}

//==============================================================================
// Test: TryRegexToSQLLike — non-convertible patterns
//==============================================================================
static void TestSQLLikeNonConvertible() {
	std::cout << "  TestSQLLikeNonConvertible..." << std::endl;

	// Character class inside alternation — not convertible
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^([a-z]+|prod)$", "s.name"), string(""));

	// \d inside alternation — not convertible
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^(\\d+|prod)$", "s.name"), string(""));

	// Nested groups — not convertible
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^((a|b)|c)$", "s.name"), string(""));

	std::cout << "  TestSQLLikeNonConvertible PASSED" << std::endl;
}

//==============================================================================
// Test: TryRegexToSQLLike — SQL injection safety
//==============================================================================
static void TestSQLLikeInjectionSafety() {
	std::cout << "  TestSQLLikeInjectionSafety..." << std::endl;

	// Single quotes in IN values are escaped
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^(O'Brien|Smith)$", "s.name"),
			  "s.name IN ('O''Brien', 'Smith')");

	std::cout << "  TestSQLLikeInjectionSafety PASSED" << std::endl;
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
	TestSQLLikeSinglePatterns();
	TestSQLLikeAlternationIN();
	TestSQLLikeAlternationOR();
	TestSQLLikeNonConvertible();
	TestSQLLikeInjectionSafety();

	std::cout << "All MSSQLCatalogFilter tests PASSED!" << std::endl;
	return 0;
}

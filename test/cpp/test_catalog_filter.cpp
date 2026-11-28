// test/cpp/test_catalog_filter.cpp
// Unit tests for MSSQLCatalogFilter (regex-based object visibility filtering)

#include <cassert>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

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

	// Prefix: ^tbl_ → col LIKE 'tbl[_]%'. The underscore is a LIKE wildcard, and
	// the regex meant it literally, so it has to be escaped or 'tbl_%' would
	// also match tblX.
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^tbl_", "o.name"),
			  "o.name LIKE 'tbl[_]%'");

	// Unanchored substring: order → col LIKE '%order%'
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("order", "o.name"),
			  "o.name LIKE '%order%'");

	// Wildcard: ^tbl_.* → col LIKE 'tbl[_]%'. The .* supplies the trailing
	// wildcard, so the unanchored end must not add a second one.
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^tbl_.*", "o.name"),
			  "o.name LIKE 'tbl[_]%'");

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
	ASSERT_EQ(result, "(o.name LIKE 'tbl[_]%' OR o.name LIKE 'fact[_]%')");

	// Unanchored alternation: top-level a|b
	result = MSSQLCatalogFilter::TryRegexToSQLLike("orders|products", "o.name");
	ASSERT_EQ(result, "(o.name LIKE '%orders%' OR o.name LIKE '%products%')");

	// Prefix only (no $): ^(tbl_|fact_)
	result = MSSQLCatalogFilter::TryRegexToSQLLike("^(tbl_|fact_)", "o.name");
	ASSERT_EQ(result, "(o.name LIKE 'tbl[_]%' OR o.name LIKE 'fact[_]%')");

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
//==============================================================================
// Test: TryRegexToSQLLike — never narrower than the regex (issue #663)
//==============================================================================
// The predicate is ANDed into the metadata queries, so a name the regex accepts
// and the predicate rejects is a schema or table the catalog loses.
static void TestSQLLikeNeverNarrower() {
	std::cout << "  TestSQLLikeNeverNarrower..." << std::endl;

	// An empty alternative matches every name: the alternatives before it are not
	// the pattern, and nothing is converted.
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("a|b|", "c"), "");
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("|a", "c"), "");
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^(a|b|)", "c"), "");
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^(a|b||c)$", "c"), "");

	// An anchor inside an alternative is an anchor, not text for the IN list.
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^(a$|b)$", "c"), "(c LIKE 'a' OR c LIKE 'b')");
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^(a|^b)$", "c"), "(c LIKE 'a' OR c LIKE 'b')");

	// An anchor that is not the pattern's first / last character is still an
	// assertion (`^^foo$` matches foo): not convertible, never the character.
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^^foo$", "c"), "");
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^foo$$", "c"), "");
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("a$b", "c"), "");
	// Escaped, it IS the character.
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^foo\\$$", "c"), "c LIKE 'foo$'");

	// What converted before still does.
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^(a|b|c)$", "c"), "c IN ('a', 'b', 'c')");
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("^a$|^b$", "c"), "c IN ('a', 'b')");
	ASSERT_EQ(MSSQLCatalogFilter::TryRegexToSQLLike("foo|bar", "c"), "(c LIKE '%foo%' OR c LIKE '%bar%')");

	std::cout << "    PASSED" << std::endl;
}

//==============================================================================
// Test: TryRegexToSQLLike — the predicate accepts every name the regex accepts
//==============================================================================
// The rule behind the cases above, checked for every pattern of up to four
// characters over the regex's own alphabet: where a pattern converts, no name
// the regex matches may be rejected by the predicate. LIKE is evaluated here as
// T-SQL does under a case-insensitive collation (% a run, _ one character, [x]
// the character x); the predicate is one of the three shapes the converter
// writes. Issue #663's patterns were 1,742 of the 96,676 that converted at
// five characters.
namespace {

char LowerAscii(char c) {
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

bool LikeMatches(const std::string &pattern, size_t i, const std::string &name, size_t j) {
	while (i < pattern.size()) {
		if (pattern[i] == '%') {
			for (size_t k = j; k <= name.size(); k++) {
				if (LikeMatches(pattern, i + 1, name, k)) {
					return true;
				}
			}
			return false;
		}
		if (j >= name.size()) {
			return false;
		}
		if (pattern[i] == '[' && i + 2 < pattern.size() && pattern[i + 2] == ']') {
			if (LowerAscii(pattern[i + 1]) != LowerAscii(name[j])) {
				return false;
			}
			i += 3;
			j++;
			continue;
		}
		if (pattern[i] != '_' && LowerAscii(pattern[i]) != LowerAscii(name[j])) {
			return false;
		}
		i++;
		j++;
	}
	return j == name.size();
}

bool EqualsIgnoringCase(const std::string &a, const std::string &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); i++) {
		if (LowerAscii(a[i]) != LowerAscii(b[i])) {
			return false;
		}
	}
	return true;
}

//! The quoted values of a predicate the converter wrote, '' read back as '.
std::vector<std::string> QuotedValues(const std::string &sql) {
	std::vector<std::string> values;
	size_t i = 0;
	while ((i = sql.find('\'', i)) != std::string::npos) {
		std::string value;
		for (i++; i < sql.size(); i++) {
			if (sql[i] == '\'') {
				if (i + 1 < sql.size() && sql[i + 1] == '\'') {
					i++;
				} else {
					break;
				}
			}
			value += sql[i];
		}
		values.push_back(value);
		i++;
	}
	return values;
}

bool PredicateAccepts(const std::string &sql, const std::string &name) {
	const bool in_list = sql.find(" LIKE '") == std::string::npos;
	for (const auto &value : QuotedValues(sql)) {
		if (in_list ? EqualsIgnoringCase(value, name) : LikeMatches(value, 0, name, 0)) {
			return true;
		}
	}
	return false;
}

//! Every string of 1..max_length characters of `alphabet`.
std::vector<std::string> AllStrings(const std::string &alphabet, size_t max_length) {
	std::vector<std::string> all;
	std::vector<std::string> last(1, "");
	for (size_t length = 1; length <= max_length; length++) {
		std::vector<std::string> next;
		for (const auto &prefix : last) {
			for (char c : alphabet) {
				next.push_back(prefix + c);
			}
		}
		all.insert(all.end(), next.begin(), next.end());
		last = std::move(next);
	}
	return all;
}

}  // namespace

static void TestSQLLikeAcceptsWhatTheRegexAccepts() {
	std::cout << "  TestSQLLikeAcceptsWhatTheRegexAccepts..." << std::endl;

	const std::vector<std::string> names = AllStrings("ab$^%_(", 3);
	size_t converted = 0;
	for (const auto &pattern : AllStrings("ab^$|().*\\%", 4)) {
		std::regex regex;
		try {
			regex = std::regex(pattern, std::regex::icase);
		} catch (const std::regex_error &) {
			continue;  // not a filter ATTACH would take
		}
		const std::string predicate = MSSQLCatalogFilter::TryRegexToSQLLike(pattern, "c");
		if (predicate.empty()) {
			continue;
		}
		converted++;
		for (const auto &name : names) {
			if (std::regex_search(name, regex) && !PredicateAccepts(predicate, name)) {
				std::cerr << "ASSERTION FAILED: the filter '" << pattern << "' matches the name '" << name
						  << "', and its predicate rejects it: " << predicate << std::endl;
				assert(false);
			}
		}
	}
	// The check is of converted patterns: it must have had some to look at.
	ASSERT_TRUE(converted > 1000);

	std::cout << "    PASSED" << std::endl;
}

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
	TestSQLLikeNeverNarrower();
	TestSQLLikeAcceptsWhatTheRegexAccepts();

	std::cout << "All MSSQLCatalogFilter tests PASSED!" << std::endl;
	return 0;
}

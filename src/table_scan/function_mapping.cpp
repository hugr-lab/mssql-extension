// Function Mapping Implementation
// Feature: 013-table-scan-filter-refactor

#include "table_scan/function_mapping.hpp"
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace duckdb {
namespace mssql {

// Convert string to lowercase
static std::string ToLower(const std::string &str) {
	std::string result = str;
	std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::tolower(c); });
	return result;
}

// Static function mapping table
// Format: {duckdb_name, sql_template, expected_args}
// Template placeholders: {0}, {1}, {2}, etc.
static const std::unordered_map<std::string, FunctionMapping> &GetFunctionMappingTable() {
	static const std::unordered_map<std::string, FunctionMapping> mappings = {
		// String functions
		{"lower", {"lower", "LOWER({0})", 1}},
		{"upper", {"upper", "UPPER({0})", 1}},
		// length/len deliberately NOT mapped: SQL Server LEN excludes trailing spaces
		// and counts UTF-16 code units, DuckDB length() counts code points including
		// them (issue #242). No exact T-SQL form on non-_SC collations. An unmapped
		// function is applied client-side by the spec-069 net, correct by construction.
		{"trim", {"trim", "LTRIM(RTRIM({0}))", 1}},
		{"ltrim", {"ltrim", "LTRIM({0})", 1}},
		{"rtrim", {"rtrim", "RTRIM({0})", 1}},

		// Note: LIKE pattern functions (prefix, suffix, contains, iprefix, isuffix, icontains)
		// are handled by EncodeLikePattern() directly for proper Unicode (N'') encoding
		// and LIKE special character escaping. Do not add them here.

		// Date/Time extraction functions
		{"year", {"year", "YEAR({0})", 1}},
		{"month", {"month", "MONTH({0})", 1}},
		{"day", {"day", "DAY({0})", 1}},
		{"hour", {"hour", "DATEPART(HOUR, {0})", 1}},
		{"minute", {"minute", "DATEPART(MINUTE, {0})", 1}},
		{"second", {"second", "DATEPART(SECOND, {0})", 1}},

		// date_diff / date_add / date_part are deliberately NOT mapped. Their
		// datepart argument is a keyword to T-SQL (DATEPART(year, x)), but it
		// arrives as a VARCHAR constant and would encode as a literal —
		// DATEPART(N'year', x) / DATEDIFF(N'day', ...) — which SQL Server rejects.
		// They are also unreachable in practice: DuckDB rewrites date_part('year',
		// x) to the function `year`, handled by the entry above. Removed rather
		// than left as a trap for the next reader (PR #269 review).

		// Arithmetic operators (in DuckDB these are function expressions)
		{"+", {"+", "({0} + {1})", 2}},
		{"-", {"-", "({0} - {1})", 2}},
		{"*", {"*", "({0} * {1})", 2}},
		// "/" NOT mapped: SQL Server does INTEGER division on integer operands
		// (5/2 = 2), DuckDB "/" is always floating division (5/2 = 2.5) (issue #242).
		{"%", {"%", "({0} % {1})", 2}},

		// Unary minus
		{"negate", {"negate", "(-{0})", 1}},
	};
	return mappings;
}

const FunctionMapping *GetFunctionMapping(const std::string &function_name) {
	std::string lower_name = ToLower(function_name);
	const auto &mappings = GetFunctionMappingTable();
	auto it = mappings.find(lower_name);
	if (it != mappings.end()) {
		return &it->second;
	}
	return nullptr;
}

bool IsFunctionSupported(const std::string &function_name) {
	return GetFunctionMapping(function_name) != nullptr;
}

bool IsLikePatternFunction(const std::string &function_name) {
	std::string lower_name = ToLower(function_name);
	return lower_name == "prefix" || lower_name == "suffix" || lower_name == "contains" || lower_name == "iprefix" ||
		   lower_name == "isuffix" || lower_name == "icontains";
}

bool IsCaseInsensitiveLikeFunction(const std::string &function_name) {
	std::string lower_name = ToLower(function_name);
	return lower_name == "iprefix" || lower_name == "isuffix" || lower_name == "icontains";
}

}  // namespace mssql
}  // namespace duckdb

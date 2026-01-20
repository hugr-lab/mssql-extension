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
	std::transform(result.begin(), result.end(), result.begin(),
				   [](unsigned char c) { return std::tolower(c); });
	return result;
}

// Static function mapping table
// Format: {duckdb_name, sql_template, expected_args}
// Template placeholders: {0}, {1}, {2}, etc.
static const std::unordered_map<std::string, FunctionMapping> &GetFunctionMappingTable() {
	static const std::unordered_map<std::string, FunctionMapping> mappings = {
		// String functions
		{"lower",   {"lower",   "LOWER({0})",           1}},
		{"upper",   {"upper",   "UPPER({0})",           1}},
		{"length",  {"length",  "LEN({0})",             1}},
		{"len",     {"len",     "LEN({0})",             1}},
		{"trim",    {"trim",    "LTRIM(RTRIM({0}))",    1}},
		{"ltrim",   {"ltrim",   "LTRIM({0})",           1}},
		{"rtrim",   {"rtrim",   "RTRIM({0})",           1}},

		// Note: LIKE pattern functions (prefix, suffix, contains, iprefix, isuffix, icontains)
		// are handled by EncodeLikePattern() directly for proper Unicode (N'') encoding
		// and LIKE special character escaping. Do not add them here.

		// Date/Time extraction functions
		{"year",        {"year",        "YEAR({0})",                    1}},
		{"month",       {"month",       "MONTH({0})",                   1}},
		{"day",         {"day",         "DAY({0})",                     1}},
		{"hour",        {"hour",        "DATEPART(HOUR, {0})",          1}},
		{"minute",      {"minute",      "DATEPART(MINUTE, {0})",        1}},
		{"second",      {"second",      "DATEPART(SECOND, {0})",        1}},

		// Date/Time arithmetic and parts
		// Note: date_diff has args: (part, start, end) -> DATEDIFF(part, start, end)
		{"date_diff",   {"date_diff",   "DATEDIFF({0}, {1}, {2})",      3}},
		// Note: date_add has args: (date, part, amount) -> DATEADD(part, amount, date) - reordered
		{"date_add",    {"date_add",    "DATEADD({1}, {2}, {0})",       3}},
		{"date_part",   {"date_part",   "DATEPART({0}, {1})",           2}},

		// Arithmetic operators (in DuckDB these are function expressions)
		{"+",           {"+",           "({0} + {1})",                  2}},
		{"-",           {"-",           "({0} - {1})",                  2}},
		{"*",           {"*",           "({0} * {1})",                  2}},
		{"/",           {"/",           "({0} / {1})",                  2}},
		{"%",           {"%",           "({0} % {1})",                  2}},

		// Unary minus
		{"negate",      {"negate",      "(-{0})",                       1}},
	};
	return mappings;
}

// Date part mappings from DuckDB to SQL Server
static const std::unordered_map<std::string, std::string> &GetDatePartMappingTable() {
	static const std::unordered_map<std::string, std::string> mappings = {
		{"year",        "year"},
		{"month",       "month"},
		{"day",         "day"},
		{"hour",        "hour"},
		{"minute",      "minute"},
		{"second",      "second"},
		{"millisecond", "millisecond"},
		{"week",        "week"},
		{"quarter",     "quarter"},
		{"dayofweek",   "weekday"},
		{"dayofyear",   "dayofyear"},
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
	return lower_name == "prefix" || lower_name == "suffix" || lower_name == "contains" ||
		   lower_name == "iprefix" || lower_name == "isuffix" || lower_name == "icontains";
}

bool IsCaseInsensitiveLikeFunction(const std::string &function_name) {
	std::string lower_name = ToLower(function_name);
	return lower_name == "iprefix" || lower_name == "isuffix" || lower_name == "icontains";
}

bool GetDatePartMapping(const std::string &duckdb_part, std::string &out_result) {
	std::string lower_part = ToLower(duckdb_part);
	const auto &mappings = GetDatePartMappingTable();
	auto it = mappings.find(lower_part);
	if (it != mappings.end()) {
		out_result = it->second;
		return true;
	}
	return false;
}

} // namespace mssql
} // namespace duckdb

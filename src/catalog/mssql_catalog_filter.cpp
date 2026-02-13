#include "catalog/mssql_catalog_filter.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Pattern Validation
//===----------------------------------------------------------------------===//

string MSSQLCatalogFilter::ValidatePattern(const string &pattern) {
	if (pattern.empty()) {
		return "";	// Empty pattern is valid (means no filter)
	}
	try {
		std::regex test_regex(pattern, std::regex::icase);
		(void)test_regex;
		return "";	// Valid
	} catch (const std::regex_error &e) {
		return StringUtil::Format("Invalid regex pattern '%s': %s", pattern, e.what());
	}
}

//===----------------------------------------------------------------------===//
// Filter Configuration
//===----------------------------------------------------------------------===//

void MSSQLCatalogFilter::SetSchemaFilter(const string &pattern) {
	if (pattern.empty()) {
		has_schema_filter_ = false;
		schema_pattern_.clear();
		return;
	}
	string error = ValidatePattern(pattern);
	if (!error.empty()) {
		throw InvalidInputException("MSSQL schema_filter error: %s", error);
	}
	schema_pattern_ = pattern;
	schema_regex_ = std::regex(pattern, std::regex::icase);
	has_schema_filter_ = true;
}

void MSSQLCatalogFilter::SetTableFilter(const string &pattern) {
	if (pattern.empty()) {
		has_table_filter_ = false;
		table_pattern_.clear();
		return;
	}
	string error = ValidatePattern(pattern);
	if (!error.empty()) {
		throw InvalidInputException("MSSQL table_filter error: %s", error);
	}
	table_pattern_ = pattern;
	table_regex_ = std::regex(pattern, std::regex::icase);
	has_table_filter_ = true;
}

//===----------------------------------------------------------------------===//
// Matching
//===----------------------------------------------------------------------===//

bool MSSQLCatalogFilter::MatchesSchema(const string &name) const {
	if (!has_schema_filter_) {
		return true;  // No filter = match all
	}
	return std::regex_search(name, schema_regex_);
}

bool MSSQLCatalogFilter::MatchesTable(const string &name) const {
	if (!has_table_filter_) {
		return true;  // No filter = match all
	}
	return std::regex_search(name, table_regex_);
}

//===----------------------------------------------------------------------===//
// State Queries
//===----------------------------------------------------------------------===//

bool MSSQLCatalogFilter::HasSchemaFilter() const {
	return has_schema_filter_;
}

bool MSSQLCatalogFilter::HasTableFilter() const {
	return has_table_filter_;
}

bool MSSQLCatalogFilter::HasFilters() const {
	return has_schema_filter_ || has_table_filter_;
}

const string &MSSQLCatalogFilter::GetSchemaPattern() const {
	return schema_pattern_;
}

const string &MSSQLCatalogFilter::GetTablePattern() const {
	return table_pattern_;
}

//===----------------------------------------------------------------------===//
// Regex → SQL Conversion Helpers
//===----------------------------------------------------------------------===//

// Convert a single regex pattern (no alternation) to a SQL LIKE expression.
// Returns "column LIKE '...'" or empty string if not convertible.
static string ConvertSinglePatternToLike(const string &pattern, const string &column_expr) {
	if (pattern.empty()) {
		return "";
	}

	string like_pattern;
	bool anchored_start = false;
	bool anchored_end = false;
	size_t i = 0;
	size_t len = pattern.size();

	if (i < len && pattern[i] == '^') {
		anchored_start = true;
		i++;
	}

	while (i < len) {
		char c = pattern[i];

		if (c == '$' && i == len - 1) {
			anchored_end = true;
			i++;
			continue;
		}

		// .* → %
		if (c == '.' && i + 1 < len && pattern[i + 1] == '*') {
			like_pattern += '%';
			i += 2;
			continue;
		}

		// .+ → _%
		if (c == '.' && i + 1 < len && pattern[i + 1] == '+') {
			like_pattern += "_%";
			i += 2;
			continue;
		}

		// . → _
		if (c == '.') {
			like_pattern += '_';
			i++;
			continue;
		}

		// Escape sequences: \. \* etc → literal character
		if (c == '\\' && i + 1 < len) {
			char next = pattern[i + 1];
			if (next == '.' || next == '*' || next == '+' || next == '?' || next == '[' || next == ']' || next == '(' ||
				next == ')' || next == '{' || next == '}' || next == '|' || next == '^' || next == '$' ||
				next == '\\') {
				if (next == '%' || next == '_' || next == '[') {
					like_pattern += '[';
					like_pattern += next;
					like_pattern += ']';
				} else {
					like_pattern += next;
				}
				i += 2;
				continue;
			}
			return "";	// Unknown escape (\d, \w, etc.)
		}

		// Non-convertible regex constructs
		if (c == '[' || c == '(' || c == '{' || c == '|' || c == '?' || c == '+' || c == '*') {
			return "";
		}

		// Escape LIKE special chars in literal text
		if (c == '%' || c == '_') {
			like_pattern += '[';
			like_pattern += c;
			like_pattern += ']';
		} else {
			like_pattern += c;
		}
		i++;
	}

	string full_like;
	if (!anchored_start) {
		full_like = "%" + like_pattern;
	} else {
		full_like = like_pattern;
	}
	if (!anchored_end) {
		full_like += "%";
	}

	// Escape single quotes for SQL injection safety
	string escaped;
	for (char c : full_like) {
		if (c == '\'') {
			escaped += "''";
		} else {
			escaped += c;
		}
	}

	return column_expr + " LIKE '" + escaped + "'";
}

// Split a string on '|' at parenthesis depth 0, respecting escape sequences.
// Returns true if 2+ alternatives found (and populates out), false otherwise.
static bool SplitAlternatives(const string &s, vector<string> &out) {
	out.clear();
	string current;
	int depth = 0;
	for (size_t i = 0; i < s.size(); i++) {
		char c = s[i];
		if (c == '\\' && i + 1 < s.size()) {
			current += c;
			current += s[++i];
			continue;
		}
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
		} else if (c == '|' && depth == 0) {
			if (current.empty()) {
				return false;  // Empty alternative
			}
			out.push_back(current);
			current.clear();
			continue;
		}
		current += c;
	}
	if (current.empty() || depth != 0) {
		return false;
	}
	out.push_back(current);
	return out.size() >= 2;
}

// Check if string contains only literal characters (no regex metacharacters)
static bool IsPlainLiteral(const string &s) {
	for (size_t i = 0; i < s.size(); i++) {
		char c = s[i];
		if (c == '.' || c == '*' || c == '+' || c == '?' || c == '[' || c == ']' || c == '(' || c == ')' || c == '{' ||
			c == '}' || c == '|' || c == '\\') {
			return false;
		}
	}
	return true;
}

// Escape a literal value for use in SQL single-quoted string
static string EscapeSQLLiteral(const string &s) {
	string escaped;
	for (char c : s) {
		if (c == '\'') {
			escaped += "''";
		} else {
			escaped += c;
		}
	}
	return escaped;
}

//===----------------------------------------------------------------------===//
// Regex → SQL Conversion (public API)
//===----------------------------------------------------------------------===//

string MSSQLCatalogFilter::TryRegexToSQLLike(const string &pattern, const string &column_expr) {
	if (pattern.empty()) {
		return "";
	}

	// Phase 1: Try alternation patterns (e.g. ^(a|b|c)$ or a|b|c)
	vector<string> alternatives;
	bool group_start = false;
	bool group_end = false;

	// Case A: ^(alt1|alt2|...)$ — anchored group
	{
		size_t start = 0;
		size_t end = pattern.size();

		if (start < end && pattern[start] == '^') {
			group_start = true;
			start++;
		}
		if (end > start && pattern[end - 1] == '$') {
			group_end = true;
			end--;
		}

		if (end - start >= 2 && pattern[start] == '(' && pattern[end - 1] == ')') {
			string inner = pattern.substr(start + 1, end - start - 2);
			SplitAlternatives(inner, alternatives);
		}

		if (alternatives.empty()) {
			group_start = false;
			group_end = false;
		}
	}

	// Case B: top-level pipe without outer group (e.g. ^a$|^b$|^c$)
	if (alternatives.empty()) {
		SplitAlternatives(pattern, alternatives);
	}

	if (alternatives.size() >= 2) {
		// Check for exact-match literals → optimize to IN(...)
		// Case A produced group anchors; Case B has per-alternative anchors
		bool all_exact = false;
		vector<string> literal_values;

		if (group_start && group_end) {
			// ^(a|b|c)$ — each alternative is a literal if IsPlainLiteral
			all_exact = true;
			for (const auto &alt : alternatives) {
				if (!IsPlainLiteral(alt)) {
					all_exact = false;
					break;
				}
				literal_values.push_back(alt);
			}
		} else if (!group_start && !group_end) {
			// ^a$|^b$|^c$ — each alternative must be ^literal$
			all_exact = true;
			for (const auto &alt : alternatives) {
				if (alt.size() < 3 || alt.front() != '^' || alt.back() != '$') {
					all_exact = false;
					break;
				}
				string inner = alt.substr(1, alt.size() - 2);
				if (!IsPlainLiteral(inner)) {
					all_exact = false;
					break;
				}
				literal_values.push_back(inner);
			}
		}

		if (all_exact && !literal_values.empty()) {
			// column IN ('a', 'b', 'c')
			string in_list;
			for (size_t j = 0; j < literal_values.size(); j++) {
				if (j > 0) {
					in_list += ", ";
				}
				in_list += "'" + EscapeSQLLiteral(literal_values[j]) + "'";
			}
			return column_expr + " IN (" + in_list + ")";
		}

		// General case: convert each alternative to LIKE, combine with OR
		vector<string> sql_parts;
		for (const auto &alt : alternatives) {
			string full_alt = alt;
			// Apply group anchors to each alternative
			if (group_start && !full_alt.empty() && full_alt[0] != '^') {
				full_alt = "^" + full_alt;
			}
			if (group_end && !full_alt.empty() && full_alt.back() != '$') {
				full_alt = full_alt + "$";
			}

			string converted = ConvertSinglePatternToLike(full_alt, column_expr);
			if (converted.empty()) {
				return "";	// Give up on entire pattern
			}
			sql_parts.push_back(converted);
		}

		string result = "(";
		for (size_t j = 0; j < sql_parts.size(); j++) {
			if (j > 0) {
				result += " OR ";
			}
			result += sql_parts[j];
		}
		result += ")";
		return result;
	}

	// Phase 2: Single pattern (no alternation)
	return ConvertSinglePatternToLike(pattern, column_expr);
}

}  // namespace duckdb

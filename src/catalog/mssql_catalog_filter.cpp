#include "catalog/mssql_catalog_filter.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Pattern Validation
//===----------------------------------------------------------------------===//

string MSSQLCatalogFilter::ValidatePattern(const string &pattern) {
	if (pattern.empty()) {
		return "";  // Empty pattern is valid (means no filter)
	}
	try {
		std::regex test_regex(pattern, std::regex::icase);
		(void)test_regex;
		return "";  // Valid
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
// Regex → SQL LIKE Conversion
//===----------------------------------------------------------------------===//

string MSSQLCatalogFilter::TryRegexToSQLLike(const string &pattern, const string &column_expr) {
	if (pattern.empty()) {
		return "";
	}

	// Walk through the regex pattern and try to build a LIKE pattern
	string like_pattern;
	bool anchored_start = false;
	bool anchored_end = false;
	size_t i = 0;
	size_t len = pattern.size();

	// Check for start anchor
	if (i < len && pattern[i] == '^') {
		anchored_start = true;
		i++;
	}

	while (i < len) {
		char c = pattern[i];

		// Check for end anchor
		if (c == '$' && i == len - 1) {
			anchored_end = true;
			i++;
			continue;
		}

		// Wildcard: .* → %
		if (c == '.' && i + 1 < len && pattern[i + 1] == '*') {
			like_pattern += '%';
			i += 2;
			continue;
		}

		// Wildcard: .+ → _%
		if (c == '.' && i + 1 < len && pattern[i + 1] == '+') {
			like_pattern += "_%";
			i += 2;
			continue;
		}

		// Single char wildcard: . → _
		if (c == '.') {
			like_pattern += '_';
			i++;
			continue;
		}

		// Escape sequences: \. \* etc → literal character
		if (c == '\\' && i + 1 < len) {
			char next = pattern[i + 1];
			// Known escape sequences that produce a literal
			if (next == '.' || next == '*' || next == '+' || next == '?' ||
				next == '[' || next == ']' || next == '(' || next == ')' ||
				next == '{' || next == '}' || next == '|' || next == '^' ||
				next == '$' || next == '\\') {
				// Escape LIKE special chars
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
			// Unknown escape (e.g. \d, \w) — not convertible
			return "";
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

	// Build the final LIKE expression
	// If not anchored at start, add leading %
	// If not anchored at end, add trailing %
	string full_like;
	if (!anchored_start) {
		full_like = "%" + like_pattern;
	} else {
		full_like = like_pattern;
	}
	if (!anchored_end) {
		full_like += "%";
	}

	// Escape single quotes in the LIKE pattern for SQL injection safety
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

} // namespace duckdb

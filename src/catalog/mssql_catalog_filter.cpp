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

// True when c is one of the characters of set. A NUL byte is in no set.
static bool IsOneOf(char c, const char *set) {
	for (; *set; set++) {
		if (*set == c) {
			return true;
		}
	}
	return false;
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

// Append a character LIKE is to match as itself: LIKE's own special chars go in
// brackets. Literal text can bring `%` and `_` here, an escape sequence only `[`
// (a bare `[` is a non-convertible construct, and `\%` / `\_` are unknown escapes).
static void AppendLikeLiteral(string &like_pattern, char c) {
	if (IsOneOf(c, "%_[")) {
		like_pattern += '[';
		like_pattern += c;
		like_pattern += ']';
	} else {
		like_pattern += c;
	}
}

// The `.` at pattern[i]. Returns the characters consumed.
static size_t ConvertDot(const string &pattern, size_t i, string &like_pattern) {
	char next = i + 1 < pattern.size() ? pattern[i + 1] : '\0';
	// .* → %
	if (next == '*') {
		like_pattern += '%';
		return 2;
	}
	// .+ → _%
	if (next == '+') {
		like_pattern += "_%";
		return 2;
	}
	// . → _
	like_pattern += '_';
	return 1;
}

// The backslash at pattern[i], which is not the last character.
// Escape sequences: \. \* etc → literal character
// Returns the characters consumed, 0 for one that is not convertible.
static size_t ConvertEscape(const string &pattern, size_t i, string &like_pattern) {
	char next = pattern[i + 1];
	if (!IsOneOf(next, ".*+?[](){}|^$\\")) {
		return 0;  // Unknown escape (\d, \w, etc.)
	}
	AppendLikeLiteral(like_pattern, next);
	return 2;
}

// Convert the construct at pattern[i] and append it to like_pattern.
// Returns the characters consumed, 0 for one that is not convertible.
static size_t ConvertConstruct(const string &pattern, size_t i, string &like_pattern) {
	char c = pattern[i];
	if (c == '.') {
		return ConvertDot(pattern, i, like_pattern);
	}
	// A backslash that ends the pattern is literal text
	if (c == '\\' && i + 1 < pattern.size()) {
		return ConvertEscape(pattern, i, like_pattern);
	}
	// Non-convertible regex constructs
	if (IsOneOf(c, "[({|?+*")) {
		return 0;
	}
	// Escape LIKE special chars in literal text
	AppendLikeLiteral(like_pattern, c);
	return 1;
}

// Add the `%` of each end the regex left unanchored.
static string OpenUnanchoredEnds(const string &like_pattern, bool anchored_start, bool anchored_end) {
	// A wildcard the pattern already produced makes the anchor's own redundant:
	// `^tbl_.*` converts to `tbl[_]%` and then, being unanchored at the end,
	// would collect a second `%`. `%%` matches the same set as `%`, so this is
	// cosmetic — but it is generated SQL that a user reads.
	string full_like;
	if (!anchored_start && (like_pattern.empty() || like_pattern.front() != '%')) {
		full_like = "%" + like_pattern;
	} else {
		full_like = like_pattern;
	}
	if (!anchored_end && (full_like.empty() || full_like.back() != '%')) {
		full_like += "%";
	}
	return full_like;
}

// Convert a single regex pattern (no alternation) to a SQL LIKE expression.
// Returns "column LIKE '...'" or empty string if not convertible.
static string ConvertSinglePatternToLike(const string &pattern, const string &column_expr) {
	if (pattern.empty()) {
		return "";
	}

	string like_pattern;
	bool anchored_start = pattern[0] == '^';
	bool anchored_end = false;
	size_t i = anchored_start ? 1 : 0;
	size_t len = pattern.size();

	while (i < len) {
		// Only as the last character is `$` the anchor (after a backslash it never gets here)
		if (pattern[i] == '$' && i == len - 1) {
			anchored_end = true;
			break;
		}
		size_t consumed = ConvertConstruct(pattern, i, like_pattern);
		if (consumed == 0) {
			return "";
		}
		i += consumed;
	}

	string full_like = OpenUnanchoredEnds(like_pattern, anchored_start, anchored_end);
	// Escape single quotes for SQL injection safety
	return column_expr + " LIKE '" + EscapeSQLLiteral(full_like) + "'";
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
	for (char c : s) {
		if (IsOneOf(c, ".*+?[](){}|\\")) {
			return false;
		}
	}
	return true;
}

namespace {

// The alternatives of a pattern, and the anchors around the group they were in:
// Case A produced group anchors; Case B has per-alternative anchors
struct RegexAlternation {
	vector<string> alternatives;
	bool group_start = false;
	bool group_end = false;
};

}  // namespace

// Case A: ^(alt1|alt2|...)$ — anchored group
// The alternatives are whatever SplitAlternatives collected, also when it answered false.
static void SplitAnchoredGroup(const string &pattern, RegexAlternation &alternation) {
	size_t start = 0;
	size_t end = pattern.size();

	if (start < end && pattern[start] == '^') {
		alternation.group_start = true;
		start++;
	}
	if (end > start && pattern[end - 1] == '$') {
		alternation.group_end = true;
		end--;
	}

	if (end - start >= 2 && pattern[start] == '(' && pattern[end - 1] == ')') {
		string inner = pattern.substr(start + 1, end - start - 2);
		SplitAlternatives(inner, alternation.alternatives);
	}

	if (alternation.alternatives.empty()) {
		alternation.group_start = false;
		alternation.group_end = false;
	}
}

// ^(a|b|c)$ — each alternative is a literal if IsPlainLiteral
static bool GroupLiterals(const vector<string> &alternatives, vector<string> &literal_values) {
	for (const auto &alt : alternatives) {
		if (!IsPlainLiteral(alt)) {
			return false;
		}
		literal_values.push_back(alt);
	}
	return true;
}

// ^a$|^b$|^c$ — each alternative must be ^literal$
static bool AnchoredLiterals(const vector<string> &alternatives, vector<string> &literal_values) {
	for (const auto &alt : alternatives) {
		if (alt.size() < 3 || alt.front() != '^' || alt.back() != '$') {
			return false;
		}
		string inner = alt.substr(1, alt.size() - 2);
		if (!IsPlainLiteral(inner)) {
			return false;
		}
		literal_values.push_back(inner);
	}
	return true;
}

// Check for exact-match literals → optimize to IN(...)
// True when every alternative is one, with them in literal_values. A group
// anchored at one end only has none.
static bool ExactLiterals(const RegexAlternation &alternation, vector<string> &literal_values) {
	if (alternation.group_start && alternation.group_end) {
		return GroupLiterals(alternation.alternatives, literal_values);
	}
	if (!alternation.group_start && !alternation.group_end) {
		return AnchoredLiterals(alternation.alternatives, literal_values);
	}
	return false;
}

// column IN ('a', 'b', 'c')
static string InList(const string &column_expr, const vector<string> &literal_values) {
	string in_list;
	for (size_t j = 0; j < literal_values.size(); j++) {
		if (j > 0) {
			in_list += ", ";
		}
		in_list += "'" + EscapeSQLLiteral(literal_values[j]) + "'";
	}
	return column_expr + " IN (" + in_list + ")";
}

// Apply group anchors to each alternative
static string AnchorAlternative(const string &alt, const RegexAlternation &alternation) {
	string full_alt = alt;
	if (alternation.group_start && !full_alt.empty() && full_alt[0] != '^') {
		full_alt = "^" + full_alt;
	}
	if (alternation.group_end && !full_alt.empty() && full_alt.back() != '$') {
		full_alt = full_alt + "$";
	}
	return full_alt;
}

// General case: convert each alternative to LIKE, combine with OR
static string AlternativesToLike(const RegexAlternation &alternation, const string &column_expr) {
	string result = "(";
	for (size_t j = 0; j < alternation.alternatives.size(); j++) {
		string converted =
			ConvertSinglePatternToLike(AnchorAlternative(alternation.alternatives[j], alternation), column_expr);
		if (converted.empty()) {
			return "";	// Give up on entire pattern
		}
		if (j > 0) {
			result += " OR ";
		}
		result += converted;
	}
	result += ")";
	return result;
}

//===----------------------------------------------------------------------===//
// Regex → SQL Conversion (public API)
//===----------------------------------------------------------------------===//

string MSSQLCatalogFilter::TryRegexToSQLLike(const string &pattern, const string &column_expr) {
	if (pattern.empty()) {
		return "";
	}

	// Phase 1: Try alternation patterns (e.g. ^(a|b|c)$ or a|b|c)
	RegexAlternation alternation;
	SplitAnchoredGroup(pattern, alternation);

	// Case B: top-level pipe without outer group (e.g. ^a$|^b$|^c$)
	if (alternation.alternatives.empty()) {
		SplitAlternatives(pattern, alternation.alternatives);
	}

	if (alternation.alternatives.size() >= 2) {
		vector<string> literal_values;
		if (ExactLiterals(alternation, literal_values) && !literal_values.empty()) {
			return InList(column_expr, literal_values);
		}
		return AlternativesToLike(alternation, column_expr);
	}

	// Phase 2: Single pattern (no alternation)
	return ConvertSinglePatternToLike(pattern, column_expr);
}

}  // namespace duckdb

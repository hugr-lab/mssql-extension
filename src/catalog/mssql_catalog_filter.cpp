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

} // namespace duckdb

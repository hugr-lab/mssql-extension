#pragma once

#include <regex>
#include <string>
#include "duckdb/common/string.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLCatalogFilter - Regex-based visibility filter for schemas and tables
//
// Filters are compiled at ATTACH time and applied during catalog discovery.
// Uses std::regex_search (partial match) with case-insensitive matching.
// For exact match, use anchors: ^dbo$
//===----------------------------------------------------------------------===//

class MSSQLCatalogFilter {
public:
	MSSQLCatalogFilter() = default;

	// Set filters from string patterns. Throws on invalid regex.
	void SetSchemaFilter(const string &pattern);
	void SetTableFilter(const string &pattern);

	// Validate a regex pattern string (returns empty string if valid, error message if invalid)
	static string ValidatePattern(const string &pattern);

	// Check if a name matches the filter (returns true if no filter set)
	bool MatchesSchema(const string &name) const;
	bool MatchesTable(const string &name) const;

	// Check if any filter is active
	bool HasSchemaFilter() const;
	bool HasTableFilter() const;
	bool HasFilters() const;

	// Get pattern strings (for display/debugging)
	const string &GetSchemaPattern() const;
	const string &GetTablePattern() const;

private:
	string schema_pattern_;
	string table_pattern_;
	std::regex schema_regex_;
	std::regex table_regex_;
	bool has_schema_filter_ = false;
	bool has_table_filter_ = false;
};

} // namespace duckdb

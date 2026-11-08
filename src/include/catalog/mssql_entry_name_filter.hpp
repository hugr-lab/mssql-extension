//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// catalog/mssql_entry_name_filter.hpp
//
// The names a pushed table_name filter can match (DuckDB #27930).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {
class TableFilter;
class LogicalType;

namespace mssql {

//! The most single-table lookups one system-catalog scan makes: names x the schemas DuckDB asks
//! about (MSSQLSchemaEntry::Scan; review of #541). Without a schema_name filter that is every
//! schema, and 16 names on 50 schemas were 800 round trips against the 50 listings it replaced.
//! Past it the scan lists, as before. A miss is remembered, so a repeat costs nothing.
constexpr idx_t MAX_LOOKED_UP_NAMES = 16;

//! The names `filter` can accept, when it is a set of equalities on the column -- `= c`, `IN (…)`,
//! an OR of those, or an AND holding one, also under the optional wrapper DuckDB pushes an IN or an
//! OR in -- and there are at most MAX_LOOKED_UP_NAMES of them.
//! False when it is anything else (LIKE, lower(), a range), and the caller lists. The names are a
//! superset of the matches where the filter is an AND: DuckDB re-checks every entry it is given.
bool EntryNameCandidates(optional_ptr<const TableFilter> filter, const LogicalType &column_type, vector<string> &names);

}  // namespace mssql
}  // namespace duckdb

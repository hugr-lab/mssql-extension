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

//! The most names a filter is answered by looking each one up, in each schema DuckDB asks about:
//! with no schema_name filter that is every schema, so 16 names on 50 schemas are 800 single-table
//! loads the first time (a miss is remembered after). Past it one listing of the schema costs less.
constexpr idx_t MAX_LOOKED_UP_NAMES = 16;

//! The names `filter` can accept, when it is a set of equalities on the column -- `= c`, `IN (…)`,
//! an OR of those, or an AND holding one, also under the optional wrapper DuckDB pushes an IN or an
//! OR in -- and there are at most MAX_LOOKED_UP_NAMES of them.
//! False when it is anything else (LIKE, lower(), a range), and the caller lists. The names are a
//! superset of the matches where the filter is an AND: DuckDB re-checks every entry it is given.
bool EntryNameCandidates(optional_ptr<const TableFilter> filter, const LogicalType &column_type, vector<string> &names);

}  // namespace mssql
}  // namespace duckdb

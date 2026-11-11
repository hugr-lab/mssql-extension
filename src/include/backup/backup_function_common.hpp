//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_function_common.hpp
//
// Spec 085: what the backup file functions share -- the path argument (one file, a glob, or a list of
// every family of a media set), the record form of a catalog's datetime values, converted the one
// way TYPE mssql converts them, and a column's refusal and DuckDB type (the catalog's functions and
// the scan's one answer). [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_catalog_model.hpp"
#include "backup/backup_sql_variant.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

//! The paths of a function's first argument (VARCHAR or LIST(VARCHAR)); NULL, a NULL element or an
//! empty list is an InvalidInputException naming `function`.
vector<std::string> PathsOf(const Value &value, const char *function);

//! A datetime as a record stores it (u32 ticks of 1/300 s, then i32 days since 1900-01-01) as TYPE
//! mssql reads one off the wire (days, then ticks: DateTimeEncoding::ConvertDatetime). NULL TIMESTAMP
//! unless 8 bytes.
Value RecordDatetimeValue(const std::string &record_form);
//! A smalldatetime as a record stores it (u16 minutes, then u16 days) through ConvertSmallDatetime.
//! NULL TIMESTAMP unless 4 bytes.
Value RecordSmallDatetimeValue(const std::string &record_form);

//! A uniqueidentifier's 16 bytes in SQL Server's order (the first three groups little-endian) as a
//! UUID; a FormatError naming `what` unless 16 bytes.
Value GuidValue(const std::string &bytes, const std::string &what);

//! The value a catalog's sql_variant holds (a partition boundary, an added column's default), typed as
//! TYPE mssql reads its base type (D6: datetime2 by its scale TIMESTAMP_S / _MS / TIMESTAMP / _NS,
//! datetimeoffset TIMESTAMP_TZ at the UTC instant, decimal exact, money DECIMAL(19, 4), a char /
//! varchar through its collation's code page). A FormatError naming `what` for a value its type's
//! size does not fit, one outside its type's range (a time past midnight, a date past 9999-12-31, an
//! offset outside +-14:00), a time(7) / datetimeoffset(7) with a 100 ns digit (not cut to the
//! microsecond), a code page with no table or a base type this does not read.
Value SqlVariantValue(const SqlVariant &variant, const std::string &what);

//! Why the scan cannot read column `c` (spec 085 § Scope's refusals), or "". One predicate for
//! mssql_backup_tables' `columns.reason` and the scan's refusal by name (D13).
std::string ColumnRefusal(const CatalogColumn &c);

//! The DuckDB type TYPE mssql reads `c` as with native types off (D6): MapSQLServerTypeToDuckDB on
//! the system type underneath an alias. mssql_backup_tables' `columns.duckdb_type` and the scan's
//! output type. Throws for a type with none.
LogicalType ColumnDuckDBType(const CatalogColumn &c);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

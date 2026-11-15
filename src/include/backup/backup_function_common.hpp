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
#include "backup/backup_lsn.hpp"
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

//! An LSN in RESTORE HEADERONLY's numeric form (vlf, then block in 10 digits, then slot in 5), a
//! DECIMAL(25, 0): mssql_backup_info's LSNs and the scan's _mssql_page_lsn.
Value LsnValue(const Lsn &lsn);

//! How the scan reads column `c` (owner, 2026-11-11: no refusal by type). A type the reader converts
//! takes the DuckDB type TYPE mssql reads it as with native types off (D6: MapSQLServerTypeToDuckDB on
//! the system type underneath an alias); a type whose conversion is still to come -- sql_variant,
//! xml, the CLR types (geometry, geography, hierarchyid, a user's), vector, json, a type the reader
//! does not know, a text whose collation is newer than the code-page tables -- is its stored bytes as
//! they are, a BLOB (`raw`), until its decoder (086, together with the TDS path's). What is not a
//! type gap is not readable: a computed column that is not persisted (no stored value) and a sparse
//! column (its value is in the row's sparse vector, a storage format: 086). One answer for
//! mssql_backup_tables' `columns` and the scan (RecordStager).
struct ColumnReading {
	LogicalType type = LogicalType::BLOB;
	bool readable = true;
	bool raw = false;
	//! Why the column is raw or not readable; empty for a converted one.
	std::string note;
};
ColumnReading ReadingOf(const CatalogColumn &c);

//! Column `c` of table `t` at a glance (mssql_backup_columns, task 16d; owner, 2026-11-11), from
//! ReadingOf: `not_stored` (a computed column not persisted), `storage_format` (sparse, a column
//! set: 086), `stored_bytes` (a type whose conversion is still to come, read as its stored bytes) or
//! `converted` -- in row or off row alike since PR 3 (`off_row_pending` is gone); `reason` says why (empty when
//! converted), and that the table is not read when mssql_backup_tables says so.
struct ColumnState {
	const char *state = "converted";
	std::string reason;
};
ColumnState StateOf(const CatalogTable &t, const CatalogColumn &c);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

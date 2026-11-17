//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_scan_function.hpp
//
// Spec 085 PR 2d, task 16: mssql_backup_scan(path, schema, table) -- a user table's rows read from a
// full backup with no SQL Server, typed as TYPE mssql reads them (D6), with the virtual _mssql_page,
// _mssql_rid and _mssql_page_lsn (D8). Task 23: the same scan behind an attached backup's table
// entries. [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_catalog_model.hpp"
#include "backup/backup_opened.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/table_column.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

//! A column the scan does not read: its name, the type it would have, why.
struct UnreadColumn {
	std::string name;
	LogicalType type;
	std::string reason;
};

//! A table's columns as the scan reads them: the readable ones (catalog column ids, types, names, in
//! the table's order) and the unread ones, virtual columns of the scan whose naming refuses the query.
struct ScanColumns {
	std::vector<int32_t> column_ids;
	vector<LogicalType> types;
	vector<std::string> names;
	std::vector<UnreadColumn> unread;
};
ScanColumns ScanColumnsOf(const CatalogTable &table);
//! The scan's virtual columns: the unread ones, then D8's (_mssql_page, _mssql_rid, _mssql_page_lsn,
//! _mssql_lob) -- what a table entry of an attached backup reports, so the binder's column ids are the
//! scan's.
virtual_column_map_t ScanVirtualColumns(const std::vector<UnreadColumn> &unread);

//! mssql_backup_scan bound on `schema`.`table` of `attached` (task 23): what the catalog's table entry
//! `entry` hands DuckDB. `version`: its number in the catalog's history now (task 24b), what a plan
//! deserialized finds it by; 0 when the history no longer has it.
TableFunction AttachedScanFunction(ClientContext &context, TableCatalogEntry &entry,
								   std::shared_ptr<const AttachedBackup> attached, uint64_t version,
								   const std::string &schema, const std::string &table,
								   unique_ptr<FunctionData> &bind_data);

void RegisterBackupScanFunction(ExtensionLoader &loader);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

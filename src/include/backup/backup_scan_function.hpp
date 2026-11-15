//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_scan_function.hpp
//
// Spec 085 PR 2d, task 16: mssql_backup_scan(path, schema, table) -- a user table's rows read from a
// full backup with no SQL Server, typed as TYPE mssql reads them (D6), with the virtual _mssql_page,
// _mssql_rid and _mssql_page_lsn (D8). [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace mssql {
namespace backup {

void RegisterBackupScanFunction(ExtensionLoader &loader);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

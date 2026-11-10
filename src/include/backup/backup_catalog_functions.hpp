//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_catalog_functions.hpp
//
// Spec 085 PR 2b step 7 (task 17): mssql_backup_tables(path), mssql_backup_views(path). [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace mssql {
namespace backup {

void RegisterBackupCatalogFunctions(ExtensionLoader &loader);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

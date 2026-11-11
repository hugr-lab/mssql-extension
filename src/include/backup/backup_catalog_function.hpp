//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_catalog_function.hpp
//
// Spec 085 PR 2b step 7b (task 17): mssql_backup_catalog(path [, base_table]). [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace mssql {
namespace backup {

void RegisterBackupCatalogFunction(ExtensionLoader &loader);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

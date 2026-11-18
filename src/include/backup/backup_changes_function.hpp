//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_changes_function.hpp
//
// Spec 085 task 24c (PR 4c-3, DQ-47): the changes between two states of a backup history --
// mssql_backup_schema_changes. [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace mssql {
namespace backup {

void RegisterBackupChangesFunctions(ExtensionLoader &loader);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

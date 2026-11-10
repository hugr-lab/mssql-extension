//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_info_function.hpp
//
// Spec 085 task 9: mssql_backup_info(path). [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace mssql {
namespace backup {

void RegisterBackupInfoFunction(ExtensionLoader &loader);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

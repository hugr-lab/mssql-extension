//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_snapshots_function.hpp
//
// Spec 085 task 24b (PR 4c-2, DQ-48): mssql_backup_snapshots(name or paths), the versions of a backup
// history, as ducklake_snapshots lists a lake's. [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace mssql {
namespace backup {

void RegisterBackupSnapshotsFunction(ExtensionLoader &loader);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

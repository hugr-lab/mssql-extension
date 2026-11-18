//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_snapshots_function.hpp
//
// Spec 085 task 24b (PR 4c-2, DQ-48): mssql_backup_snapshots(name or paths), the versions of a backup
// history, as ducklake_snapshots lists a lake's. [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_history.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <memory>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

void RegisterBackupSnapshotsFunction(ExtensionLoader &loader);

//! The history `paths` name: an attached backup's (its transaction's versions) when one path names one,
//! else the files' (ReadBackupHistory); `default_version` its version read without AT, `attached` (when
//! given) whether it is an attached backup's.
std::shared_ptr<const BackupHistory> HistoryOf(ClientContext &context, const std::vector<std::string> &paths,
											   uint64_t &default_version, bool *attached = nullptr);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_transactions_function.hpp
//
// Spec 085 task 27 (PR 5, owner 2026-11-20): the log a backup carries, read with no server --
// mssql_backup_transactions (one row per transaction of the tail: its state, its records, the tables it
// changed and what its changes did to them, D11's flags by transaction) and
// mssql_backup_transaction_records (one row per record: fn_dump_dblog's header fields, the table, and
// the record's contents as a VARIANT, a row image decoded into its columns where the reader can). A
// data backup's tail (FirstLSN to LastLSN), or a log backup's whole log. [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace mssql {
namespace backup {

void RegisterBackupTransactionsFunctions(ExtensionLoader &loader);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

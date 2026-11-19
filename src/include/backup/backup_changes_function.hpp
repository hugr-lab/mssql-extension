//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_changes_function.hpp
//
// Spec 085 task 24c (PR 4c-3, DQ-47): the changes between two states of a backup history --
// mssql_backup_schema_changes. [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_opened.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <memory>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

void RegisterBackupChangesFunctions(ExtensionLoader &loader);

//! The two states a change function compares (DQ-47): the full and the full with `diff_paths`, or
//! versions `start_value` / `end_value` of the history of `paths` (an attached backup's or the files';
//! a number, or a time; no start: the end's full alone; no end: the default version).
struct TwoStates {
	std::shared_ptr<OpenedCatalog> old_catalog, new_catalog;					 // none when `same`
	Value start = Value(LogicalType::BIGINT), end = Value(LogicalType::BIGINT);	 // NULL for diff :=
	bool same = false;															 // one state: no change
	bool history_break = false;	 // the database restored or recreated between them
	Lsn old_checkpoint;			 // the older state's last backup's checkpoint
	bool same_full = false;		 // the newer state a differential over the older state's full
	//! Every object's models where an ATTACH's filters narrow the states' (OpenBackupCatalog gives the
	//! attached backup's own): what a rowset's owner is found in, so a switch in from a table the filters
	//! leave out is still a reload. None without filters.
	std::shared_ptr<const CatalogModel> old_whole, new_whole;
};
TwoStates OpenTwoStates(ClientContext &context, const std::vector<std::string> &paths,
						const std::vector<std::string> &diff_paths, const Value &start_value, const Value &end_value,
						const char *function);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

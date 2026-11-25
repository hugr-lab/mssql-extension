//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_scan_internal.hpp
//
// Spec 085: what mssql_backup_scan (backup_scan_function.cpp) and mssql_backup_table_changes
// (backup_table_changes_function.cpp) share -- a table bound for reading, a partition's cursor, a row's
// _mssql_rid. Internal to those two files. [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_cd_record.hpp"
#include "backup/backup_lob.hpp"
#include "backup/backup_opened.hpp"
#include "backup/backup_record_stager.hpp"
#include "backup/backup_scan_function.hpp"
#include "duckdb/function/table_function.hpp"

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace duckdb {
class ExtensionLoader;
namespace mssql {
namespace backup {

struct ScanBindData : public TableFunctionData {
	//! The function the call named, for what it says: mssql_backup_scan, or mssql_backup_catalog (schema sys).
	std::string function = "mssql_backup_scan";
	vector<std::string> paths;
	vector<std::string> diff;  // task 24: a differential over the full on `paths`
	BackupAt at;			   // task 24b: a version of the history of `paths`
	std::string schema;
	std::string table;
	std::shared_ptr<OpenedCatalog> catalog;
	const CatalogTable *entry = nullptr;  // in catalog->Model()
	std::vector<int32_t> column_ids;	  // the scan's columns: catalog column ids, in output order
	vector<LogicalType> types;
	vector<std::string> names;		   // of column_ids
	std::vector<UnreadColumn> unread;  // UNREAD_COLUMN_START + i
	//! `max_threads` as the call gave it; 0 = not given (DuckDB's `threads`). Clamped at init.
	idx_t max_threads = 0;
	//! `max_memory` in bytes as the call gave it; 0 = not given (DuckDB's `memory_limit`). Clamped at
	//! init; half of it is the budget of the scan's reservations (D17).
	idx_t max_memory = 0;
	//! D11's `fuzzy` as the call (or the attached catalog) gave it; "" = the setting mssql_backup_fuzzy.
	std::string fuzzy;
	//! Spec 093 T8: `geometry_as` as the call (or the attached catalog) gave it; "" = 'geometry'.
	std::string geometry_as;
	TypeForms Forms() const {
		return TypeFormsOf(geometry_as, (function + ": geometry_as").c_str());
	}
	//! An attached backup's table entry the scan reads (task 23), for the binder, and that backup, read
	//! through its catalog, never found again by name; none for a file's (and for a plan deserialized,
	//! which resolves `paths` and checks the columns).
	optional_ptr<TableCatalogEntry> table_entry;
	std::shared_ptr<const AttachedBackup> attached;

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<ScanBindData>();
		copy->function = function;
		copy->paths = paths;
		copy->diff = diff;
		copy->at = at;
		copy->schema = schema;
		copy->table = table;
		copy->max_threads = max_threads;
		copy->max_memory = max_memory;
		copy->fuzzy = fuzzy;
		copy->geometry_as = geometry_as;
		copy->catalog = catalog;
		copy->entry = entry;
		copy->column_ids = column_ids;
		copy->types = types;
		copy->names = names;
		copy->unread = unread;
		copy->table_entry = table_entry;
		copy->attached = attached;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other_p) const override {
		const auto &other = other_p.Cast<ScanBindData>();
		return paths == other.paths && diff == other.diff && at.Same(other.at) && schema == other.schema &&
			   table == other.table && max_threads == other.max_threads && max_memory == other.max_memory &&
			   fuzzy == other.fuzzy && Forms() == other.Forms();
	}
	//! Every EXECUTE binds again: the file is read at bind.
	bool SupportStatementCache() const override {
		return false;
	}
};

// One partition's place in the scan: its layout, in-row unit, IAM-listed pages, and the rid's parts.
struct PartitionCursor {
	const CatalogPartition *partition = nullptr;
	const SysAllocUnit *unit = nullptr;
	LobUnits off_row;  // its LOB_DATA and ROW_OVERFLOW_DATA units, 0 where it has none
	//! In page order, each once; an attached backup's kept list (task 23), else this scan's.
	std::shared_ptr<const std::vector<PagePointer>> pages;
	std::vector<size_t> key;  // layout indexes of the clustered key, in ordkey order
	bool has_uniquifier = false;
	size_t uniquifier = 0;
};

//! What a chunk's reservation is held to (D17) when a vector of the table's widest declared rows would
//! need more: its chunks close on the bytes actually staged (RecordStager::LimitBytes), so a
//! varchar(8000) holding a few bytes still fills a vector, and the reservation stays this size.
constexpr uint64_t CHUNK_RESERVATION_BYTES = 8ull << 20;

//! Partition `p`'s cursor: its in-row unit, the pages its IAM chain lists (in page order, each once), and
//! the parts of its rows' _mssql_rid.
PartitionCursor BuildPartition(const ScanBindData &data, const CatalogPartition &p);

//! A row's _mssql_rid (D8).
std::string RowId(const ScanBindData &data, const PartitionCursor &cursor, const RowRecord &row, const RowLocation &at);

//! mssql_backup_table_changes / _insertions / _deletions, on `paths` (a path or a list of them).
void RegisterBackupTableChangesFunctions(ExtensionLoader &loader, const vector<LogicalType> &paths);

//! Spec 093 T8: the WARNINGs a statement's stagers' CLR notes call for (RecordStager::ClrNotes), each said
//! once a column and kind a statement -- a curve under GEOMETRY, bytes that are not MS-SSCLRT, bytes that
//! are not a hierarchyid's -- whichever task met it first. The values are NULL, their rows read (owner,
//! 2026-11-20).
class ClrWarnings {
public:
	void Say(ClientContext &context, const char *function, const std::vector<RecordStager::ClrNote> &notes);

private:
	std::mutex lock_;
	std::set<std::string> said_;
};

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

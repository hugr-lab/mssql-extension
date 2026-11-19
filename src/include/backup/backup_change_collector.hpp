//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_change_collector.hpp
//
// Spec 085 task 24d (PR 4c-3, DQ-47): what mssql_backup_table_changes (backup_table_changes_function.cpp)
// and its collector (backup_change_collector.cpp) share -- the call's bind data, a row to return, a
// state's side, the memory the pairing holds, and the collector's interface. Internal. [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_changes_function.hpp"
#include "backup/backup_opened.hpp"
#include "backup/backup_scan_internal.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

constexpr const char *CHANGES_FUNCTION = "mssql_backup_table_changes";

enum class ChangeKind : uint8_t { Insert, Delete, Preimage, Postimage };

//! Which rows a call returns: every change, the rows an importer inserts (inserts and postimages), or
//! those it deletes (deletes and preimages).
enum class ChangeListing : uint8_t { All, Insertions, Deletions };

//! A row to return: its state (the older or the newer), partition (of that state's table), place, and
//! what it is.
struct ChangeItem {
	bool old_side = false;
	size_t partition = 0;
	PagePointer page;
	uint16_t slot = 0;
	ChangeKind kind = ChangeKind::Insert;
	//! Its page's place among the collection's pages (ChangeCollector's work list, in page order): where a row
	//! that moved is returned from once paired (PlacedChanges).
	uint64_t work = 0;
};

//! One state's side of the comparison: the table, read as the scan reads it (BuildPartition, RowId).
struct ChangeSide {
	ScanBindData data;
	std::vector<PartitionCursor> cursors;  // by partition index of data.entry
};

struct TableChangesBindData : public TableFunctionData {
	vector<std::string> paths, diff;
	std::string schema, table;
	Value start, end;
	ChangeListing listing = ChangeListing::All;
	const char *function = CHANGES_FUNCTION;  // ChangesFunction(listing)
	TwoStates states;
	const CatalogTable *new_table = nullptr;  // in states.new_catalog's model
	const CatalogTable *old_table = nullptr;  // in states.old_catalog's; none: created (or a reload)
	bool all_inserts = false;				  // created, or to reload: its whole content inserted
	ScanColumns columns;					  // the newer state's
	std::vector<int32_t> old_staged;		  // the newer state's columns the older state has, in order
	std::vector<idx_t> old_outputs;			  // their places among `columns`

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<TableChangesBindData>();
		copy->paths = paths;
		copy->diff = diff;
		copy->schema = schema;
		copy->table = table;
		copy->start = start;
		copy->end = end;
		copy->listing = listing;
		copy->function = function;
		copy->states = states;
		copy->new_table = new_table;
		copy->old_table = old_table;
		copy->all_inserts = all_inserts;
		copy->columns = columns;
		copy->old_staged = old_staged;
		copy->old_outputs = old_outputs;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other_p) const override {
		const auto &other = other_p.Cast<TableChangesBindData>();
		auto same = [](const Value &a, const Value &b) {
			return a.IsNull() == b.IsNull() && (a.IsNull() || (a.type() == b.type() && a == b));
		};
		return paths == other.paths && diff == other.diff && schema == other.schema && table == other.table &&
			   same(start, other.start) && same(end, other.end) && listing == other.listing;
	}
	//! Every EXECUTE binds again: the files are read at bind.
	bool SupportStatementCache() const override {
		return false;
	}
};

//! What the pairing holds outside DuckDB's buffers (one partition of the rows left unpaired, and its map),
//! reserved in steps of STEP (DuckDB's memory_limit) and given back when the call ends; past what DuckDB
//! can give, refused by name.
class CollectMemory {
public:
	static constexpr uint64_t STEP = 4ull << 20;

	CollectMemory(BufferManager &buffers, std::string what) : buffers_(buffers), what_(std::move(what)) {}
	~CollectMemory() {
		if (reserved_ > 0) {
			buffers_.FreeReservedMemory(reserved_);
		}
	}
	CollectMemory(const CollectMemory &) = delete;
	CollectMemory &operator=(const CollectMemory &) = delete;

	void Add(uint64_t bytes) {
		if (bytes == 0) {
			return;
		}
		std::lock_guard<std::mutex> guard(lock_);
		used_ += bytes;
		while (used_ > reserved_) {
			try {
				buffers_.ReserveMemory(STEP);
			} catch (OutOfMemoryException &e) {
				throw OutOfMemoryException(
					"%s: the row changes held to pair them (%s) pass what DuckDB's memory_limit "
					"leaves: %s",
					what_, StringUtil::BytesToHumanReadableString(used_), e.what());
			}
			reserved_ += STEP;
		}
	}
	void Release(uint64_t bytes) {
		std::lock_guard<std::mutex> guard(lock_);
		used_ -= std::min(used_, bytes);
	}

private:
	BufferManager &buffers_;
	std::string what_;
	std::mutex lock_;
	uint64_t used_ = 0;
	uint64_t reserved_ = 0;
};

//! One state's side of a table, read as the scan reads it: its partitions' cursors.
ChangeSide MakeSide(const std::shared_ptr<OpenedCatalog> &catalog, const CatalogTable &entry);

//! A task's buffers, reused unit to unit (backup_change_collector.cpp).
struct CollectTask;

//! The row changes of a table between two states, collected over units of its pages by DuckDB's tasks:
//! each unit's changes returned by the task that collected it, the rows left unpaired paired at the end
//! (backup_change_collector.cpp).
class ChangeCollector {
public:
	ChangeCollector(ClientContext &context, const TableChangesBindData &data, const ChangeSide &now,
					const ChangeSide *before, CollectMemory &memory, bool sha256);
	~ChangeCollector();
	ChangeCollector(const ChangeCollector &) = delete;
	ChangeCollector &operator=(const ChangeCollector &) = delete;

	//! The units of 64 pages the work is cut into.
	size_t Units() const;
	//! A task's buffers.
	std::shared_ptr<CollectTask> NewTask() const;
	//! Unit `unit`'s changes into `items`, the rows it leaves unpaired kept by `task`.
	void CollectUnit(CollectTask &task, size_t unit, std::vector<ChangeItem> &items);
	//! A unit's rows left unpaired, put with the task's others.
	void HandIn(CollectTask &task);
	//! A task about to claim units.
	void Enter();
	//! The partitions the rows left unpaired are kept in (each task adds to every one).
	idx_t LonePartitions() const;
	//! A task that found no unit left: true for the one that pairs what every unit left.
	bool Leave(CollectTask &task);
	//! Every row left unpaired paired, into partitions by place (the pairing's task, once).
	void PlaceAll(ClientContext &context);
	//! The next list of the paired rows, held (`bytes`) until ReleaseItems; false when none is left.
	bool TakePlaced(ClientContext &context, std::vector<ChangeItem> &items, uint64_t &bytes);
	void ReleaseItems(uint64_t bytes);

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

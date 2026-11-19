//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_table_changes_function.cpp
//
// Spec 085 task 24d (PR 4c-3, DQ-47): mssql_backup_table_changes / _insertions / _deletions -- a table's
// row changes between two states of a backup history, read as mssql_backup_scan reads the table
// (backup_scan_internal.hpp). [DuckDB]
//===----------------------------------------------------------------------===//

#include "backup/backup_scan_function.hpp"
#include "backup/backup_scan_internal.hpp"

#include "backup/backup_changes_function.hpp"
#include "backup/backup_function_common.hpp"
#include "backup/backup_hash.hpp"
#include "backup/backup_opened.hpp"
#include "backup/backup_record_stager.hpp"
#include "backup/backup_reservation.hpp"
#include "backup/backup_schema_changes.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/radix_partitioning.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/types/column/partitioned_column_data.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "mssql_function_docs.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

//===----------------------------------------------------------------------===//
// Row changes (spec 085 task 24d, DQ-47): mssql_backup_table_changes / _insertions / _deletions
//===----------------------------------------------------------------------===//

constexpr const char *CHANGES_FUNCTION = "mssql_backup_table_changes";

enum class ChangeKind : uint8_t { Insert, Delete, Preimage, Postimage };

const char *ChangeName(ChangeKind kind) {
	switch (kind) {
	case ChangeKind::Insert:
		return "insert";
	case ChangeKind::Delete:
		return "delete";
	case ChangeKind::Preimage:
		return "update_preimage";
	default:
		return "update_postimage";
	}
}

//! Which rows a call returns: every change, the rows an importer inserts (inserts and postimages), or
//! those it deletes (deletes and preimages).
enum class ChangeListing : uint8_t { All, Insertions, Deletions };

//! The name a listing is called by, for its errors.
const char *ChangesFunction(ChangeListing listing);

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

//! A value's digest and its length.
struct ValueDigest {
	Digest digest;
	uint64_t length = 0;
	bool operator==(const ValueDigest &other) const {
		return length == other.length && digest == other.digest;
	}
};

//! A live row of a page: its slot, _mssql_rid, and its record's bytes as stored.
struct PageRow {
	uint16_t slot = 0;
	std::string rid;
	std::string bytes;
	//! Values stored off row (a table whose LOB pages changed, OffRowCheck): its value trees touch a
	//! changed LOB page; then their values' hashes, one per off-row column in layout order.
	bool touches = false;
	std::vector<ValueDigest> hashes;
	//! The record's bytes with its off-row pointers zeroed (a table with values stored off row): a shrink
	//! moves LOB pages and rewrites the pointers to them, the values the same (measured, diffs.sql).
	std::string masked;
};

//! A value's bytes hashed as they are assembled, with the length beside it.
class HashSink : public LobSink {
public:
	void Data(const uint8_t *data, size_t size) override {
		hash_.Update(data, size);
		length_ += size;
	}
	void UseSha256() {
		hash_.UseSha256();
	}
	ValueDigest Result() {
		ValueDigest v;
		v.digest = hash_.Finish();
		v.length = length_;
		return v;
	}

private:
	BytesHash hash_;
	uint64_t length_ = 0;
};

//! A value tree's pages, a map-only walk's (the leaves' places from their parents, not read).
class NodeSink : public LobSink {
public:
	void Data(const uint8_t *, size_t) override {}
	void Node(const LobNode &node) override {
		pages.push_back(node.page);
	}
	std::vector<PagePointer> pages;
};

constexpr uint64_t MAX_CHANGE_VALUE_BYTES = (1ull << 31) - 1;
constexpr uint32_t VERSION_TAG_BYTES = 14;	// a record's version tag, CD or not (E20, E22)

uint64_t PageKeyOf(uint16_t file, uint32_t page);

//! The off-row side of a comparison (a table whose LOB / row-overflow pages changed): the state's pages,
//! the partition's units, the changed LOB pages; whether to tell a row's trees touching them, and to
//! hash a touching row's values.
struct OffRowCheck {
	const LobPageFetch *fetch = nullptr;
	LobUnits units;
	const std::vector<uint64_t> *changed = nullptr;	 // PageKeyOf, sorted
	bool hash_always = false;						 // hash every row's values, touching or not
	bool sha256 = false;							 // mssql_backup_change_hash
};

//! Each off-row value of `row`, by layout index.
void ForEachOffRow(const RowsetLayout &layout, const RowRecord &row,
				   const std::function<void(size_t, const OffRowPointer &)> &fn) {
	for (size_t k = 0; k < layout.columns.size(); k++) {
		const PhysicalColumn &c = layout.columns[k];
		if (c.Dropped() || c.Uniquifier()) {
			continue;
		}
		const ValueRef v = row.Locate(layout, k);
		if (v.state == ValueState::OffRow) {
			fn(k, ParseOffRowPointer(v.data, v.size, layout.table + "." + c.name));
		}
	}
}

//! `row`'s off-row values checked as `check` says, into `out`.
void CheckOffRow(const RowsetLayout &layout, const RowRecord &row, const OffRowCheck &check, PageRow &out) {
	ForEachOffRow(layout, row, [&](size_t k, const OffRowPointer &pointer) {
		const std::string what = layout.table + "." + layout.columns[k].name;
		if (check.changed && !check.changed->empty() && !out.touches) {
			NodeSink nodes;
			WalkLob(*check.fetch, pointer, check.units, LobWalk::MapOnly, MAX_CHANGE_VALUE_BYTES, what, nodes);
			for (const auto &p : nodes.pages) {
				if (std::binary_search(check.changed->begin(), check.changed->end(), PageKeyOf(p.file, p.page))) {
					out.touches = true;
					break;
				}
			}
		}
	});
	if (out.touches || check.hash_always) {
		ForEachOffRow(layout, row, [&](size_t k, const OffRowPointer &pointer) {
			HashSink hash;
			if (check.sha256) {
				hash.UseSha256();
			}
			WalkLob(*check.fetch, pointer, check.units, LobWalk::Assemble, MAX_CHANGE_VALUE_BYTES,
					layout.table + "." + layout.columns[k].name, hash);
			out.hashes.push_back(hash.Result());
		});
	}
}

//! The records of `page` by where they lie: each live slot's offset, the next record's start (or the slot
//! array), its slot; by offset.
struct RecordSpan {
	uint16_t offset = 0, end = 0, slot = 0;
};

void RecordSpans(const Page &page, std::vector<RecordSpan> &out) {
	out.clear();
	const uint16_t slots = page.header.slot_count;
	if (static_cast<size_t>(slots) * 2 + PAGE_HEADER_SIZE > PAGE_SIZE) {
		return;	 // ReadPageRows names it
	}
	for (uint16_t slot = 0; slot < slots; slot++) {
		const size_t pos = PAGE_SIZE - 2 - 2 * static_cast<size_t>(slot);
		const uint16_t offset = static_cast<uint16_t>(page.bytes[pos] | (page.bytes[pos + 1] << 8));
		if (offset >= PAGE_HEADER_SIZE) {
			RecordSpan span;
			span.offset = offset;
			span.slot = slot;
			out.push_back(span);
		}
	}
	std::sort(out.begin(), out.end(), [](const RecordSpan &a, const RecordSpan &b) { return a.offset < b.offset; });
	const uint16_t slot_array = static_cast<uint16_t>(PAGE_SIZE - 2u * slots);
	for (size_t i = 0; i < out.size(); i++) {
		out[i].end = i + 1 < out.size() ? out[i + 1].offset : slot_array;
	}
}

//! The live rows of `page` (a data page of `cursor`'s unit, read) as the scan finds them: ghosts and
//! stubs skipped, a forwarded record at its place before it moved.
void ReadPageRows(const ScanBindData &data, const PartitionCursor &cursor, const Page &page, std::vector<PageRow> &out,
				  const OffRowCheck *check = nullptr, int32_t only_slot = -1, const std::vector<bool> *skip = nullptr) {
	out.clear();
	const RowsetLayout &layout = data.entry->Layout(*cursor.partition);
	const uint16_t slots = page.header.slot_count;
	if (static_cast<size_t>(slots) * 2 + PAGE_HEADER_SIZE > PAGE_SIZE) {
		throw FormatError("backup: page " + page.header.page_id.ToString() + " has " + std::to_string(slots) +
						  " slots, more than an 8 KB page holds");
	}
	// A compressed row's values read: its key's, and every column's when its off-row values are checked.
	std::vector<bool> needed(layout.columns.size(), check != nullptr);
	for (size_t k : cursor.key) {
		needed[k] = true;
	}
	if (cursor.has_uniquifier) {
		needed[cursor.uniquifier] = true;
	}
	// Each record bounded by the next one's start, as the scan bounds it; two slots naming one record
	// refused (review of #549).
	static thread_local std::vector<RecordSpan> spans;	// kept by the thread, page to page
	static thread_local std::vector<uint16_t> bound;
	RecordSpans(page, spans);
	bound.assign(slots, 0);
	for (size_t i = 0; i < spans.size(); i++) {
		if (i > 0 && spans[i].offset == spans[i - 1].offset) {
			throw FormatError("backup: page " + page.header.page_id.ToString() + " has slots " +
							  std::to_string(spans[i - 1].slot) + " and " + std::to_string(spans[i].slot) +
							  " naming one record");
		}
		bound[spans[i].slot] = spans[i].end;
	}
	CdRecord cd;
	CdRow cd_row;
	for (uint16_t slot = 0; slot < slots; slot++) {
		if ((only_slot >= 0 && slot != only_slot) || (skip && slot < skip->size() && (*skip)[slot])) {
			continue;
		}
		const size_t pos = PAGE_SIZE - 2 - 2 * static_cast<size_t>(slot);
		const uint16_t offset = static_cast<uint16_t>(page.bytes[pos] | (page.bytes[pos + 1] << 8));
		if (offset < PAGE_HEADER_SIZE) {
			continue;  // a freed slot
		}
		RowLocation at;
		at.page = page.header.page_id;
		at.slot = slot;
		RowRecord row;
		uint32_t record_end = 0;
		RecordView fixed;
		if (layout.compression == 1) {
			ReadCdRecord(page.bytes.data(), PAGE_SIZE, PAGE_SIZE - 2u * slots, offset, data.table.c_str(), cd);
			if (cd.kind == CdRecord::Kind::Ghost || cd.kind == CdRecord::Kind::Stub) {
				continue;
			}
			if (cd.kind == CdRecord::Kind::Forwarded) {
				cd.BackPointer(at.page.page, at.page.file, at.slot);
			}
			cd_row.Read(cd, layout, &needed);
			row = RowRecord::Compressed(cd, cd_row);
			record_end = cd.end;
		} else {
			fixed = ReadSlot(page.bytes.data(), PAGE_SIZE, slots, slot, data.table.c_str());
			if (fixed.Type() != RECORD_PRIMARY && fixed.Type() != RECORD_FORWARDED) {
				continue;
			}
			if (fixed.Type() == RECORD_FORWARDED) {
				at = ForwardedFrom(fixed, layout.table);
			}
			row = RowRecord::Fixed(fixed, RowVarCount(fixed, layout.table));
			record_end = fixed.End();
		}
		if (static_cast<size_t>(offset) + record_end > bound[slot]) {
			throw FormatError("backup: page " + page.header.page_id.ToString() + " has a record at offset " +
							  std::to_string(offset) + " that runs past the next record's start (" +
							  std::to_string(bound[slot]) + ")");
		}
		PageRow r;
		r.slot = slot;
		r.rid = RowId(data, cursor, row, at);
		// Compared without a version tag (row versioning adds and rewrites it, the values the same): its
		// 14 bytes at the end and the header bit that says it is there.
		const bool tagged = layout.compression == 1 ? (cd.header & CD_VERSION_TAG) != 0 : fixed.HasVersionTag();
		const uint32_t compared_end =
			tagged && record_end >= VERSION_TAG_BYTES ? record_end - VERSION_TAG_BYTES : record_end;
		r.bytes.assign(reinterpret_cast<const char *>(page.bytes.data()) + offset, compared_end);
		if (tagged && !r.bytes.empty()) {
			r.bytes[0] = static_cast<char>(static_cast<uint8_t>(r.bytes[0]) &
										   ~(layout.compression == 1 ? CD_VERSION_TAG : STATUS_VERSION_TAG));
		}
		if (check) {
			CheckOffRow(layout, row, *check, r);
			// The pointers zeroed where they lie in the record: in place for a record as stored; a compressed
			// one's values are copied out of it (CdRow), a pointer's bytes found again in its long data.
			r.masked = r.bytes;
			const uint8_t *start = page.bytes.data() + offset;
			ForEachOffRow(layout, row, [&](size_t, const OffRowPointer &pointer) {
				size_t at_byte = static_cast<size_t>(pointer.bytes - start);
				if (pointer.bytes < start || at_byte + pointer.size > r.masked.size()) {
					at_byte = r.masked.find(std::string(reinterpret_cast<const char *>(pointer.bytes), pointer.size));
				}
				if (at_byte != std::string::npos && at_byte + pointer.size <= r.masked.size()) {
					std::fill(r.masked.begin() + at_byte, r.masked.begin() + at_byte + pointer.size, '\0');
				}
			});
		}
		out.push_back(std::move(r));
	}
}

//! Records two versions of a page share unchanged: the same bytes at the same offset are the same row
//! (its key is in them) and the same values -- a write moves nothing it does not rewrite. Marks their
//! slots in `old_same` / `new_same`, so only the others are read for their _mssql_rid. A heap's (`same_slot`)
//! must lie in the same slot too: its rid is its place.
void SameRecords(const Page &old_page, const Page &new_page, bool same_slot, std::vector<bool> &old_same,
				 std::vector<bool> &new_same) {
	static thread_local std::vector<RecordSpan> old_spans, new_spans;  // kept by the thread
	RecordSpans(old_page, old_spans);
	RecordSpans(new_page, new_spans);
	old_same.assign(old_page.header.slot_count, false);
	new_same.assign(new_page.header.slot_count, false);
	size_t o = 0;
	for (const auto &n : new_spans) {
		while (o < old_spans.size() && old_spans[o].offset < n.offset) {
			o++;
		}
		if (o == old_spans.size()) {
			break;
		}
		const auto &old_span = old_spans[o];
		if (old_span.offset == n.offset && old_span.end == n.end && (!same_slot || old_span.slot == n.slot) &&
			std::memcmp(old_page.bytes.data() + n.offset, new_page.bytes.data() + n.offset, n.end - n.offset) == 0) {
			old_same[old_span.slot] = true;
			new_same[n.slot] = true;
		}
	}
}

//! A page of a state's table and its partition index; a state's are sorted by page.
struct PageOwner {
	uint64_t key = 0;  // file << 32 | page
	uint32_t partition = 0;
	bool operator<(const PageOwner &other) const {
		return key < other.key;
	}
};
using PageOwners = std::vector<PageOwner>;

uint64_t PageKeyOf(uint16_t file, uint32_t page) {
	return (static_cast<uint64_t>(file) << 32) | page;
}

PageOwners OwnersOf(const ChangeSide &side) {
	PageOwners owners;
	for (size_t i = 0; i < side.cursors.size(); i++) {
		for (const auto &p : *side.cursors[i].pages) {
			PageOwner o;
			o.key = PageKeyOf(p.file, p.page);
			o.partition = static_cast<uint32_t>(i);
			owners.push_back(o);
		}
	}
	std::sort(owners.begin(), owners.end());
	for (size_t i = 1; i < owners.size(); i++) {
		if (owners[i].key == owners[i - 1].key) {
			throw FormatError("backup: page " + std::to_string(owners[i].key >> 32) + ":" +
							  std::to_string(owners[i].key & 0xFFFFFFFFu) + " of " + side.data.schema + "." +
							  side.data.table + " is listed by two of its partitions' IAM chains");
		}
	}
	return owners;
}

//! The owner of `page` among `owners`, or null.
const PageOwner *FindOwner(const PageOwners &owners, uint16_t file, uint32_t page) {
	PageOwner probe;
	probe.key = PageKeyOf(file, page);
	auto it = std::lower_bound(owners.begin(), owners.end(), probe);
	return it != owners.end() && it->key == probe.key ? &*it : nullptr;
}

ChangeSide MakeSide(const std::shared_ptr<OpenedCatalog> &catalog, const CatalogTable &entry) {
	ChangeSide side;
	side.data.catalog = catalog;
	side.data.entry = &entry;
	side.data.schema = entry.schema_name;
	side.data.table = entry.name;
	for (const auto &p : entry.partitions) {
		side.cursors.push_back(BuildPartition(side.data, p));
	}
	return side;
}

//! The table's LOB / row-overflow pages written since the older state (`changed` the page filter, then
//! their LSNs). Measured (lob_changes.sql): a change to a value stored off row rewrites LOB pages only,
//! the data row's page and LSN untouched -- the data pages alone would miss it, so a row whose value tree
//! touches one of these is compared by its values too.
std::vector<uint64_t> ChangedOffRowPages(const ChangeSide &now, const std::function<bool(const PagePointer &)> &changed,
										 const Lsn &checkpoint) {
	std::vector<uint64_t> pages;  // PageKeyOf, sorted
	const PageStore &store = *now.data.catalog->pages->store;
	Page page;
	for (const auto &u : now.data.entry->units) {
		if (u.unit.type != 2 && u.unit.type != 3) {
			continue;
		}
		for (const auto &p : UnitPages(store, u.unit)) {
			if (!changed(p)) {
				continue;
			}
			// An IAM chain lists its extents whole: a page of one that is not live holds nothing.
			if (!store.Read(p.file, p.page, page)) {
				continue;
			}
			if (!(page.header.lsn <= checkpoint)) {
				pages.push_back(PageKeyOf(p.file, p.page));
			}
		}
	}
	std::sort(pages.begin(), pages.end());
	pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
	return pages;
}

void AddChange(std::vector<ChangeItem> &items, bool old_side, size_t partition, const PagePointer &page,
			   const PageRow &row, ChangeKind kind) {
	ChangeItem item;
	item.old_side = old_side;
	item.partition = partition;
	item.page = page;
	item.slot = row.slot;
	item.kind = kind;
	items.push_back(std::move(item));
}

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

//! A row left unpaired on its page, as kept to pair across the table: where it lies (it is read again from
//! there) and digests of what it holds, not its bytes.
struct LoneRow {
	ChangeItem item;
	std::string rid;
	Digest record;
	bool has_masked = false;  // a table with values stored off row: its record with the pointers zeroed
	Digest masked;
	bool touches = false;  // its value trees touch a changed LOB page: `values` are its values' digests
	std::vector<ValueDigest> values;
};

//! DuckDB's radix partitions, a partition's allocator released once it is taken (the allocators are shared by
//! every task's partitions, so moving a collection out frees none of its blocks).
class ReleasablePartitions : public RadixPartitionedColumnData {
public:
	ReleasablePartitions(ClientContext &context, vector<LogicalType> types, idx_t radix_bits)
		: RadixPartitionedColumnData(context, std::move(types), radix_bits, 0) {}
	void Release(idx_t i) {
		if (i < partitions.size()) {
			partitions[i].reset();
		}
		if (i < allocators->allocators.size()) {
			allocators->allocators[i].reset();
		}
	}
};

//! The rows left unpaired, every unit's, in partitions by a hash of _mssql_rid: DuckDB's own partitioned
//! collections (its hash join's), in buffers DuckDB evicts to its temp_directory past memory_limit. The
//! pairing takes one partition at a time.
class LoneRows {
public:
	LoneRows(ClientContext &context, idx_t radix_bits, CollectMemory &memory)
		: radix_bits_(radix_bits),
		  data_(make_uniq<ReleasablePartitions>(context, Types(), radix_bits)),
		  memory_(memory) {}

	idx_t Partitions() const {
		return RadixPartitioning::NumberOfPartitions(radix_bits_);
	}

	//! The rows held, every partition's (once every unit is handed in).
	uint64_t Count() {
		uint64_t count = 0;
		for (auto &part : data_->GetPartitions()) {
			count += part ? part->Count() : 0;
		}
		return count;
	}

	//! A task's own partitions, appended to over its units and put with the others' when it is done (so
	//! a partition's chunks are filled, not cut at every unit).
	struct Appender {
		unique_ptr<PartitionedColumnData> data;
		unique_ptr<PartitionedColumnDataAppendState> state;	 // pins a chunk's blocks of each partition
		DataChunk chunk;
	};

	//! A unit's rows, into the task's own partitions.
	void Add(Appender &appender, const std::vector<LoneRow> &rows) {
		if (rows.empty()) {
			return;
		}
		if (!appender.data) {
			appender.data = data_->CreateShared();
			appender.state = make_uniq<PartitionedColumnDataAppendState>();
			appender.data->InitializeAppendState(*appender.state);
			appender.chunk.Initialize(Allocator::DefaultAllocator(), Types());
		}
		for (size_t from = 0; from < rows.size(); from += STANDARD_VECTOR_SIZE) {
			const idx_t count = std::min<idx_t>(STANDARD_VECTOR_SIZE, rows.size() - from);
			DataChunk &chunk = appender.chunk;
			chunk.Reset();
			auto *hash = FlatVector::GetDataMutable<hash_t>(chunk.data[0]);
			auto *rid = FlatVector::GetDataMutable<string_t>(chunk.data[1]);
			auto *old_side = FlatVector::GetDataMutable<bool>(chunk.data[2]);
			auto *partition = FlatVector::GetDataMutable<uint64_t>(chunk.data[3]);
			auto *file = FlatVector::GetDataMutable<uint16_t>(chunk.data[4]);
			auto *page = FlatVector::GetDataMutable<uint32_t>(chunk.data[5]);
			auto *slot = FlatVector::GetDataMutable<uint16_t>(chunk.data[6]);
			auto *record = FlatVector::GetDataMutable<string_t>(chunk.data[7]);
			auto *masked = FlatVector::GetDataMutable<string_t>(chunk.data[8]);
			auto &masked_valid = FlatVector::ValidityMutable(chunk.data[8]);
			auto *touches = FlatVector::GetDataMutable<bool>(chunk.data[9]);
			auto *values = FlatVector::GetDataMutable<string_t>(chunk.data[10]);
			auto *work = FlatVector::GetDataMutable<uint64_t>(chunk.data[11]);
			for (idx_t k = 0; k < count; k++) {
				const LoneRow &r = rows[from + k];
				hash[k] = Hash(r.rid.data(), r.rid.size());
				rid[k] = StringVector::AddStringOrBlob(chunk.data[1], r.rid);
				old_side[k] = r.item.old_side;
				partition[k] = r.item.partition;
				file[k] = r.item.page.file;
				page[k] = r.item.page.page;
				slot[k] = r.item.slot;
				record[k] = StringVector::AddStringOrBlob(
					chunk.data[7], string_t(reinterpret_cast<const char *>(r.record.bytes), sizeof(r.record.bytes)));
				if (r.has_masked) {
					masked[k] = StringVector::AddStringOrBlob(
						chunk.data[8],
						string_t(reinterpret_cast<const char *>(r.masked.bytes), sizeof(r.masked.bytes)));
				} else {
					masked_valid.SetInvalid(k);
				}
				touches[k] = r.touches;
				std::string encoded;
				for (const auto &v : r.values) {
					encoded.append(reinterpret_cast<const char *>(v.digest.bytes), sizeof(v.digest.bytes));
					encoded.append(reinterpret_cast<const char *>(&v.length), sizeof(v.length));
				}
				values[k] = StringVector::AddStringOrBlob(chunk.data[10], encoded);
				work[k] = r.item.work;
			}
			chunk.SetChildCardinality(count);
			appender.data->Append(*appender.state, chunk);
		}
	}

	//! A task's partitions, put with every other task's (any task, at once).
	void Finish(Appender &appender) {
		if (!appender.data) {
			return;
		}
		appender.data->FlushAppendState(*appender.state);
		data_->Combine(*appender.data);
		appender.state.reset();
		appender.data.reset();
		appender.chunk.Destroy();
	}

	//! Partition `i`'s rows, taken out (once every unit is handed in) and its blocks freed; `bytes` what
	//! they hold out of DuckDB's buffers, as rows and in a map by rid, reserved before they are built.
	std::vector<LoneRow> Take(idx_t i, uint64_t &bytes) {
		std::vector<LoneRow> rows;
		bytes = 0;
		auto &partitions = data_->GetPartitions();
		if (i >= partitions.size() || !partitions[i]) {
			return rows;
		}
		auto part = std::move(partitions[i]);
		bytes = part->Count() * (sizeof(LoneRow) + 96);
		memory_.Add(bytes);
		rows.reserve(part->Count());
		for (auto &chunk : part->Chunks()) {
			const auto *rid = FlatVector::GetData<string_t>(chunk.data[1]);
			const auto *old_side = FlatVector::GetData<bool>(chunk.data[2]);
			const auto *partition = FlatVector::GetData<uint64_t>(chunk.data[3]);
			const auto *file = FlatVector::GetData<uint16_t>(chunk.data[4]);
			const auto *page = FlatVector::GetData<uint32_t>(chunk.data[5]);
			const auto *slot = FlatVector::GetData<uint16_t>(chunk.data[6]);
			const auto *record = FlatVector::GetData<string_t>(chunk.data[7]);
			const auto *masked = FlatVector::GetData<string_t>(chunk.data[8]);
			const auto &masked_valid = FlatVector::Validity(chunk.data[8]);
			const auto *touches = FlatVector::GetData<bool>(chunk.data[9]);
			const auto *values = FlatVector::GetData<string_t>(chunk.data[10]);
			const auto *work = FlatVector::GetData<uint64_t>(chunk.data[11]);
			for (idx_t k = 0; k < chunk.size(); k++) {
				LoneRow r;
				r.item.work = work[k];
				r.rid = rid[k].GetString();
				r.item.old_side = old_side[k];
				r.item.partition = partition[k];
				r.item.page.file = file[k];
				r.item.page.page = page[k];
				r.item.slot = slot[k];
				std::memcpy(r.record.bytes, record[k].GetData(), sizeof(r.record.bytes));
				r.has_masked = masked_valid.RowIsValid(k);
				if (r.has_masked) {
					std::memcpy(r.masked.bytes, masked[k].GetData(), sizeof(r.masked.bytes));
				}
				r.touches = touches[k];
				const char *v = values[k].GetData();
				const size_t entry = sizeof(Digest::bytes) + sizeof(uint64_t);
				for (size_t at = 0; at + entry <= values[k].GetSize(); at += entry) {
					ValueDigest d;
					std::memcpy(d.digest.bytes, v + at, sizeof(d.digest.bytes));
					std::memcpy(&d.length, v + at + sizeof(d.digest.bytes), sizeof(d.length));
					r.values.push_back(d);
				}
				const uint64_t more = 2 * r.rid.size() + r.values.size() * sizeof(ValueDigest);
				memory_.Add(more);
				bytes += more;
				rows.push_back(std::move(r));
			}
		}
		part.reset();
		data_->Release(i);
		return rows;
	}

private:
	static vector<LogicalType> Types() {
		return {LogicalType::HASH,		LogicalType::BLOB,	   LogicalType::BOOLEAN,   LogicalType::UBIGINT,
				LogicalType::USMALLINT, LogicalType::UINTEGER, LogicalType::USMALLINT, LogicalType::BLOB,
				LogicalType::BLOB,		LogicalType::BOOLEAN,  LogicalType::BLOB,	   LogicalType::UBIGINT};
	}

	idx_t radix_bits_;
	unique_ptr<ReleasablePartitions> data_;
	CollectMemory &memory_;
};

//! The rows that moved, once paired, in partitions by their page's place (ChangeItem::work, in the work list's
//! page order: a partition is a run of pages), DuckDB's partitioned collections again, evicted to its
//! temp_directory past memory_limit. Returned a partition at a time, sorted by place (KeepListed), so a page is
//! read once for its rows: in the pairing's partitions (by rid) every page holding a moved row was read again
//! for each of them (a rebuild's 16 times; in a compressed differential its block decoded again).
class PlacedChanges {
public:
	PlacedChanges(ClientContext &context, idx_t radix_bits, uint64_t works, CollectMemory &memory)
		: radix_bits_(radix_bits),
		  works_(std::max<uint64_t>(works, 1)),
		  data_(make_uniq<ReleasablePartitions>(context, Types(), radix_bits)),
		  memory_(memory) {
		appending_ = data_->CreateShared();
		state_ = make_uniq<PartitionedColumnDataAppendState>();
		appending_->InitializeAppendState(*state_);
		chunk_.Initialize(Allocator::DefaultAllocator(), Types());
	}

	idx_t Partitions() const {
		return RadixPartitioning::NumberOfPartitions(radix_bits_);
	}

	//! Paired items, from one task (the pairing's). (Items a unit pairs on its own pages are returned with the
	//! unit's and never come here: their `work` is not set.)
	void Append(const std::vector<ChangeItem> &items) {
		const uint64_t partitions = Partitions();
		const idx_t shift = RadixPartitioning::Shift(radix_bits_);
		for (size_t from = 0; from < items.size(); from += STANDARD_VECTOR_SIZE) {
			const idx_t count = std::min<idx_t>(STANDARD_VECTOR_SIZE, items.size() - from);
			chunk_.Reset();
			auto *hash = FlatVector::GetDataMutable<hash_t>(chunk_.data[0]);
			auto *old_side = FlatVector::GetDataMutable<bool>(chunk_.data[1]);
			auto *partition = FlatVector::GetDataMutable<uint64_t>(chunk_.data[2]);
			auto *file = FlatVector::GetDataMutable<uint16_t>(chunk_.data[3]);
			auto *page = FlatVector::GetDataMutable<uint32_t>(chunk_.data[4]);
			auto *slot = FlatVector::GetDataMutable<uint16_t>(chunk_.data[5]);
			auto *kind = FlatVector::GetDataMutable<uint8_t>(chunk_.data[6]);
			auto *work = FlatVector::GetDataMutable<uint64_t>(chunk_.data[7]);
			for (idx_t k = 0; k < count; k++) {
				const ChangeItem &item = items[from + k];
				// The partition is the hash's radix bits: a run of the work list each.
				const uint64_t at = std::min<uint64_t>(partitions - 1, item.work * partitions / works_);
				hash[k] = static_cast<hash_t>(at) << shift;
				old_side[k] = item.old_side;
				partition[k] = item.partition;
				file[k] = item.page.file;
				page[k] = item.page.page;
				slot[k] = item.slot;
				kind[k] = static_cast<uint8_t>(item.kind);
				work[k] = item.work;
			}
			chunk_.SetChildCardinality(count);
			appending_->Append(*state_, chunk_);
		}
	}

	//! Every item appended.
	void Finish() {
		appending_->FlushAppendState(*state_);
		data_->Combine(*appending_);
		state_.reset();	 // its pinned blocks, every partition's
		appending_.reset();
		chunk_.Reset();	 // reused for the scans
	}

	//! The next items, at most `most`, a partition at a time: a partition larger than that (its moved rows on few
	//! pages) in pieces, each its own list (its pages read once a piece). `bytes` what they hold out of DuckDB's
	//! buffers, reserved as they are built; false when every partition is returned.
	bool TakeNext(size_t most, std::vector<ChangeItem> &items, uint64_t &bytes) {
		items.clear();
		bytes = 0;
		auto &partitions = data_->GetPartitions();
		while (items.size() < most) {
			if (!current_) {
				if (next_ >= partitions.size()) {
					break;
				}
				current_index_ = next_++;
				current_ = std::move(partitions[current_index_]);
				if (!current_) {
					continue;
				}
				current_->InitializeScan(scan_);
			}
			if (!current_->Scan(scan_, chunk_)) {
				current_.reset();
				data_->Release(current_index_);
				if (!items.empty()) {
					break;	// a list of one partition
				}
				continue;
			}
			const uint64_t more = chunk_.size() * sizeof(ChangeItem);
			memory_.Add(more);
			bytes += more;
			const auto *old_side = FlatVector::GetData<bool>(chunk_.data[1]);
			const auto *partition = FlatVector::GetData<uint64_t>(chunk_.data[2]);
			const auto *file = FlatVector::GetData<uint16_t>(chunk_.data[3]);
			const auto *page = FlatVector::GetData<uint32_t>(chunk_.data[4]);
			const auto *slot = FlatVector::GetData<uint16_t>(chunk_.data[5]);
			const auto *kind = FlatVector::GetData<uint8_t>(chunk_.data[6]);
			const auto *work = FlatVector::GetData<uint64_t>(chunk_.data[7]);
			for (idx_t k = 0; k < chunk_.size(); k++) {
				ChangeItem item;
				item.old_side = old_side[k];
				item.partition = partition[k];
				item.page.file = file[k];
				item.page.page = page[k];
				item.slot = slot[k];
				item.kind = static_cast<ChangeKind>(kind[k]);
				item.work = work[k];
				items.push_back(item);
			}
		}
		return !items.empty();
	}

private:
	static vector<LogicalType> Types() {
		return {LogicalType::HASH,	   LogicalType::BOOLEAN,   LogicalType::UBIGINT,  LogicalType::USMALLINT,
				LogicalType::UINTEGER, LogicalType::USMALLINT, LogicalType::UTINYINT, LogicalType::UBIGINT};
	}

	idx_t radix_bits_;
	uint64_t works_;
	unique_ptr<ReleasablePartitions> data_;
	unique_ptr<PartitionedColumnData> appending_;  // the pairing's own partitions, put in data_ at Finish
	unique_ptr<PartitionedColumnDataAppendState> state_;
	DataChunk chunk_;  // appended from, then scanned into
	// The partition being returned (TakeNext).
	unique_ptr<ColumnDataCollection> current_;
	ColumnDataScanState scan_;
	idx_t current_index_ = 0;
	idx_t next_ = 0;
	CollectMemory &memory_;
};

//! What a task found on a unit's pages.
struct CollectTask {
	size_t work = 0;  // the page of the work list it reads
	std::vector<ChangeItem> items;
	std::vector<LoneRow> lone;	  // the rows left unpaired on a unit's pages
	LoneRows::Appender appender;  // all its units' rows left unpaired
	BytesHash hash;
	// Its buffers, reused page to page.
	Page page, old_page;
	std::vector<PageRow> rows, old_rows;
	std::vector<bool> old_same, new_same;
};

//! A page of the collection: of the newer state's table, or one of the older state's the newer table no
//! longer has (freed, moved by a shrink).
struct CollectWork {
	bool old_only = false;
	size_t partition = 0;
	PagePointer page;
};

//! The changes of the table between the two states (DQ-47). Built at InitGlobal; its pages are claimed a
//! unit at a time by DuckDB's tasks of the call (`CollectUnit`, inside a BindFiles of both states), as the
//! scan's are, and each unit's changes returned by its task; the last task to find no unit left pairs
//! what every unit left (`PairPartition`). Each newer-state page that changed since the older state's checkpoint
//! (one the differential holds, when it lies over the older state's full) is read with its older version,
//! the records both hold unchanged at one offset skipped (the same bytes: one row, unchanged) and the
//! others paired by _mssql_rid; the older state's pages the newer table no longer has give their rows; a
//! row left unpaired on one page (moved, a page split) is paired across the table at the end.
class ChangeCollector {
public:
	ChangeCollector(ClientContext &context, const TableChangesBindData &data, const ChangeSide &now,
					const ChangeSide *before, CollectMemory &memory, bool sha256)
		: context_(context),
		  data_(data),
		  now_(now),
		  before_(before),
		  memory_(memory),
		  new_store_(*now.data.catalog->pages->store),
		  name_(data.schema + "." + data.table),
		  sha256_(sha256) {
		new_fetch_ = FetchOf(new_store_);
		for (size_t i = 0; i < now.cursors.size(); i++) {
			for (const auto &p : *now.cursors[i].pages) {
				CollectWork w;
				w.partition = i;
				w.page = p;
				work_.push_back(w);
			}
		}
		if (!before) {
			memory_.Add(work_.size() * sizeof(CollectWork));
			return;
		}
		old_store_ = before->data.catalog->pages->store.get();
		old_fetch_ = FetchOf(*old_store_);
		checkpoint_ = data.states.old_checkpoint;
		old_owners_ = OwnersOf(*before);
		const PageOwners new_owners = OwnersOf(now);
		// Held for the call: the work list and the older state's owners (and, briefly, the newer's).
		memory_.Add((old_owners_.size() + new_owners.size()) * sizeof(PageOwner));
		for (const auto &o : old_owners_) {
			if (!FindOwner(new_owners, static_cast<uint16_t>(o.key >> 32), static_cast<uint32_t>(o.key))) {
				CollectWork w;
				w.old_only = true;
				w.partition = o.partition;
				w.page.file = static_cast<uint16_t>(o.key >> 32);
				w.page.page = static_cast<uint32_t>(o.key);
				work_.push_back(w);
			}
		}
		memory_.Release(new_owners.size() * sizeof(PageOwner));
		changed_lob_ = ChangedOffRowPages(
			now, [this](const PagePointer &p) { return MayHaveChanged(p); }, checkpoint_);
		memory_.Add(work_.size() * sizeof(CollectWork) + changed_lob_.size() * sizeof(uint64_t));
		lob_changed_ = !changed_lob_.empty();
		// The rows left unpaired: at most every row of both states (an index rebuilt moves them all), about
		// 240 bytes each while paired; partitions enough that one is half of memory_limit (paired when every
		// other task is done) and no more: each task buffers a chunk of every partition, measured on 20
		// million rows under 1 GB: 16 partitions 4.9 s, 32 6.5 s, 64 9.7 s.
		const uint64_t rows =
			std::max<uint64_t>(static_cast<uint64_t>(std::max<int64_t>(0, data.new_table->row_count)) +
								   static_cast<uint64_t>(std::max<int64_t>(0, data.old_table->row_count)),
							   work_.size() * 64);	// a stale or unknown count: a page holds dozens of rows
		const uint64_t limit = BufferManager::GetBufferManager(context).GetMaxMemory();
		idx_t bits = 4;
		while (bits < 10 && ((rows * 240) >> bits) > limit / 2) {
			bits++;
		}
		lone_ = make_uniq<LoneRows>(context, bits, memory);
		// A table with values stored off row compares its rows' records with their pointers masked, and
		// their values where the masked records agree but the records do not (a shrink moved the LOB pages).
		for (const auto &c : now.cursors) {
			off_row_table_ = off_row_table_ || c.off_row.lob != 0 || c.off_row.row_overflow != 0;
		}
	}

	void Work(CollectTask &task, size_t index) {
		const CollectWork &w = work_[index];
		if (!before_) {
			InsertAll(task, w);
		} else if (w.old_only) {
			OldOnly(task, w);
		} else {
			Compare(task, w);
		}
	}

	//! Pages a unit takes, as the scan's.
	static constexpr size_t UNIT_PAGES = 64;

	size_t Units() const {
		return (work_.size() + UNIT_PAGES - 1) / UNIT_PAGES;
	}

	//! The changes of one unit's pages into `task` (its items, its rows left unpaired).
	void CollectUnit(CollectTask &task, size_t unit) {
		if (sha256_) {
			task.hash.UseSha256();
		}
		const size_t from = unit * UNIT_PAGES;
		for (size_t i = from; i < std::min(work_.size(), from + UNIT_PAGES); i++) {
			task.work = i;
			Work(task, i);
		}
	}

	//! A unit's rows left unpaired, kept by its task.
	void HandIn(CollectTask &task) {
		if (lone_) {
			lone_->Add(task.appender, task.lone);
		}
		task.lone.clear();
	}

	//! A task about to claim units.
	void Enter() {
		active_.fetch_add(1);
	}

	//! A task that found no unit left: its rows put with the others'; true for the one task that pairs
	//! them all (`PairPartition`), the last to leave -- every unit is claimed, and done by then.
	bool Leave(CollectTask &task) {
		if (lone_) {
			lone_->Finish(task.appender);
		}
		return active_.fetch_sub(1) == 1 && !paired_.exchange(true);
	}

	idx_t LonePartitions() const {
		return lone_ ? lone_->Partitions() : 0;
	}

	//! Rows that moved, once every unit is handed in, one partition at a time: one rid unpaired on both
	//! sides is one row, changed or not.
	std::vector<ChangeItem> PairPartition(idx_t partition) {
		uint64_t held = 0;
		std::vector<LoneRow> rows = lone_->Take(partition, held);  // reserved by Take
		std::vector<ChangeItem> items;
		{
			std::unordered_map<std::string, std::pair<int64_t, int64_t>> by_rid;  // old, new index into rows
			by_rid.reserve(rows.size());
			for (size_t k = 0; k < rows.size(); k++) {
				auto &entry = by_rid.emplace(rows[k].rid, std::make_pair(int64_t(-1), int64_t(-1))).first->second;
				int64_t &at = rows[k].item.old_side ? entry.first : entry.second;
				if (at >= 0) {
					Duplicate(rows[k].item.old_side ? "two pages of the older state" : "two pages of the newer state");
				}
				at = static_cast<int64_t>(k);
			}
			for (const auto &e : by_rid) {
				if (e.second.first < 0) {
					items.push_back(rows[e.second.second].item);
					items.back().kind = ChangeKind::Insert;
				} else if (e.second.second < 0) {
					items.push_back(rows[e.second.first].item);
					items.back().kind = ChangeKind::Delete;
				} else if (!SameLone(rows[e.second.first], rows[e.second.second])) {
					items.push_back(rows[e.second.first].item);
					items.back().kind = ChangeKind::Preimage;
					items.push_back(rows[e.second.second].item);
					items.back().kind = ChangeKind::Postimage;
				}
			}
		}
		memory_.Release(held);
		return items;
	}

	//! Every partition of the rows left unpaired paired, into partitions by place (PlacedChanges), once every
	//! unit is handed in: the pairing's task. Partitions enough that one is at most a 32nd of memory_limit
	//! (an item a row at most: a moved row's two versions are one item or none), and no more than an eighth of
	//! it holds the chunks of while they are added to (a block each).
	void PlaceAll(ClientContext &context) {
		auto &buffers = BufferManager::GetBufferManager(context);
		const uint64_t limit = buffers.GetMaxMemory();
		const uint64_t most = (lone_ ? lone_->Count() : 0) * sizeof(ChangeItem);
		idx_t bits = 0;
		while (bits < RadixPartitioning::MAX_RADIX_BITS && (most >> bits) > limit / 32 &&
			   (uint64_t(2) << bits) * buffers.GetBlockAllocSize() <= limit / 8) {
			bits++;
		}
		placed_ = make_uniq<PlacedChanges>(context, bits, work_.size(), memory_);
		for (idx_t i = 0; i < LonePartitions(); i++) {
			if (context.IsInterrupted()) {
				throw InterruptException();
			}
			placed_->Append(PairPartition(i));
		}
		placed_->Finish();
	}

	//! The next list of the paired rows (PlaceAll): at most a 32nd of memory_limit, held (`bytes`) until
	//! ReleaseItems; false when none is left.
	bool TakePlaced(ClientContext &context, std::vector<ChangeItem> &items, uint64_t &bytes) {
		const uint64_t limit = BufferManager::GetBufferManager(context).GetMaxMemory();
		const size_t most =
			static_cast<size_t>(std::max<uint64_t>(STANDARD_VECTOR_SIZE, limit / 32 / sizeof(ChangeItem)));
		return placed_ && placed_->TakeNext(most, items, bytes);
	}
	void ReleaseItems(uint64_t bytes) {
		memory_.Release(bytes);
	}

private:
	//! A value's fragments read through the state's pages; a long walk asks for interruption.
	LobPageFetch FetchOf(const PageStore &store) {
		ClientContext *context = &context_;
		const PageStore *at_store = &store;
		auto fetched = std::make_shared<std::atomic<uint32_t>>(0);
		return LobPageFetch([at_store, context, fetched](const PagePointer &at, Page &out) {
			if ((++*fetched & 255) == 0 && context->IsInterrupted()) {
				throw InterruptException();
			}
			at_store->ReadLive(at.file, at.page, out, "a fragment of a value stored off row");
		});
	}

	//! Changed since the older state: a page the differential holds when it lies over the older state's full
	//! (the rest is the full's, unchanged), every page otherwise; then by its LSN.
	bool MayHaveChanged(const PagePointer &p) const {
		if (!data_.states.same_full) {
			return true;
		}
		PageLocation location;
		return now_.data.catalog->pages->index.LocateOwn(p.file, p.page, location);
	}

	[[noreturn]] void Duplicate(const std::string &where) const {
		throw FormatError("backup: " + name_ + " holds two rows of one _mssql_rid on " + where);
	}

	//! The checks of a partition's newer rows: their value trees against the changed LOB pages (hashed when
	//! they touch one).
	OffRowCheck NewCheck(size_t partition) const {
		OffRowCheck check;
		check.fetch = &new_fetch_;
		check.units = now_.cursors[partition].off_row;
		check.changed = &changed_lob_;
		check.sha256 = sha256_;
		return check;
	}

	//! The older rows' check: the masked record only, no tree walked, no value hashed.
	OffRowCheck MaskOnly(size_t partition) const {
		OffRowCheck check;
		check.fetch = &old_fetch_;
		check.units = before_->cursors[partition].off_row;
		return check;
	}

	std::vector<ValueDigest> HashesOf(bool old_state, size_t partition, const PagePointer &p, uint16_t slot) const {
		const ChangeSide &side = old_state ? *before_ : now_;
		const PageStore &store = old_state ? *old_store_ : new_store_;
		OffRowCheck check;
		check.fetch = old_state ? &old_fetch_ : &new_fetch_;
		check.units = side.cursors[partition].off_row;
		check.hash_always = true;
		check.sha256 = sha256_;
		static thread_local Page one_page;	// kept by the thread: no page allocated a value
		std::vector<PageRow> one;
		if (ReadUnitDataPage(store, *side.cursors[partition].unit, p, name_, one_page)) {
			ReadPageRows(side.data, side.cursors[partition], one_page, one, &check, slot);
		}
		return one.empty() ? std::vector<ValueDigest>() : one.front().hashes;
	}

	//! SameRow over two rows left unpaired, by their digests.
	bool SameLone(const LoneRow &old_row, const LoneRow &new_row) const {
		if (old_row.record == new_row.record) {
			return !new_row.touches ||
				   HashesOf(true, old_row.item.partition, old_row.item.page, old_row.item.slot) == new_row.values;
		}
		if (!off_row_table_ || !old_row.has_masked || !new_row.has_masked || old_row.masked != new_row.masked) {
			return false;
		}
		const auto new_values = new_row.touches
									? new_row.values
									: HashesOf(false, new_row.item.partition, new_row.item.page, new_row.item.slot);
		return HashesOf(true, old_row.item.partition, old_row.item.page, old_row.item.slot) == new_values;
	}

	//! A row left unpaired on its page, kept by its digests.
	static void KeepLone(CollectTask &task, bool old_side, size_t partition, const PagePointer &page,
						 const PageRow &row) {
		LoneRow r;
		r.item.old_side = old_side;
		r.item.partition = partition;
		r.item.page = page;
		r.item.slot = row.slot;
		r.item.work = task.work;
		r.rid = row.rid;
		r.record = task.hash.Of(row.bytes);
		r.has_masked = !row.masked.empty();
		if (r.has_masked) {
			r.masked = task.hash.Of(row.masked);
		}
		r.touches = row.touches;
		r.values = row.hashes;
		task.lone.push_back(std::move(r));
	}

	//! Whether a row's two versions are one: the same record, unless its value tree touches a changed LOB
	//! page (then its values decide); a record that differs only in its off-row pointers decides by its
	//! values too.
	bool SameRow(const PageRow &old_row, size_t old_partition, const PagePointer &old_page, const PageRow &new_row,
				 size_t new_partition, const PagePointer &new_page) const {
		if (old_row.bytes == new_row.bytes) {
			return !new_row.touches || HashesOf(true, old_partition, old_page, old_row.slot) == new_row.hashes;
		}
		if (!off_row_table_ || old_row.masked.empty() || old_row.masked != new_row.masked) {
			return false;
		}
		const auto new_hashes =
			new_row.touches ? new_row.hashes : HashesOf(false, new_partition, new_page, new_row.slot);
		return HashesOf(true, old_partition, old_page, old_row.slot) == new_hashes;
	}

	//! Created, or to reload: every row of the newer state inserted.
	void InsertAll(CollectTask &task, const CollectWork &w) {
		if (!ReadUnitDataPage(new_store_, *now_.cursors[w.partition].unit, w.page, name_, task.page)) {
			return;
		}
		ReadPageRows(now_.data, now_.cursors[w.partition], task.page, task.rows);
		for (const auto &r : task.rows) {
			AddChange(task.items, false, w.partition, w.page, r, ChangeKind::Insert);
		}
	}

	//! An older page the newer table no longer has: its rows, to pair across the table.
	void OldOnly(CollectTask &task, const CollectWork &w) {
		if (!ReadUnitDataPage(*old_store_, *before_->cursors[w.partition].unit, w.page, name_, task.page)) {
			return;
		}
		const OffRowCheck mask_only = MaskOnly(w.partition);
		ReadPageRows(before_->data, before_->cursors[w.partition], task.page, task.old_rows,
					 off_row_table_ ? &mask_only : nullptr);
		for (const auto &r : task.old_rows) {
			KeepLone(task, true, w.partition, w.page, r);
		}
	}

	//! A page not written since the older state holds the same rows, but a row's value stored off row may
	//! have changed under it: when any LOB page changed, the row's two versions on the page (`read`: whether
	//! task.page already holds it).
	void Unwritten(CollectTask &task, const CollectWork &w, bool read) {
		const PageOwner *owner = FindOwner(old_owners_, w.page.file, w.page.page);
		if (!owner) {
			return;
		}
		if (!read && !ReadUnitDataPage(new_store_, *now_.cursors[w.partition].unit, w.page, name_, task.page)) {
			return;
		}
		const OffRowCheck check = NewCheck(w.partition);
		ReadPageRows(now_.data, now_.cursors[w.partition], task.page, task.rows, &check);
		for (const auto &r : task.rows) {
			if (!r.touches || HashesOf(true, owner->partition, w.page, r.slot) == r.hashes) {
				continue;
			}
			AddChange(task.items, true, owner->partition, w.page, r, ChangeKind::Preimage);	 // the same record
			AddChange(task.items, false, w.partition, w.page, r, ChangeKind::Postimage);
		}
	}

	void Compare(CollectTask &task, const CollectWork &w) {
		const size_t i = w.partition;
		const PagePointer &p = w.page;
		if (!MayHaveChanged(p)) {
			if (lob_changed_) {
				Unwritten(task, w, false);
			}
			return;
		}
		const bool live = ReadUnitDataPage(new_store_, *now_.cursors[i].unit, p, name_, task.page);
		if (live && task.page.header.lsn <= checkpoint_) {
			if (lob_changed_) {
				Unwritten(task, w, true);
			}
			return;	 // not written since the older state
		}
		task.rows.clear();
		task.old_rows.clear();
		size_t old_partition = 0;
		bool old_live = false;
		const PageOwner *owner = FindOwner(old_owners_, p.file, p.page);
		if (owner) {
			old_partition = owner->partition;
			old_live = ReadUnitDataPage(*old_store_, *before_->cursors[old_partition].unit, p, name_, task.old_page);
		}
		// The records both versions hold unchanged are skipped (no _mssql_rid built for them) -- unless a
		// value stored off row may have changed under one (its record the same, its LOB pages not).
		const bool skip_same = live && old_live && !lob_changed_;
		if (skip_same) {
			SameRecords(task.old_page, task.page, now_.data.entry->shape == TableShape::Heap, task.old_same,
						task.new_same);
		}
		const OffRowCheck check = NewCheck(i);
		if (live) {
			ReadPageRows(now_.data, now_.cursors[i], task.page, task.rows, off_row_table_ ? &check : nullptr, -1,
						 skip_same ? &task.new_same : nullptr);
		}
		if (old_live) {
			const OffRowCheck mask_only = MaskOnly(old_partition);
			ReadPageRows(before_->data, before_->cursors[old_partition], task.old_page, task.old_rows,
						 off_row_table_ ? &mask_only : nullptr, -1, skip_same ? &task.old_same : nullptr);
		}
		std::map<std::string, const PageRow *> old_by_rid;
		for (const auto &r : task.old_rows) {
			if (!old_by_rid.emplace(r.rid, &r).second) {
				Duplicate("page " + p.ToString() + " of the older state");
			}
		}
		std::unordered_set<std::string> new_rids;
		for (const auto &r : task.rows) {
			if (!new_rids.insert(r.rid).second) {
				Duplicate("page " + p.ToString() + " of the newer state");
			}
			auto o = old_by_rid.find(r.rid);
			if (o == old_by_rid.end()) {
				KeepLone(task, false, i, p, r);
				continue;
			}
			// The same record may hold another value off row (its tree's pages rewritten, measured), and
			// another record the same values (its pointers rewritten by a shrink).
			if (!SameRow(*o->second, old_partition, p, r, i, p)) {
				AddChange(task.items, true, old_partition, p, *o->second, ChangeKind::Preimage);
				AddChange(task.items, false, i, p, r, ChangeKind::Postimage);
			}
			old_by_rid.erase(o);
		}
		for (const auto &o : old_by_rid) {
			KeepLone(task, true, old_partition, p, *o.second);
		}
	}

	ClientContext &context_;
	const TableChangesBindData &data_;
	const ChangeSide &now_;
	const ChangeSide *before_;
	CollectMemory &memory_;
	const PageStore &new_store_;
	const PageStore *old_store_ = nullptr;
	std::string name_;
	bool sha256_;
	Lsn checkpoint_;
	LobPageFetch new_fetch_, old_fetch_;
	PageOwners old_owners_;
	std::vector<uint64_t> changed_lob_;	 // PageKeyOf, sorted
	bool lob_changed_ = false;
	bool off_row_table_ = false;
	std::vector<CollectWork> work_;
	std::unique_ptr<LoneRows> lone_;
	std::unique_ptr<PlacedChanges> placed_;
	std::atomic<size_t> active_{0};
	std::atomic<bool> paired_{false};
};

//! The call's state: the collection's units, claimed by DuckDB's tasks one at a time (as the scan's).
struct TableChangesState : public GlobalTableFunctionState {
	std::unique_ptr<ChangeSide> old_side, new_side;
	std::unique_ptr<CollectMemory> memory;
	std::unique_ptr<ChangeCollector> collector;
	std::atomic<size_t> next_unit{0};
	idx_t max_threads = 1;
	//! D17, as the scan's: a chunk's staging reserved for the call that stages it, by side (the newer, the
	//! older -- whose values are staged and then cast: twice), against half of memory_limit.
	std::unique_ptr<ReservationGate> gate;
	uint64_t chunk_bytes[2] = {0, 0};
	uint64_t chunk_allowance[2] = {0, 0};
	uint64_t virtual_row = 0;

	idx_t MaxThreads() const override {
		return max_threads;
	}
};

//! A task's state: the changes of the unit it claimed, returned a chunk at a time (`batch` its index; the
//! pairing's past every unit).
struct TableChangesLocal : public LocalTableFunctionState {
	CollectTask task;
	std::vector<ChangeItem> queue;	// by side, partition, page, slot
	size_t pos = 0;
	idx_t batch = 0;
	bool entered = false, done = false;	 // claimed units (Enter), and found none left (Leave)
	bool pairing = false;				 // the last to leave: it pairs what every unit left
	bool placed = false;				 // its rows paired into partitions by place (PlaceAll)
	idx_t place_next = 0;				 // the lists of them returned (each its own batch index)
	uint64_t held_items = 0;			 // the bytes of the partition it returns (TakePlaced)
	std::unique_ptr<RecordStager> new_stager, old_stager;
	LobPageFetch new_fetch, old_fetch;
	DataChunk old_values;  // the older state's columns, before their cast to the newer state's types
};

unique_ptr<FunctionData> TableChangesBind(ClientContext &context, TableFunctionBindInput &input,
										  vector<LogicalType> &return_types, vector<Identifier> &names,
										  ChangeListing listing) {
	auto data = make_uniq<TableChangesBindData>();
	data->listing = listing;
	data->function = ChangesFunction(listing);
	data->paths = PathsOf(input.inputs[0], data->function);
	data->diff = DiffPathsOf(input.named_parameters, data->function);
	if (input.inputs[1].IsNull() || input.inputs[2].IsNull()) {
		throw InvalidInputException("%s: the schema and the table must not be NULL", data->function);
	}
	data->schema = input.inputs[1].ToString();
	data->table = input.inputs[2].ToString();
	for (const auto &kv : input.named_parameters) {
		if (kv.first == "start_version") {
			data->start = kv.second;
		} else if (kv.first == "end_version") {
			data->end = kv.second;
		}
	}
	if (!data->diff.empty() && (!data->start.IsNull() || !data->end.IsNull())) {
		throw InvalidInputException(
			"%s: diff := names its two states (the full, and the full with the "
			"differential): start_version / end_version do not go beside it",
			data->function);
	}
	try {
		data->states = OpenTwoStates(context, std::vector<std::string>(data->paths.begin(), data->paths.end()),
									 std::vector<std::string>(data->diff.begin(), data->diff.end()), data->start,
									 data->end, data->function);
		// The newer state's table names the columns, whatever the states (one state: no rows).
		if (data->states.same) {
			BackupAt at;
			at.unit = "version";
			at.value = data->states.end;
			data->states.new_catalog.reset(
				OpenBackupCatalog(context, std::vector<std::string>(data->paths.begin(), data->paths.end()), true,
								  CatalogScope(), {}, at)
					.release());
		}
		const CatalogModel &new_model = data->states.new_catalog->Model();
		data->new_table = new_model.FindTable(data->schema, data->table);
		if (!data->new_table) {
			throw InvalidInputException("%s: the newer state has no table %s.%s (mssql_backup_tables lists them)",
										data->function, data->schema, data->table);
		}
		if (!data->new_table->decodable) {
			throw InvalidInputException("%s: %s.%s is not read: %s", data->function, data->schema, data->table,
										data->new_table->reason);
		}
		if (!data->states.same) {
			// The older state's table: by object id (a rename is a rename), by name across a break.
			const CatalogModel &old_model = data->states.old_catalog->Model();
			for (const auto &t : old_model.tables) {
				if (data->states.history_break
						? (t.schema_name == data->new_table->schema_name && t.name == data->new_table->name)
						: t.object_id == data->new_table->object_id) {
					data->old_table = &t;
				}
			}
			// A reload (DQ-47): the table recreated, a partition switched in, a break -- its whole content.
			bool reload = !data->old_table || data->states.history_break;
			if (!reload) {
				// Against the whole models: a partition switched in is a rowset another table owned.
				const ViewDefinition none = [](bool, const CatalogView &) { return std::string(); };
				for (const auto &c : CompareCatalogs(old_model, new_model, false, none)) {
					reload = reload || (c.change_type == "table_reload" && c.object_id == data->new_table->object_id);
				}
			}
			data->all_inserts = reload;
			if (!reload && !data->old_table->decodable) {
				throw InvalidInputException("%s: %s.%s is not read in the older state: %s", data->function,
											data->schema, data->table, data->old_table->reason);
			}
		}
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", data->function, e.what());
	}
	data->columns = ScanColumnsOf(*data->new_table);
	if (data->old_table && !data->all_inserts) {
		const ScanColumns old_columns = ScanColumnsOf(*data->old_table);
		std::set<int32_t> readable(old_columns.column_ids.begin(), old_columns.column_ids.end());
		for (idx_t i = 0; i < data->columns.column_ids.size(); i++) {
			if (readable.count(data->columns.column_ids[i])) {
				data->old_staged.push_back(data->columns.column_ids[i]);
				data->old_outputs.push_back(i);
			}
		}
	}
	names.emplace_back("change_type");
	return_types.push_back(LogicalType::VARCHAR);
	for (idx_t i = 0; i < data->columns.names.size(); i++) {
		names.emplace_back(data->columns.names[i]);
		return_types.push_back(data->columns.types[i]);
	}
	names.emplace_back("_mssql_rid");
	return_types.push_back(LogicalType::BLOB);
	return std::move(data);
}

unique_ptr<FunctionData> TableChangesAllBind(ClientContext &context, TableFunctionBindInput &input,
											 vector<LogicalType> &return_types, vector<Identifier> &names) {
	return TableChangesBind(context, input, return_types, names, ChangeListing::All);
}
unique_ptr<FunctionData> TableInsertionsBind(ClientContext &context, TableFunctionBindInput &input,
											 vector<LogicalType> &return_types, vector<Identifier> &names) {
	return TableChangesBind(context, input, return_types, names, ChangeListing::Insertions);
}
unique_ptr<FunctionData> TableDeletionsBind(ClientContext &context, TableFunctionBindInput &input,
											vector<LogicalType> &return_types, vector<Identifier> &names) {
	return TableChangesBind(context, input, return_types, names, ChangeListing::Deletions);
}

unique_ptr<GlobalTableFunctionState> TableChangesInit(ClientContext &context, TableFunctionInitInput &input) {
	const auto &data = input.bind_data->Cast<TableChangesBindData>();
	auto state = make_uniq<TableChangesState>();
	if (data.states.same) {
		return std::move(state);
	}
	try {
		BindFiles bound_new(data.states.new_catalog->Binding());
		BindFiles bound_old(data.states.old_catalog->Binding());
		state->new_side = make_uniq<ChangeSide>(MakeSide(data.states.new_catalog, *data.new_table));
		if (data.old_table && !data.all_inserts) {
			state->old_side = make_uniq<ChangeSide>(MakeSide(data.states.old_catalog, *data.old_table));
		}
		state->memory = make_uniq<CollectMemory>(BufferManager::GetBufferManager(context), data.function);
		Value hash;
		const bool sha256 = context.TryGetCurrentSetting("mssql_backup_change_hash", hash) && !hash.IsNull() &&
							StringUtil::CIEquals(hash.ToString(), "sha256");
		state->collector =
			make_uniq<ChangeCollector>(context, data, *state->new_side, state->old_side.get(), *state->memory, sha256);
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", data.function, e.what());
	}
	auto &buffers = BufferManager::GetBufferManager(context);
	// DuckDB's `threads`, but no more tasks than a quarter of memory_limit holds the buffers of: each task adds
	// the rows it leaves unpaired to every partition of them through a chunk it keeps (a block of their values
	// and one of their strings each), and the smaller memory_limit, the more partitions (measured: 16 tasks of
	// 64 partitions under 256 MB were out of memory at once; DuckDB's own operators bound their threads alike).
	const uint64_t per_task = state->collector->LonePartitions() * 2 * buffers.GetBlockAllocSize();
	idx_t tasks = std::max<idx_t>(1, context.db->NumberOfThreads());
	if (per_task > 0) {
		tasks = std::max<idx_t>(1, std::min<idx_t>(tasks, buffers.GetMaxMemory() / 4 / per_task));
	}
	state->max_threads = tasks;
	// A chunk's reservation, as the scan's (Init): the stager's fixed part and twice a row's variable
	// values plus a rid, for a vector of the widest rows, or CHUNK_RESERVATION_BYTES with the chunk
	// closing on what it staged; never past the budget unless one row is.
	const uint64_t budget = buffers.GetMaxMemory() / 2;
	state->virtual_row = sizeof(std::string) + 1 + 1700 + 4;
	auto size_side = [&](int side, const CatalogTable &table, const std::vector<int32_t> &staged, uint64_t copies) {
		RecordStager probe(table, staged);
		const uint64_t fixed = probe.FixedStagingBytes();
		const uint64_t row = 2 * probe.VarRowBytes() + state->virtual_row;
		const uint64_t vector = fixed + STANDARD_VECTOR_SIZE * row;
		uint64_t chunk = std::min(vector, std::max(CHUNK_RESERVATION_BYTES, fixed + row));
		chunk = std::min(chunk, std::max(budget / copies, fixed + row));
		state->chunk_allowance[side] = chunk < vector ? chunk - fixed : 0;
		state->chunk_bytes[side] = copies * chunk;
	};
	size_side(0, *data.new_table, data.columns.column_ids, 1);
	if (data.old_table && !data.all_inserts) {
		size_side(1, *data.old_table, data.old_staged, 2);
	}
	BufferManager *manager = &buffers;
	state->gate = make_uniq<ReservationGate>(
		budget,
		[manager](uint64_t bytes, std::string &why) {
			try {
				manager->ReserveMemory(bytes);
				return true;
			} catch (OutOfMemoryException &e) {
				why = e.what();
				return false;
			}
		},
		[manager](uint64_t bytes) { manager->FreeReservedMemory(bytes); });
	return std::move(state);
}

//! The changes of `listing`'s kinds, in the order a chunk takes them.
void KeepListed(const TableChangesBindData &data, std::vector<ChangeItem> &items) {
	if (data.listing != ChangeListing::All) {
		const bool insertions = data.listing == ChangeListing::Insertions;
		items.erase(std::remove_if(items.begin(), items.end(),
								   [insertions](const ChangeItem &item) {
									   return (item.kind == ChangeKind::Insert || item.kind == ChangeKind::Postimage) !=
											  insertions;
								   }),
					items.end());
	}
	std::sort(items.begin(), items.end(), [](const ChangeItem &a, const ChangeItem &b) {
		return std::make_tuple(a.old_side, a.partition, a.page.file, a.page.page, a.slot) <
			   std::make_tuple(b.old_side, b.partition, b.page.file, b.page.page, b.slot);
	});
}

//! The next changes to return, into local.queue: the rest of its unit, then a newly claimed unit's, then
//! (for the last task to find none left) the pairing's, paired into partitions by place and returned one at a time.
//! False when none are left.
bool NextChanges(ClientContext &context, const TableChangesBindData &data, TableChangesState &global,
				 TableChangesLocal &local) {
	while (local.pos >= local.queue.size()) {
		local.queue.clear();
		local.pos = 0;
		if (local.pairing) {
			global.collector->ReleaseItems(local.held_items);
			local.held_items = 0;
			if (!local.placed) {
				global.collector->PlaceAll(context);
				local.placed = true;
			}
			if (context.IsInterrupted()) {
				throw InterruptException();
			}
			if (!global.collector->TakePlaced(context, local.queue, local.held_items)) {
				local.pairing = false;
				return false;
			}
			local.batch = global.collector->Units() + local.place_next;
			local.place_next++;
			KeepListed(data, local.queue);
			continue;
		}
		if (local.done) {
			return false;
		}
		if (!local.entered) {
			global.collector->Enter();
			local.entered = true;
		}
		const size_t unit = global.next_unit.fetch_add(1);
		if (unit >= global.collector->Units()) {
			local.done = true;
			local.pairing = global.collector->Leave(local.task);
			continue;
		}
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		local.task.items.clear();
		global.collector->CollectUnit(local.task, unit);
		local.queue.swap(local.task.items);
		local.batch = unit;
		global.collector->HandIn(local.task);
		KeepListed(data, local.queue);
	}
	return true;
}

OperatorPartitionData TableChangesPartitionData(ClientContext &, TableFunctionGetPartitionInput &input) {
	if (input.partition_info.RequiresPartitionColumns()) {
		throw InternalException("%s: partition columns are not supported", CHANGES_FUNCTION);
	}
	return OperatorPartitionData(input.local_state->Cast<TableChangesLocal>().batch);
}

unique_ptr<LocalTableFunctionState> TableChangesInitLocal(ExecutionContext &execution, TableFunctionInitInput &input,
														  GlobalTableFunctionState *global_state) {
	const auto &data = input.bind_data->Cast<TableChangesBindData>();
	auto local = make_uniq<TableChangesLocal>();
	if (data.states.same) {
		return std::move(local);
	}
	ClientContext *context = &execution.client;
	auto fetch_for = [context](const OpenedCatalog &catalog) {
		const PageStore *store = catalog.pages->store.get();
		auto fetched = std::make_shared<uint32_t>(0);
		return LobPageFetch([store, context, fetched](const PagePointer &at, Page &out) {
			if ((++*fetched & 255) == 0 && context->IsInterrupted()) {
				throw InterruptException();
			}
			store->ReadLive(at.file, at.page, out, "a fragment of a value stored off row");
		});
	};
	const auto &global = global_state->Cast<TableChangesState>();
	local->new_stager = make_uniq<RecordStager>(*data.new_table, data.columns.column_ids);
	local->new_stager->LimitBytes(global.chunk_allowance[0], global.virtual_row);
	local->new_fetch = fetch_for(*data.states.new_catalog);
	if (data.old_table && !data.all_inserts) {
		local->old_stager = make_uniq<RecordStager>(*data.old_table, data.old_staged);
		local->old_stager->LimitBytes(global.chunk_allowance[1], global.virtual_row);
		local->old_fetch = fetch_for(*data.states.old_catalog);
		local->old_values.Initialize(Allocator::DefaultAllocator(), local->old_stager->Types());
	}
	return std::move(local);
}

void TableChangesExecute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	const auto &data = input.bind_data->Cast<TableChangesBindData>();
	auto &global = input.global_state->Cast<TableChangesState>();
	auto &local = input.local_state->Cast<TableChangesLocal>();
	if (data.states.same) {
		output.SetChildCardinality(0);
		return;
	}
	try {
		BindFiles bound_new(data.states.new_catalog->Binding());
		BindFiles bound_old(data.states.old_catalog->Binding());
		// A chunk's items: of one state and one partition (one layout), STANDARD_VECTOR_SIZE at most.
		if (!NextChanges(context, data, global, local)) {
			output.SetChildCardinality(0);
			return;
		}
		const size_t begin = local.pos;
		size_t end = begin;
		while (end < local.queue.size() && end - begin < STANDARD_VECTOR_SIZE &&
			   local.queue[end].old_side == local.queue[begin].old_side &&
			   local.queue[end].partition == local.queue[begin].partition) {
			end++;
		}
		const ChangeItem &first = local.queue[begin];
		const bool old_side = first.old_side;
		// The chunk's staging reserved for this call only (D17, as the scan's Execute).
		const uint64_t chunk_bytes = global.chunk_bytes[old_side ? 1 : 0];
		CallReservations held(*global.gate, [&context]() { return context.IsInterrupted(); });
		held.Acquire(chunk_bytes, chunk_bytes);
		const ChangeSide &side = old_side ? *global.old_side : *global.new_side;
		const PartitionCursor &cursor = side.cursors[first.partition];
		RecordStager &stager = old_side ? *local.old_stager : *local.new_stager;
		const RowsetLayout &layout = side.data.entry->Layout(*cursor.partition);
		stager.UseLayout(layout);
		stager.UseOffRow(old_side ? &local.old_fetch : &local.new_fetch, cursor.off_row);
		std::vector<Vector *> targets;
		if (old_side) {
			local.old_values.Reset();
			for (idx_t c = 0; c < local.old_values.ColumnCount(); c++) {
				targets.push_back(&local.old_values.data[c]);
			}
		} else {
			for (idx_t c = 0; c < data.columns.types.size(); c++) {
				targets.push_back(&output.data[1 + c]);
			}
		}
		stager.BeginChunk(targets);
		const PageStore &store = *side.data.catalog->pages->store;
		const std::string name = side.data.schema + "." + side.data.table;
		Page page;
		bool have = false;
		std::vector<bool> needed;
		stager.MarkNeeded(needed);
		for (size_t k : cursor.key) {
			needed[k] = true;
		}
		if (cursor.has_uniquifier) {
			needed[cursor.uniquifier] = true;
		}
		CdRecord cd;
		CdRow cd_row;
		std::vector<std::string> rids;
		std::vector<ChangeKind> kinds;
		size_t i = begin;
		for (; i < end && !stager.Full(); i++) {
			const ChangeItem &item = local.queue[i];
			if (!have || !(page.header.page_id == item.page)) {
				if (!ReadUnitDataPage(store, *cursor.unit, item.page, name, page)) {
					throw FormatError("backup: page " + item.page.ToString() + " of " + name +
									  " is no longer a data page of the table on its second read");
				}
				have = true;
			}
			RowLocation at;
			at.page = page.header.page_id;
			at.slot = item.slot;
			RowRecord row;
			RecordView fixed;
			if (layout.compression == 1) {
				const size_t pos = PAGE_SIZE - 2 - 2 * static_cast<size_t>(item.slot);
				const uint16_t offset = static_cast<uint16_t>(page.bytes[pos] | (page.bytes[pos + 1] << 8));
				ReadCdRecord(page.bytes.data(), PAGE_SIZE, PAGE_SIZE - 2u * page.header.slot_count, offset,
							 side.data.table.c_str(), cd);
				if (cd.kind == CdRecord::Kind::Forwarded) {
					cd.BackPointer(at.page.page, at.page.file, at.slot);
				}
				cd_row.Read(cd, layout, &needed);
				row = RowRecord::Compressed(cd, cd_row);
			} else {
				fixed =
					ReadSlot(page.bytes.data(), PAGE_SIZE, page.header.slot_count, item.slot, side.data.table.c_str());
				if (fixed.Type() == RECORD_FORWARDED) {
					at = ForwardedFrom(fixed, layout.table);
				}
				row = RowRecord::Fixed(fixed, RowVarCount(fixed, layout.table));
			}
			// A row's values stored off row reserved before it is staged (D17): a refusal with rows staged
			// closes the chunk before this row; with none, the chunk's reservation is taken again with it.
			const OffRowNeed need = stager.PrepareRow(row);
			// Named when refused: the row by its place, what it holds off row, what was asked.
			auto refused = [&](uint64_t bytes, const ReservationRefused &e) {
				return OutOfMemoryException(
					"%s: staging the row of %s.%s on page %s slot %u, whose values stored "
					"off row (%s) take %s of the %s reserved: %s",
					data.function, data.schema, data.table, item.page.ToString(), static_cast<unsigned>(item.slot),
					stager.DescribePrepared(), StringUtil::BytesToHumanReadableString(need.Total()),
					StringUtil::BytesToHumanReadableString(bytes), e.what());
			};
			bool fits = need.Total() == 0;
			if (!fits) {
				try {
					fits = held.TryAcquire(need.Total(), need.Ours());
				} catch (ReservationRefused &e) {
					throw refused(need.Total(), e);
				}
			}
			if (!fits) {
				if (stager.Count() > 0) {
					break;
				}
				held.ReleaseAll();
				try {
					held.Acquire(chunk_bytes + need.Total(), chunk_bytes + need.Ours());
				} catch (ReservationRefused &e) {
					throw refused(chunk_bytes + need.Total(), e);
				}
			}
			stager.Append(row);
			rids.push_back(RowId(side.data, cursor, row, at));
			kinds.push_back(item.kind);
		}
		local.pos = i;
		const idx_t count = stager.Count();
		stager.FinalizeChunk();
		if (old_side) {
			// The older state's values on the newer state's columns: by column id, cast to the newer types;
			// a column the older state has not is NULL.
			std::vector<bool> filled(data.columns.types.size(), false);
			for (idx_t c = 0; c < data.old_outputs.size(); c++) {
				Vector &target = output.data[1 + data.old_outputs[c]];
				// An older value the newer type cannot take (the column's type changed) is NULL, said once a
				// chunk; DuckDB's cast, not the server's ALTER COLUMN.
				std::string error;
				if (!VectorOperations::TryCast(context, local.old_values.data[c], target, count, &error)) {
					DUCKDB_LOG_WARNING(context, "%s: %s.%s: an older value of %s is NULL in the newer type: %s",
									   data.function, data.schema, data.table, data.columns.names[data.old_outputs[c]],
									   error);
				}
				filled[data.old_outputs[c]] = true;
			}
			for (idx_t c = 0; c < filled.size(); c++) {
				if (!filled[c]) {
					output.data[1 + c].SetVectorType(VectorType::CONSTANT_VECTOR);
					ConstantVector::SetNull(output.data[1 + c], true);
				}
			}
		}
		Vector &kind_vector = output.data[0];
		Vector &rid_vector = output.data[1 + data.columns.types.size()];
		auto *kind_out = FlatVector::GetDataMutable<string_t>(kind_vector);
		auto *rid_out = FlatVector::GetDataMutable<string_t>(rid_vector);
		for (idx_t i = 0; i < count; i++) {
			kind_out[i] = StringVector::AddString(kind_vector, ChangeName(kinds[i]));
			rid_out[i] = StringVector::AddStringOrBlob(rid_vector, rids[i]);
		}
		output.SetChildCardinality(count);
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", data.function, e.what());
	} catch (ReservationInterrupted &) {
		throw InterruptException();
	} catch (ReservationRefused &e) {
		throw OutOfMemoryException("%s: staging a chunk of %s.%s's changes: %s", data.function, data.schema, data.table,
								   e.what());
	}
}

const char *ChangesFunction(ChangeListing listing) {
	switch (listing) {
	case ChangeListing::Insertions:
		return "mssql_backup_table_insertions";
	case ChangeListing::Deletions:
		return "mssql_backup_table_deletions";
	default:
		return CHANGES_FUNCTION;
	}
}

TableFunction MakeTableChangesFunction(const char *name, const LogicalType &path, table_function_bind_t bind) {
	TableFunction f(name, {path, LogicalType::VARCHAR, LogicalType::VARCHAR}, TableChangesExecute, bind,
					TableChangesInit, TableChangesInitLocal);
	f.get_partition_data = TableChangesPartitionData;
	f.GetSignature().AddKeywordOnly("start_version", LogicalType::ANY, Value());
	f.GetSignature().AddKeywordOnly("end_version", LogicalType::ANY, Value());
	AddDiffParameter(f);
	return f;
}

}  // namespace

void RegisterBackupTableChangesFunctions(ExtensionLoader &loader, const vector<LogicalType> &paths) {
	struct ChangesFunction {
		const char *name;
		table_function_bind_t bind;
		const char *description;
	};
	const ChangesFunction CHANGES[] = {
		{"mssql_backup_table_changes", TableChangesAllBind,
		 "The row changes of a table between two states of a SQL Server backup history, as DuckLake's "
		 "table_changes names them: change_type insert, delete, update_preimage, update_postimage, then the "
		 "table's columns in the newer state's schema and _mssql_rid (the row's key: a clustered table's key, a "
		 "heap row's place). The newer state's pages written since the older state are read with their older "
		 "versions (a differential's own pages when it lies over the older state's full), and the older state's "
		 "pages the table no longer has; rows paired by _mssql_rid and compared as stored; when LOB pages changed, "
		 "every row's value tree is walked too. A created or reloaded table: its rows inserted. The states as "
		 "mssql_backup_schema_changes takes them."},
		{"mssql_backup_table_insertions", TableInsertionsBind,
		 "The rows an importer inserts for a table between two states of a backup history: the inserts and the "
		 "updates' new rows (mssql_backup_table_changes' insert and update_postimage)."},
		{"mssql_backup_table_deletions", TableDeletionsBind,
		 "The rows an importer deletes for a table between two states of a backup history: the deletes and the "
		 "updates' old rows (mssql_backup_table_changes' delete and update_preimage), their key and values."}};
	for (const auto &c : CHANGES) {
		TableFunctionSet changes(c.name);
		for (const auto &path : paths) {
			changes.AddFunction(MakeTableChangesFunction(c.name, path, c.bind));
		}
		mssql::RegisterDocumentedFunction(
			loader, std::move(changes),
			{{},
			 c.description,
			 {std::string("FROM ") + c.name + "('bk', 'dbo', 'orders', start_version := 41, end_version := 42)",
			  std::string("FROM ") + c.name +
				  "('/backups/sales_full.bak', 'dbo', 'orders', diff := '/backups/sales_diff.bak')"},
			 {"backup"}});
	}
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

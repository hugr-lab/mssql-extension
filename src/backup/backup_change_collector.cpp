//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_change_collector.cpp
//
// Spec 085 task 24d (PR 4c-3, DQ-47): the row changes between two states of a table, collected for
// mssql_backup_table_changes -- the pages that may have changed read with their older versions, records
// compared, the rows that moved paired across the table (DuckDB's partitioned collections, spilling).
// [DuckDB]
//===----------------------------------------------------------------------===//

#include "backup/backup_change_collector.hpp"
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

//! A task's last LOB page of each state, kept across its rows (the roots of the values of a page's rows
//! lie side by side): what their nodes are compared on.
struct LobNodePages {
	uint64_t new_key = UINT64_MAX, old_key = UINT64_MAX;  // PageKeyOf, UINT64_MAX none
	Page new_page, old_page;
	bool old_live = false;	// the older state has the page (old_page)
};

namespace {

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
	//! The row's values but its off-row ones (a table with values stored off row: ReadPageRows): a shrink
	//! moves LOB pages and rewrites the pointers to them, and drops a dropped column's bytes, the values the
	//! same (measured, diffs.sql and lob_changes.sql).
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
		nodes.push_back(node);
	}
	std::vector<LobNode> nodes;
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
	//! The older state's pages: a node on a changed page touches the change only when its record differs
	//! there (a page not live there: it does). None: every node on a changed page does.
	const PageStore *old_store = nullptr;
	LobNodePages *node_pages = nullptr;	 // with old_store: the task's
	bool hash_always = false;			 // hash every row's values, touching or not
	bool sha256 = false;				 // mssql_backup_change_hash
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
			// A node on a changed page whose record is the older state's at its place is that node: a value
			// whose every node is the same is the same value (each record holds its links, a leaf its bytes).
			// So a root on a page another value's change rewrote is no change (spec 085, the LOB tail:
			// measured on lob_changes.sql, a change rewrites its own root and leaves the roots beside it).
			const uint64_t unit = pointer.form == OffRowForm::RowOverflow ? check.units.row_overflow : check.units.lob;
			for (const auto &n : nodes.nodes) {
				if (!std::binary_search(check.changed->begin(), check.changed->end(),
										PageKeyOf(n.page.file, n.page.page))) {
					continue;
				}
				if (!check.old_store || !check.node_pages) {
					out.touches = true;
					break;
				}
				LobNodePages &pages = *check.node_pages;
				const uint64_t key = PageKeyOf(n.page.file, n.page.page);
				if (pages.new_key != key) {
					(*check.fetch)(n.page, pages.new_page);
					pages.new_key = key;
				}
				if (pages.old_key != key) {
					// Fetch, not Read: the older state may never have used the page (not live, or Pending in a
					// backup taken under load) -- the node is then another, never an error.
					pages.old_live =
						check.old_store->Fetch(n.page.file, n.page.page, pages.old_page) == PageState::Present;
					pages.old_key = key;
				}
				// A node the newer state holds no fragment for (a leaf the map-only walk did not read) touches
				// too: assembled, its value names what is wrong with it.
				const std::string record = LobFragmentRecord(pages.new_page, n.slot, unit);
				if (!pages.old_live || record.empty() || record != LobFragmentRecord(pages.old_page, n.slot, unit)) {
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
	RecordSpans(page.bytes.data(), PAGE_SIZE, slots, spans);
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
			// The row's values as the scan takes them, its off-row ones left to their digests: what decides
			// whether a row whose record differs is another row. A shrink rewrites the off-row pointers (the
			// values the same; measured, diffs.sql) and drops the bytes of a dropped column as it moves a
			// record (measured on lob_changes.sql: the records of a table whose varchar(max) was dropped lost
			// its variable column; review of #580), and a version tag comes and goes. Per column (dropped
			// ones and the uniquifier, which the _mssql_rid holds, left out), in the layout's order: its
			// column id (two states' layouts differ when the table's columns changed between them: a column
			// copied into a new one and dropped holds the same value under another id), its state -- NULL, a
			// value, off row -- and a value's length and bytes; a bit as 0 or 1, from its byte in the record
			// or from its default (a metadata-only ADD COLUMN: the record has none). An off-row pointer must
			// lie in the compared bytes: equal records stand for equal pointers (the trees are compared by
			// them).
			r.masked.clear();
			const uint8_t *start = page.bytes.data() + offset;
			const uint8_t *compared = start + r.bytes.size();
			for (size_t k = 0; k < layout.columns.size(); k++) {
				const PhysicalColumn &c = layout.columns[k];
				if (c.Dropped() || c.Uniquifier()) {
					continue;
				}
				const ValueRef v = row.Locate(layout, k);
				const uint32_t id = static_cast<uint32_t>(c.rscolid);
				r.masked.append(reinterpret_cast<const char *>(&id), sizeof(id));
				if (v.state == ValueState::OffRow) {
					if (v.data < start || v.data > compared || v.size > static_cast<size_t>(compared - v.data)) {
						throw FormatError("backup: page " + page.header.page_id.ToString() + " slot " +
										  std::to_string(slot) + ": the off-row pointer of " + c.name +
										  " does not lie in its record");
					}
					r.masked.push_back('\2');
					continue;
				}
				if (v.state == ValueState::Null) {
					r.masked.push_back('\0');
					continue;
				}
				r.masked.push_back('\1');
				if (c.xtype == XTYPE_BIT && v.size >= 1) {
					const bool set =
						v.state == ValueState::Default ? v.data[0] != 0 : ((v.data[0] >> c.bit_position) & 1) != 0;
					r.masked.push_back(set ? '\1' : '\0');
					continue;
				}
				const uint32_t size = v.size;
				r.masked.append(reinterpret_cast<const char *>(&size), sizeof(size));
				r.masked.append(reinterpret_cast<const char *>(v.data), v.size);
			}
		}
		out.push_back(std::move(r));
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

//! A row left unpaired on its page, as kept to pair across the table: where it lies (it is read again from
//! there) and digests of what it holds, not its bytes.
struct LoneRow {
	ChangeItem item;
	std::string rid;
	Digest record;
	bool has_masked = false;  // a table with values stored off row: the digest of its values but the off-row ones
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

}  // namespace

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
	LobNodePages node_pages;
};

namespace {

//! A page of the collection: of the newer state's table, or one of the older state's the newer table no
//! longer has (freed, moved by a shrink).
struct CollectWork {
	bool old_only = false;
	size_t partition = 0;
	PagePointer page;
};

}  // namespace

//! The changes of the table between the two states (DQ-47). Built at InitGlobal; its pages are claimed a
//! unit at a time by DuckDB's tasks of the call (`CollectUnit`, inside a BindFiles of both states), as the
//! scan's are, and each unit's changes returned by its task; the last task to find no unit left pairs
//! what every unit left (`PairPartition`). Each newer-state page that changed since the older state's checkpoint
//! (one the differential holds, when it lies over the older state's full) is read with its older version,
//! the records both hold unchanged at one offset skipped (the same bytes: one row, unchanged) and the
//! others paired by _mssql_rid; the older state's pages the newer table no longer has give their rows; a
//! row left unpaired on one page (moved, a page split) is paired across the table at the end.
class ChangeCollector::Impl {
public:
	Impl(ClientContext &context, const TableChangesBindData &data, const ChangeSide &now, const ChangeSide *before,
		 CollectMemory &memory, bool sha256)
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
		// A table with values stored off row compares the rows whose records differ by their values: the
		// in-row ones, then the off-row ones' digests where those agree (a shrink moved the LOB pages).
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
		check.old_store = old_store_;
		check.sha256 = sha256_;
		return check;
	}

	//! The older rows' check: their in-row values only, no tree walked, no value hashed.
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
		OffRowCheck check = NewCheck(w.partition);
		check.node_pages = &task.node_pages;
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
			SameRecords(task.old_page.bytes.data(), task.old_page.header.slot_count, task.page.bytes.data(),
						task.page.header.slot_count, PAGE_SIZE, now_.data.entry->shape == TableShape::Heap,
						task.old_same, task.new_same);
		}
		OffRowCheck check = NewCheck(i);
		check.node_pages = &task.node_pages;
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

ChangeCollector::ChangeCollector(ClientContext &context, const TableChangesBindData &data, const ChangeSide &now,
								 const ChangeSide *before, CollectMemory &memory, bool sha256)
	: impl_(new Impl(context, data, now, before, memory, sha256)) {}
ChangeCollector::~ChangeCollector() = default;

size_t ChangeCollector::Units() const {
	return impl_->Units();
}
std::shared_ptr<CollectTask> ChangeCollector::NewTask() const {
	return std::make_shared<CollectTask>();
}
void ChangeCollector::CollectUnit(CollectTask &task, size_t unit, std::vector<ChangeItem> &items) {
	task.items.clear();
	impl_->CollectUnit(task, unit);
	items.swap(task.items);
}
void ChangeCollector::HandIn(CollectTask &task) {
	impl_->HandIn(task);
}
void ChangeCollector::Enter() {
	impl_->Enter();
}
idx_t ChangeCollector::LonePartitions() const {
	return impl_->LonePartitions();
}
bool ChangeCollector::Leave(CollectTask &task) {
	return impl_->Leave(task);
}
void ChangeCollector::PlaceAll(ClientContext &context) {
	impl_->PlaceAll(context);
}
bool ChangeCollector::TakePlaced(ClientContext &context, std::vector<ChangeItem> &items, uint64_t &bytes) {
	return impl_->TakePlaced(context, items, bytes);
}
void ChangeCollector::ReleaseItems(uint64_t bytes) {
	impl_->ReleaseItems(bytes);
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

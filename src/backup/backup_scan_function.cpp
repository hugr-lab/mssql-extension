//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_scan_function.cpp
//
// Spec 085 PR 2d, task 16: mssql_backup_scan(path, schema, table). The table's columns as ReadingOf
// reads them (D6's types; a type still to be converted as its stored bytes; a computed column that is
// not persisted is not a column of the scan, having no stored value), plus the virtual _mssql_page,
// _mssql_rid and _mssql_page_lsn (D8). Projection pushdown; no filter pushdown (§ SQL surface).
// Cardinality from the catalog's row count. Rows in page order (file, then page) within each
// partition's in-row unit (the pages its IAM chain lists, DQ-40), one unit at a time on one thread (PR 2e makes units
// of 64 pages parallel). The backup is opened at bind -- the columns are the file's -- and every EXECUTE binds again.
// [DuckDB]
//===----------------------------------------------------------------------===//

#include "backup/backup_scan_function.hpp"

#include "backup/backup_function_common.hpp"
#include "backup/backup_opened.hpp"
#include "backup/backup_record_stager.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "mssql_function_docs.hpp"

#include <algorithm>
#include <cstdio>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

const char *const FUNCTION = "mssql_backup_scan";

const column_t PAGE_COLUMN = VIRTUAL_COLUMN_START;
const column_t RID_COLUMN = VIRTUAL_COLUMN_START + 1;
const column_t PAGE_LSN_COLUMN = VIRTUAL_COLUMN_START + 2;
//! The columns the scan cannot read (no stored value, a sparse one, text / ntext / image until PR 3) are
//! virtual from here on: `SELECT *` leaves them out, and naming one refuses the query by name, with
//! its reason (owner, review of #552).
const column_t UNREAD_COLUMN_START = VIRTUAL_COLUMN_START + 16;

//! A column the scan does not read: its name, the type it would have, why.
struct UnreadColumn {
	std::string name;
	LogicalType type;
	std::string reason;
};

struct ScanBindData : public TableFunctionData {
	vector<std::string> paths;
	std::string schema;
	std::string table;
	std::shared_ptr<OpenedCatalog> catalog;
	const CatalogTable *entry = nullptr;  // in catalog->model
	std::vector<int32_t> column_ids;	  // the scan's columns: catalog column ids, in output order
	vector<LogicalType> types;
	vector<std::string> names;		   // of column_ids
	std::vector<UnreadColumn> unread;  // UNREAD_COLUMN_START + i

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<ScanBindData>();
		copy->paths = paths;
		copy->schema = schema;
		copy->table = table;
		copy->catalog = catalog;
		copy->entry = entry;
		copy->column_ids = column_ids;
		copy->types = types;
		copy->names = names;
		copy->unread = unread;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other_p) const override {
		const auto &other = other_p.Cast<ScanBindData>();
		return paths == other.paths && schema == other.schema && table == other.table;
	}
	//! Every EXECUTE binds again: the file is read at bind.
	bool SupportStatementCache() const override {
		return false;
	}
};

// The backup on `paths` opened and `schema`.`table` found; its columns into `data`.
void BindTable(ClientContext &context, ScanBindData &data, vector<Identifier> *names) {
	try {
		data.catalog = std::shared_ptr<OpenedCatalog>(
			OpenBackupCatalog(context, std::vector<std::string>(data.paths.begin(), data.paths.end())).release());
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", FUNCTION, e.what());
	}
	data.entry = data.catalog->model.FindTable(data.schema, data.table);
	if (!data.entry) {
		throw InvalidInputException("%s: the backup has no table %s.%s (mssql_backup_tables lists them)", FUNCTION,
									data.schema, data.table);
	}
	if (!data.entry->decodable) {
		throw InvalidInputException("%s: %s.%s is not read: %s", FUNCTION, data.schema, data.table, data.entry->reason);
	}
	std::vector<UnreadColumn> unread;
	for (const auto &c : data.entry->columns) {
		const ColumnReading reading = ReadingOf(c);
		if (!reading.readable) {
			unread.push_back(UnreadColumn{c.name, reading.type, reading.note});
			continue;
		}
		data.column_ids.push_back(c.column_id);
		data.types.push_back(reading.type);
		data.names.push_back(c.name);
		if (names) {
			names->emplace_back(c.name);
		}
	}
	// An unread column is a virtual one only when its name is no other column's: a readable column and
	// the service columns win, and the binder must not pick among equal names (review of #552).
	auto taken = [&](const std::string &name) {
		for (const char *service : {"_mssql_page", "_mssql_rid", "_mssql_page_lsn"}) {
			if (StringUtil::CIEquals(name, service)) {
				return true;
			}
		}
		for (const auto &n : data.names) {
			if (StringUtil::CIEquals(name, n)) {
				return true;
			}
		}
		return false;
	};
	for (auto &u : unread) {
		if (!taken(u.name)) {
			data.unread.push_back(std::move(u));
		}
	}
}

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
							  vector<Identifier> &names) {
	auto data = make_uniq<ScanBindData>();
	data->paths = PathsOf(input.inputs[0], FUNCTION);
	if (input.inputs[1].IsNull() || input.inputs[2].IsNull()) {
		throw InvalidInputException("%s: the schema and the table must not be NULL", FUNCTION);
	}
	data->schema = input.inputs[1].ToString();
	data->table = input.inputs[2].ToString();
	BindTable(context, *data, &names);
	return_types = data->types;
	return std::move(data);
}

void Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p, const BoundTableFunction &) {
	const auto &data = bind_data_p->Cast<ScanBindData>();
	serializer.WriteProperty(100, "paths", data.paths);
	serializer.WriteProperty(101, "schema", data.schema);
	serializer.WriteProperty(102, "table", data.table);
	serializer.WriteProperty(103, "types", data.types);
	serializer.WriteProperty(104, "names", data.names);
	vector<std::string> unread;
	for (const auto &u : data.unread) {
		unread.push_back(u.name);
	}
	serializer.WriteProperty(105, "unread", unread);
}

unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundTableFunction &) {
	auto data = make_uniq<ScanBindData>();
	data->paths = deserializer.ReadProperty<vector<std::string>>(100, "paths");
	data->schema = deserializer.ReadProperty<std::string>(101, "schema");
	data->table = deserializer.ReadProperty<std::string>(102, "table");
	const auto types = deserializer.ReadProperty<vector<LogicalType>>(103, "types");
	const auto names = deserializer.ReadProperty<vector<std::string>>(104, "names");
	const auto unread = deserializer.ReadProperty<vector<std::string>>(105, "unread");
	BindTable(deserializer.Get<ClientContext &>(), *data, nullptr);
	// The plan's column positions and vectors are those it was bound with: a file at the same path that
	// reads otherwise now (a nightly backup replaced: a column added, dropped, renamed or retyped) is
	// refused, never read into positions or vectors of other columns.
	vector<std::string> now_unread;
	for (const auto &u : data->unread) {
		now_unread.push_back(u.name);
	}
	if (data->types != types || data->names != names || now_unread != unread) {
		throw InvalidInputException(
			"%s: %s.%s in the backup at the same path reads otherwise than when the "
			"query was planned (the file changed): plan the query again",
			FUNCTION, data->schema, data->table);
	}
	return std::move(data);
}

unique_ptr<NodeStatistics> Cardinality(ClientContext &, const FunctionData *bind_data) {
	if (!bind_data) {
		return nullptr;
	}
	const auto &data = bind_data->Cast<ScanBindData>();
	if (!data.entry->row_count_known) {
		return nullptr;	 // a count the catalog does not know is no estimate (not 0 rows)
	}
	// An estimate, never a maximum: the count is the catalog's (approximate, and behind the pages in a
	// backup taken under writes), and a maximum below a LIMIT lets DuckDB drop the LIMIT.
	return make_uniq<NodeStatistics>(static_cast<idx_t>(data.entry->row_count));
}

virtual_column_map_t VirtualColumns(ClientContext &, optional_ptr<FunctionData> bind_data) {
	virtual_column_map_t result;
	if (bind_data) {
		const auto &unread = bind_data->Cast<ScanBindData>().unread;
		for (size_t i = 0; i < unread.size(); i++) {
			result.insert(make_pair(UNREAD_COLUMN_START + i, TableColumn(Identifier(unread[i].name), unread[i].type)));
		}
	}
	// count(*) projects this one rather than the table's first column, which may not be readable.
	result.insert(make_pair(COLUMN_IDENTIFIER_EMPTY, TableColumn("", LogicalType::BOOLEAN)));
	result.insert(make_pair(PAGE_COLUMN, TableColumn("_mssql_page", LogicalType::VARCHAR)));
	result.insert(make_pair(RID_COLUMN, TableColumn("_mssql_rid", LogicalType::BLOB)));
	result.insert(make_pair(PAGE_LSN_COLUMN, TableColumn("_mssql_page_lsn", LogicalType::DECIMAL(25, 0))));
	return result;
}

// One partition's place in the scan: its layout, in-row unit, IAM-listed pages, and the rid's parts.
struct PartitionCursor {
	const CatalogPartition *partition = nullptr;
	const SysAllocUnit *unit = nullptr;
	std::vector<PagePointer> pages;
	std::vector<size_t> key;  // layout indexes of the clustered key, in ordkey order
	bool has_uniquifier = false;
	size_t uniquifier = 0;
};

struct ScanState : public GlobalTableFunctionState {
	std::unique_ptr<RecordStager> stager;
	std::vector<idx_t> stager_outputs;	// output positions of the stager's columns
	idx_t page_output = DConstants::INVALID_INDEX;
	idx_t rid_output = DConstants::INVALID_INDEX;
	idx_t lsn_output = DConstants::INVALID_INDEX;
	std::vector<idx_t> empty_outputs;  // COLUMN_IDENTIFIER_EMPTY (count(*)): a constant NULL

	size_t next_partition = 0;
	PartitionCursor cursor;
	bool have_partition = false;
	size_t next_page = 0;
	Page page;
	bool have_page = false;
	uint16_t next_slot = 0;
	std::vector<uint16_t> offsets;	// the current page's slot offsets, a freed slot as 0
	std::vector<uint16_t> bounds;	// where each slot's record must end (the next record's start)

	// The chunk's virtual values, row by row (only those projected are filled).
	std::vector<std::string> page_keys;
	std::vector<std::string> rids;
	std::vector<hugeint_t> lsns;

	idx_t MaxThreads() const override {
		return 1;
	}
};

unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &input) {
	const auto &data = input.bind_data->Cast<ScanBindData>();
	auto state = make_uniq<ScanState>();
	std::vector<int32_t> staged;
	for (idx_t c = 0; c < input.column_ids.size(); c++) {
		const column_t id = input.column_ids[c];
		if (id == PAGE_COLUMN) {
			state->page_output = c;
		} else if (id == RID_COLUMN) {
			state->rid_output = c;
		} else if (id == PAGE_LSN_COLUMN) {
			state->lsn_output = c;
		} else if (id == COLUMN_IDENTIFIER_EMPTY) {
			state->empty_outputs.push_back(c);
		} else if (id >= UNREAD_COLUMN_START && id - UNREAD_COLUMN_START < data.unread.size()) {
			const UnreadColumn &u = data.unread[id - UNREAD_COLUMN_START];
			throw InvalidInputException("%s: %s.%s.%s is not read: %s", FUNCTION, data.schema, data.table, u.name,
										u.reason);
		} else if (id < data.column_ids.size()) {
			staged.push_back(data.column_ids[id]);
			state->stager_outputs.push_back(c);
		} else {
			throw InternalException("%s: column id %llu is none of the scan's", FUNCTION,
									static_cast<unsigned long long>(id));
		}
	}
	state->stager = make_uniq<RecordStager>(*data.entry, staged);
	return std::move(state);
}

std::string PageKey(const PagePointer &p) {
	char buf[16];
	snprintf(buf, sizeof(buf), "01%04X%08X", static_cast<unsigned>(p.file), static_cast<unsigned>(p.page));
	return buf;
}

void PutU16(std::string &out, uint16_t v) {
	out.push_back(static_cast<char>(v & 0xFF));
	out.push_back(static_cast<char>(v >> 8));
}

void PutU32(std::string &out, uint32_t v) {
	for (int i = 0; i < 4; i++) {
		out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
	}
}

// _mssql_rid (D8): a heap row's place before it moved (its own, or a forwarded record's back pointer),
// 0x00 ‖ u32 page ‖ u16 file ‖ u16 slot; a clustered row 0x01 ‖ per key column in ordkey order u16
// length ‖ its stored bytes (0xFFFF for NULL; a bit as one byte 0 / 1) ‖ u32 uniquifier when the index
// is not unique (0 when absent). No partition number, which SPLIT / MERGE renumber: a partitioned
// clustered index carries its partitioning column as a key column (measured, 2022: added by the server
// to a non-unique index on another column, ordkey 2 of 3), so rows of two partitions differ in it.
std::string RowId(const ScanBindData &data, const PartitionCursor &cursor, const RecordView &record,
				  const PagePointer &page, uint16_t slot) {
	std::string rid;
	const RowsetLayout &layout = data.entry->Layout(*cursor.partition);
	if (data.entry->shape == TableShape::Heap) {
		RowLocation at;
		at.page = page;
		at.slot = slot;
		if (record.Type() == RECORD_FORWARDED) {
			at = ForwardedFrom(record, layout.table);
		}
		rid.push_back('\x00');
		PutU32(rid, at.page.page);
		PutU16(rid, at.page.file);
		PutU16(rid, at.slot);
		return rid;
	}
	rid.push_back('\x01');
	const size_t var_count = RowVarCount(record, layout.table);
	for (size_t k : cursor.key) {
		const ValueRef v = LocateValue(record, var_count, layout, k);
		const PhysicalColumn &c = layout.columns[k];
		if (v.state == ValueState::Null) {
			PutU16(rid, 0xFFFF);
			continue;
		}
		if (v.state == ValueState::OffRow) {
			throw FormatError("backup: " + layout.table + "." + c.name +
							  ", a key column, holds a value stored off row");
		}
		if (c.xtype == XTYPE_BIT && v.state == ValueState::Inline) {
			if (c.offset <= 0) {
				throw FormatError("backup: " + layout.table + "." + c.name +
								  ", a bit key column, is not at a fixed offset");
			}
			PutU16(rid, 1);
			rid.push_back(static_cast<char>((v.data[0] >> c.bit_position) & 1));
			continue;
		}
		if (v.size >= 0xFFFF) {
			// 0xFFFF is NULL's length: a value that long is named rather than taken for one (as the
			// catalog's rid; review of #549).
			throw FormatError("backup: " + layout.table + "." + c.name + ", a key column, holds a value of " +
							  std::to_string(v.size) + " bytes, past what _mssql_rid encodes");
		}
		PutU16(rid, static_cast<uint16_t>(v.size));
		rid.append(reinterpret_cast<const char *>(v.data), v.size);
	}
	if (cursor.has_uniquifier) {
		const ValueRef u = LocateValue(record, var_count, layout, cursor.uniquifier);
		uint32_t value = 0;
		if (u.state == ValueState::Inline && u.size == 4) {
			value = static_cast<uint32_t>(u.data[0]) | (static_cast<uint32_t>(u.data[1]) << 8) |
					(static_cast<uint32_t>(u.data[2]) << 16) | (static_cast<uint32_t>(u.data[3]) << 24);
		} else if (u.state == ValueState::Inline && u.size != 0) {
			throw FormatError("backup: " + layout.table + "'s uniquifier holds " + std::to_string(u.size) +
							  " bytes, not 0 or 4");
		} else if (u.state == ValueState::OffRow || u.state == ValueState::Default) {
			// Absent (an empty or missing variable column) is 0; stored off row or as an added default
			// it is no uniquifier the reader knows: named, not taken for 0.
			throw FormatError("backup: " + layout.table + "'s uniquifier is stored " +
							  (u.state == ValueState::OffRow ? "off row" : "as a default"));
		}
		PutU32(rid, value);
	}
	return rid;
}

// The next partition with an in-row unit into state.cursor; false when none is left.
bool NextPartition(const ScanBindData &data, ScanState &state) {
	const PageStore &store = *data.catalog->pages->store;
	while (state.next_partition < data.entry->partitions.size()) {
		const CatalogPartition &p = data.entry->partitions[state.next_partition++];
		PartitionCursor cursor;
		cursor.partition = &p;
		for (const auto &u : data.entry->units) {
			if (u.unit.owner_id == p.rowset_id && u.unit.type == 1) {
				if (cursor.unit) {
					throw FormatError("backup: " + data.schema + "." + data.table + "'s partition " +
									  std::to_string(p.partition_number) + " has two in-row allocation units");
				}
				cursor.unit = &u.unit;
			}
		}
		if (!cursor.unit) {
			throw FormatError("backup: " + data.schema + "." + data.table + "'s partition " +
							  std::to_string(p.partition_number) + " has no in-row allocation unit");
		}
		const RowsetLayout &layout = data.entry->Layout(p);
		std::vector<std::pair<int16_t, size_t>> key;
		for (size_t i = 0; i < layout.columns.size(); i++) {
			const PhysicalColumn &c = layout.columns[i];
			if (c.Uniquifier()) {
				cursor.has_uniquifier = true;
				cursor.uniquifier = i;
			} else if (!c.Dropped() && c.ordkey > 0) {
				key.emplace_back(c.ordkey, i);
			}
		}
		std::sort(key.begin(), key.end());
		for (const auto &k : key) {
			cursor.key.push_back(k.second);
		}
		// Page order (§ Scan): the IAM chain lists its single pages before its extents, so its order is
		// not the pages'; a page's rows stay together either way.
		cursor.pages = UnitPages(store, *cursor.unit);
		std::sort(cursor.pages.begin(), cursor.pages.end(), [](const PagePointer &x, const PagePointer &y) {
			return x.file != y.file ? x.file < y.file : x.page < y.page;
		});
		// A page an IAM chain lists twice (a single page that is also in an extent run) is read once:
		// its rows would otherwise be handed out twice (review of #552).
		cursor.pages.erase(std::unique(cursor.pages.begin(), cursor.pages.end(),
									   [](const PagePointer &x, const PagePointer &y) {
										   return x.file == y.file && x.page == y.page;
									   }),
						   cursor.pages.end());
		state.stager->UseLayout(layout);
		state.cursor = std::move(cursor);
		state.have_partition = true;
		state.next_page = 0;
		state.have_page = false;
		return true;
	}
	return false;
}

// The next live data page of the partition's unit into state.page; false when the unit has no more.
bool NextPage(ClientContext &context, const ScanBindData &data, ScanState &state) {
	const PageStore &store = *data.catalog->pages->store;
	while (state.next_page < state.cursor.pages.size()) {
		// A unit of pages with no live row (ghosts, freed pages) is walked here without a row to return:
		// Ctrl-C is asked page by page.
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		const PagePointer at = state.cursor.pages[state.next_page++];
		if (!ReadUnitDataPage(store, *state.cursor.unit, at, data.schema + "." + data.table, state.page)) {
			continue;
		}
		const uint16_t slots = state.page.header.slot_count;
		if (static_cast<size_t>(slots) * 2 + PAGE_HEADER_SIZE > PAGE_SIZE) {
			throw FormatError("backup: page " + at.ToString() + " has " + std::to_string(slots) +
							  " slots, more than an 8 KB page holds");
		}
		state.offsets.assign(slots, 0);
		std::vector<uint16_t> live;
		for (uint16_t s = 0; s < slots; s++) {
			const size_t pos = PAGE_SIZE - 2 - 2 * static_cast<size_t>(s);
			const uint16_t offset = static_cast<uint16_t>(state.page.bytes[pos] | (state.page.bytes[pos + 1] << 8));
			if (offset >= PAGE_HEADER_SIZE) {
				state.offsets[s] = offset;
				live.push_back(offset);
			}
		}
		// Two slots at one record would hand its row out twice.
		std::sort(live.begin(), live.end());
		if (std::adjacent_find(live.begin(), live.end()) != live.end()) {
			throw FormatError("backup: page " + at.ToString() + " has two slots pointing to one record");
		}
		// Where each slot's record must end: the next record's start, or the slot array (records lie
		// apart, as PageRows checks for the system tables; review of #549). Checked per record in Execute
		// against RecordView::End, with no second read of the record.
		state.bounds.assign(slots, 0);
		const uint16_t slot_array = static_cast<uint16_t>(PAGE_SIZE - 2u * slots);
		for (uint16_t s = 0; s < slots; s++) {
			if (state.offsets[s] != 0) {
				auto next = std::upper_bound(live.begin(), live.end(), state.offsets[s]);
				state.bounds[s] = next == live.end() ? slot_array : *next;
			}
		}
		state.next_slot = 0;
		state.have_page = true;
		return true;
	}
	return false;
}

void WriteVirtual(ScanState &state, DataChunk &output, idx_t count) {
	if (state.page_output != DConstants::INVALID_INDEX) {
		Vector &v = output.data[state.page_output];
		auto *out = FlatVector::GetDataMutable<string_t>(v);
		for (idx_t i = 0; i < count; i++) {
			out[i] = StringVector::AddString(v, state.page_keys[i]);
		}
	}
	if (state.rid_output != DConstants::INVALID_INDEX) {
		Vector &v = output.data[state.rid_output];
		auto *out = FlatVector::GetDataMutable<string_t>(v);
		for (idx_t i = 0; i < count; i++) {
			out[i] = StringVector::AddStringOrBlob(v, state.rids[i]);
		}
	}
	if (state.lsn_output != DConstants::INVALID_INDEX) {
		auto *out = FlatVector::GetDataMutable<hugeint_t>(output.data[state.lsn_output]);
		for (idx_t i = 0; i < count; i++) {
			out[i] = state.lsns[i];
		}
	}
}

hugeint_t LsnNumber(const Lsn &lsn) {
	return hugeint_t(static_cast<int64_t>(lsn.vlf)) * hugeint_t(static_cast<int64_t>(1000000000000000LL)) +
		   hugeint_t(static_cast<int64_t>(lsn.block)) * hugeint_t(static_cast<int64_t>(100000)) +
		   hugeint_t(static_cast<int64_t>(lsn.slot));
}

void Execute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	const auto &data = input.bind_data->Cast<ScanBindData>();
	auto &state = input.global_state->Cast<ScanState>();
	try {
		std::vector<Vector *> targets;
		for (idx_t c : state.stager_outputs) {
			targets.push_back(&output.data[c]);
		}
		state.stager->BeginChunk(targets);
		state.page_keys.clear();
		state.rids.clear();
		state.lsns.clear();
		while (!state.stager->Full()) {
			if (!state.have_partition && !NextPartition(data, state)) {
				break;
			}
			if (!state.have_page && !NextPage(context, data, state)) {
				state.have_partition = false;
				continue;
			}
			if (state.next_slot >= state.offsets.size()) {
				state.have_page = false;
				continue;
			}
			const uint16_t slot = state.next_slot++;
			if (state.offsets[slot] == 0) {
				continue;  // a freed slot
			}
			const RecordView record =
				ReadSlot(state.page.bytes.data(), PAGE_SIZE, state.page.header.slot_count, slot, data.table.c_str());
			if (record.Type() != RECORD_PRIMARY && record.Type() != RECORD_FORWARDED) {
				continue;  // ghosts, stubs (the row is read where it lives), index and LOB records
			}
			if (static_cast<uint32_t>(state.offsets[slot]) + record.End() > state.bounds[slot]) {
				throw FormatError("backup: page " + state.page.header.page_id.ToString() + " has a record at offset " +
								  std::to_string(state.offsets[slot]) + " that runs past the next record's start (" +
								  std::to_string(state.bounds[slot]) + ")");
			}
			state.stager->Append(record);
			if (state.page_output != DConstants::INVALID_INDEX) {
				state.page_keys.push_back(PageKey(state.page.header.page_id));
			}
			if (state.rid_output != DConstants::INVALID_INDEX) {
				state.rids.push_back(RowId(data, state.cursor, record, state.page.header.page_id, slot));
			}
			if (state.lsn_output != DConstants::INVALID_INDEX) {
				state.lsns.push_back(LsnNumber(state.page.header.lsn));
			}
		}
		const idx_t count = state.stager->Count();
		state.stager->FinalizeChunk();
		WriteVirtual(state, output, count);
		for (idx_t c : state.empty_outputs) {
			output.data[c].SetVectorType(VectorType::CONSTANT_VECTOR);
			ConstantVector::SetNull(output.data[c], true);
		}
		output.SetChildCardinality(count);
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", FUNCTION, e.what());
	}
}

}  // namespace

void RegisterBackupScanFunction(ExtensionLoader &loader) {
	TableFunctionSet set(FUNCTION);
	const vector<LogicalType> paths{LogicalType(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR)};
	for (const auto &path : paths) {
		TableFunction f(FUNCTION, {path, LogicalType::VARCHAR, LogicalType::VARCHAR}, Execute, Bind, Init);
		f.projection_pushdown = true;
		f.get_virtual_columns = VirtualColumns;
		f.cardinality = Cardinality;
		f.serialize = Serialize;
		f.deserialize = Deserialize;
		set.AddFunction(f);
	}
	mssql::RegisterDocumentedFunction(
		loader, std::move(set),
		{{},
		 "Reads a user table's rows from a SQL Server full backup, with no SQL Server, typed as TYPE mssql reads "
		 "them; a type whose conversion is still to come is its stored bytes. Virtual columns: _mssql_page (the "
		 "row's page), _mssql_rid (its identity: a heap row's original place, a clustered row's key) and "
		 "_mssql_page_lsn (the page's last change).",
		 {"SELECT * FROM mssql_backup_scan('/backups/sales_full.bak', 'dbo', 'orders')",
		  "SELECT _mssql_page, _mssql_rid, id FROM mssql_backup_scan('/backups/sales_full.bak', 'dbo', 'orders')"},
		 {"backup"}});
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

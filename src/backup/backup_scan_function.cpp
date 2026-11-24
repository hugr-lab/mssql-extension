//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_scan_function.cpp
//
// Spec 085 PR 2d, task 16: mssql_backup_scan(path, schema, table). The table's columns as ReadingOf
// reads them (D6's types; a type still to be converted as its stored bytes; a computed column that is
// not persisted is not a column of the scan, having no stored value), plus the virtual _mssql_page,
// _mssql_rid and _mssql_page_lsn (D8). Projection pushdown; no filter pushdown (§ SQL surface).
// Cardinality from the catalog's row count. Parallel over units (PR 2e, task 16b, § Scan): each
// partition's in-row allocation unit lists its pages through its IAM chain (DQ-40), when its first unit
// is claimed; the list, in page order (file, then page), is cut into units of UNIT_PAGES pages (more
// for few rows a page), and a task claims one unit at a time. A chunk holds one unit's rows and
// carries the unit's place in that order as its batch index, so DuckDB can keep
// the single-threaded order (partition, then page) where order is preserved. `max_threads` bounds the
// tasks (D17; DuckDB's `threads` by default, a value above it clamped to it with a WARNING). The backup
// is opened at bind -- the columns are the file's -- and every EXECUTE binds again. [DuckDB]
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
#include "codec/clr/ssclrt_wkb.hpp"
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

const char *const FUNCTION = "mssql_backup_scan";
//! The scan of a table of schema sys, under the name a catalog mirror calls it by.
const char *const CATALOG_FUNCTION = "mssql_backup_catalog";

const column_t PAGE_COLUMN = VIRTUAL_COLUMN_START;
const column_t RID_COLUMN = VIRTUAL_COLUMN_START + 1;
const column_t PAGE_LSN_COLUMN = VIRTUAL_COLUMN_START + 2;
//! PR 3 (task 20, DQ-46): the row's values stored off row and their trees (backup_lob_map), NULL when
//! it has none.
const column_t LOB_COLUMN = VIRTUAL_COLUMN_START + 3;
//! The columns the scan cannot read (no stored value, a sparse one) are
//! virtual from here on: `SELECT *` leaves them out, and naming one refuses the query by name, with
//! its reason (owner, review of #552).
const column_t UNREAD_COLUMN_START = VIRTUAL_COLUMN_START + 16;

//! The scan's model: the one table, or view of the name (task 16e: the bind described every table of
//! the database for it; 16f: an indexed view's rows are read as a table's).
CatalogScope TableScope(const std::string &schema, const std::string &table) {
	CatalogScope scope;
	scope.by_schema = true;
	scope.schemas = {schema};
	scope.by_table = true;
	scope.tables = {table};
	scope.by_view = true;
	scope.views = {table};
	return scope;
}

// The backup on `paths` opened and `schema`.`table` found; its columns into `data`.
void BindTable(ClientContext &context, ScanBindData &data, vector<Identifier> *names) {
	try {
		data.catalog = std::shared_ptr<OpenedCatalog>(
			data.attached ? OpenAttachedCatalog(context, *data.attached).release()
						  : OpenBackupCatalog(context, std::vector<std::string>(data.paths.begin(), data.paths.end()),
											  true, TableScope(data.schema, data.table),
											  std::vector<std::string>(data.diff.begin(), data.diff.end()), data.at)
								.release());
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", data.function.c_str(), e.what());
	}
	data.entry = data.catalog->Model().FindTable(data.schema, data.table);
	if (!data.entry) {
		// An indexed view's clustered index, read as a clustered table's (task 16f); a view without one
		// stores no rows.
		data.entry = data.catalog->Model().FindIndexedView(data.schema, data.table);
	}
	if (!data.entry && data.catalog->Model().FindView(data.schema, data.table)) {
		throw InvalidInputException(
			"%s: %s.%s is a view that stores no rows: only an indexed view's (a unique "
			"clustered index on it) are read (mssql_backup_views says which)",
			data.function.c_str(), data.schema, data.table);
	}
	if (!data.entry) {
		auto attached = data.attached ? data.attached : FindAttachedBackup(context, data.paths.front(), data.at);
		if (attached && data.paths.size() == 1) {
			throw InvalidInputException(
				"%s: the backup attached as %s has no table or indexed view %s.%s that its filters keep "
				"(schema_filter / table_filter, schema_exclude / table_exclude; mssql_backup_tables('%s') and "
				"mssql_backup_views('%s') list them)",
				data.function.c_str(), attached->name, data.schema, data.table, attached->name, attached->name);
		}
		throw InvalidInputException(
			"%s: the backup has no table or indexed view %s.%s (mssql_backup_tables and "
			"mssql_backup_views list them)",
			data.function.c_str(), data.schema, data.table);
	}
	if (!data.entry->decodable) {
		throw InvalidInputException("%s: %s.%s is not read: %s", data.function.c_str(), data.schema, data.table,
									data.entry->reason);
	}
	ScanColumns columns = ScanColumnsOf(*data.entry, data.Forms());
	data.column_ids = std::move(columns.column_ids);
	data.types = std::move(columns.types);
	data.names = std::move(columns.names);
	data.unread = std::move(columns.unread);
	if (names) {
		for (const auto &n : data.names) {
			names->emplace_back(n);
		}
	}
}

//! The scan of table `table` of schema `schema` of the backup on `paths` (a path, a list, an attached name),
//! with the call's named parameters: mssql_backup_scan's, and mssql_backup_catalog's (schema sys).
unique_ptr<FunctionData> BindScan(ClientContext &context, const char *function, const Value &paths, const Value &schema,
								  const Value &table, TableFunctionBindInput &input, vector<LogicalType> &return_types,
								  vector<Identifier> &names) {
	auto data = make_uniq<ScanBindData>();
	data->function = function;
	data->paths = PathsOf(paths, function);
	data->diff = DiffPathsOf(input.named_parameters, function);
	data->at = BackupAtOf(input.named_parameters, function);
	if (data->paths.size() == 1) {
		// Over an attached backup, the version pinned now by its files (task 24b, owner 2026-11-19).
		PinAttachedVersion(context, data->paths.front(), data->at);
	}
	if (schema.IsNull() || table.IsNull()) {
		throw InvalidInputException("%s: the schema and the table must not be NULL", function);
	}
	data->schema = schema.ToString();
	data->table = table.ToString();
	data->fuzzy = FuzzyParameterOf(input.named_parameters, function);
	if (data->fuzzy.empty() && data->paths.size() == 1) {
		// Over an attached name, the ATTACH's fuzzy option before the setting, as its tables' scans take it.
		data->fuzzy = AttachedFuzzy(context, data->paths.front());
	}
	// Spec 093 T8: the call's geometry_as, else the ATTACH's; checked now.
	auto geometry_as = input.named_parameters.find("geometry_as");
	if (geometry_as != input.named_parameters.end() && !geometry_as->second.IsNull()) {
		data->geometry_as =
			GeometryAsGiven(geometry_as->second.ToString(), (std::string(function) + ": geometry_as").c_str());
	} else if (data->paths.size() == 1) {
		data->geometry_as = AttachedGeometryAs(context, data->paths.front());
	}
	data->Forms();
	for (const auto &kv : input.named_parameters) {
		if (kv.first == "max_threads" && !kv.second.IsNull()) {
			if (kv.second.GetValue<int64_t>() < 1) {
				throw InvalidInputException("%s: max_threads must be at least 1, not %s", function,
											kv.second.ToString());
			}
			data->max_threads = static_cast<idx_t>(kv.second.GetValue<int64_t>());
			const idx_t threads = static_cast<idx_t>(context.db->NumberOfThreads());
			if (data->max_threads > threads) {
				DUCKDB_LOG_WARNING(context, "%s: max_threads %llu is above DuckDB's threads (%llu): clamped to it",
								   function, static_cast<unsigned long long>(data->max_threads),
								   static_cast<unsigned long long>(threads));
			}
		}
	}
	for (const auto &kv : input.named_parameters) {
		if (kv.first == "max_memory" && !kv.second.IsNull()) {
			// DuckDB's own syntax ('2GB', '512MiB'), or bytes as a bare number; an unlimited one ('-1',
			// 'none', any case) is DuckDB's limit. Any other '-...' is refused, not read as unlimited.
			const std::string given = StringUtil::Lower(StringUtil::Replace(kv.second.ToString(), " ", ""));
			optional_idx bytes;
			bool parsed = !given.empty() && (given[0] != '-' || given == "-1");
			if (parsed && given.size() <= 19 &&
				std::all_of(given.begin(), given.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
				bytes = optional_idx(std::stoull(given));  // 19 digits fit in 64 bits
			} else if (parsed) {
				try {
					bytes = DBConfig::ParseMemoryLimit(given);
				} catch (std::exception &) {
					parsed = false;
				}
			}
			if (!parsed) {
				throw InvalidInputException("%s: max_memory '%s' is not a size (as memory_limit takes, e.g. '2GB')",
											function, kv.second.ToString());
			}
			if (bytes.IsValid() && bytes.GetIndex() == 0) {
				throw InvalidInputException("%s: max_memory must be more than 0", function);
			}
			data->max_memory = bytes.IsValid() ? bytes.GetIndex() : 0;
			const idx_t limit = BufferManager::GetBufferManager(context).GetMaxMemory();
			if (data->max_memory > limit) {
				DUCKDB_LOG_WARNING(context, "%s: max_memory %s is above DuckDB's memory_limit (%s): clamped to it",
								   function, StringUtil::BytesToHumanReadableString(data->max_memory),
								   StringUtil::BytesToHumanReadableString(limit));
			}
		}
	}
	BindTable(context, *data, &names);
	return_types = data->types;
	return std::move(data);
}

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
							  vector<Identifier> &names) {
	return BindScan(context, FUNCTION, input.inputs[0], input.inputs[1], input.inputs[2], input, return_types, names);
}

//! mssql_backup_catalog(path, base_table): the table of schema sys, read by the scan as any table (owner,
//! 2026-11-22: one reader for every table; a system base table, an internal one). An MS-shipped table of another
//! schema (cdc.*) is mssql_backup_scan's, with its schema.
unique_ptr<FunctionData> CatalogBind(ClientContext &context, TableFunctionBindInput &input,
									 vector<LogicalType> &return_types, vector<Identifier> &names) {
	return BindScan(context, CATALOG_FUNCTION, input.inputs[0], Value("sys"), input.inputs[1], input, return_types,
					names);
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
	serializer.WritePropertyWithDefault<idx_t>(106, "max_threads", data.max_threads, 0);
	serializer.WritePropertyWithDefault<idx_t>(107, "max_memory", data.max_memory, 0);
	serializer.WritePropertyWithDefault<vector<std::string>>(108, "diff", data.diff);
	serializer.WritePropertyWithDefault<std::string>(109, "at_unit", data.at.unit);
	serializer.WritePropertyWithDefault<Value>(110, "at_value", data.at.value, Value());
	serializer.WritePropertyWithDefault<std::string>(111, "at_key", data.at.key);
	serializer.WritePropertyWithDefault<std::string>(112, "fuzzy", data.fuzzy);
	serializer.WritePropertyWithDefault<std::string>(113, "function", data.function, FUNCTION);
	serializer.WritePropertyWithDefault<std::string>(114, "geometry_as", data.geometry_as);
}

unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundTableFunction &) {
	auto data = make_uniq<ScanBindData>();
	data->paths = deserializer.ReadProperty<vector<std::string>>(100, "paths");
	data->schema = deserializer.ReadProperty<std::string>(101, "schema");
	data->table = deserializer.ReadProperty<std::string>(102, "table");
	const auto types = deserializer.ReadProperty<vector<LogicalType>>(103, "types");
	const auto names = deserializer.ReadProperty<vector<std::string>>(104, "names");
	const auto unread = deserializer.ReadProperty<vector<std::string>>(105, "unread");
	data->max_threads = deserializer.ReadPropertyWithExplicitDefault<idx_t>(106, "max_threads", 0);
	data->max_memory = deserializer.ReadPropertyWithExplicitDefault<idx_t>(107, "max_memory", 0);
	data->diff = deserializer.ReadPropertyWithDefault<vector<std::string>>(108, "diff");
	data->at.unit = deserializer.ReadPropertyWithDefault<std::string>(109, "at_unit");
	data->at.value = deserializer.ReadPropertyWithExplicitDefault<Value>(110, "at_value", Value());
	data->at.key = deserializer.ReadPropertyWithDefault<std::string>(111, "at_key");
	data->fuzzy = deserializer.ReadPropertyWithDefault<std::string>(112, "fuzzy");
	data->function = deserializer.ReadPropertyWithExplicitDefault<std::string>(113, "function", FUNCTION);
	data->geometry_as = deserializer.ReadPropertyWithDefault<std::string>(114, "geometry_as");
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
			data->function.c_str(), data->schema, data->table);
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
	static const std::vector<UnreadColumn> none;
	return ScanVirtualColumns(bind_data ? bind_data->Cast<ScanBindData>().unread : none);
}

//! The pages of a unit (§ Scan): what a task claims at once, and what one batch index covers. Cut from
//! the IAM chain's list, so a unit's pages may include some with no row of the table (not live, an IAM
//! page, an upper index level), skipped as the unit is read. A chunk holds one unit's rows, so where the
//! catalog's rows per page are too few for 64 pages to fill a vector (wide rows: one a page), a unit is
//! cut longer, up to MAX_UNIT_PAGES (review: 64-row chunks otherwise).
constexpr size_t UNIT_PAGES = 64;
constexpr size_t MAX_UNIT_PAGES = 2048;

// What every task shares: the projection (built at init, then only read), and where the next unit is
// cut, under `claim`: the partition being cut, its next page and unit length, the next partition to
// list, and the next batch index. A partition's pages are listed when its first unit is claimed, so
// only the lists of the partitions being read are held (review: every list at init cost the memory of
// all of them and one thread's walk of every IAM chain before the first row).
struct ScanGlobalState : public GlobalTableFunctionState {
	std::vector<int32_t> staged;		// catalog column ids the stager reads, in output order
	std::vector<idx_t> stager_outputs;	// output positions of the stager's columns
	idx_t page_output = DConstants::INVALID_INDEX;
	idx_t rid_output = DConstants::INVALID_INDEX;
	idx_t lsn_output = DConstants::INVALID_INDEX;
	idx_t lob_output = DConstants::INVALID_INDEX;
	std::vector<idx_t> empty_outputs;  // COLUMN_IDENTIFIER_EMPTY (count(*)): a constant NULL
	ClrWarnings clr_warnings;		   // spec 093 T8

	std::mutex claim;
	std::shared_ptr<const PartitionCursor> cutting;	 // the partition units are being cut from
	size_t cut_page = 0;
	size_t unit_pages = UNIT_PAGES;
	size_t next_partition = 0;
	idx_t next_batch = 0;
	idx_t max_threads = 1;
	//! D17: a chunk's decoding is reserved before it runs (`chunk_bytes`, ChunkBound), against half
	//! of the call's max_memory and DuckDB's memory (the EXTENSION tag), and released after it.
	std::unique_ptr<ReservationGate> gate;
	uint64_t chunk_bytes = 0;
	uint64_t chunk_allowance = 0;  // RecordStager::LimitBytes: 0 when a vector of the widest rows fits
	uint64_t virtual_row = 0;
	idx_t max_memory = 0;  // the call's, clamped: for the refusal's words

	//! For the progress bar: rows staged by every task, and the catalog's count (-1 unknown).
	std::atomic<uint64_t> rows_read{0};
	//! A page taken into use while the backup ran met by any task: said once per scan.
	mutable std::atomic<bool> pending_warned{false};
	//! A value stored off row on a Pending page, read as NULL, said once a scan too (its own WARNING).
	mutable std::atomic<bool> pending_values_warned{false};
	int64_t row_count = -1;

	idx_t MaxThreads() const override {
		return max_threads;
	}
};

// One task's place: the unit it reads, its page, and the chunk's virtual values.
struct ScanLocalState : public LocalTableFunctionState {
	std::unique_ptr<RecordStager> stager;
	const RowsetLayout *layout = nullptr;  // the stager's (UseLayout), to switch only between partitions
	LobPageFetch off_row_fetch;			   // the stager's pages for values stored off row (live ones only)

	std::atomic<bool> *pending_warned = nullptr;	// the scan's: a Pending page said once (ScanGlobalState)
	std::shared_ptr<const PartitionCursor> cursor;	// of the unit being read
	idx_t batch = 0;								// the unit's batch index
	bool have_unit = false;
	size_t next_page = 0;
	size_t end_page = 0;
	Page page;
	bool have_page = false;
	uint16_t next_slot = 0;
	//! The stager's PendingValues as of its last appended row: a row prepared and then left for the next
	//! chunk (no reservation) is not one the WARNING may name (review of #591).
	idx_t pending_appended = 0;
	CdRecord cd_record;	 // a ROW-compressed rowset's record and its values (spec 086 PR 1), reused
	CdRow cd_row;
	PageCompression page_ci;		// a PAGE page's compression information (spec 086 PR 2); not present on a ROW page
	std::vector<bool> cd_needed;	// the layout's columns this scan reads: the stager's, the key's
	std::vector<uint16_t> offsets;	// the current page's slot offsets, a freed slot as 0
	std::vector<uint16_t> bounds;	// where each slot's record must end (the next record's start)

	// The chunk's virtual values, row by row (only those projected are filled).
	std::vector<std::string> page_keys;
	std::vector<std::string> rids;
	std::vector<hugeint_t> lsns;
	std::vector<std::string> lob_maps;	// empty: NULL
};

}  // namespace

// Partition `p`'s cursor: its in-row unit, the pages its IAM chain lists (in page order, each once),
// and the layout indexes of its clustered key.
PartitionCursor BuildPartition(const ScanBindData &data, const CatalogPartition &p) {
	const PageStore &store = *data.catalog->pages->store;
	PartitionCursor cursor;
	cursor.partition = &p;
	for (const auto &u : data.entry->units) {
		if (u.unit.owner_id != p.rowset_id) {
			continue;
		}
		// Each kind once: a rowset's in-row, LOB and row-overflow units (E21: a rebuilt partition owns
		// its own, a LOB unit kept), the units a pointer of its rows may name.
		const char *kind = u.unit.type == 1	  ? "in-row"
						   : u.unit.type == 2 ? "LOB"
						   : u.unit.type == 3 ? "row-overflow"
											  : nullptr;
		if (!kind) {
			continue;
		}
		const bool twice = (u.unit.type == 1 && cursor.unit) || (u.unit.type == 2 && cursor.off_row.lob) ||
						   (u.unit.type == 3 && cursor.off_row.row_overflow);
		if (twice) {
			throw FormatError("backup: " + data.schema + "." + data.table + "'s partition " +
							  std::to_string(p.partition_number) + " has two " + kind + " allocation units");
		}
		if (u.unit.type == 1) {
			cursor.unit = &u.unit;
		} else if (u.unit.type == 2) {
			cursor.off_row.lob = u.unit.auid;
		} else {
			cursor.off_row.row_overflow = u.unit.auid;
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
	const SysAllocUnit &unit = *cursor.unit;
	auto list = [&store, &unit]() {
		// Page order (§ Scan): the IAM chain lists its single pages before its extents, so its order is
		// not the pages'; a page's rows stay together either way.
		std::vector<PagePointer> pages = UnitPages(store, unit);
		std::sort(pages.begin(), pages.end(), [](const PagePointer &x, const PagePointer &y) {
			return x.file != y.file ? x.file < y.file : x.page < y.page;
		});
		// A page an IAM chain lists twice (a single page that is also in an extent run) is read once:
		// its rows would otherwise be handed out twice (review of #552).
		pages.erase(std::unique(pages.begin(), pages.end(),
								[](const PagePointer &x, const PagePointer &y) {
									return x.file == y.file && x.page == y.page;
								}),
					pages.end());
		return pages;
	};
	cursor.pages = data.catalog->unit_pages ? data.catalog->unit_pages->Get(unit.auid, list)
											: std::make_shared<const std::vector<PagePointer>>(list());
	return cursor;
}

namespace {

// What the scan's own virtual values hold a row, for the reservation: a page key (a std::string, 14
// characters in place), a rid (its std::string with a key of 1,700 bytes at most, a shape byte and a
// uniquifier), an LSN. Their vectors' string heaps, like the table columns', are DuckDB's own
// allocations, which it accounts itself.
uint64_t VirtualRowBytes(const ScanGlobalState &global) {
	uint64_t bytes = 0;
	if (global.page_output != DConstants::INVALID_INDEX) {
		bytes += sizeof(std::string);
	}
	if (global.rid_output != DConstants::INVALID_INDEX) {
		bytes += sizeof(std::string) + 1 + 1700 + 4;
	}
	if (global.lsn_output != DConstants::INVALID_INDEX) {
		bytes += sizeof(hugeint_t);
	}
	if (global.lob_output != DConstants::INVALID_INDEX) {
		// The map's entries for a row of in-row roots and row-overflow pointers; a deep tree's nodes are
		// reserved with its value (S4).
		bytes += sizeof(std::string) + 256;
	}
	return bytes;
}

unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
	const auto &data = input.bind_data->Cast<ScanBindData>();
	// D11 (task 27): the table's status from the backup's log tail, acted on as fuzzy := says -- at init, not
	// bind: a DESCRIBE or an EXPLAIN reads no tail, and each EXECUTE says it once (review of #591: a PREPARE
	// and two EXECUTEs logged three WARNINGs).
	CheckStatementFuzzy(context, *data.catalog, BaseUnitsOf(*data.entry),
						data.entry->schema_name + "." + data.entry->name, data.schema + "." + data.table,
						FuzzyModeOf(context, data.fuzzy, data.function.c_str()), data.function.c_str());
	auto state = make_uniq<ScanGlobalState>();
	std::vector<int32_t> &staged = state->staged;
	for (idx_t c = 0; c < input.column_ids.size(); c++) {
		const column_t id = input.column_ids[c];
		if (id == PAGE_COLUMN) {
			state->page_output = c;
		} else if (id == RID_COLUMN) {
			state->rid_output = c;
		} else if (id == PAGE_LSN_COLUMN) {
			state->lsn_output = c;
		} else if (id == LOB_COLUMN) {
			state->lob_output = c;
		} else if (id == COLUMN_IDENTIFIER_EMPTY) {
			state->empty_outputs.push_back(c);
		} else if (id >= UNREAD_COLUMN_START && id - UNREAD_COLUMN_START < data.unread.size()) {
			const UnreadColumn &u = data.unread[id - UNREAD_COLUMN_START];
			throw InvalidInputException("%s: %s.%s.%s is not read: %s", data.function.c_str(), data.schema, data.table,
										u.name, u.reason);
		} else if (id < data.column_ids.size()) {
			staged.push_back(data.column_ids[id]);
			state->stager_outputs.push_back(c);
		} else {
			throw InternalException("%s: column id %llu is none of the scan's", data.function.c_str(),
									static_cast<unsigned long long>(id));
		}
	}
	// D17: DuckDB's threads, or fewer when the call asks for fewer. The units are cut as they are claimed,
	// so their count is estimated from the catalog: a unit holds a vector's rows or more but for the
	// last of a partition (UNIT_PAGES, MAX_UNIT_PAGES), so a task beyond rows / vector + partitions
	// would find none. An unknown count bounds nothing.
	idx_t threads = static_cast<idx_t>(context.db->NumberOfThreads());
	if (data.max_threads != 0) {
		threads = std::min(threads, data.max_threads);
	}
	if (data.entry->row_count_known && data.entry->row_count >= 0) {
		state->row_count = data.entry->row_count;
		const idx_t units = static_cast<idx_t>(data.entry->row_count) / STANDARD_VECTOR_SIZE + 1 +
							static_cast<idx_t>(data.entry->partitions.size());
		threads = std::min(threads, units);
	}
	state->max_threads = std::max<idx_t>(1, threads);
	// D17: the call's max_memory, DuckDB's memory_limit at most; half of it for the reservations.
	auto &buffers = BufferManager::GetBufferManager(context);
	const idx_t limit = buffers.GetMaxMemory();
	state->max_memory = data.max_memory == 0 ? limit : std::min(limit, data.max_memory);
	// A chunk's reservation (§ Resources, decode staging): what the stager holds -- its fixed part, and
	// twice a row's variable values (buffers grow by doubling) -- and the virtual values, for a vector of
	// the widest rows, or CHUNK_RESERVATION_BYTES with the chunk closing on what it staged; never past
	// the budget unless one row is (then refused by name at the first chunk).
	RecordStager probe(*data.entry, state->staged, data.Forms());
	const uint64_t fixed = probe.FixedStagingBytes();
	const uint64_t virtual_row = VirtualRowBytes(*state);
	const uint64_t row = 2 * probe.VarRowBytes() + virtual_row;
	const uint64_t budget = state->max_memory / 2;
	const uint64_t vector = fixed + STANDARD_VECTOR_SIZE * row;
	uint64_t chunk = std::min(vector, std::max(CHUNK_RESERVATION_BYTES, fixed + row));
	chunk = std::min(chunk, std::max(budget, fixed + row));
	state->chunk_bytes = chunk;
	state->chunk_allowance = chunk < vector ? chunk - fixed : 0;
	state->virtual_row = virtual_row;
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

unique_ptr<LocalTableFunctionState> InitLocal(ExecutionContext &execution, TableFunctionInitInput &input,
											  GlobalTableFunctionState *global_state) {
	const auto &data = input.bind_data->Cast<ScanBindData>();
	const auto &global = global_state->Cast<ScanGlobalState>();
	auto local = make_uniq<ScanLocalState>();
	local->pending_warned = &global.pending_warned;
	local->stager = make_uniq<RecordStager>(*data.entry, global.staged, data.Forms());
	local->stager->LimitBytes(global.chunk_allowance, global.virtual_row);
	const PageStore &store = *data.catalog->pages->store;
	const std::string fragment = "a fragment of a value of " + data.schema + "." + data.table + " stored off row";
	// A value's walk asks for interruption every 256 fragments: one 2 GiB value is about 267,000 of
	// them, read between two of the scan's own checks (NextPage's, per in-row page; review of S3).
	ClientContext *context = &execution.client;
	auto fetched = std::make_shared<uint32_t>(0);
	const ScanBindData *bound = &data;
	local->off_row_fetch = [&store, fragment, context, fetched, bound](const PagePointer &at, Page &out) {
		if ((++*fetched & 255) == 0 && context->IsInterrupted()) {
			throw InterruptException();
		}
		try {
			store.ReadLive(at.file, at.page, out, fragment.c_str());
		} catch (ZeroCopyError &e) {
			ThrowPendingOrFormat(*context, bound->catalog->opened, e);
		}
	};
	local->stager->MapOffRow(global.lob_output != DConstants::INVALID_INDEX);
	// A value stored off row on a Pending page, as the policy treats an in-row one: NULL under 'warn'
	// (said once a table and scan) and 'ignore', the scan refused under 'error' (review of #591).
	local->stager->PendingValuesAsNull(FuzzyModeOf(execution.client, data.fuzzy, data.function.c_str()) !=
									   FuzzyMode::Error);
	return std::move(local);
}

// How many pages a unit of `cursor` takes: UNIT_PAGES, or more where its partition's rows per page
// (the catalog's rows over the pages its IAM chain lists) would leave 64 pages short of a vector.
size_t UnitPagesOf(const PartitionCursor &cursor) {
	const int64_t rows = cursor.partition->rows;
	const size_t pages = cursor.pages->size();
	if (rows <= 0 || pages == 0) {
		return UNIT_PAGES;	// unknown or empty: nothing to size by
	}
	// ceil(vector * pages / rows) pages hold about a vector's rows.
	const uint64_t want = (static_cast<uint64_t>(STANDARD_VECTOR_SIZE) * pages + static_cast<uint64_t>(rows) - 1) /
						  static_cast<uint64_t>(rows);
	return static_cast<size_t>(std::max<uint64_t>(UNIT_PAGES, std::min<uint64_t>(MAX_UNIT_PAGES, want)));
}

// The next unit into `local`; false when every partition is cut. Cut under the claim lock, in
// partition order, then page order, with batch indexes in that order: each task's only grow, as
// DuckDB requires, and DuckDB can give the single-threaded order back where order is kept. The
// partition the next unit needs is listed here (its IAM chain walked), by the task that claims it.
// The walk is I/O under the shared lock, once per partition, on purpose: the units are cut in order,
// so every task waiting on the lock needs this very list for its next unit too. Outside the lock it
// would be walked by each of them, or they would wait on a "being listed" mark instead -- the same
// wait. Tasks holding a unit keep decoding meanwhile; they take the lock only when it is read
// (review of #553).
bool ClaimUnit(const ScanBindData &data, ScanGlobalState &global, ScanLocalState &local) {
	std::lock_guard<std::mutex> guard(global.claim);
	while (!global.cutting || global.cut_page >= global.cutting->pages->size()) {
		global.cutting.reset();
		if (global.next_partition >= data.entry->partitions.size()) {
			return false;
		}
		const CatalogPartition &p = data.entry->partitions[global.next_partition++];
		global.cutting = std::make_shared<const PartitionCursor>(BuildPartition(data, p));
		global.cut_page = 0;
		global.unit_pages = UnitPagesOf(*global.cutting);
	}
	local.cursor = global.cutting;
	local.batch = global.next_batch++;
	local.next_page = global.cut_page;
	local.end_page = std::min(global.cutting->pages->size(), global.cut_page + global.unit_pages);
	global.cut_page = local.end_page;
	local.have_page = false;
	local.have_unit = true;
	const RowsetLayout &layout = data.entry->Layout(*local.cursor->partition);
	if (local.layout != &layout) {
		local.stager->UseLayout(layout);
		local.layout = &layout;
		// The columns a compressed row's values are read for: the projected ones and _mssql_rid's (its
		// key and uniquifier, also named in a refusal's words).
		local.stager->MarkNeeded(local.cd_needed);
		for (size_t k : local.cursor->key) {
			local.cd_needed[k] = true;
		}
		if (local.cursor->has_uniquifier) {
			local.cd_needed[local.cursor->uniquifier] = true;
		}
	}
	// Every claim: partitions sharing a layout have units of their own.
	local.stager->UseOffRow(&local.off_row_fetch, local.cursor->off_row);
	return true;
}

OperatorPartitionData PartitionData(ClientContext &, TableFunctionGetPartitionInput &input) {
	if (input.partition_info.RequiresPartitionColumns()) {
		throw InternalException("%s: partition columns are not supported", FUNCTION);
	}
	return OperatorPartitionData(input.local_state->Cast<ScanLocalState>().batch);
}

double Progress(ClientContext &, const FunctionData *, const GlobalTableFunctionState *global_state) {
	const auto &global = global_state->Cast<ScanGlobalState>();
	if (global.row_count < 0) {
		return -1;	// no count to measure against
	}
	if (global.row_count == 0) {
		return 100.0;
	}
	// The catalog's count is approximate: capped, never past the end.
	const double done = static_cast<double>(global.rows_read.load()) / static_cast<double>(global.row_count);
	return 100.0 * std::min(1.0, done);
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

}  // namespace

// _mssql_rid (D8): a heap row's place before it moved (its own, or a forwarded record's back pointer),
// 0x00 ‖ u32 page ‖ u16 file ‖ u16 slot; a clustered row 0x01 ‖ per key column in ordkey order u16
// length ‖ its stored bytes (0xFFFF for NULL; a bit as one byte 0 / 1) ‖ u32 uniquifier when the index
// is not unique (0 when absent). No partition number, which SPLIT / MERGE renumber: a partitioned
// clustered index carries its partitioning column as a key column (measured, 2022: added by the server
// to a non-unique index on another column, ordkey 2 of 3), so rows of two partitions differ in it.
// `at`: where the row lives, a forwarded record's place before it moved (its back pointer).
std::string RowId(const ScanBindData &data, const PartitionCursor &cursor, const RowRecord &row,
				  const RowLocation &at) {
	std::string rid;
	const RowsetLayout &layout = data.entry->Layout(*cursor.partition);
	if (data.entry->shape == TableShape::Heap) {
		rid.push_back('\x00');
		PutU32(rid, at.page.page);
		PutU16(rid, at.page.file);
		PutU16(rid, at.slot);
		return rid;
	}
	rid.push_back('\x01');
	for (size_t k : cursor.key) {
		const ValueRef v = row.Locate(layout, k);
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
			// A compressed row's bit is put in its byte as the uncompressed record holds it (CdRow).
			if (!row.cd && c.offset <= 0) {
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
		const ValueRef u = row.Locate(layout, cursor.uniquifier);
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

namespace {

// The next live data page of the unit into local.page; false when the unit has no more.
bool NextPage(ClientContext &context, const ScanBindData &data, ScanLocalState &local) {
	const PageStore &store = *data.catalog->pages->store;
	while (local.next_page < local.end_page) {
		// A unit of pages with no live row (ghosts, freed pages) is walked here without a row to return:
		// Ctrl-C is asked page by page.
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		const PagePointer at = (*local.cursor->pages)[local.next_page++];
		try {
			try {
				if (!ReadUnitDataPage(store, *local.cursor->unit, at, data.schema + "." + data.table, local.page)) {
					continue;
				}
			} catch (ZeroCopyError &e) {
				ThrowPendingOrFormat(context, data.catalog->opened, e);
			}
		} catch (PendingPageError &e) {
			// A page that became live while the backup ran: its rows are in the log tail only (spec 090
			// replays it). Until then the table's other pages are read as of the copy, as every fuzzy table
			// is -- what was committed when the backup began is in them -- and the page is named: refused
			// under fuzzy := 'error', said under 'warn', passed over under 'ignore'.
			const FuzzyMode mode = FuzzyModeOf(context, data.fuzzy, data.function.c_str());
			if (mode == FuzzyMode::Error) {
				throw InvalidInputException(
					"%s: %s.%s: %s (fuzzy := 'error'; under 'warn' the table's other pages "
					"are read)",
					data.function.c_str(), data.schema, data.table, e.what());
			}
			// One WARNING per table and scan, as for the table's status (review of #591): the first page met.
			if (mode == FuzzyMode::Warn && local.pending_warned && !local.pending_warned->exchange(true)) {
				DUCKDB_LOG_WARNING(context,
								   "%s: %s.%s: %s; its rows are not read, the table's other pages are (a fuzzy "
								   "backup: spec 090 replays the tail; any further such page of the table is "
								   "passed over without another WARNING)",
								   data.function.c_str(), data.schema, data.table, e.what());
			}
			continue;
		}
		const uint16_t slots = local.page.header.slot_count;
		if (static_cast<size_t>(slots) * 2 + PAGE_HEADER_SIZE > PAGE_SIZE) {
			throw FormatError("backup: page " + at.ToString() + " has " + std::to_string(slots) +
							  " slots, more than an 8 KB page holds");
		}
		local.offsets.assign(slots, 0);
		std::vector<uint16_t> live;
		for (uint16_t s = 0; s < slots; s++) {
			const size_t pos = PAGE_SIZE - 2 - 2 * static_cast<size_t>(s);
			const uint16_t offset = static_cast<uint16_t>(local.page.bytes[pos] | (local.page.bytes[pos + 1] << 8));
			if (offset >= PAGE_HEADER_SIZE) {
				local.offsets[s] = offset;
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
		local.bounds.assign(slots, 0);
		const uint16_t slot_array = static_cast<uint16_t>(PAGE_SIZE - 2u * slots);
		for (uint16_t s = 0; s < slots; s++) {
			if (local.offsets[s] != 0) {
				auto next = std::upper_bound(live.begin(), live.end(), local.offsets[s]);
				local.bounds[s] = next == live.end() ? slot_array : *next;
			}
		}
		// PAGE compression (spec 086 PR 2): a page with header flag 0x80 holds its compression information at
		// offset 96, which every record follows; a page without it is a ROW page of the rowset.
		local.page_ci.present = false;
		if (data.entry->Layout(*local.cursor->partition).compression == 2 && (local.page.header.type_flags & 0x80)) {
			ReadPageCompression(local.page.bytes.data(), PAGE_SIZE, slot_array, data.table.c_str(), local.page_ci);
			if (!live.empty() && live.front() < local.page_ci.end) {
				throw FormatError("backup: page " + at.ToString() + " has a record at offset " +
								  std::to_string(live.front()) + " inside its compression information (to " +
								  std::to_string(local.page_ci.end) + ")");
			}
		}
		local.next_slot = 0;
		local.have_page = true;
		return true;
	}
	return false;
}

void WriteVirtual(const ScanGlobalState &global, ScanLocalState &local, DataChunk &output, idx_t count) {
	if (global.page_output != DConstants::INVALID_INDEX) {
		Vector &v = output.data[global.page_output];
		auto *out = FlatVector::GetDataMutable<string_t>(v);
		for (idx_t i = 0; i < count; i++) {
			out[i] = StringVector::AddString(v, local.page_keys[i]);
		}
	}
	if (global.rid_output != DConstants::INVALID_INDEX) {
		Vector &v = output.data[global.rid_output];
		auto *out = FlatVector::GetDataMutable<string_t>(v);
		for (idx_t i = 0; i < count; i++) {
			out[i] = StringVector::AddStringOrBlob(v, local.rids[i]);
		}
	}
	if (global.lsn_output != DConstants::INVALID_INDEX) {
		auto *out = FlatVector::GetDataMutable<hugeint_t>(output.data[global.lsn_output]);
		for (idx_t i = 0; i < count; i++) {
			out[i] = local.lsns[i];
		}
	}
	if (global.lob_output != DConstants::INVALID_INDEX) {
		Vector &v = output.data[global.lob_output];
		auto *out = FlatVector::GetDataMutable<string_t>(v);
		auto &validity = FlatVector::ValidityMutable(v);
		for (idx_t i = 0; i < count; i++) {
			if (local.lob_maps[i].empty()) {
				validity.SetInvalid(i);
			} else {
				out[i] = StringVector::AddStringOrBlob(v, local.lob_maps[i]);
			}
		}
	}
}

hugeint_t LsnNumber(const Lsn &lsn) {
	return hugeint_t(static_cast<int64_t>(lsn.vlf)) * hugeint_t(static_cast<int64_t>(1000000000000000LL)) +
		   hugeint_t(static_cast<int64_t>(lsn.block)) * hugeint_t(static_cast<int64_t>(100000)) +
		   hugeint_t(static_cast<int64_t>(lsn.slot));
}

//! What a call is reserving, for a refusal's words: a chunk's decoding (`row` empty), or the row whose
//! values stored off row are being reserved, as a refusal names it.
struct Reserving {
	std::string row;
	uint64_t bytes = 0;
};

//! The row being reserved, named only when refused: by its _mssql_rid and place, and what it holds off row.
void DescribeReserving(const ScanBindData &data, const ScanLocalState &local, const RowRecord &row,
					   const RowLocation &at, uint16_t slot, uint64_t bytes, Reserving &reserving) {
	static const char *digits = "0123456789ABCDEF";
	std::string rid = "0x";
	for (unsigned char b : RowId(data, *local.cursor, row, at)) {
		rid.push_back(digits[b >> 4]);
		rid.push_back(digits[b & 15]);
	}
	reserving.row = "the row of _mssql_rid " + rid + " (page " + local.page.header.page_id.ToString() + " slot " +
					std::to_string(slot) + "), whose values stored off row (" + local.stager->DescribePrepared() + "),";
	reserving.bytes = bytes;
}

//! What the scan does next: a slot to read, something to look at again (the next page, the next unit),
//! or the chunk's end.
enum class SlotStep { Row, Again, Stop };

//! The next slot of the unit being read into `slot`.
SlotStep NextSlot(ClientContext &context, const ScanBindData &data, ScanGlobalState &global, ScanLocalState &local,
				  uint16_t &slot) {
	if (!local.have_unit) {
		// A chunk holds one unit's rows: its batch index is the unit's. The next unit is claimed
		// by the next call, after DuckDB has taken this chunk's index.
		if (local.stager->Count() > 0 || !ClaimUnit(data, global, local)) {
			return SlotStep::Stop;
		}
	}
	if (!local.have_page && !NextPage(context, data, local)) {
		local.have_unit = false;
		return SlotStep::Again;
	}
	if (local.next_slot >= local.offsets.size()) {
		local.have_page = false;
		return SlotStep::Again;
	}
	slot = local.next_slot++;
	return local.offsets[slot] == 0 ? SlotStep::Again : SlotStep::Row;	// 0: a freed slot
}

//! The record of `slot` as a row (`row` points into `fixed` or the local state's CD row): false for one
//! that is no live row. `at` is where the row is, or was before it was forwarded.
bool ReadScanRow(const ScanBindData &data, ScanLocalState &local, const RowsetLayout &layout, uint16_t slot,
				 RecordView &fixed, RowRecord &row, RowLocation &at) {
	at.page = local.page.header.page_id;
	at.slot = slot;
	uint32_t record_end = 0;
	if (layout.compression == 1 || layout.compression == 2) {
		// ROW compression (spec 086 PR 1): a CD record, its values put in the uncompressed form; PAGE (PR 2)
		// the same, its symbols and prefixes resolved through the page's compression information.
		CdRecord &cd = local.cd_record;
		ReadCdRecord(local.page.bytes.data(), PAGE_SIZE, PAGE_SIZE - 2u * local.page.header.slot_count,
					 local.offsets[slot], data.table.c_str(), cd);
		if (cd.kind == CdRecord::Kind::Ghost || cd.kind == CdRecord::Kind::Stub) {
			return false;  // ghosts, stubs (the row is read where it lives)
		}
		if (cd.kind == CdRecord::Kind::Forwarded) {
			cd.BackPointer(at.page.page, at.page.file, at.slot);
		}
		local.cd_row.Read(cd, layout, &local.cd_needed, layout.compression == 2 ? &local.page_ci : nullptr);
		row = RowRecord::Compressed(cd, local.cd_row);
		record_end = cd.end;
	} else {
		fixed = ReadSlot(local.page.bytes.data(), PAGE_SIZE, local.page.header.slot_count, slot, data.table.c_str());
		if (fixed.Type() != RECORD_PRIMARY && fixed.Type() != RECORD_FORWARDED) {
			return false;  // ghosts, stubs (the row is read where it lives), index and LOB records
		}
		if (fixed.Type() == RECORD_FORWARDED) {
			at = ForwardedFrom(fixed, layout.table);
		}
		row = RowRecord::Fixed(fixed, RowVarCount(fixed, layout.table));
		record_end = fixed.End();
	}
	if (static_cast<uint32_t>(local.offsets[slot]) + record_end > local.bounds[slot]) {
		throw FormatError("backup: page " + local.page.header.page_id.ToString() + " has a record at offset " +
						  std::to_string(local.offsets[slot]) + " that runs past the next record's start (" +
						  std::to_string(local.bounds[slot]) + ")");
	}
	return true;
}

//! A row's values stored off row (`need`) reserved before it is staged (D17, PR 3 S4). Never while
//! waiting holding another reservation: a refusal with rows staged closes the chunk before this row
//! (false: its slot is read again by the next call); with none, the chunk's reservation is given back
//! and taken again with the row's, in one wait.
bool ReserveOffRow(const ScanBindData &data, const ScanGlobalState &global, ScanLocalState &local,
				   CallReservations &held, const OffRowNeed &need, const RowRecord &row, const RowLocation &at,
				   uint16_t slot, Reserving &reserving) {
	bool fits = false;
	try {
		// False is "not now": others hold or wait, or DuckDB has no room (alone, the Acquire
		// below names it); a row past the whole budget throws ReservationRefused instead,
		// since no release can make room: named here.
		fits = held.TryAcquire(need.Total(), need.Ours());
	} catch (ReservationRefused &) {
		DescribeReserving(data, local, row, at, slot, need.Total(), reserving);
		throw;
	}
	if (fits) {
		return true;
	}
	if (local.stager->Count() > 0) {
		local.next_slot = slot;
		return false;
	}
	DescribeReserving(data, local, row, at, slot, global.chunk_bytes + need.Total(), reserving);
	held.ReleaseAll();
	held.Acquire(global.chunk_bytes + need.Total(), global.chunk_bytes + need.Ours());
	reserving.row.clear();
	reserving.bytes = global.chunk_bytes;
	return true;
}

//! The virtual columns' values of the row just staged, kept for WriteVirtual.
void KeepVirtual(const ScanBindData &data, const ScanGlobalState &global, ScanLocalState &local, const RowRecord &row,
				 const RowLocation &at) {
	if (global.page_output != DConstants::INVALID_INDEX) {
		local.page_keys.push_back(PageKey(local.page.header.page_id));
	}
	if (global.rid_output != DConstants::INVALID_INDEX) {
		local.rids.push_back(RowId(data, *local.cursor, row, at));
	}
	if (global.lsn_output != DConstants::INVALID_INDEX) {
		local.lsns.push_back(LsnNumber(local.page.header.lsn));
	}
	if (global.lob_output != DConstants::INVALID_INDEX) {
		local.lob_maps.push_back(local.stager->LastRowLobMap());
	}
}

//! Rows staged until the chunk is full, its unit is read, or a row's off-row values must wait.
void StageRows(ClientContext &context, const ScanBindData &data, ScanGlobalState &global, ScanLocalState &local,
			   CallReservations &held, Reserving &reserving) {
	while (!local.stager->Full()) {
		uint16_t slot = 0;
		const SlotStep step = NextSlot(context, data, global, local, slot);
		if (step == SlotStep::Stop) {
			break;
		}
		if (step == SlotStep::Again) {
			continue;
		}
		const RowsetLayout &layout = data.entry->Layout(*local.cursor->partition);
		RowLocation at;
		RecordView fixed;
		RowRecord row;
		if (!ReadScanRow(data, local, layout, slot, fixed, row, at)) {
			continue;
		}
		const OffRowNeed need = local.stager->PrepareRow(row);
		if (need.Total() > 0 && !ReserveOffRow(data, global, local, held, need, row, at, slot, reserving)) {
			break;
		}
		local.stager->Append(row);
		local.pending_appended = local.stager->PendingValues();
		KeepVirtual(data, global, local, row, at);
	}
}

//! Once a scan: a value stored off row on a Pending page was read as NULL (fuzzy := 'warn').
void WarnPendingValues(ClientContext &context, const ScanBindData &data, ScanGlobalState &global,
					   const ScanLocalState &local) {
	if (local.pending_appended > 0 && !global.pending_values_warned.load() &&
		FuzzyModeOf(context, data.fuzzy, data.function.c_str()) == FuzzyMode::Warn &&
		!global.pending_values_warned.exchange(true)) {
		DUCKDB_LOG_WARNING(context,
						   "%s: %s.%s: a value stored off row is read as NULL, its row read: %s (a fuzzy backup: "
						   "spec 090 replays the tail; further such values of the table are NULL without another "
						   "WARNING; fuzzy := 'error' refuses the table)",
						   data.function.c_str(), data.schema, data.table, local.stager->FirstPending().c_str());
	}
}

//! A reservation refused, in the scan's words: past the scan's budget (InvalidInput), or DuckDB with no
//! room for it (OutOfMemory).
[[noreturn]] void ThrowRefused(ClientContext &context, const ScanBindData &data, const ScanGlobalState &global,
							   const Reserving &reserving, const ReservationRefused &e) {
	auto &buffers = BufferManager::GetBufferManager(context);
	const std::string temp =
		buffers.HasTemporaryDirectory() ? "'" + buffers.GetTemporaryDirectory() + "'" : "none (nothing spills)";
	const std::string need = StringUtil::BytesToHumanReadableString(reserving.bytes);
	const std::string what = reserving.row.empty() ? std::string("decoding a chunk") : reserving.row;
	const std::string max = StringUtil::BytesToHumanReadableString(global.max_memory);
	if (e.past_budget) {
		throw InvalidInputException(
			"%s: %s.%s: %s need%s up to %s, past the scan's half (%s) of "
			"max_memory %s; raise max_memory or memory_limit (temp_directory %s)",
			data.function.c_str(), data.schema, data.table, what, reserving.row.empty() ? "s" : "", need,
			StringUtil::BytesToHumanReadableString(global.gate->Budget()), max, temp);
	}
	throw OutOfMemoryException(
		"%s: %s.%s: %s need%s up to %s and DuckDB cannot make room for it "
		"with nothing else of the scan held (max_memory %s, temp_directory %s): %s",
		data.function.c_str(), data.schema, data.table, what, reserving.row.empty() ? "s" : "", need, max, temp,
		e.duckdb_reason);
}

void Execute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	const auto &data = input.bind_data->Cast<ScanBindData>();
	auto &global = input.global_state->Cast<ScanGlobalState>();
	auto &local = input.local_state->Cast<ScanLocalState>();
	// Every page this call reads, through the statement's files (the backup's state may be cached, D9).
	BindFiles bound(data.catalog->Binding());
	Reserving reserving;
	reserving.bytes = global.chunk_bytes;
	try {
		// A unit first: the call that finds none left returns no rows and reserves nothing.
		if (!local.have_unit && !ClaimUnit(data, global, local)) {
			output.SetChildCardinality(0);
			return;
		}
		// The chunk's decoding reserved for this call only: a task between calls holds no reservation,
		// so one blocked in DuckDB's pipeline (an order-keeping sink waiting for an earlier batch) never
		// keeps one the task reading that batch waits for. What it keeps unaccounted between calls is
		// its staging's capacity, at most about its reservation (buffers keep capacity across chunks).
		CallReservations held(*global.gate, [&context]() { return context.IsInterrupted(); });
		held.Acquire(global.chunk_bytes, global.chunk_bytes);
		std::vector<Vector *> targets;
		for (idx_t c : global.stager_outputs) {
			targets.push_back(&output.data[c]);
		}
		local.stager->BeginChunk(targets);
		local.page_keys.clear();
		local.rids.clear();
		local.lsns.clear();
		local.lob_maps.clear();
		StageRows(context, data, global, local, held, reserving);
		const idx_t count = local.stager->Count();
		global.rows_read += count;
		WarnPendingValues(context, data, global, local);
		local.stager->FinalizeChunk();
		global.clr_warnings.Say(context, data.function.c_str(), local.stager->ClrNotes());
		WriteVirtual(global, local, output, count);
		for (idx_t c : global.empty_outputs) {
			output.data[c].SetVectorType(VectorType::CONSTANT_VECTOR);
			ConstantVector::SetNull(output.data[c], true);
		}
		output.SetChildCardinality(count);
	} catch (PendingValueError &e) {
		// A value stored off row on a Pending page under fuzzy := 'error' (the stager reads it as NULL
		// otherwise). Any other Pending page met here (an IAM page) is the FormatError below.
		throw InvalidInputException("%s: %s.%s: %s (fuzzy := 'error'; under 'warn' the value is read as NULL)",
									data.function.c_str(), data.schema, data.table, e.what());
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", data.function.c_str(), e.what());
	} catch (ReservationInterrupted &) {
		throw InterruptException();
	} catch (ReservationRefused &e) {
		ThrowRefused(context, data, global, reserving, e);
	}
}

//! mssql_backup_scan's table function over `path`'s type; `catalog`: mssql_backup_catalog's (schema sys, its
//! table the second argument), the same scan.
TableFunction MakeScanFunction(const LogicalType &path, bool catalog = false) {
	TableFunction f =
		catalog ? TableFunction(CATALOG_FUNCTION, {path, LogicalType::VARCHAR}, Execute, CatalogBind, Init, InitLocal)
				: TableFunction(FUNCTION, {path, LogicalType::VARCHAR, LogicalType::VARCHAR}, Execute, Bind, Init,
								InitLocal);
	// NULL (the default): DuckDB's threads.
	f.GetSignature().AddKeywordOnly("max_threads", LogicalType::BIGINT, Value(LogicalType::BIGINT));
	// NULL (the default): DuckDB's memory_limit. A size as memory_limit takes it ('2GB').
	f.GetSignature().AddKeywordOnly("max_memory", LogicalType::VARCHAR, Value(LogicalType::VARCHAR));
	AddDiffParameter(f);
	AddFuzzyParameter(f);
	// Spec 093 T8: TYPE mssql's geometry_as; NULL (the default): the ATTACH's, else 'geometry'.
	f.GetSignature().AddKeywordOnly("geometry_as", LogicalType::VARCHAR, Value(LogicalType::VARCHAR));
	AddVersionParameters(f);
	f.projection_pushdown = true;
	f.get_partition_data = PartitionData;
	f.table_scan_progress = Progress;
	f.get_virtual_columns = VirtualColumns;
	f.cardinality = Cardinality;
	f.serialize = Serialize;
	f.deserialize = Deserialize;
	return f;
}

}  // namespace

void ClrWarnings::Say(ClientContext &context, const char *function, const std::vector<RecordStager::ClrNote> &notes) {
	for (const auto &n : notes) {
		std::lock_guard<std::mutex> guard(lock_);
		if (n.spatial.not_geometry > 0 && said_.insert(n.name + "|curve").second) {
			DUCKDB_LOG_WARNING(context,
							   "%s: column %s (%s) holds values a GEOMETRY cannot hold (a CircularString, "
							   "CompoundCurve, CurvePolygon or FullGlobe, or collections nested deeper than 15), "
							   "read as NULL, their rows read; geometry_as 'wkb' (ISO WKB, the curves included) or "
							   "'native' (the stored bytes), on the call or as an ATTACH option, reads them",
							   function, n.name.c_str(), n.type.c_str());
		}
		if (n.spatial.malformed > 0 && said_.insert(n.name + "|malformed").second) {
			DUCKDB_LOG_WARNING(context,
							   "%s: column %s (%s) holds values whose stored bytes cannot be read as %s (the "
							   "first: %s), read as NULL, their rows read; geometry_as 'native' reads the stored "
							   "bytes",
							   function, n.name.c_str(), n.type.c_str(), n.type.c_str(),
							   codec::ssclrt::DescribeStatus(n.spatial.first_malformed));
		}
		if (n.hierarchyid.malformed > 0 && said_.insert(n.name + "|hierarchyid").second) {
			DUCKDB_LOG_WARNING(context,
							   "%s: column %s (hierarchyid) holds values whose stored bytes cannot be read as a "
							   "hierarchyid (the first: %s), read as NULL, their rows read",
							   function, n.name.c_str(),
							   codec::hierarchyid::DescribeStatus(n.hierarchyid.first_malformed));
		}
	}
}

ScanColumns ScanColumnsOf(const CatalogTable &table, const TypeForms &forms) {
	ScanColumns result;
	std::vector<UnreadColumn> unread;
	for (const auto &c : table.columns) {
		const ColumnReading reading = ReadingOf(c, forms);
		if (!reading.readable) {
			unread.push_back(UnreadColumn{c.DuckDBName(), reading.type, reading.note});
			continue;
		}
		result.column_ids.push_back(c.column_id);
		result.types.push_back(reading.type);
		result.names.push_back(c.DuckDBName());
	}
	// An unread column is a virtual one only when its name is no other column's: a readable column and
	// the service columns win, and the binder must not pick among equal names (review of #552).
	auto taken = [&](const std::string &name) {
		for (const char *service : {"_mssql_page", "_mssql_rid", "_mssql_page_lsn", "_mssql_lob"}) {
			if (StringUtil::CIEquals(name, service)) {
				return true;
			}
		}
		for (const auto &n : result.names) {
			if (StringUtil::CIEquals(name, n)) {
				return true;
			}
		}
		return false;
	};
	for (auto &u : unread) {
		if (!taken(u.name)) {
			result.unread.push_back(std::move(u));
		}
	}
	return result;
}

virtual_column_map_t ScanVirtualColumns(const std::vector<UnreadColumn> &unread) {
	virtual_column_map_t result;
	for (size_t i = 0; i < unread.size(); i++) {
		result.insert(make_pair(UNREAD_COLUMN_START + i, TableColumn(Identifier(unread[i].name), unread[i].type)));
	}
	// count(*) projects this one rather than the table's first column, which may not be readable.
	result.insert(make_pair(COLUMN_IDENTIFIER_EMPTY, TableColumn("", LogicalType::BOOLEAN)));
	result.insert(make_pair(PAGE_COLUMN, TableColumn("_mssql_page", LogicalType::VARCHAR)));
	result.insert(make_pair(RID_COLUMN, TableColumn("_mssql_rid", LogicalType::BLOB)));
	result.insert(make_pair(PAGE_LSN_COLUMN, TableColumn("_mssql_page_lsn", LogicalType::DECIMAL(25, 0))));
	result.insert(make_pair(LOB_COLUMN, TableColumn("_mssql_lob", LogicalType::BLOB)));
	return result;
}

TableFunction AttachedScanFunction(ClientContext &context, TableCatalogEntry &entry,
								   std::shared_ptr<const AttachedBackup> attached, const std::string &key,
								   const std::string &schema, const std::string &table, const std::string &fuzzy,
								   const std::string &geometry_as, unique_ptr<FunctionData> &bind_data) {
	auto data = make_uniq<ScanBindData>();
	data->paths = {attached->name};
	// A plan deserialized finds the version again by its backups and files, not its number.
	data->at.key = key;
	data->schema = schema;
	data->table = table;
	data->table_entry = &entry;
	data->fuzzy = fuzzy;
	data->geometry_as = geometry_as;
	data->attached = std::move(attached);
	BindTable(context, *data, nullptr);
	bind_data = std::move(data);
	TableFunction f = MakeScanFunction(LogicalType::VARCHAR);
	// The binder names a table entry's columns -- D8's virtual ones included -- through it.
	f.get_table_entry = [](optional_ptr<const FunctionData> data_p) -> optional_ptr<TableCatalogEntry> {
		return data_p ? data_p->Cast<ScanBindData>().table_entry : nullptr;
	};
	return f;
}

void RegisterBackupScanFunction(ExtensionLoader &loader) {
	TableFunctionSet set(FUNCTION);
	const vector<LogicalType> paths{LogicalType(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR)};
	for (const auto &path : paths) {
		set.AddFunction(MakeScanFunction(path));
	}
	RegisterBackupTableChangesFunctions(loader, paths);
	TableFunctionSet catalog(CATALOG_FUNCTION);
	for (const auto &path : paths) {
		catalog.AddFunction(MakeScanFunction(path, true));
	}
	mssql::RegisterDocumentedFunction(
		loader, std::move(catalog),
		{{},
		 "Reads a table of schema sys of a SQL Server full backup -- a system base table (the catalog's own, as "
		 "sysschobjs) or an internal one (Query Store's, a queue's messages, ...) -- as "
		 "mssql_backup_scan(path, 'sys', table) reads it: its rows typed as TYPE mssql reads them and the same "
		 "virtual columns. A table of another schema, MS-shipped or not (cdc.*), is mssql_backup_scan's. "
		 "mssql_backup_tables lists every table, of every kind (its type column).",
		 {"SELECT id, name, type FROM mssql_backup_catalog('/backups/sales_full.bak', 'sysschobjs')",
		  "SELECT * FROM mssql_backup_catalog('/backups/sales_full.bak', 'plan_persist_query_text')"},
		 {"backup"}});
	mssql::RegisterDocumentedFunction(
		loader, std::move(set),
		{{},
		 "Reads a table's rows from a SQL Server full backup, with no SQL Server, typed as TYPE mssql reads "
		 "them, values stored off row (LOBs, row-overflow, text) included; a type whose conversion is still to "
		 "come is its stored bytes. Virtual columns: _mssql_page (the row's page), _mssql_rid (its identity: a "
		 "heap row's original place, a clustered row's key), _mssql_page_lsn (the page's last change) and "
		 "_mssql_lob (the row's off-row values and their trees, NULL when it has none). Parallel on DuckDB's threads; "
		 "max_threads := n uses fewer, "
		 "max_memory := '2GB' bounds what it "
		 "reserves for decoding.",
		 {"SELECT * FROM mssql_backup_scan('/backups/sales_full.bak', 'dbo', 'orders')",
		  "SELECT count(*) FROM mssql_backup_scan('/backups/sales_full.bak', 'dbo', 'orders', max_threads := 2)",
		  "SELECT _mssql_page, _mssql_rid, id FROM mssql_backup_scan('/backups/sales_full.bak', 'dbo', 'orders')"},
		 {"backup"}});
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

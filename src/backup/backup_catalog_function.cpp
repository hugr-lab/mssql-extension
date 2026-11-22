// Spec 085 PR 2b step 7b (task 17): mssql_backup_catalog(path [, base_table]) -- a full backup's system
// base tables as stored, for an importer to mirror (022 D3). [DuckDB]
//
// With no base_table: the base tables it reads (name, object id, rows, columns) -- every system base
// table of the database (sysschobjs type 'S') whose shape it reads. With one: its rows, its columns
// typed by its own description (sysrscols, named by syscolpars) through D6's map
// (MSSQLColumnInfo::MapSQLServerTypeToDuckDB, native types off), sql_variant as its raw bytes (BLOB:
// a VARIANT is spec 086's), plus the virtual columns of D8: _mssql_page (the row's page, '01' + 4 hex
// digits of file + 8 of page) and _mssql_rid (0x01, then per clustered key column in ordkey order u16
// length and its stored bytes, 0xFFFF for NULL). Every row of every table, sysobjvalues' module source
// and sysowners' password hashes included: whoever holds the backup -- and an encrypted one's key --
// may read all of it (spec 085 § Security, revised by the owner 2026-11-21; there was an allow-list).
// A table whose shape the reader does not serve (a heap, a non-unique clustered index, a bit key: no
// _mssql_rid) is refused by name, and left out of the listing. A value it cannot give (an off-row one
// past its bound or the call's, bytes not of their type) is NULL with one WARNING a column, its row
// read (owner, 2026-11-20).
//
// The rows are produced a page at a time: the global state holds the unit's IAM-listed pages and a
// cursor, never the table; an off-row value is read as its row is produced, within one value's bound
// and a bound for the call. The catalog (headers, pages, bootstrap tables -- not the model) is opened
// at bind, since a table's columns depend on the version that wrote the file, and every EXECUTE binds
// again; a prepared statement's bind data keeps that catalog, and so its files open, until DEALLOCATE
// (within its ClientContext, as backup_source allows).

#include "backup/backup_catalog_function.hpp"

#include "backup/backup_function_common.hpp"
#include "backup/backup_lob.hpp"
#include "backup/backup_opened.hpp"
#include "catalog/mssql_column_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "mssql_function_docs.hpp"
#include "utf8proc_wrapper.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

const char *const FUNCTION = "mssql_backup_catalog";

const column_t PAGE_COLUMN = VIRTUAL_COLUMN_START;
const column_t RID_COLUMN = VIRTUAL_COLUMN_START + 1;

//! The system type names of the xtypes a base table holds (sys.types' names, for D6's map).
const char *SystemTypeName(uint8_t xtype) {
	switch (xtype) {
	case XTYPE_UNIQUEIDENTIFIER:
		return "uniqueidentifier";
	case XTYPE_TINYINT:
		return "tinyint";
	case XTYPE_SMALLINT:
		return "smallint";
	case XTYPE_INT:
		return "int";
	case XTYPE_SMALLDATETIME:
		return "smalldatetime";
	case XTYPE_DATETIME:
		return "datetime";
	case XTYPE_BIT:
		return "bit";
	case XTYPE_BIGINT:
		return "bigint";
	case XTYPE_VARBINARY:
		return "varbinary";
	case XTYPE_VARCHAR:
		return "varchar";
	case XTYPE_BINARY:
		return "binary";
	case XTYPE_CHAR:
		return "char";
	case XTYPE_NVARCHAR:
		return "nvarchar";
	case XTYPE_NCHAR:
		return "nchar";
	default:
		return nullptr;
	}
}

LogicalType TypeOf(const RowsetLayout &layout, const PhysicalColumn &c) {
	if (c.xtype == XTYPE_SQL_VARIANT) {
		return LogicalType::BLOB;  // the sql_variant's own bytes: a VARIANT is spec 086's
	}
	const char *name = SystemTypeName(c.xtype);
	if (!name) {
		// A type the base tables' values are not converted from (decimal, datetime2, image, ... on some
		// version): its stored bytes, as CellValue gives them, never a refusal of the whole listing
		// (review of #549).
		return LogicalType::BLOB;
	}
	// sysrscols.ti gives a MAX type a length of 0 (E20).
	const bool variable = c.xtype == XTYPE_VARBINARY || c.xtype == XTYPE_VARCHAR || c.xtype == XTYPE_NVARCHAR;
	const int16_t length = variable && c.length == 0 ? int16_t(-1) : static_cast<int16_t>(c.length);
	// The backup reads datetime2(7) as TIMESTAMP_NS until spec 093 T8 hooks mssql_datetime2_7_as.
	return MSSQLColumnInfo::MapSQLServerTypeToDuckDB(name, length, c.precision, c.scale,
													 MSSQLDatetime2Read::Nanoseconds);
}

//! Why the reader does not serve a base table of this layout ("" when it does): _mssql_rid needs a
//! unique clustered key whose stored bytes it keeps.
std::string NotServed(const RowsetLayout &layout) {
	if (layout.columns.size() > MAX_SYSTEM_TABLE_COLUMNS) {
		return "it is described with " + std::to_string(layout.columns.size()) +
			   " physical columns, more than the reader takes";
	}
	bool keyed = false;
	for (const auto &c : layout.columns) {
		if (c.Uniquifier()) {
			return "its clustered index is not unique, which the reader does not serve";
		}
		if (!c.Dropped() && c.ordkey > 0) {
			if (c.xtype == XTYPE_BIT) {
				return "its key column " + c.name + " is a bit, whose stored byte the reader does not keep";
			}
			keyed = true;
		}
	}
	return keyed ? std::string() : std::string("it has no clustered key, which _mssql_rid needs");
}

//! The system base tables of the database whose shape the reader serves, by name.
std::vector<const SysObject *> ServedTables(const BootstrapTables &tables) {
	std::vector<const SysObject *> out;
	for (const auto &o : tables.objects) {
		if (o.type != "S " || tables.SystemObject(o.name) != &o) {
			continue;
		}
		try {
			if (NotServed(tables.BaseLayout(o.id)).empty()) {
				out.push_back(&o);
			}
		} catch (FormatError &) {
			// A table described in a way the reader does not take: named when it is asked for (Bind).
		}
	}
	std::sort(out.begin(), out.end(), [](const SysObject *a, const SysObject *b) { return a->name < b->name; });
	return out;
}

struct BaseTableBindData : public TableFunctionData {
	vector<std::string> paths;
	vector<std::string> diff;  // task 24
	BackupAt at;			   // task 24b
	std::string table;		   // "" lists the tables served
	std::string fuzzy;		   // D11: fuzzy := as given, else the ATTACH's option; "" = the setting
	std::shared_ptr<OpenedCatalog> catalog;
	RowsetLayout layout;			 // the table's
	std::vector<size_t> columns;	 // its served columns: layout indexes (not dropped, not the uniquifier)
	std::vector<LogicalType> types;	 // of `columns`
	std::vector<size_t> key;		 // layout indexes of its clustered key, in ordkey order

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<BaseTableBindData>();
		copy->paths = paths;
		copy->diff = diff;
		copy->at = at;
		copy->table = table;
		copy->fuzzy = fuzzy;
		copy->catalog = catalog;
		copy->layout = layout;
		copy->columns = columns;
		copy->types = types;
		copy->key = key;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other_p) const override {
		const auto &other = other_p.Cast<BaseTableBindData>();
		return paths == other.paths && diff == other.diff && at.Same(other.at) && table == other.table &&
			   fuzzy == other.fuzzy;
	}
	//! Every EXECUTE binds again: the file is read at bind.
	bool SupportStatementCache() const override {
		return false;
	}
};

struct BaseTableState : public GlobalTableFunctionState {
	std::vector<column_t> column_ids;
	std::vector<const SysObject *> listed;	// the listing's tables
	const SysAllocUnit *unit = nullptr;
	const SysAllocUnit *lob = nullptr;
	std::vector<PagePointer> pages;	 // the unit's IAM-listed pages
	size_t next_page = 0;
	std::vector<SystemRow> pending;	 // the kept rows of the page being produced
	size_t next_row = 0;
	uint64_t off_row_bytes = 0;
	size_t next_listed = 0;		  // the listing's cursor
	bool pending_warned = false;  // a page that became live while the backup ran, said once
	//! A value the reader cannot give is NULL, never a refusal of the table (owner, 2026-11-20): per
	//! output column (and _mssql_rid last) how many, the first's reason, and whether its WARNING is said.
	struct Unread {
		uint64_t count = 0;
		std::string first;
		bool warned = false;
	};
	std::vector<Unread> unread;
	void NoteUnread(size_t at, const std::string &why) {
		if (unread[at].count++ == 0) {
			unread[at].first = why;
		}
	}
};

LogicalType ListingColumnType() {
	child_list_t<LogicalType> f;
	f.emplace_back("name", LogicalType::VARCHAR);
	f.emplace_back("duckdb_type", LogicalType::VARCHAR);
	f.emplace_back("key_ordinal", LogicalType::INTEGER);
	return LogicalType::STRUCT(std::move(f));
}

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
							  vector<Identifier> &names) {
	auto data = make_uniq<BaseTableBindData>();
	data->paths = PathsOf(input.inputs[0], FUNCTION);
	data->diff = DiffPathsOf(input.named_parameters, FUNCTION);
	data->at = BackupAtOf(input.named_parameters, FUNCTION);
	data->fuzzy = FuzzyParameterOf(input.named_parameters, FUNCTION);
	if (data->paths.size() == 1) {
		// Over an attached backup, the version pinned now by its files (task 24b, owner 2026-11-19).
		PinAttachedVersion(context, data->paths.front(), data->at);
		if (data->fuzzy.empty()) {
			data->fuzzy = AttachedFuzzy(context, data->paths.front());
		}
	}
	if (input.inputs.size() > 1) {
		if (input.inputs[1].IsNull()) {
			throw InvalidInputException("%s: the base table is NULL", FUNCTION);
		}
		data->table = input.inputs[1].ToString();
	}
	try {
		data->catalog =
			OpenBackupCatalog(context, std::vector<std::string>(data->paths.begin(), data->paths.end()), false,
							  CatalogScope(), std::vector<std::string>(data->diff.begin(), data->diff.end()), data->at);
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", FUNCTION, e.what());
	}
	if (data->table.empty()) {
		names = {"base_table", "object_id", "row_count", "columns"};
		return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::BIGINT,
						LogicalType::LIST(ListingColumnType())};
		return std::move(data);
	}
	const BootstrapTables &tables = *data->catalog->tables;
	const SysObject *object = tables.SystemObject(data->table);
	if (!object) {
		throw InvalidInputException("%s: the database has no system table %s", FUNCTION, data->table);
	}
	try {
		data->layout = tables.BaseLayout(object->id);
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", FUNCTION, e.what());
	}
	data->layout.table = data->table;
	const std::string why = NotServed(data->layout);
	if (!why.empty()) {
		throw InvalidInputException("%s: %s is not read: %s", FUNCTION, data->table, why);
	}
	std::vector<std::pair<int16_t, size_t>> key;
	for (size_t i = 0; i < data->layout.columns.size(); i++) {
		const PhysicalColumn &c = data->layout.columns[i];
		if (c.Dropped()) {
			continue;
		}
		data->columns.push_back(i);
		data->types.push_back(TypeOf(data->layout, c));
		names.emplace_back(c.name);
		return_types.push_back(data->types.back());
		if (c.ordkey > 0) {
			key.emplace_back(c.ordkey, i);
		}
	}
	std::sort(key.begin(), key.end());
	for (const auto &k : key) {
		data->key.push_back(k.second);
	}
	// D11 for a base table (review of #591: its rows were read from a dirty or stale copy without a word):
	// its units against the backup's log tail, as the scan checks a user table's.
	std::vector<uint64_t> units;
	for (const SysAllocUnit *unit : tables.UnitsOf(data->layout.rowset_id)) {
		units.push_back(unit->auid);
	}
	CheckStatementFuzzy(context, *data->catalog, units, "sys." + data->table, "sys." + data->table,
						FuzzyModeOf(context, data->fuzzy, FUNCTION), FUNCTION);
	return std::move(data);
}

virtual_column_map_t VirtualColumns(ClientContext &, optional_ptr<FunctionData> bind_data) {
	virtual_column_map_t result;
	if (bind_data && bind_data->Cast<BaseTableBindData>().table.empty()) {
		return result;	// the listing has none
	}
	result.insert(make_pair(PAGE_COLUMN, TableColumn("_mssql_page", LogicalType::VARCHAR)));
	result.insert(make_pair(RID_COLUMN, TableColumn("_mssql_rid", LogicalType::BLOB)));
	return result;
}

unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &input) {
	const auto &data = input.bind_data->Cast<BaseTableBindData>();
	auto state = make_uniq<BaseTableState>();
	state->column_ids = input.column_ids;
	state->unread.resize(data.columns.size() + 1);
	if (data.table.empty()) {
		state->listed = ServedTables(*data.catalog->tables);
		return std::move(state);
	}
	BindFiles bound(data.catalog->Binding());
	const BootstrapTables &tables = *data.catalog->tables;
	const PageStore &store = *data.catalog->pages->store;
	try {
		state->unit = &tables.InRowUnit(data.layout.rowset_id, data.table);
		state->lob = tables.FindLobUnit(data.layout.rowset_id);
		state->pages = UnitPages(store, *state->unit);
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", FUNCTION, e.what());
	}
	return std::move(state);
}

// The kept rows of the next data page of the unit into state.pending; false when no page is left.
bool LoadNextPage(ClientContext &context, const BaseTableBindData &data, BaseTableState &state) {
	const PageStore &store = *data.catalog->pages->store;
	Page page;
	while (state.next_page < state.pages.size()) {
		const PagePointer at = state.pages[state.next_page++];
		try {
			try {
				if (!ReadUnitDataPage(store, *state.unit, at, data.table, page)) {
					continue;
				}
			} catch (ZeroCopyError &e) {
				ThrowPendingOrFormat(context, data.catalog->opened, e);
			}
		} catch (PendingPageError &e) {
			// A page that became live while the backup ran, as the scan takes it: refused under fuzzy :=
			// 'error', said once under 'warn', passed over under 'ignore' (review of #591: it failed the
			// statement whatever the policy).
			const FuzzyMode mode = FuzzyModeOf(context, data.fuzzy, FUNCTION);
			if (mode == FuzzyMode::Error) {
				throw InvalidInputException(
					"%s: sys.%s: %s (fuzzy := 'error'; under 'warn' the table's other pages are read)", FUNCTION,
					data.table, e.what());
			}
			if (mode == FuzzyMode::Warn && !state.pending_warned) {
				state.pending_warned = true;
				DUCKDB_LOG_WARNING(context,
								   "%s: sys.%s: %s; its rows are not read, the table's other pages are (a fuzzy "
								   "backup: spec 090 replays the tail; any further such page of the table is "
								   "passed over without another WARNING)",
								   FUNCTION, data.table, e.what());
			}
			continue;
		}
		state.pending.clear();
		state.next_row = 0;
		ForEachPageRow(page, data.layout, [&](const SystemRow &row) {
			SystemRow kept = row;
			// An off-row value (a long definition) read in full, within its bound and the call's; one that
			// cannot be read, or past either bound, is NULL with a WARNING (Execute), its row read.
			for (size_t o = 0; o < data.columns.size(); o++) {
				const size_t i = data.columns[o];
				SystemValue &v = kept.values[i];
				if (v.is_null || !v.off_row) {
					continue;
				}
				const std::string what = data.table + "." + data.layout.columns[i].name + " at " + row.page.ToString() +
										 " slot " + std::to_string(row.slot);
				try {
					if (!state.lob) {
						throw FormatError("backup: " + what + " is stored off-row, and the table has no LOB unit");
					}
					std::string bytes = ReadLobValue(store, v.bytes, state.lob->auid, what, MAX_MODULE_TEXT_BYTES);
					if (state.off_row_bytes + bytes.size() > MAX_CALL_OFF_ROW_BYTES) {
						throw FormatError("backup: " + what + ": the call's off-row values would pass " +
										  std::to_string(MAX_CALL_OFF_ROW_BYTES) + " bytes in all");
					}
					state.off_row_bytes += bytes.size();
					v.bytes = std::move(bytes);
				} catch (FormatError &e) {
					v.is_null = true;
					v.bytes.clear();
					state.NoteUnread(o, e.what());
				}
				v.off_row = false;
			}
			state.pending.push_back(std::move(kept));
		});
		if (!state.pending.empty()) {
			return true;
		}
	}
	return false;
}

std::string PageKey(const PagePointer &p) {
	char buf[16];
	snprintf(buf, sizeof(buf), "01%04X%08X", static_cast<unsigned>(p.file), static_cast<unsigned>(p.page));
	return buf;
}

std::string RowId(const BaseTableBindData &data, const SystemRow &row) {
	std::string rid(1, '\x01');
	for (size_t i : data.key) {
		const SystemValue &v = row.values[i];
		if (v.is_null) {
			rid.append("\xFF\xFF", 2);
			continue;
		}
		// u16 lengths, 0xFFFF the NULL marker: a key value of 65535 bytes or more (an off-row one, read in
		// full) cannot be written, and is named rather than truncated into another key (review of #549).
		if (v.bytes.size() >= 0xFFFF) {
			throw FormatError("backup: " + data.table + "." + data.layout.columns[i].name + ", a key column, holds " +
							  std::to_string(v.bytes.size()) + " bytes, more than _mssql_rid's u16 length takes");
		}
		const uint16_t length = static_cast<uint16_t>(v.bytes.size());
		rid.push_back(static_cast<char>(length & 0xFF));
		rid.push_back(static_cast<char>(length >> 8));
		rid += v.bytes;
	}
	return rid;
}

Value CellValue(const BaseTableBindData &data, size_t layout_index, const LogicalType &type, const SystemValue &v) {
	if (v.is_null) {
		return Value(type);
	}
	const PhysicalColumn &c = data.layout.columns[layout_index];
	auto size = [&](size_t want, const char *what) {
		if (v.bytes.size() != want) {
			throw FormatError("backup: " + data.table + "." + c.name + " holds " + std::to_string(v.bytes.size()) +
							  " bytes, not a " + what + "'s " + std::to_string(want));
		}
	};
	switch (c.xtype) {
	case XTYPE_TINYINT:
	case XTYPE_SMALLINT:
	case XTYPE_INT:
	case XTYPE_BIGINT:
	case XTYPE_BIT:
		return Value::BIGINT(ValueInt(data.layout, layout_index, v)).DefaultCastAs(type);
	case XTYPE_NVARCHAR:
	case XTYPE_NCHAR:
		return Value(ValueText(data.layout, layout_index, v));
	case XTYPE_VARCHAR:
	case XTYPE_CHAR:
		// The base tables' char columns hold ASCII (an object's type); anything else is named, not guessed.
		if (!Utf8Proc::IsValid(v.bytes.data(), v.bytes.size())) {
			throw FormatError("backup: " + data.table + "." + c.name + " holds bytes that are not UTF-8");
		}
		return Value(v.bytes);
	case XTYPE_DATETIME:
		size(8, "datetime");
		return RecordDatetimeValue(v.bytes);
	case XTYPE_SMALLDATETIME:
		size(4, "smalldatetime");
		return RecordSmallDatetimeValue(v.bytes);
	case XTYPE_UNIQUEIDENTIFIER:
		return GuidValue(v.bytes, data.table + "." + c.name);
	default:  // binary, varbinary, sql_variant
		return Value::BLOB(reinterpret_cast<const_data_ptr_t>(v.bytes.data()), v.bytes.size());
	}
}

void ExecuteListing(const BaseTableBindData &data, BaseTableState &state, DataChunk &output) {
	const BootstrapTables &tables = *data.catalog->tables;
	idx_t count = 0;
	for (; state.next_listed < state.listed.size() && count < STANDARD_VECTOR_SIZE; state.next_listed++) {
		const SysObject *object = state.listed[state.next_listed];
		const std::string &name = object->name;
		RowsetLayout layout = tables.BaseLayout(object->id);
		layout.table = name;
		// The rowset's own count: the rows served.
		Value rows(LogicalType::BIGINT);
		for (const SysRowset *r : tables.RowsetsOf(object->id)) {
			if (r->rowset_id == layout.rowset_id) {
				rows = Value::BIGINT(r->rows);
			}
		}
		vector<Value> columns;
		for (const auto &c : layout.columns) {
			if (c.Dropped() || c.Uniquifier()) {
				continue;
			}
			child_list_t<Value> f;
			f.emplace_back("name", Value(c.name));
			f.emplace_back("duckdb_type", Value(TypeOf(layout, c).ToString()));
			f.emplace_back("key_ordinal", c.ordkey > 0 ? Value::INTEGER(c.ordkey) : Value(LogicalType::INTEGER));
			columns.push_back(Value::STRUCT(std::move(f)));
		}
		const std::vector<Value> row = {Value(name), Value::INTEGER(object->id), rows,
										Value::LIST(ListingColumnType(), std::move(columns))};
		for (idx_t c = 0; c < state.column_ids.size(); c++) {
			const column_t id = state.column_ids[c];
			if (id < row.size()) {
				output.data[c].SetValue(count, row[id]);
			}
		}
		count++;
	}
	output.SetChildCardinality(count);
}

//! One WARNING a column a call, as its first value read as NULL is met.
void SurfaceUnread(ClientContext &context, const BaseTableBindData &data, BaseTableState &state) {
	for (size_t o = 0; o < state.unread.size(); o++) {
		auto &u = state.unread[o];
		if (u.count == 0 || u.warned) {
			continue;
		}
		u.warned = true;
		const std::string column = o < data.columns.size() ? data.layout.columns[data.columns[o]].name : "_mssql_rid";
		DUCKDB_LOG_WARNING(context,
						   "%s: %s.%s: a value the reader cannot give is read as NULL, its row read (further ones of "
						   "the column without another WARNING): %s",
						   FUNCTION, data.table, column, u.first);
	}
}

void Execute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	const auto &data = input.bind_data->Cast<BaseTableBindData>();
	auto &state = input.global_state->Cast<BaseTableState>();
	BindFiles bound(data.catalog->Binding());
	try {
		if (data.table.empty()) {
			ExecuteListing(data, state, output);
			return;
		}
		idx_t count = 0;
		while (count < STANDARD_VECTOR_SIZE) {
			if (state.next_row >= state.pending.size() && !LoadNextPage(context, data, state)) {
				break;
			}
			const SystemRow &row = state.pending[state.next_row++];
			for (idx_t c = 0; c < state.column_ids.size(); c++) {
				const column_t id = state.column_ids[c];
				if (id == PAGE_COLUMN) {
					output.data[c].SetValue(count, Value(PageKey(row.page)));
				} else if (id == RID_COLUMN) {
					try {
						const std::string rid = RowId(data, row);
						output.data[c].SetValue(
							count, Value::BLOB(reinterpret_cast<const_data_ptr_t>(rid.data()), rid.size()));
					} catch (FormatError &e) {
						output.data[c].SetValue(count, Value(LogicalType::BLOB));
						state.NoteUnread(data.columns.size(), e.what());
					}
				} else if (id < data.columns.size()) {
					const size_t index = data.columns[id];
					try {
						output.data[c].SetValue(count, CellValue(data, index, data.types[id], row.values[index]));
					} catch (FormatError &e) {
						output.data[c].SetValue(count, Value(data.types[id]));
						state.NoteUnread(id, e.what());
					}
				}
			}
			count++;
		}
		output.SetChildCardinality(count);
		SurfaceUnread(context, data, state);
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", FUNCTION, e.what());
	}
}

}  // namespace

void RegisterBackupCatalogFunction(ExtensionLoader &loader) {
	TableFunctionSet set(FUNCTION);
	const vector<LogicalType> paths{LogicalType(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR)};
	for (const auto &path : paths) {
		for (size_t arity = 1; arity <= 2; arity++) {
			vector<LogicalType> arguments{path};
			if (arity == 2) {
				arguments.push_back(LogicalType::VARCHAR);
			}
			TableFunction f(FUNCTION, arguments, Execute, Bind, Init);
			AddDiffParameter(f);
			AddVersionParameters(f);
			AddFuzzyParameter(f);
			f.projection_pushdown = true;
			f.get_virtual_columns = VirtualColumns;
			set.AddFunction(f);
		}
	}
	mssql::RegisterDocumentedFunction(
		loader, std::move(set),
		{{},
		 "Reads a system base table of the database in a SQL Server full backup, as stored, for an importer to "
		 "mirror the catalog: its rows, its columns typed by its own description, and the virtual _mssql_page "
		 "(the row's page) and _mssql_rid (its clustered key). With no table, lists the base tables it reads: "
		 "every system base table with a unique clustered key, all its rows.",
		 {"SELECT * FROM mssql_backup_catalog('/backups/sales_full.bak')",
		  "SELECT _mssql_page, id, name, type FROM mssql_backup_catalog('/backups/sales_full.bak', 'sysschobjs')"},
		 {"backup"}});
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

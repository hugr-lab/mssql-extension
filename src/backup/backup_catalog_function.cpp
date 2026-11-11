// Spec 085 PR 2b step 7b (task 17): mssql_backup_catalog(path [, base_table]) -- a full backup's system
// base tables as stored, for an importer to mirror (022 D3). [DuckDB]
//
// With no base_table: the allow-listed base tables (name, object id, rows, columns). With one: its
// rows, its columns typed by its own description (sysrscols, named by syscolpars) through D6's map
// (MSSQLColumnInfo::MapSQLServerTypeToDuckDB, native types off), sql_variant as its raw bytes (BLOB:
// a VARIANT is spec 086's), plus the virtual columns of D8: _mssql_page (the row's page, '01' + 4 hex
// digits of file + 8 of page) and _mssql_rid (0x01, then per clustered key column in ordkey order u16
// length and its stored bytes, 0xFFFF for NULL). The allow-list is spec 085 § Security's, plus
// sysscalartypes (the type names an alias column needs). Two tables are served by value class only,
// each row checked against the object it belongs to (DQ-11: module source never leaves):
// sysobjvalues' valclass 1 of a view (not one WITH ENCRYPTION) or a default, with subobjid and valnum
// 0; valclass 2 of a computed column of a user table; valclass 30 of a partition function; and
// sysseobjvalues' valclass 1 (an added column's default, the one class measured). Any other table,
// such as sysowners (password hashes), is refused by name.
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
#include "duckdb/main/client_context.hpp"
#include "mssql_function_docs.hpp"
#include "utf8proc_wrapper.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

const char *const FUNCTION = "mssql_backup_catalog";

// spec 085 § Security: the mirror set of E1§4.8, sysobjvalues (by value class), sysidxstats,
// sysiscols, the partitioning tables of E7; and sysscalartypes.
const char *const ALLOWED[] = {"sysallocunits",	  "sysrowsets",		  "sysschobjs",	   "syscolpars",  "sysrscols",
							   "sysseobjvalues",  "sysclsobjs",		  "sysobjvalues",  "sysidxstats", "sysiscols",
							   "systypedsubobjs", "syssingleobjrefs", "sysscalartypes"};

const column_t PAGE_COLUMN = VIRTUAL_COLUMN_START;
const column_t RID_COLUMN = VIRTUAL_COLUMN_START + 1;

bool Allowed(const std::string &name) {
	for (const char *a : ALLOWED) {
		if (name == a) {
			return true;
		}
	}
	return false;
}

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
	const int16_t length = c.length == 0xFFFF ? int16_t(-1) : static_cast<int16_t>(c.length);
	return MSSQLColumnInfo::MapSQLServerTypeToDuckDB(name, length, c.precision, c.scale);
}

std::string AllowedList() {
	std::string out;
	for (const char *a : ALLOWED) {
		out += (out.empty() ? "" : ", ") + std::string(a);
	}
	return out;
}

//! The rows a base table may serve: every row, but sysobjvalues' by ObjectValueFilter (DQ-11) and
//! sysseobjvalues' by its valclass 1.
struct RowFilter {
	enum class Kind : uint8_t { All, ObjectValues, AddedDefaults } kind = Kind::All;
	size_t valclass = 0, objid = 0, subobjid = 0, valnum = 0;
	std::unique_ptr<ObjectValueFilter> object_values;

	bool Keep(const RowsetLayout &layout, const SystemRow &row) const {
		if (kind == Kind::All) {
			return true;
		}
		const int64_t cls = ValueInt(layout, valclass, row.values[valclass], -1);
		if (kind == Kind::AddedDefaults) {
			return cls == 1;
		}
		return object_values->Keep(cls, static_cast<int32_t>(ValueInt(layout, objid, row.values[objid], 0)),
								   ValueInt(layout, subobjid, row.values[subobjid], -1),
								   ValueInt(layout, valnum, row.values[valnum], -1));
	}
};

RowFilter FilterFor(const PageStore &store, const BootstrapTables &tables, const std::string &table,
					const RowsetLayout &layout) {
	RowFilter f;
	if (table == "sysseobjvalues") {
		f.kind = RowFilter::Kind::AddedDefaults;
		f.valclass = layout.IndexOf("valclass");
		return f;
	}
	if (table != "sysobjvalues") {
		return f;
	}
	f.kind = RowFilter::Kind::ObjectValues;
	f.valclass = layout.IndexOf("valclass");
	f.objid = layout.IndexOf("objid");
	f.subobjid = layout.IndexOf("subobjid");
	f.valnum = layout.IndexOf("valnum");
	f.object_values.reset(new ObjectValueFilter(tables, ReadPartitionFunctions(store, tables)));
	return f;
}

struct BaseTableBindData : public TableFunctionData {
	vector<std::string> paths;
	std::string table;	// "" lists the allow-listed tables
	std::shared_ptr<OpenedCatalog> catalog;
	RowsetLayout layout;			 // the table's
	std::vector<size_t> columns;	 // its served columns: layout indexes (not dropped, not the uniquifier)
	std::vector<LogicalType> types;	 // of `columns`
	std::vector<size_t> key;		 // layout indexes of its clustered key, in ordkey order

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<BaseTableBindData>();
		copy->paths = paths;
		copy->table = table;
		copy->catalog = catalog;
		copy->layout = layout;
		copy->columns = columns;
		copy->types = types;
		copy->key = key;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other_p) const override {
		const auto &other = other_p.Cast<BaseTableBindData>();
		return paths == other.paths && table == other.table;
	}
	//! Every EXECUTE binds again: the file is read at bind.
	bool SupportStatementCache() const override {
		return false;
	}
};

struct BaseTableState : public GlobalTableFunctionState {
	std::vector<column_t> column_ids;
	RowFilter filter;
	const SysAllocUnit *unit = nullptr;
	const SysAllocUnit *lob = nullptr;
	std::vector<PagePointer> pages;	 // the unit's IAM-listed pages
	size_t next_page = 0;
	std::vector<SystemRow> pending;	 // the kept rows of the page being produced
	size_t next_row = 0;
	uint64_t off_row_bytes = 0;
	size_t next_listed = 0;	 // the listing's cursor
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
	if (input.inputs.size() > 1) {
		if (input.inputs[1].IsNull()) {
			throw InvalidInputException("%s: the base table is NULL", FUNCTION);
		}
		data->table = input.inputs[1].ToString();
		if (!Allowed(data->table)) {
			throw InvalidInputException("%s: %s is not a base table the reader serves (it serves %s)", FUNCTION,
										data->table, AllowedList());
		}
	}
	try {
		data->catalog =
			OpenBackupCatalog(context, std::vector<std::string>(data->paths.begin(), data->paths.end()), false);
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", FUNCTION, e.what());
	}
	if (data->table.empty()) {
		names = {"base_table", "object_id", "row_count", "columns"};
		return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::BIGINT,
						LogicalType::LIST(ListingColumnType())};
		return std::move(data);
	}
	const BootstrapTables &tables = data->catalog->tables;
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
	if (data->layout.columns.size() > MAX_SYSTEM_TABLE_COLUMNS) {
		throw InvalidInputException("%s: %s is described with %d physical columns, more than the reader takes",
									FUNCTION, data->table, static_cast<int>(data->layout.columns.size()));
	}
	std::vector<std::pair<int16_t, size_t>> key;
	for (size_t i = 0; i < data->layout.columns.size(); i++) {
		const PhysicalColumn &c = data->layout.columns[i];
		if (c.Uniquifier()) {
			throw InvalidInputException("%s: %s's clustered index is not unique, which the reader does not serve",
										FUNCTION, data->table);
		}
		if (c.Dropped()) {
			continue;
		}
		data->columns.push_back(i);
		data->types.push_back(TypeOf(data->layout, c));
		names.emplace_back(c.name);
		return_types.push_back(data->types.back());
		if (c.ordkey > 0) {
			if (c.xtype == XTYPE_BIT) {
				throw InvalidInputException(
					"%s: %s's key column %s is a bit, whose stored byte the reader does not keep", FUNCTION,
					data->table, c.name);
			}
			key.emplace_back(c.ordkey, i);
		}
	}
	if (key.empty()) {
		throw InvalidInputException("%s: %s has no clustered key, which _mssql_rid needs", FUNCTION, data->table);
	}
	std::sort(key.begin(), key.end());
	for (const auto &k : key) {
		data->key.push_back(k.second);
	}
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
	if (data.table.empty()) {
		return std::move(state);
	}
	const BootstrapTables &tables = data.catalog->tables;
	const PageStore &store = *data.catalog->pages->store;
	try {
		state->filter = FilterFor(store, tables, data.table, data.layout);
		state->unit = &tables.InRowUnit(data.layout.rowset_id, data.table);
		state->lob = tables.FindLobUnit(data.layout.rowset_id);
		state->pages = UnitPages(store, *state->unit);
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", FUNCTION, e.what());
	}
	return std::move(state);
}

// The kept rows of the next data page of the unit into state.pending; false when no page is left.
bool LoadNextPage(const BaseTableBindData &data, BaseTableState &state) {
	const PageStore &store = *data.catalog->pages->store;
	Page page;
	while (state.next_page < state.pages.size()) {
		const PagePointer at = state.pages[state.next_page++];
		if (!ReadUnitDataPage(store, *state.unit, at, data.table, page)) {
			continue;
		}
		state.pending.clear();
		state.next_row = 0;
		ForEachPageRow(page, data.layout, [&](const SystemRow &row) {
			if (!state.filter.Keep(data.layout, row)) {
				return;
			}
			SystemRow kept = row;
			// An off-row value (a long definition) read in full, within its bound and the call's.
			for (size_t i : data.columns) {
				SystemValue &v = kept.values[i];
				if (v.is_null || !v.off_row) {
					continue;
				}
				const std::string what = data.table + "." + data.layout.columns[i].name + " at " + row.page.ToString() +
										 " slot " + std::to_string(row.slot);
				if (!state.lob) {
					throw FormatError("backup: " + what + " is stored off-row, and the table has no LOB unit");
				}
				v.bytes = ReadLobValue(store, v.bytes, state.lob->auid, what, MAX_MODULE_TEXT_BYTES);
				v.off_row = false;
				state.off_row_bytes += v.bytes.size();
				if (state.off_row_bytes > MAX_CALL_OFF_ROW_BYTES) {
					throw FormatError("backup: " + data.table + "'s off-row values pass " +
									  std::to_string(MAX_CALL_OFF_ROW_BYTES) +
									  " bytes in all, more than one call reads");
				}
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
	case XTYPE_UNIQUEIDENTIFIER: {
		size(16, "uniqueidentifier");
		const uint8_t *b = reinterpret_cast<const uint8_t *>(v.bytes.data());
		// SQL Server's GUID byte order: the first three groups little-endian.
		char text[37];
		snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[3], b[2],
				 b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
		return Value(text).DefaultCastAs(LogicalType::UUID);
	}
	default:  // binary, varbinary, sql_variant
		return Value::BLOB(reinterpret_cast<const_data_ptr_t>(v.bytes.data()), v.bytes.size());
	}
}

void ExecuteListing(const BaseTableBindData &data, BaseTableState &state, DataChunk &output) {
	const BootstrapTables &tables = data.catalog->tables;
	idx_t count = 0;
	for (; state.next_listed < sizeof(ALLOWED) / sizeof(ALLOWED[0]) && count < STANDARD_VECTOR_SIZE;
		 state.next_listed++) {
		const std::string name = ALLOWED[state.next_listed];
		const SysObject *object = tables.SystemObject(name);
		if (!object) {
			continue;  // not in this version
		}
		RowsetLayout layout = tables.BaseLayout(object->id);
		layout.table = name;
		// The rowset's own count; for a table served by value class it is not the rows served: NULL.
		Value rows(LogicalType::BIGINT);
		if (name != "sysobjvalues" && name != "sysseobjvalues") {
			for (const SysRowset *r : tables.RowsetsOf(object->id)) {
				if (r->rowset_id == layout.rowset_id) {
					rows = Value::BIGINT(r->rows);
				}
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

void Execute(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	const auto &data = input.bind_data->Cast<BaseTableBindData>();
	auto &state = input.global_state->Cast<BaseTableState>();
	try {
		if (data.table.empty()) {
			ExecuteListing(data, state, output);
			return;
		}
		idx_t count = 0;
		while (count < STANDARD_VECTOR_SIZE) {
			if (state.next_row >= state.pending.size() && !LoadNextPage(data, state)) {
				break;
			}
			const SystemRow &row = state.pending[state.next_row++];
			for (idx_t c = 0; c < state.column_ids.size(); c++) {
				const column_t id = state.column_ids[c];
				if (id == PAGE_COLUMN) {
					output.data[c].SetValue(count, Value(PageKey(row.page)));
				} else if (id == RID_COLUMN) {
					const std::string rid = RowId(data, row);
					output.data[c].SetValue(count,
											Value::BLOB(reinterpret_cast<const_data_ptr_t>(rid.data()), rid.size()));
				} else if (id < data.columns.size()) {
					const size_t index = data.columns[id];
					output.data[c].SetValue(count, CellValue(data, index, data.types[id], row.values[index]));
				}
			}
			count++;
		}
		output.SetChildCardinality(count);
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
		 "(the row's page) and _mssql_rid (its clustered key). With no table, lists the base tables it serves "
		 "(an allow-list; sysobjvalues only by value class: view, default and computed-column definitions and "
		 "partition boundaries, never module source).",
		 {"SELECT * FROM mssql_backup_catalog('/backups/sales_full.bak')",
		  "SELECT _mssql_page, id, name, type FROM mssql_backup_catalog('/backups/sales_full.bak', 'sysschobjs')"},
		 {"backup"}});
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

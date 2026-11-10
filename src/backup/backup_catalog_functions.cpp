// Spec 085 PR 2b step 7 (task 17): mssql_backup_tables(path) and mssql_backup_views(path) -- the
// catalog model of a full backup (backup_catalog_model), one row per user table / view (spec 085
// § Functions). [DuckDB]
//
// The catalog is opened at init (stages 1-4, OpenBackupCatalog), so DESCRIBE reads no file and every
// execution reads the file anew; the opened-file cache shared across statements is task 22. Columns
// whose source is a later PR are NULL until it lands: fuzzy_status / open_transactions /
// stale_transactions (D11, PR 5), allocation_units.live_pages (a unit's IAM chain is walked at its
// table's first scan, DQ-40, not to list it), partitioning (PR 2c). A column's DuckDB type is D6's:
// MSSQLColumnInfo::MapSQLServerTypeToDuckDB, TYPE mssql's map with native types off (a plain VARCHAR
// where mssql_catalog_native_types reports MSSQL_NVARCHAR(n)).

#include "backup/backup_catalog_functions.hpp"

#include "backup/backup_bytes.hpp"
#include "backup/backup_collation.hpp"
#include "backup/backup_opened.hpp"
#include "catalog/mssql_column_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "mssql_function_docs.hpp"
#include "tds/encoding/datetime_encoding.hpp"

#include <cstring>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

struct Column {
	const char *name;
	LogicalType type;
};

LogicalType ColumnStructType() {
	child_list_t<LogicalType> f;
	f.emplace_back("column_id", LogicalType::INTEGER);
	f.emplace_back("name", LogicalType::VARCHAR);
	f.emplace_back("sql_type", LogicalType::VARCHAR);
	f.emplace_back("system_type_id", LogicalType::INTEGER);
	f.emplace_back("max_length", LogicalType::INTEGER);
	f.emplace_back("precision", LogicalType::INTEGER);
	f.emplace_back("scale", LogicalType::INTEGER);
	f.emplace_back("collation", LogicalType::VARCHAR);
	f.emplace_back("code_page", LogicalType::INTEGER);
	f.emplace_back("is_nullable", LogicalType::BOOLEAN);
	f.emplace_back("is_identity", LogicalType::BOOLEAN);
	f.emplace_back("is_computed", LogicalType::BOOLEAN);
	f.emplace_back("is_sparse", LogicalType::BOOLEAN);
	f.emplace_back("duckdb_type", LogicalType::VARCHAR);
	f.emplace_back("decodable", LogicalType::BOOLEAN);
	f.emplace_back("reason", LogicalType::VARCHAR);
	return LogicalType::STRUCT(std::move(f));
}

LogicalType UnitStructType() {
	child_list_t<LogicalType> f;
	f.emplace_back("au_id", LogicalType::BIGINT);
	f.emplace_back("type", LogicalType::VARCHAR);
	f.emplace_back("rowset_id", LogicalType::BIGINT);
	f.emplace_back("index_id", LogicalType::INTEGER);
	f.emplace_back("partition_number", LogicalType::INTEGER);
	f.emplace_back("compression", LogicalType::VARCHAR);
	f.emplace_back("filegroup_id", LogicalType::INTEGER);
	f.emplace_back("first_page", LogicalType::VARCHAR);
	f.emplace_back("root_page", LogicalType::VARCHAR);
	f.emplace_back("first_iam_page", LogicalType::VARCHAR);
	f.emplace_back("live_pages", LogicalType::BIGINT);
	return LogicalType::STRUCT(std::move(f));
}

LogicalType PartitioningType() {
	child_list_t<LogicalType> f;
	f.emplace_back("function_name", LogicalType::VARCHAR);
	f.emplace_back("scheme_name", LogicalType::VARCHAR);
	f.emplace_back("column_name", LogicalType::VARCHAR);
	f.emplace_back("column_id", LogicalType::INTEGER);
	f.emplace_back("parameter_type", LogicalType::VARCHAR);
	f.emplace_back("range_right", LogicalType::BOOLEAN);
	f.emplace_back("boundaries", LogicalType::LIST(LogicalType::VARIANT()));
	f.emplace_back("destinations", LogicalType::LIST(LogicalType::VARCHAR));
	f.emplace_back("next_used", LogicalType::VARCHAR);
	return LogicalType::STRUCT(std::move(f));
}

std::vector<Column> TableColumns() {
	return {{"schema_name", LogicalType::VARCHAR},
			{"table_name", LogicalType::VARCHAR},
			{"object_id", LogicalType::INTEGER},
			{"shape", LogicalType::VARCHAR},
			{"compression", LogicalType::VARCHAR},
			{"partition_count", LogicalType::INTEGER},
			{"row_count", LogicalType::BIGINT},
			{"created", LogicalType::TIMESTAMP},
			{"modified", LogicalType::TIMESTAMP},
			{"decodable", LogicalType::BOOLEAN},
			{"reason", LogicalType::VARCHAR},
			{"fuzzy_status", LogicalType::VARCHAR},
			{"open_transactions", LogicalType::INTEGER},
			{"stale_transactions", LogicalType::INTEGER},
			{"columns", LogicalType::LIST(ColumnStructType())},
			{"allocation_units", LogicalType::LIST(UnitStructType())},
			{"partitioning", PartitioningType()}};
}

std::vector<Column> ViewColumns() {
	return {{"schema_name", LogicalType::VARCHAR},	{"view_name", LogicalType::VARCHAR},
			{"object_id", LogicalType::INTEGER},	{"definition", LogicalType::VARCHAR},
			{"is_encrypted", LogicalType::BOOLEAN}, {"created", LogicalType::TIMESTAMP},
			{"modified", LogicalType::TIMESTAMP},	{"reason", LogicalType::VARCHAR}};
}

Value Text(const std::string &s) {
	return Value(s);
}

Value OptionalText(const std::string &s) {
	return s.empty() ? Value(LogicalType::VARCHAR) : Value(s);
}

//! A datetime as the catalog stores it (u32 ticks of 1/300 s, then i32 days since 1900-01-01), read as
//! TYPE mssql reads one off the wire (days, then ticks): one conversion for both.
Value DatetimeValue(const std::string &record_form) {
	if (record_form.size() != 8) {
		return Value(LogicalType::TIMESTAMP);
	}
	uint8_t wire[8];
	std::memcpy(wire, record_form.data() + 4, 4);
	std::memcpy(wire + 4, record_form.data(), 4);
	return Value::TIMESTAMP(tds::encoding::DateTimeEncoding::ConvertDatetime(wire));
}

std::string CompressionName(uint8_t level) {
	switch (level) {
	case 0:
		return "none";
	case 1:
		return "row";
	case 2:
		return "page";
	case 3:
		return "columnstore";
	case 4:
		return "columnstore_archive";
	default:
		return "level " + std::to_string(level);
	}
}

std::string ShapeName(TableShape shape) {
	switch (shape) {
	case TableShape::Heap:
		return "heap";
	case TableShape::Clustered:
		return "clustered";
	default:
		return "columnstore";
	}
}

std::string UnitTypeName(uint8_t type) {
	switch (type) {
	case 0:
		return "DROPPED";
	case 1:
		return "IN_ROW_DATA";
	case 2:
		return "LOB_DATA";
	case 3:
		return "ROW_OVERFLOW_DATA";
	default:
		return "type " + std::to_string(type);
	}
}

Value PointerValue(const PagePointer &p) {
	return p.IsNull() ? Value(LogicalType::VARCHAR) : Value(p.ToString());
}

//! The type as T-SQL declares it: a system type with its length, precision or scale; an alias or a
//! CLR type by its own name.
std::string SqlType(const CatalogColumn &c) {
	if (c.system_type_name.empty() || c.type_name != c.system_type_name) {
		return c.type_name;
	}
	switch (c.system_type_id) {
	case 165:  // varbinary
	case 167:  // varchar
	case 173:  // binary
	case 175:  // char
		return c.type_name + "(" + (c.max_length < 0 ? std::string("max") : std::to_string(c.max_length)) + ")";
	case 231:  // nvarchar
	case 239:  // nchar
		return c.type_name + "(" + (c.max_length < 0 ? std::string("max") : std::to_string(c.max_length / 2)) + ")";
	case 106:  // decimal
	case 108:  // numeric
		return c.type_name + "(" + std::to_string(c.precision) + "," + std::to_string(c.scale) + ")";
	case 41:  // time
	case 42:  // datetime2
	case 43:  // datetimeoffset
		return c.type_name + "(" + std::to_string(c.scale) + ")";
	default:
		return c.type_name;
	}
}

//! Why the scan cannot read column `c` (spec 085 § Scope's refusals), or "".
std::string ColumnRefusal(const CatalogColumn &c, uint32_t code_page) {
	switch (c.system_type_id) {
	case 98:
		return "sql_variant is not read (spec 086)";
	case 241:
		return "xml is not read (spec 086)";
	case 240:
		return (c.system_type_name.empty() ? "the CLR type " + c.type_name : c.system_type_name) +
			   " is not read (spec 086)";
	case 34:
	case 35:
	case 99:
		return "text, ntext and image are not read yet";
	default:
		break;
	}
	if (c.system_type_name.empty() || c.system_type_name == "json" || c.system_type_name == "vector") {
		return (c.type_name.empty() ? "type " + std::to_string(c.user_type_id) : c.type_name) +
			   " is not read (spec 086)";
	}
	const bool code_page_text = c.system_type_id == 167 || c.system_type_id == 175 || c.system_type_id == 35;
	if (code_page_text && c.collation_id != 0 && !CollationById(static_cast<int32_t>(c.collation_id))) {
		return "its collation (CollationId " + std::to_string(c.collation_id) + ") is not one the reader knows";
	}
	if ((c.system_type_id == 167 || c.system_type_id == 175) &&
		(code_page == 932 || code_page == 936 || code_page == 949 || code_page == 950)) {
		return "a varchar of double-byte code page " + std::to_string(code_page) + " is not read (spec 086)";
	}
	if (c.is_computed && !c.is_persisted) {
		return "a computed column that is not persisted is not stored";
	}
	if (c.is_sparse || c.is_column_set) {
		return "sparse columns are not read (spec 086)";
	}
	return std::string();
}

Value ColumnValue(const CatalogColumn &c) {
	const CollationRow *collation = c.collation_id ? CollationById(static_cast<int32_t>(c.collation_id)) : nullptr;
	const uint32_t code_page = collation ? collation->code_page : 0;
	std::string reason = ColumnRefusal(c, code_page);
	Value duckdb_type(LogicalType::VARCHAR);
	if (!c.system_type_name.empty()) {
		try {
			duckdb_type =
				Value(MSSQLColumnInfo::MapSQLServerTypeToDuckDB(c.system_type_name, c.max_length, c.precision, c.scale)
						  .ToString());
		} catch (std::exception &e) {
			if (reason.empty()) {
				reason = c.system_type_name + " has no DuckDB type: " + e.what();
			}
		}
	}
	child_list_t<Value> f;
	f.emplace_back("column_id", Value::INTEGER(c.column_id));
	f.emplace_back("name", Text(c.name));
	f.emplace_back("sql_type", Text(SqlType(c)));
	f.emplace_back("system_type_id", Value::INTEGER(c.system_type_id));
	f.emplace_back("max_length", Value::INTEGER(c.max_length));
	f.emplace_back("precision", Value::INTEGER(c.precision));
	f.emplace_back("scale", Value::INTEGER(c.scale));
	f.emplace_back("collation", collation ? Value(collation->name) : Value(LogicalType::VARCHAR));
	f.emplace_back("code_page",
				   collation ? Value::INTEGER(static_cast<int32_t>(code_page)) : Value(LogicalType::INTEGER));
	f.emplace_back("is_nullable", Value::BOOLEAN(c.is_nullable));
	f.emplace_back("is_identity", Value::BOOLEAN(c.is_identity));
	f.emplace_back("is_computed", Value::BOOLEAN(c.is_computed));
	f.emplace_back("is_sparse", Value::BOOLEAN(c.is_sparse));
	f.emplace_back("duckdb_type", duckdb_type);
	f.emplace_back("decodable", Value::BOOLEAN(reason.empty()));
	f.emplace_back("reason", OptionalText(reason));
	return Value::STRUCT(std::move(f));
}

Value UnitValue(const CatalogUnit &u) {
	child_list_t<Value> f;
	f.emplace_back("au_id", Value::BIGINT(static_cast<int64_t>(u.unit.auid)));
	f.emplace_back("type", Text(UnitTypeName(u.unit.type)));
	f.emplace_back("rowset_id", Value::BIGINT(static_cast<int64_t>(u.unit.owner_id)));
	f.emplace_back("index_id", Value::INTEGER(u.index_id));
	f.emplace_back("partition_number", Value::INTEGER(u.partition_number));
	f.emplace_back("compression", Text(CompressionName(u.compression)));
	f.emplace_back("filegroup_id", Value::INTEGER(u.unit.filegroup_id));
	f.emplace_back("first_page", PointerValue(u.unit.first_page));
	f.emplace_back("root_page", PointerValue(u.unit.root_page));
	f.emplace_back("first_iam_page", PointerValue(u.unit.first_iam));
	f.emplace_back("live_pages", Value(LogicalType::BIGINT));  // the scan's (DQ-40)
	return Value::STRUCT(std::move(f));
}

std::vector<Value> TableRow(const CatalogTable &t) {
	std::string compression;
	for (const auto &p : t.partitions) {
		const std::string name = CompressionName(p.compression);
		compression = compression.empty() || compression == name ? name : "mixed";
	}
	vector<Value> columns, units;
	for (const auto &c : t.columns) {
		columns.push_back(ColumnValue(c));
	}
	for (const auto &u : t.units) {
		units.push_back(UnitValue(u));
	}
	std::vector<Value> row;
	row.push_back(Text(t.schema_name));
	row.push_back(Text(t.name));
	row.push_back(Value::INTEGER(t.object_id));
	row.push_back(Text(ShapeName(t.shape)));
	row.push_back(OptionalText(compression));
	row.push_back(Value::INTEGER(static_cast<int32_t>(t.partitions.size())));
	row.push_back(Value::BIGINT(t.row_count));
	row.push_back(DatetimeValue(t.created));
	row.push_back(DatetimeValue(t.modified));
	row.push_back(Value::BOOLEAN(t.decodable));
	row.push_back(OptionalText(t.reason));
	row.push_back(Value(LogicalType::VARCHAR));	 // fuzzy_status: D11 (PR 5)
	row.push_back(Value(LogicalType::INTEGER));	 // open_transactions
	row.push_back(Value(LogicalType::INTEGER));	 // stale_transactions
	row.push_back(Value::LIST(ColumnStructType(), std::move(columns)));
	row.push_back(Value::LIST(UnitStructType(), std::move(units)));
	row.push_back(Value(PartitioningType()));  // PR 2c
	return row;
}

std::vector<Value> ViewRow(const PageStore &store, const CatalogView &v) {
	Value definition(LogicalType::VARCHAR);
	std::string reason = v.reason;
	auto add = [&reason](std::string why) {
		if (why.compare(0, 8, "backup: ") == 0) {
			why = why.substr(8);
		}
		reason = reason.empty() ? why : reason + "; " + why;
	};
	if (!v.is_encrypted && v.definition.state != ModuleDefinition::State::Absent) {
		try {
			definition = Value(ReadModuleDefinition(store, v.definition, "the definition of view " + v.name));
		} catch (FormatError &e) {
			add(e.what());
		}
	} else if (!v.is_encrypted) {
		add("the catalog holds no definition of the view");
	}
	std::vector<Value> row;
	row.push_back(Text(v.schema_name));
	row.push_back(Text(v.name));
	row.push_back(Value::INTEGER(v.object_id));
	row.push_back(definition);
	row.push_back(Value::BOOLEAN(v.is_encrypted));
	row.push_back(DatetimeValue(v.created));
	row.push_back(DatetimeValue(v.modified));
	row.push_back(OptionalText(reason));
	return row;
}

enum class Listing : uint8_t { Tables, Views };

struct CatalogListBindData : public TableFunctionData {
	vector<std::string> paths;
	Listing listing = Listing::Tables;

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<CatalogListBindData>();
		copy->paths = paths;
		copy->listing = listing;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other_p) const override {
		const auto &other = other_p.Cast<CatalogListBindData>();
		return paths == other.paths && listing == other.listing;
	}
};

struct CatalogListState : public GlobalTableFunctionState {
	std::unique_ptr<OpenedCatalog> catalog;
	size_t next = 0;
};

const char *FunctionName(Listing listing) {
	return listing == Listing::Tables ? "mssql_backup_tables" : "mssql_backup_views";
}

vector<std::string> PathsOf(const Value &value, const char *function) {
	vector<std::string> paths;
	if (value.IsNull()) {
		throw InvalidInputException("%s: the path is NULL", function);
	}
	if (value.type().id() == LogicalTypeId::LIST) {
		for (const auto &child : ListValue::GetChildren(value)) {
			if (child.IsNull()) {
				throw InvalidInputException("%s: a NULL in the list of paths", function);
			}
			paths.push_back(child.ToString());
		}
		if (paths.empty()) {
			throw InvalidInputException("%s: an empty list of paths", function);
		}
	} else {
		paths.push_back(value.ToString());
	}
	return paths;
}

template <Listing L>
unique_ptr<FunctionData> Bind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
							  vector<Identifier> &names) {
	auto data = make_uniq<CatalogListBindData>();
	data->listing = L;
	data->paths = PathsOf(input.inputs[0], FunctionName(L));
	for (const auto &column : L == Listing::Tables ? TableColumns() : ViewColumns()) {
		names.emplace_back(column.name);
		return_types.push_back(column.type);
	}
	return std::move(data);
}

unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
	const auto &data = input.bind_data->Cast<CatalogListBindData>();
	auto state = make_uniq<CatalogListState>();
	try {
		state->catalog = OpenBackupCatalog(context, std::vector<std::string>(data.paths.begin(), data.paths.end()));
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", FunctionName(data.listing), e.what());
	}
	return std::move(state);
}

void Execute(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	const auto &data = input.bind_data->Cast<CatalogListBindData>();
	auto &state = input.global_state->Cast<CatalogListState>();
	const CatalogModel &model = state.catalog->model;
	const size_t total = data.listing == Listing::Tables ? model.tables.size() : model.views.size();
	idx_t count = 0;
	while (state.next < total && count < STANDARD_VECTOR_SIZE) {
		const std::vector<Value> row = data.listing == Listing::Tables
										   ? TableRow(model.tables[state.next])
										   : ViewRow(*state.catalog->pages->store, model.views[state.next]);
		for (idx_t c = 0; c < row.size(); c++) {
			output.data[c].SetValue(count, row[c]);
		}
		state.next++;
		count++;
	}
	output.SetChildCardinality(count);
}

template <Listing L>
void Register(ExtensionLoader &loader, const char *description, std::vector<std::string> examples) {
	TableFunctionSet set(FunctionName(L));
	const vector<LogicalType> arguments{LogicalType(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR)};
	for (const auto &argument : arguments) {
		set.AddFunction(TableFunction(FunctionName(L), {argument}, Execute, Bind<L>, Init));
	}
	mssql::RegisterDocumentedFunction(loader, std::move(set), {{}, description, std::move(examples), {"backup"}});
}

}  // namespace

void RegisterBackupCatalogFunctions(ExtensionLoader &loader) {
	Register<Listing::Tables>(
		loader,
		"Lists the user tables in a SQL Server full backup (.bak), read from the file alone: schema, name, "
		"shape (heap / clustered / columnstore), compression, partitions, rows, dates, whether the reader can "
		"scan it (and why not), and per table its columns (SQL Server types, the DuckDB types TYPE mssql reads them "
		"as with native types off, collations, flags) and "
		"allocation units. The path is one file, a glob, or a list: every family (stripe) of one media set.",
		{"SELECT schema_name, table_name, row_count FROM mssql_backup_tables('/backups/sales_full.bak')",
		 "SELECT c.name, c.sql_type, c.duckdb_type FROM (SELECT unnest(columns) AS c FROM "
		 "mssql_backup_tables('/backups/sales_full.bak') WHERE table_name = 'orders')"});
	Register<Listing::Views>(
		loader,
		"Lists the views in a SQL Server full backup (.bak), with their T-SQL definitions as stored (NULL for "
		"a view created WITH ENCRYPTION, which the reader does not de-obfuscate), read from the file alone.",
		{"SELECT schema_name, view_name, definition FROM mssql_backup_views('/backups/sales_full.bak')"});
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

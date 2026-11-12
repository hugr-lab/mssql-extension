// Spec 085 PR 2b step 7 (task 17): mssql_backup_tables(path) and mssql_backup_views(path) -- the
// catalog model of a full backup (backup_catalog_model), one row per user table / view (spec 085
// § Functions). [DuckDB]
//
// The catalog is opened at init (stages 1-4, OpenBackupCatalog), so DESCRIBE reads no file and every
// execution reads the file anew; the opened-file cache shared across statements is task 22. Columns
// whose source is a later PR are NULL until it lands: fuzzy_status / open_transactions /
// stale_transactions (D11, PR 5), allocation_units.live_pages (a unit's IAM chain is walked at its
// table's first scan, DQ-40, not to list it). A column's DuckDB type is D6's:
// MSSQLColumnInfo::MapSQLServerTypeToDuckDB, TYPE mssql's map with native types off (a plain VARCHAR
// where mssql_catalog_native_types reports MSSQL_NVARCHAR(n)).

#include "backup/backup_tables_function.hpp"

#include "backup/backup_bytes.hpp"
#include "backup/backup_collation.hpp"
#include "backup/backup_function_common.hpp"
#include "backup/backup_opened.hpp"
#include "backup/backup_sql_variant.hpp"
#include "catalog/mssql_column_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "mssql_function_docs.hpp"
#include "tds/encoding/datetime_encoding.hpp"

#include <cstring>
#include <set>

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
	f.emplace_back("reason", LogicalType::VARCHAR);
	return LogicalType::STRUCT(std::move(f));
}

LogicalType PartitionStructType() {
	child_list_t<LogicalType> f;
	f.emplace_back("partition_number", LogicalType::INTEGER);
	f.emplace_back("rows", LogicalType::BIGINT);
	f.emplace_back("compression", LogicalType::VARCHAR);
	f.emplace_back("filegroup", LogicalType::VARCHAR);
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
			{"partitioning", PartitioningType()},
			{"partitions", LogicalType::LIST(PartitionStructType())}};
}

std::vector<Column> SchemaColumns() {
	return {{"schema_id", LogicalType::INTEGER},
			{"schema_name", LogicalType::VARCHAR},
			{"table_count", LogicalType::INTEGER},
			{"view_count", LogicalType::INTEGER}};
}

std::vector<Column> ColumnColumns() {
	return {{"schema_name", LogicalType::VARCHAR}, {"table_name", LogicalType::VARCHAR},
			{"column_id", LogicalType::INTEGER},   {"column_name", LogicalType::VARCHAR},
			{"sql_type", LogicalType::VARCHAR},	   {"system_type", LogicalType::VARCHAR},
			{"duckdb_type", LogicalType::VARCHAR}, {"state", LogicalType::VARCHAR},
			{"reason", LogicalType::VARCHAR}};
}

std::vector<Column> ViewColumns() {
	return {{"schema_name", LogicalType::VARCHAR},
			{"view_name", LogicalType::VARCHAR},
			{"object_id", LogicalType::INTEGER},
			{"definition", LogicalType::VARCHAR},
			{"is_encrypted", LogicalType::BOOLEAN},
			{"created", LogicalType::TIMESTAMP},
			{"modified", LogicalType::TIMESTAMP},
			{"reason", LogicalType::VARCHAR},
			// Task 16f: an indexed view's stored rows (NULL shape, compression and rows otherwise).
			{"is_indexed", LogicalType::BOOLEAN},
			{"shape", LogicalType::VARCHAR},
			{"compression", LogicalType::VARCHAR},
			{"row_count", LogicalType::BIGINT},
			// Whether mssql_backup_scan reads the stored rows, and why not (NULL for a plain view).
			{"decodable", LogicalType::BOOLEAN},
			{"scan_reason", LogicalType::VARCHAR}};
}

Value Text(const std::string &s) {
	return Value(s);
}

Value OptionalText(const std::string &s) {
	return s.empty() ? Value(LogicalType::VARCHAR) : Value(s);
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

Value ColumnValue(const CatalogColumn &c) {
	const CollationRow *collation = c.collation_id ? CollationById(static_cast<int32_t>(c.collation_id)) : nullptr;
	const uint32_t code_page = collation ? collation->code_page : 0;
	const ColumnReading reading = ReadingOf(c);
	child_list_t<Value> f;
	f.emplace_back("column_id", Value::INTEGER(c.column_id));
	f.emplace_back("name", Text(c.name));
	f.emplace_back("sql_type", Text(SqlTypeText(c.type_name, c.system_type_name, c.system_type_id, c.max_length,
												c.precision, c.scale)));
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
	f.emplace_back("duckdb_type", Value(reading.type.ToString()));
	f.emplace_back("decodable", Value::BOOLEAN(reading.readable));
	f.emplace_back("reason", OptionalText(reading.note));
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

//! A partitioned table's partitioning (NULL on a filegroup): its boundaries as VARIANTs of the
//! parameter's DuckDB type; with a reason, the boundaries NULL (D13: named, never guessed).
Value PartitioningValue(const CatalogTable &t) {
	if (!t.partitioned) {
		return Value(PartitioningType());
	}
	const CatalogPartitioning &p = t.partitioning;
	// The model's verdict is the one given: it has decoded every boundary already (BoundaryCheck), so a
	// reason here is its reason, and without one each boundary decodes. With a reason the boundaries
	// are NULL (D13: never a partial list); the destinations and NEXT USED are given when the model
	// kept them (only the boundaries could not be given).
	const std::string &reason = p.reason;
	Value boundaries(LogicalType::LIST(LogicalType::VARIANT()));
	Value destinations(LogicalType::LIST(LogicalType::VARCHAR));
	if (reason.empty() || !p.destinations.empty()) {
		vector<Value> names;
		for (const auto &d : p.destinations) {
			names.push_back(Value(d));
		}
		destinations = Value::LIST(LogicalType::VARCHAR, std::move(names));
	}
	if (reason.empty()) {
		vector<Value> values;
		for (size_t i = 0; i < p.boundaries.size(); i++) {
			if (p.boundaries[i].is_null) {
				values.push_back(Value(LogicalType::VARIANT()));
				continue;
			}
			const std::string what = "boundary " + std::to_string(i + 1) + " of " + p.function_name;
			values.push_back(SqlVariantValue(ParseSqlVariant(p.boundaries[i].bytes, what), what)
								 .DefaultCastAs(LogicalType::VARIANT()));
		}
		boundaries = Value::LIST(LogicalType::VARIANT(), std::move(values));
	}
	child_list_t<Value> f;
	f.emplace_back("function_name", OptionalText(p.function_name));
	f.emplace_back("scheme_name", OptionalText(p.scheme_name));
	f.emplace_back("column_name", OptionalText(p.column_name));
	f.emplace_back("column_id", p.column_id ? Value::INTEGER(p.column_id) : Value(LogicalType::INTEGER));
	f.emplace_back("parameter_type",
				   OptionalText(SqlTypeText(p.parameter_type_name, p.parameter_system_type_name, p.parameter_xtype,
											p.parameter_length, p.parameter_precision, p.parameter_scale)));
	f.emplace_back("range_right",
				   p.function_name.empty() ? Value(LogicalType::BOOLEAN) : Value::BOOLEAN(p.range_right));
	f.emplace_back("boundaries", boundaries);
	f.emplace_back("destinations", destinations);
	f.emplace_back("next_used", OptionalText(p.next_used));	 // empty unless the model kept it
	f.emplace_back("reason", OptionalText(reason));
	return Value::STRUCT(std::move(f));
}

//! The partitions' compression: one name, or `mixed` when they differ.
std::string CompressionOf(const CatalogTable &t) {
	std::string compression;
	for (const auto &p : t.partitions) {
		const std::string name = CompressionName(p.compression);
		compression = compression.empty() || compression == name ? name : "mixed";
	}
	return compression;
}

std::vector<Value> TableRow(const CatalogTable &t) {
	const std::string compression = CompressionOf(t);
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
	row.push_back(t.row_count_known ? Value::BIGINT(t.row_count) : Value(LogicalType::BIGINT));
	row.push_back(RecordDatetimeValue(t.created));
	row.push_back(RecordDatetimeValue(t.modified));
	row.push_back(Value::BOOLEAN(t.decodable));
	row.push_back(OptionalText(t.reason));
	row.push_back(Value(LogicalType::VARCHAR));	 // fuzzy_status: D11 (PR 5)
	row.push_back(Value(LogicalType::INTEGER));	 // open_transactions
	row.push_back(Value(LogicalType::INTEGER));	 // stale_transactions
	row.push_back(Value::LIST(ColumnStructType(), std::move(columns)));
	row.push_back(Value::LIST(UnitStructType(), std::move(units)));
	row.push_back(PartitioningValue(t));
	vector<Value> partitions;
	for (const auto &p : t.partitions) {
		child_list_t<Value> f;
		f.emplace_back("partition_number", Value::INTEGER(p.partition_number));
		f.emplace_back("rows", p.rows < 0 ? Value(LogicalType::BIGINT) : Value::BIGINT(p.rows));
		f.emplace_back("compression", Text(CompressionName(p.compression)));
		f.emplace_back("filegroup", OptionalText(p.filegroup));
		partitions.push_back(Value::STRUCT(std::move(f)));
	}
	row.push_back(Value::LIST(PartitionStructType(), std::move(partitions)));
	return row;
}

// One schema (sysclsobjs class 50, every one the database has, the system schemas and the roles'
// included, as sys.schemas lists them) with the user tables and views the catalog lists in it.
std::vector<Value> SchemaRow(const CatalogModel &model, const CatalogSchema &s) {
	int32_t tables = 0, views = 0;
	for (const auto &t : model.tables) {
		tables += t.schema_id == s.id;
	}
	for (const auto &v : model.views) {
		views += v.schema_id == s.id;
	}
	return {Value::INTEGER(s.id), Text(s.name), Value::INTEGER(tables), Value::INTEGER(views)};
}

// `off_row_bytes`: the call's off-row definitions so far, within MAX_CALL_OFF_ROW_BYTES in all (as
// mssql_backup_catalog's; review of #549): past it a view's definition is NULL with the reason.
std::vector<Value> ViewRow(const PageStore &store, const CatalogModel &model, const CatalogView &v,
						   uint64_t &off_row_bytes) {
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
			const bool off_row = v.definition.state == ModuleDefinition::State::Deferred;
			if (off_row && off_row_bytes >= MAX_CALL_OFF_ROW_BYTES) {
				add("its definition is stored off row, and the call's off-row definitions pass " +
					std::to_string(MAX_CALL_OFF_ROW_BYTES) + " bytes in all");
			} else {
				std::string text = ReadModuleDefinition(store, v.definition, "the definition of view " + v.name);
				if (off_row) {
					off_row_bytes += text.size();
				}
				definition = Value(std::move(text));
			}
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
	row.push_back(RecordDatetimeValue(v.created));
	row.push_back(RecordDatetimeValue(v.modified));
	row.push_back(OptionalText(reason));
	const CatalogTable *stored = v.indexed ? model.FindIndexedView(v.schema_name, v.name) : nullptr;
	row.push_back(Value::BOOLEAN(stored != nullptr));
	row.push_back(stored ? Text(ShapeName(stored->shape)) : Value(LogicalType::VARCHAR));
	row.push_back(stored ? OptionalText(CompressionOf(*stored)) : Value(LogicalType::VARCHAR));
	row.push_back(stored && stored->row_count_known ? Value::BIGINT(stored->row_count) : Value(LogicalType::BIGINT));
	row.push_back(stored ? Value::BOOLEAN(stored->decodable) : Value(LogicalType::BOOLEAN));
	row.push_back(stored ? OptionalText(stored->reason) : Value(LogicalType::VARCHAR));
	return row;
}

// mssql_backup_columns (task 16d): one row per column of a user table, its state at a glance (StateOf).
std::vector<Value> ColumnRow(const CatalogTable &t, const CatalogColumn &c) {
	const ColumnReading reading = ReadingOf(c);
	const ColumnState state = StateOf(t, c);
	return {Text(t.schema_name), Text(t.name), Value::INTEGER(c.column_id), Text(c.name),
			Text(SqlTypeText(c.type_name, c.system_type_name, c.system_type_id, c.max_length, c.precision, c.scale)),
			// The system type underneath, declared (an alias type's base: Name -> nvarchar(50)); NULL for a
			// user-defined CLR type, which has none.
			OptionalText(SqlTypeText(c.system_type_name, c.system_type_name, c.system_type_id, c.max_length,
									 c.precision, c.scale)),
			Value(reading.type.ToString()), Value(state.state), OptionalText(state.reason)};
}

enum class Listing : uint8_t { Tables, Views, Schemas, Columns };

struct CatalogListBindData : public TableFunctionData {
	vector<std::string> paths;
	Listing listing = Listing::Tables;
	//! mssql_backup_columns' optional schema and table: only that schema's (that table's) columns.
	bool by_schema = false;
	bool by_table = false;
	std::string schema;
	std::string table;
	//! The objects the model is built for (task 16e): the listing's kind, the arguments, and the
	//! names the query's filters ask for (PushdownNames).
	CatalogScope scope;

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<CatalogListBindData>();
		copy->paths = paths;
		copy->listing = listing;
		copy->by_schema = by_schema;
		copy->by_table = by_table;
		copy->schema = schema;
		copy->table = table;
		copy->scope = scope;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other_p) const override {
		const auto &other = other_p.Cast<CatalogListBindData>();
		return paths == other.paths && listing == other.listing && by_schema == other.by_schema &&
			   by_table == other.by_table && schema == other.schema && table == other.table && scope == other.scope;
	}
	bool Wants(const CatalogTable &t) const {
		return (!by_schema || t.schema_name == schema) && (!by_table || t.name == table);
	}
};

struct CatalogListState : public GlobalTableFunctionState {
	std::unique_ptr<OpenedCatalog> catalog;
	size_t next = 0;
	size_t next_column = 0;		 // mssql_backup_columns: the next column of table `next`
	uint64_t off_row_bytes = 0;	 // the views' off-row definitions read so far
};

const char *FunctionName(Listing listing) {
	return listing == Listing::Tables	 ? "mssql_backup_tables"
		   : listing == Listing::Views	 ? "mssql_backup_views"
		   : listing == Listing::Columns ? "mssql_backup_columns"
										 : "mssql_backup_schemas";
}

//! A name argument of mssql_backup_columns (schema, table): a NULL is refused naming it.
std::string NameArgument(const Value &value, const char *what) {
	if (value.IsNull()) {
		throw InvalidInputException("mssql_backup_columns: the %s is NULL (leave it out to list every %s)", what, what);
	}
	return StringValue::Get(value);
}

template <Listing L>
unique_ptr<FunctionData> Bind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
							  vector<Identifier> &names) {
	auto data = make_uniq<CatalogListBindData>();
	data->listing = L;
	data->paths = PathsOf(input.inputs[0], FunctionName(L));
	if (L == Listing::Columns && input.inputs.size() > 1) {
		data->by_schema = true;
		data->schema = NameArgument(input.inputs[1], "schema");
	}
	if (L == Listing::Columns && input.inputs.size() > 2) {
		data->by_table = true;
		data->table = NameArgument(input.inputs[2], "table");
	}
	// What the listing describes: a table listing no view, a view listing no table; the arguments.
	data->scope.no_views = L == Listing::Tables || L == Listing::Columns;
	data->scope.no_tables = L == Listing::Views;
	if (data->by_schema) {
		data->scope.by_schema = true;
		data->scope.schemas = {data->schema};
	}
	if (data->by_table) {
		data->scope.by_table = true;
		data->scope.tables = {data->table};
	}
	for (const auto &column : L == Listing::Tables	  ? TableColumns()
							  : L == Listing::Views	  ? ViewColumns()
							  : L == Listing::Columns ? ColumnColumns()
													  : SchemaColumns()) {
		names.emplace_back(column.name);
		return_types.push_back(column.type);
	}
	return std::move(data);
}

//! The names `filter` asks one column for: `c = 'x'`, `c IN ('x', ...)`, or an OR of those on one
//! column, `c` a bare column of the function (its output index into `column`). False for anything else
//! (a function of the column, a collation, another type, a NULL): that filter narrows nothing.
bool NamesOf(const Expression &filter, const LogicalGet &get, idx_t &column, std::set<std::string> &names) {
	auto column_of = [&](const Expression &e, idx_t &out) {
		if (e.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		const idx_t at = e.Cast<BoundColumnRefExpression>().Binding().column_index;
		const auto &ids = get.GetColumnIds();
		if (at >= ids.size() || ids[at].IsVirtualColumn()) {
			return false;
		}
		out = ids[at].GetPrimaryIndex();
		return true;
	};
	auto name_of = [](const Expression &e, std::string &out) {
		if (e.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			return false;
		}
		const Value &v = e.Cast<BoundConstantExpression>().GetValue();
		if (v.IsNull() || v.type().id() != LogicalTypeId::VARCHAR) {
			return false;
		}
		out = StringValue::Get(v);
		return true;
	};
	if (filter.GetExpressionType() == ExpressionType::COMPARE_EQUAL &&
		filter.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		const auto &f = filter.Cast<BoundFunctionExpression>();
		const Expression &l = BoundComparisonExpression::Left(f);
		const Expression &r = BoundComparisonExpression::Right(f);
		std::string name;
		if ((column_of(l, column) && name_of(r, name)) || (column_of(r, column) && name_of(l, name))) {
			names.insert(name);
			return true;
		}
		return false;
	}
	if (filter.GetExpressionType() == ExpressionType::COMPARE_IN &&
		filter.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
		const auto &children = filter.Cast<BoundOperatorExpression>().GetChildren();
		if (children.size() < 2 || !column_of(*children[0], column)) {
			return false;
		}
		for (size_t i = 1; i < children.size(); i++) {
			std::string name;
			if (!name_of(*children[i], name)) {
				return false;
			}
			names.insert(name);
		}
		return true;
	}
	if (filter.GetExpressionType() == ExpressionType::CONJUNCTION_OR &&
		filter.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		bool first = true;
		for (const auto &child : filter.Cast<BoundConjunctionExpression>().GetChildren()) {
			idx_t at = 0;
			if (!NamesOf(*child, get, at, names) || (!first && at != column)) {
				return false;
			}
			column = at;
			first = false;
		}
		return !first;
	}
	return false;
}

//! `set` narrowed to `names` (the filters are ANDed: an intersection).
void Narrow(bool &by, std::set<std::string> &set, const std::set<std::string> &names) {
	if (!by) {
		by = true;
		set = names;
		return;
	}
	std::set<std::string> both;
	for (const auto &n : names) {
		if (set.count(n)) {
			both.insert(n);
		}
	}
	set = std::move(both);
}

//! Task 16e: the query's filters on the listing's name columns narrow the objects the model is built
//! for. Every filter stays in `filters`, so DuckDB still applies it: the scope only decides what is
//! described, never which rows are returned -- a filter misread here costs work, not rows.
template <Listing L>
void PushdownNames(ClientContext &, LogicalGet &get, FunctionData *bind_data_p,
				   vector<unique_ptr<Expression>> &filters) {
	auto &data = bind_data_p->Cast<CatalogListBindData>();
	const auto columns = L == Listing::Tables	 ? TableColumns()
						 : L == Listing::Views	 ? ViewColumns()
						 : L == Listing::Columns ? ColumnColumns()
												 : SchemaColumns();
	for (const auto &filter : filters) {
		idx_t column = 0;
		std::set<std::string> names;
		if (!NamesOf(*filter, get, column, names) || column >= columns.size()) {
			continue;
		}
		const std::string name = columns[column].name;
		if (name == "schema_name") {
			Narrow(data.scope.by_schema, data.scope.schemas, names);
		} else if (name == "table_name") {
			Narrow(data.scope.by_table, data.scope.tables, names);
		} else if (name == "view_name") {
			Narrow(data.scope.by_view, data.scope.views, names);
		}
	}
}

//! The scope as EXPLAIN shows it (task 16e): what the call describes, so a narrowing can be seen.
InsertionOrderPreservingMap<string> ScopeToString(TableFunctionToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	if (!input.bind_data) {
		return result;
	}
	const CatalogScope &scope = input.bind_data->Cast<CatalogListBindData>().scope;
	auto names = [](const std::set<std::string> &set) {
		std::string out;
		for (const auto &n : set) {
			out += (out.empty() ? "" : ", ") + n;
		}
		return "{" + out + "}";
	};
	std::string text;
	auto part = [&text](const std::string &p) { text += (text.empty() ? "" : "; ") + p; };
	if (scope.by_schema) {
		part("schemas " + names(scope.schemas));
	}
	if (scope.no_tables) {
		part("no tables");
	} else if (scope.by_table) {
		part("tables " + names(scope.tables));
	}
	if (scope.no_views) {
		part("no views");
	} else if (scope.by_view) {
		part("views " + names(scope.views));
	}
	result["Describes"] = text.empty() ? "every table and view" : text;
	return result;
}

unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
	const auto &data = input.bind_data->Cast<CatalogListBindData>();
	auto state = make_uniq<CatalogListState>();
	try {
		state->catalog = OpenBackupCatalog(context, std::vector<std::string>(data.paths.begin(), data.paths.end()),
										   true, data.scope);
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", FunctionName(data.listing), e.what());
	}
	return std::move(state);
}

void Execute(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	const auto &data = input.bind_data->Cast<CatalogListBindData>();
	auto &state = input.global_state->Cast<CatalogListState>();
	const CatalogModel &model = state.catalog->model;
	if (data.listing == Listing::Columns) {
		idx_t count = 0;
		while (state.next < model.tables.size() && count < STANDARD_VECTOR_SIZE) {
			const CatalogTable &t = model.tables[state.next];
			if (!data.Wants(t) || state.next_column >= t.columns.size()) {
				state.next++;
				state.next_column = 0;
				continue;
			}
			const std::vector<Value> row = ColumnRow(t, t.columns[state.next_column]);
			for (idx_t c = 0; c < row.size(); c++) {
				output.data[c].SetValue(count, row[c]);
			}
			state.next_column++;
			count++;
		}
		output.SetChildCardinality(count);
		return;
	}
	const size_t total = data.listing == Listing::Tables  ? model.tables.size()
						 : data.listing == Listing::Views ? model.views.size()
														  : model.schemas.size();
	idx_t count = 0;
	while (state.next < total && count < STANDARD_VECTOR_SIZE) {
		const std::vector<Value> row =
			data.listing == Listing::Tables ? TableRow(model.tables[state.next])
			: data.listing == Listing::Views
				? ViewRow(*state.catalog->pages->store, model, model.views[state.next], state.off_row_bytes)
				: SchemaRow(model, model.schemas[state.next]);
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
	auto add = [&](vector<LogicalType> args) {
		TableFunction f(FunctionName(L), std::move(args), Execute, Bind<L>, Init);
		f.pushdown_complex_filter = PushdownNames<L>;
		f.to_string = ScopeToString;
		set.AddFunction(std::move(f));
	};
	for (const auto &argument : arguments) {
		add({argument});
		if (L == Listing::Columns) {
			// (path, schema) and (path, schema, table).
			add({argument, LogicalType::VARCHAR});
			add({argument, LogicalType::VARCHAR, LogicalType::VARCHAR});
		}
	}
	mssql::RegisterDocumentedFunction(loader, std::move(set), {{}, description, std::move(examples), {"backup"}});
}

}  // namespace

void RegisterBackupTablesFunctions(ExtensionLoader &loader) {
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
	Register<Listing::Columns>(
		loader,
		"Lists every column of the user tables in a SQL Server full backup (.bak), one row each, with its state at "
		"a glance (sql_type as declared, an alias type by its name, system_type the type underneath): converted (read "
		"as duckdb_type), stored_bytes (read as its stored bytes until its type's "
		"conversion comes), off_row_pending (a value stored off row is read from spec 085 PR 3), not_stored (a "
		"computed column not persisted) or storage_format (a sparse column, a column set), and why. Optionally "
		"one schema's or one table's (names as stored). The path is one file, a glob, or a list: every family of "
		"one media set.",
		{"SELECT table_name, column_name, state, reason FROM mssql_backup_columns('/backups/sales_full.bak') "
		 "WHERE state <> 'converted'",
		 "SELECT column_name, sql_type, duckdb_type FROM mssql_backup_columns('/backups/sales_full.bak', 'sales', "
		 "'orders')"});
	Register<Listing::Schemas>(
		loader,
		"Lists the schemas of the database in a SQL Server full backup (.bak), as sys.schemas does (the system "
		"schemas and the roles' included), with the user tables and views in each, read from the file alone.",
		{"SELECT schema_name, table_count, view_count FROM mssql_backup_schemas('/backups/sales_full.bak')"});
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

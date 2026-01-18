#include "mssql_functions.hpp"
#include "mssql_storage.hpp"
#include "query/mssql_query_executor.hpp"
#include "connection/mssql_pool_manager.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include <chrono>
#include <cstdlib>

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetFunctionDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char* env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define MSSQL_FN_DEBUG_LOG(level, fmt, ...) \
	do { if (GetFunctionDebugLevel() >= level) { \
		fprintf(stderr, "[MSSQL FN] " fmt "\n", ##__VA_ARGS__); \
	} } while(0)

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLResultStreamRegistry Implementation
//===----------------------------------------------------------------------===//

MSSQLResultStreamRegistry& MSSQLResultStreamRegistry::Instance() {
	static MSSQLResultStreamRegistry instance;
	return instance;
}

uint64_t MSSQLResultStreamRegistry::Register(std::unique_ptr<MSSQLResultStream> stream) {
	std::lock_guard<std::mutex> lock(mutex_);
	uint64_t id = next_id_++;
	streams_[id] = std::move(stream);
	MSSQL_FN_DEBUG_LOG(1, "Registry: registered stream id=%llu", (unsigned long long)id);
	return id;
}

std::unique_ptr<MSSQLResultStream> MSSQLResultStreamRegistry::Retrieve(uint64_t id) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = streams_.find(id);
	if (it == streams_.end()) {
		MSSQL_FN_DEBUG_LOG(1, "Registry: stream id=%llu not found", (unsigned long long)id);
		return nullptr;
	}
	auto stream = std::move(it->second);
	streams_.erase(it);
	MSSQL_FN_DEBUG_LOG(1, "Registry: retrieved stream id=%llu", (unsigned long long)id);
	return stream;
}

//===----------------------------------------------------------------------===//
// mssql_execute implementation
//===----------------------------------------------------------------------===//

unique_ptr<FunctionData> MSSQLExecuteBindData::Copy() const {
	auto result = make_uniq<MSSQLExecuteBindData>();
	result->context_name = context_name;
	result->sql_statement = sql_statement;
	return std::move(result);
}

bool MSSQLExecuteBindData::Equals(const FunctionData &other) const {
	auto &other_data = other.Cast<MSSQLExecuteBindData>();
	return context_name == other_data.context_name && sql_statement == other_data.sql_statement;
}

unique_ptr<FunctionData> MSSQLExecuteBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	// Extract arguments
	if (input.inputs.size() != 2) {
		throw InvalidInputException("MSSQL Error: mssql_execute requires 2 arguments: context_name and sql_statement");
	}

	auto bind_data = make_uniq<MSSQLExecuteBindData>();
	bind_data->context_name = input.inputs[0].GetValue<string>();
	bind_data->sql_statement = input.inputs[1].GetValue<string>();

	// Validate context exists
	auto &manager = MSSQLContextManager::Get(*context.db);
	if (!manager.HasContext(bind_data->context_name)) {
		throw InvalidInputException(
		    "MSSQL Error: Unknown context '%s'. Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET ...)",
		    bind_data->context_name, bind_data->context_name);
	}

	// Set return schema
	return_types.push_back(LogicalType::BOOLEAN);
	names.push_back("success");
	return_types.push_back(LogicalType::BIGINT);
	names.push_back("affected_rows");
	return_types.push_back(LogicalType::VARCHAR);
	names.push_back("message");

	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState> MSSQLExecuteInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<MSSQLExecuteGlobalState>();
}

void MSSQLExecuteFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &global_state = data.global_state->Cast<MSSQLExecuteGlobalState>();

	if (global_state.done) {
		output.SetCardinality(0);
		return;
	}

	// For stub implementation, return success with 1 affected row
	output.SetCardinality(1);

	// success = true
	auto &success_vec = output.data[0];
	FlatVector::GetData<bool>(success_vec)[0] = true;

	// affected_rows = 1
	auto &rows_vec = output.data[1];
	FlatVector::GetData<int64_t>(rows_vec)[0] = 1;

	// message
	auto &message_vec = output.data[2];
	FlatVector::GetData<string_t>(message_vec)[0] = StringVector::AddString(message_vec, "Query executed successfully (stub)");

	global_state.done = true;
}

//===----------------------------------------------------------------------===//
// mssql_scan implementation
//===----------------------------------------------------------------------===//

unique_ptr<FunctionData> MSSQLScanBindData::Copy() const {
	auto result = make_uniq<MSSQLScanBindData>();
	result->context_name = context_name;
	result->query = query;
	result->return_types = return_types;
	result->column_names = column_names;
	result->result_stream_id = result_stream_id;
	return std::move(result);
}

bool MSSQLScanBindData::Equals(const FunctionData &other) const {
	auto &other_data = other.Cast<MSSQLScanBindData>();
	return context_name == other_data.context_name && query == other_data.query;
}

MSSQLScanGlobalState::~MSSQLScanGlobalState() {
	// Connection is automatically returned to pool when shared_ptr in result_stream is released
	// This may trigger Cancel() if stream is still active
	result_stream.reset();

	// Log total scan time (from first call to destruction, including cancel/cleanup)
	if (timing_started) {
		auto end = std::chrono::steady_clock::now();
		auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - scan_start).count();
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanGlobalState::~dtor - total scan time: %ldms (including cancel)", (long)total_ms);
	}
}

idx_t MSSQLScanGlobalState::MaxThreads() const {
	// Single-threaded streaming
	return 1;
}

unique_ptr<FunctionData> MSSQLScanBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_start = std::chrono::steady_clock::now();
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanBind: START");

	// Extract arguments
	if (input.inputs.size() != 2) {
		throw InvalidInputException("MSSQL Error: mssql_scan requires 2 arguments: context_name and query");
	}

	auto bind_data = make_uniq<MSSQLScanBindData>();
	bind_data->context_name = input.inputs[0].GetValue<string>();
	bind_data->query = input.inputs[1].GetValue<string>();

	// Validate context exists
	auto &manager = MSSQLContextManager::Get(*context.db);
	if (!manager.HasContext(bind_data->context_name)) {
		throw InvalidInputException(
		    "MSSQL Error: Unknown context '%s'. Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET ...)",
		    bind_data->context_name, bind_data->context_name);
	}

	// Execute query to get schema from COLMETADATA
	auto exec_start = std::chrono::steady_clock::now();
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanBind: executing query for schema...");
	MSSQLQueryExecutor executor(bind_data->context_name);
	auto result_stream = executor.Execute(context, bind_data->query);
	auto exec_end = std::chrono::steady_clock::now();
	auto exec_ms = std::chrono::duration_cast<std::chrono::milliseconds>(exec_end - exec_start).count();
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanBind: query executed in %ldms", (long)exec_ms);

	// Get schema from result stream
	const auto& stream_types = result_stream->GetColumnTypes();
	return_types.clear();
	for (const auto& type : stream_types) {
		return_types.push_back(type);
	}

	names.clear();
	for (const auto& name : result_stream->GetColumnNames()) {
		names.push_back(name);
	}

	bind_data->return_types = return_types;
	bind_data->column_names = names;

	// Register the result stream for later retrieval in InitGlobal
	// This avoids executing the query twice (which causes 30s timeout on large datasets)
	bind_data->result_stream_id = MSSQLResultStreamRegistry::Instance().Register(std::move(result_stream));
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanBind: registered result_stream_id=%llu", (unsigned long long)bind_data->result_stream_id);

	auto bind_end = std::chrono::steady_clock::now();
	auto bind_ms = std::chrono::duration_cast<std::chrono::milliseconds>(bind_end - bind_start).count();
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanBind: END (total %ldms)", (long)bind_ms);

	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState> MSSQLScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto init_start = std::chrono::steady_clock::now();
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: START");

	auto &bind_data = input.bind_data->Cast<MSSQLScanBindData>();
	auto result = make_uniq<MSSQLScanGlobalState>();
	result->context_name = bind_data.context_name;

	// Try to retrieve pre-initialized result stream from registry
	// This was created in Bind and avoids executing the query twice
	if (bind_data.result_stream_id != 0) {
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: retrieving result_stream_id=%llu", (unsigned long long)bind_data.result_stream_id);
		result->result_stream = MSSQLResultStreamRegistry::Instance().Retrieve(bind_data.result_stream_id);
		if (result->result_stream) {
			auto init_end = std::chrono::steady_clock::now();
			auto init_ms = std::chrono::duration_cast<std::chrono::milliseconds>(init_end - init_start).count();
			MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: retrieved from registry in %ldms", (long)init_ms);
			return std::move(result);
		}
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: result stream not found in registry, re-executing query");
	}

	// Fallback: re-execute the query (this shouldn't happen normally)
	auto exec_start = std::chrono::steady_clock::now();
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: executing query for data...");
	MSSQLQueryExecutor executor(bind_data.context_name);
	result->result_stream = executor.Execute(context, bind_data.query);
	auto exec_end = std::chrono::steady_clock::now();
	auto exec_ms = std::chrono::duration_cast<std::chrono::milliseconds>(exec_end - exec_start).count();
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: query executed in %ldms", (long)exec_ms);

	auto init_end = std::chrono::steady_clock::now();
	auto init_ms = std::chrono::duration_cast<std::chrono::milliseconds>(init_end - init_start).count();
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: END (total %ldms)", (long)init_ms);

	return std::move(result);
}

unique_ptr<LocalTableFunctionState> MSSQLScanInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                       GlobalTableFunctionState *global_state) {
	return make_uniq<MSSQLScanLocalState>();
}

void MSSQLScanFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &global_state = data.global_state->Cast<MSSQLScanGlobalState>();

	// Start timing on first call
	if (!global_state.timing_started) {
		global_state.scan_start = std::chrono::steady_clock::now();
		global_state.timing_started = true;
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanFunction: FIRST CALL - scan started");
	}

	// Check if we're done
	if (global_state.done || !global_state.result_stream) {
		auto scan_end = std::chrono::steady_clock::now();
		auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(scan_end - global_state.scan_start).count();
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanFunction: SCAN COMPLETE - total=%ldms", (long)total_ms);
		output.SetCardinality(0);
		return;
	}

	// Check for query cancellation (Ctrl+C)
	if (context.interrupted) {
		global_state.result_stream->Cancel();
		global_state.done = true;
		output.SetCardinality(0);
		return;
	}

	// Fill chunk from result stream
	try {
		idx_t rows = global_state.result_stream->FillChunk(output);
		if (rows == 0) {
			global_state.done = true;
			// Surface any warnings
			global_state.result_stream->SurfaceWarnings(context);
		}
	} catch (const Exception& e) {
		global_state.done = true;
		throw;
	}
}

//===----------------------------------------------------------------------===//
// MSSQLCatalogScanBindData Implementation
//===----------------------------------------------------------------------===//

unique_ptr<FunctionData> MSSQLCatalogScanBindData::Copy() const {
	auto result = make_uniq<MSSQLCatalogScanBindData>();
	result->context_name = context_name;
	result->schema_name = schema_name;
	result->table_name = table_name;
	result->all_types = all_types;
	result->all_column_names = all_column_names;
	result->return_types = return_types;
	result->column_names = column_names;
	result->result_stream_id = result_stream_id;
	return std::move(result);
}

bool MSSQLCatalogScanBindData::Equals(const FunctionData &other) const {
	auto &other_data = other.Cast<MSSQLCatalogScanBindData>();
	return context_name == other_data.context_name &&
	       schema_name == other_data.schema_name &&
	       table_name == other_data.table_name;
}

//===----------------------------------------------------------------------===//
// Catalog-based Table Scan Implementation
//===----------------------------------------------------------------------===//

// Helper to escape SQL Server bracket identifier (] becomes ]])
static string EscapeBracketIdentifier(const string &name) {
	string result;
	result.reserve(name.size() + 2);
	for (char c : name) {
		result += c;
		if (c == ']') {
			result += ']';  // Double the ] character
		}
	}
	return result;
}

// Helper to escape SQL Server string literal (' becomes '')
static string EscapeStringLiteral(const string &value) {
	string result;
	result.reserve(value.size() + 10);
	for (char c : value) {
		result += c;
		if (c == '\'') {
			result += '\'';  // Double the ' character
		}
	}
	return result;
}

// Forward declaration for recursive filter conversion
static string ConvertFilterToSQL(const TableFilter &filter, const string &column_name, const LogicalType &column_type);

// Convert a DuckDB Value to SQL Server literal
static string ValueToSQLLiteral(const Value &value, const LogicalType &type) {
	if (value.IsNull()) {
		return "NULL";
	}

	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return value.GetValue<bool>() ? "1" : "0";

	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UBIGINT:
		return value.ToString();

	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return value.ToString();

	case LogicalTypeId::DECIMAL:
		return value.ToString();

	case LogicalTypeId::VARCHAR:
		// Use N'' for NVARCHAR compatibility and escape single quotes
		return "N'" + EscapeStringLiteral(value.ToString()) + "'";

	case LogicalTypeId::DATE: {
		auto date_val = value.GetValue<date_t>();
		return "'" + Date::ToString(date_val) + "'";
	}

	case LogicalTypeId::TIME:
		// TIME is stored as microseconds since midnight
		return "'" + value.ToString() + "'";

	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_TZ: {
		auto ts_val = value.GetValue<timestamp_t>();
		return "'" + Timestamp::ToString(ts_val) + "'";
	}

	case LogicalTypeId::UUID: {
		return "'" + value.ToString() + "'";
	}

	case LogicalTypeId::BLOB: {
		// Convert blob to hex string for SQL Server
		auto blob_val = value.GetValueUnsafe<string_t>();
		string hex = "0x";
		for (idx_t i = 0; i < blob_val.GetSize(); i++) {
			char buf[3];
			snprintf(buf, sizeof(buf), "%02X", (unsigned char)blob_val.GetData()[i]);
			hex += buf;
		}
		return hex;
	}

	default:
		// For other types, try ToString and quote as string
		return "N'" + EscapeStringLiteral(value.ToString()) + "'";
	}
}

// Convert comparison type to SQL operator
static string ComparisonTypeToOperator(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return " = ";
	case ExpressionType::COMPARE_NOTEQUAL:
		return " <> ";
	case ExpressionType::COMPARE_LESSTHAN:
		return " < ";
	case ExpressionType::COMPARE_GREATERTHAN:
		return " > ";
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return " <= ";
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return " >= ";
	default:
		throw InternalException("Unsupported comparison type for filter pushdown: %s",
		                        ExpressionTypeToString(type));
	}
}

// Convert a ConstantFilter to SQL
static string ConvertConstantFilterToSQL(const ConstantFilter &filter, const string &column_name,
                                         const LogicalType &column_type) {
	string sql = column_name;
	sql += ComparisonTypeToOperator(filter.comparison_type);
	sql += ValueToSQLLiteral(filter.constant, column_type);
	return sql;
}

// Convert an InFilter to SQL
static string ConvertInFilterToSQL(const InFilter &filter, const string &column_name,
                                   const LogicalType &column_type) {
	string sql = column_name + " IN (";
	for (idx_t i = 0; i < filter.values.size(); i++) {
		if (i > 0) {
			sql += ", ";
		}
		sql += ValueToSQLLiteral(filter.values[i], column_type);
	}
	sql += ")";
	return sql;
}

// Convert an IsNullFilter to SQL
static string ConvertIsNullFilterToSQL(const string &column_name) {
	return column_name + " IS NULL";
}

// Convert an IsNotNullFilter to SQL
static string ConvertIsNotNullFilterToSQL(const string &column_name) {
	return column_name + " IS NOT NULL";
}

// Convert a ConjunctionOrFilter to SQL
static string ConvertConjunctionOrFilterToSQL(const ConjunctionOrFilter &filter, const string &column_name,
                                               const LogicalType &column_type) {
	if (filter.child_filters.empty()) {
		return "";
	}
	vector<string> conditions;
	for (const auto &child : filter.child_filters) {
		string child_sql = ConvertFilterToSQL(*child, column_name, column_type);
		if (!child_sql.empty()) {
			conditions.push_back(child_sql);
		}
	}
	if (conditions.empty()) {
		return "";
	}
	if (conditions.size() == 1) {
		return conditions[0];
	}
	string sql = "(";
	for (idx_t i = 0; i < conditions.size(); i++) {
		if (i > 0) {
			sql += " OR ";
		}
		sql += conditions[i];
	}
	sql += ")";
	return sql;
}

// Convert a ConjunctionAndFilter to SQL
static string ConvertConjunctionAndFilterToSQL(const ConjunctionAndFilter &filter, const string &column_name,
                                                const LogicalType &column_type) {
	if (filter.child_filters.empty()) {
		return "";
	}
	vector<string> conditions;
	for (const auto &child : filter.child_filters) {
		string child_sql = ConvertFilterToSQL(*child, column_name, column_type);
		if (!child_sql.empty()) {
			conditions.push_back(child_sql);
		}
	}
	if (conditions.empty()) {
		return "";
	}
	if (conditions.size() == 1) {
		return conditions[0];
	}
	string sql = "(";
	for (idx_t i = 0; i < conditions.size(); i++) {
		if (i > 0) {
			sql += " AND ";
		}
		sql += conditions[i];
	}
	sql += ")";
	return sql;
}

// Main filter conversion function
static string ConvertFilterToSQL(const TableFilter &filter, const string &column_name, const LogicalType &column_type) {
	switch (filter.filter_type) {
	case TableFilterType::CONSTANT_COMPARISON:
		return ConvertConstantFilterToSQL(filter.Cast<ConstantFilter>(), column_name, column_type);

	case TableFilterType::IS_NULL:
		return ConvertIsNullFilterToSQL(column_name);

	case TableFilterType::IS_NOT_NULL:
		return ConvertIsNotNullFilterToSQL(column_name);

	case TableFilterType::IN_FILTER:
		return ConvertInFilterToSQL(filter.Cast<InFilter>(), column_name, column_type);

	case TableFilterType::CONJUNCTION_OR:
		return ConvertConjunctionOrFilterToSQL(filter.Cast<ConjunctionOrFilter>(), column_name, column_type);

	case TableFilterType::CONJUNCTION_AND:
		return ConvertConjunctionAndFilterToSQL(filter.Cast<ConjunctionAndFilter>(), column_name, column_type);

	case TableFilterType::OPTIONAL_FILTER:
	case TableFilterType::STRUCT_EXTRACT:
	case TableFilterType::DYNAMIC_FILTER:
	case TableFilterType::EXPRESSION_FILTER:
		// These filter types cannot be pushed down to SQL Server
		MSSQL_FN_DEBUG_LOG(1, "Filter type %d cannot be pushed down, will be applied locally",
		                   (int)filter.filter_type);
		return "";

	default:
		MSSQL_FN_DEBUG_LOG(1, "Unknown filter type %d, will be applied locally", (int)filter.filter_type);
		return "";
	}
}

// Build WHERE clause from TableFilterSet
// Returns empty string if no filters can be pushed down
// Note: DuckDB's filter column indices are based on the projected column list (column_ids),
// not the original table column indices. We need to map through column_ids to get the actual
// table column index.
static string BuildWhereClause(const TableFilterSet &filters, const vector<string> &all_column_names,
                               const vector<LogicalType> &all_types, const vector<column_t> &column_ids) {
	vector<string> where_conditions;

	for (const auto &filter_entry : filters.filters) {
		idx_t projected_col_idx = filter_entry.first;

		// Map from projected column index to actual table column index
		// DuckDB passes filter indices based on the projection (column_ids)
		idx_t table_col_idx;
		if (column_ids.empty()) {
			// No projection - use filter index directly as table column index
			table_col_idx = projected_col_idx;
		} else if (projected_col_idx >= column_ids.size()) {
			MSSQL_FN_DEBUG_LOG(1, "Filter column index %llu out of projected range (%zu), skipping",
			                   (unsigned long long)projected_col_idx, column_ids.size());
			continue;
		} else {
			// Map through column_ids to get actual table column index
			table_col_idx = column_ids[projected_col_idx];
		}

		if (table_col_idx >= all_column_names.size()) {
			MSSQL_FN_DEBUG_LOG(1, "Table column index %llu out of range (%zu), skipping",
			                   (unsigned long long)table_col_idx, all_column_names.size());
			continue;
		}

		const string &col_name = all_column_names[table_col_idx];
		const LogicalType &col_type = all_types[table_col_idx];
		string escaped_col = "[" + EscapeBracketIdentifier(col_name) + "]";

		MSSQL_FN_DEBUG_LOG(2, "  filter: projected_idx=%llu -> table_idx=%llu -> column=%s",
		                   (unsigned long long)projected_col_idx, (unsigned long long)table_col_idx, col_name.c_str());

		string condition = ConvertFilterToSQL(*filter_entry.second, escaped_col, col_type);
		if (!condition.empty()) {
			where_conditions.push_back(condition);
			MSSQL_FN_DEBUG_LOG(2, "  filter condition: %s", condition.c_str());
		}
	}

	if (where_conditions.empty()) {
		return "";
	}

	// Combine all conditions with AND
	string where_clause = " WHERE ";
	for (idx_t i = 0; i < where_conditions.size(); i++) {
		if (i > 0) {
			where_clause += " AND ";
		}
		where_clause += where_conditions[i];
	}

	return where_clause;
}

// Note: MSSQLCatalogScanBind is not used directly - bind_data is set by GetScanFunction
// and passed to InitGlobal/Execute. We keep this for completeness but it shouldn't be called.
unique_ptr<FunctionData> MSSQLCatalogScanBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	// This bind function is not used for catalog scans - bind_data is set in GetScanFunction
	throw InternalException("MSSQLCatalogScanBind should not be called directly");
}

unique_ptr<GlobalTableFunctionState> MSSQLCatalogScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	MSSQL_FN_DEBUG_LOG(1, "MSSQLCatalogScanInitGlobal: START");

	auto &bind_data = input.bind_data->Cast<MSSQLCatalogScanBindData>();
	auto result = make_uniq<MSSQLScanGlobalState>();
	result->context_name = bind_data.context_name;

	// Build column list based on projection pushdown (column_ids)
	// column_ids contains the indices of columns needed from the table
	string column_list;
	const auto &column_ids = input.column_ids;

	MSSQL_FN_DEBUG_LOG(1, "MSSQLCatalogScanInitGlobal: projection has %zu columns (table has %zu)",
	                   column_ids.size(), bind_data.all_column_names.size());

	if (column_ids.empty()) {
		// No projection - select all columns (shouldn't happen normally)
		for (idx_t i = 0; i < bind_data.all_column_names.size(); i++) {
			if (i > 0) {
				column_list += ", ";
			}
			column_list += "[" + EscapeBracketIdentifier(bind_data.all_column_names[i]) + "]";
		}
	} else {
		// Build SELECT with only projected columns
		for (idx_t i = 0; i < column_ids.size(); i++) {
			if (i > 0) {
				column_list += ", ";
			}
			column_t col_idx = column_ids[i];
			if (col_idx >= bind_data.all_column_names.size()) {
				throw InternalException("Column index %llu out of range (table has %zu columns)",
				                        (unsigned long long)col_idx, bind_data.all_column_names.size());
			}
			column_list += "[" + EscapeBracketIdentifier(bind_data.all_column_names[col_idx]) + "]";
			MSSQL_FN_DEBUG_LOG(2, "  column[%llu] = %s", (unsigned long long)i, bind_data.all_column_names[col_idx].c_str());
		}
	}

	// Generate the query: SELECT [col1], [col2], ... FROM [schema].[table]
	string query = "SELECT " + column_list + " FROM [" + EscapeBracketIdentifier(bind_data.schema_name) +
	               "].[" + EscapeBracketIdentifier(bind_data.table_name) + "]";

	// Build WHERE clause from filter pushdown
	if (input.filters && !input.filters->filters.empty()) {
		MSSQL_FN_DEBUG_LOG(1, "MSSQLCatalogScanInitGlobal: filter pushdown with %zu filter(s)",
		                   input.filters->filters.size());
		string where_clause = BuildWhereClause(*input.filters, bind_data.all_column_names,
		                                       bind_data.all_types, column_ids);
		if (!where_clause.empty()) {
			query += where_clause;
			MSSQL_FN_DEBUG_LOG(1, "MSSQLCatalogScanInitGlobal: added WHERE clause: %s", where_clause.c_str());
		}
	}

	MSSQL_FN_DEBUG_LOG(1, "MSSQLCatalogScanInitGlobal: generated query = %s", query.c_str());

	// Execute the query
	MSSQLQueryExecutor executor(bind_data.context_name);
	result->result_stream = executor.Execute(context, query);

	return std::move(result);
}

TableFunction GetMSSQLCatalogScanFunction() {
	// Create table function without arguments - bind_data is set from TableEntry
	TableFunction func("mssql_catalog_scan", {}, MSSQLScanFunction, MSSQLCatalogScanBind,
	                   MSSQLCatalogScanInitGlobal, MSSQLScanInitLocal);

	// Enable projection pushdown - allows DuckDB to tell us which columns are needed
	// The column_ids will be passed to InitGlobal via TableFunctionInitInput
	func.projection_pushdown = true;

	// Enable filter pushdown - allows DuckDB to push WHERE conditions to SQL Server
	// The filters will be passed to InitGlobal via TableFunctionInitInput
	func.filter_pushdown = true;

	return func;
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void RegisterMSSQLFunctions(ExtensionLoader &loader) {
	// mssql_execute(context_name VARCHAR, sql_statement VARCHAR)
	// -> (success BOOLEAN, affected_rows BIGINT, message VARCHAR)
	TableFunction mssql_execute("mssql_execute", {LogicalType::VARCHAR, LogicalType::VARCHAR}, MSSQLExecuteFunction,
	                            MSSQLExecuteBind, MSSQLExecuteInitGlobal);
	loader.RegisterFunction(mssql_execute);

	// mssql_scan(context_name VARCHAR, query VARCHAR)
	// -> (id INTEGER, name VARCHAR) for stub
	TableFunction mssql_scan("mssql_scan", {LogicalType::VARCHAR, LogicalType::VARCHAR}, MSSQLScanFunction,
	                         MSSQLScanBind, MSSQLScanInitGlobal, MSSQLScanInitLocal);
	loader.RegisterFunction(mssql_scan);
}

}  // namespace duckdb

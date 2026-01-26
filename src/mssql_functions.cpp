#include "mssql_functions.hpp"
#include <chrono>
#include <cstdlib>
#include "catalog/mssql_catalog.hpp"
#include "connection/mssql_pool_manager.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "mssql_storage.hpp"
#include "query/mssql_query_executor.hpp"
#include "query/mssql_simple_query.hpp"
#include "tds/tds_connection.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetFunctionDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define MSSQL_FN_DEBUG_LOG(level, fmt, ...)                         \
	do {                                                            \
		if (GetFunctionDebugLevel() >= level) {                     \
			fprintf(stderr, "[MSSQL FN] " fmt "\n", ##__VA_ARGS__); \
		}                                                           \
	} while (0)

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLResultStreamRegistry Implementation
//===----------------------------------------------------------------------===//

MSSQLResultStreamRegistry &MSSQLResultStreamRegistry::Instance() {
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
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanGlobalState::~dtor - total scan time: %ldms (including cancel)",
						   (long)total_ms);
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
	const auto &stream_types = result_stream->GetColumnTypes();
	return_types.clear();
	for (const auto &type : stream_types) {
		return_types.push_back(type);
	}

	names.clear();
	for (const auto &name : result_stream->GetColumnNames()) {
		names.push_back(name);
	}

	bind_data->return_types = return_types;
	bind_data->column_names = names;

	// Register the result stream for later retrieval in InitGlobal
	// This avoids executing the query twice (which causes 30s timeout on large datasets)
	bind_data->result_stream_id = MSSQLResultStreamRegistry::Instance().Register(std::move(result_stream));
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanBind: registered result_stream_id=%llu",
					   (unsigned long long)bind_data->result_stream_id);

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
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: retrieving result_stream_id=%llu",
						   (unsigned long long)bind_data.result_stream_id);
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
		auto total_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(scan_end - global_state.scan_start).count();
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
	} catch (const Exception &e) {
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
	result->complex_filter_where_clause = complex_filter_where_clause;
	// RowId support fields
	result->rowid_requested = rowid_requested;
	result->pk_column_names = pk_column_names;
	result->pk_column_types = pk_column_types;
	result->pk_result_indices = pk_result_indices;
	result->pk_is_composite = pk_is_composite;
	result->rowid_type = rowid_type;
	return std::move(result);
}

bool MSSQLCatalogScanBindData::Equals(const FunctionData &other) const {
	auto &other_data = other.Cast<MSSQLCatalogScanBindData>();
	return context_name == other_data.context_name && schema_name == other_data.schema_name &&
		   table_name == other_data.table_name;
}

//===----------------------------------------------------------------------===//
// mssql_exec Scalar Function Implementation
//===----------------------------------------------------------------------===//

// Bind data for mssql_exec - stores context name (attached database name)
struct MSSQLExecBindData : public FunctionData {
	string context_name;

	MSSQLExecBindData(string context_name_p) : context_name(std::move(context_name_p)) {}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<MSSQLExecBindData>(context_name);
	}

	bool Equals(const FunctionData &other) const override {
		auto &other_data = other.Cast<MSSQLExecBindData>();
		return context_name == other_data.context_name;
	}
};

// Bind function for mssql_exec
static unique_ptr<FunctionData> MSSQLExecBind(ClientContext &context, ScalarFunction &bound_function,
											  vector<unique_ptr<Expression>> &arguments) {
	// First argument is the context name (attached database name, must be constant)
	if (arguments[0]->HasParameter()) {
		throw InvalidInputException("mssql_exec: context_name must be a constant, not a parameter");
	}

	// Extract the context name if it's a constant
	string context_name;
	if (arguments[0]->IsFoldable()) {
		auto context_val = ExpressionExecutor::EvaluateScalar(context, *arguments[0]);
		context_name = context_val.ToString();

		// Validate the context exists (attached database)
		auto &manager = MSSQLContextManager::Get(*context.db);
		if (!manager.HasContext(context_name)) {
			throw BinderException(
				"mssql_exec: Unknown context '%s'. Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET "
				"...)",
				context_name, context_name);
		}
	}

	return make_uniq<MSSQLExecBindData>(context_name);
}

// Execute function for mssql_exec
static void MSSQLExecExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &bind_data = state.expr.Cast<BoundFunctionExpression>().bind_info->Cast<MSSQLExecBindData>();

	auto &context_names = args.data[0];
	auto &sql_statements = args.data[1];

	UnaryExecutor::Execute<string_t, int64_t>(sql_statements, result, args.size(), [&](string_t sql_str) -> int64_t {
		// Get the context name from bind data or first argument
		string context_name = bind_data.context_name;
		if (context_name.empty()) {
			// Get from runtime argument
			auto context_val = context_names.GetValue(0);
			context_name = context_val.ToString();
		}

		string sql = sql_str.GetString();

		MSSQL_FN_DEBUG_LOG(1, "mssql_exec: context=%s, sql=%s", context_name.c_str(), sql.c_str());

		// Get context from the state
		auto &client_context = state.GetContext();

		// Get the MSSQL context and catalog
		auto &manager = MSSQLContextManager::Get(*client_context.db);
		auto ctx = manager.GetContext(context_name);
		if (!ctx) {
			throw InvalidInputException(
				"mssql_exec: Unknown context '%s'. Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET "
				"...)",
				context_name, context_name);
		}

		// Get the catalog and check if it's read-only
		if (!ctx->attached_db) {
			throw InvalidInputException("mssql_exec: Context '%s' has no attached database", context_name);
		}

		auto &catalog = ctx->attached_db->GetCatalog().Cast<MSSQLCatalog>();
		if (catalog.IsReadOnly()) {
			throw InvalidInputException("Cannot execute mssql_exec: catalog '%s' is attached in read-only mode",
										context_name);
		}

		// Get connection from the catalog's pool
		auto &pool = catalog.GetConnectionPool();
		auto connection = pool.Acquire();

		if (!connection) {
			throw IOException("mssql_exec: Failed to acquire connection from pool for '%s'", context_name);
		}

		// Execute the SQL
		try {
			auto query_result = MSSQLSimpleQuery::Execute(*connection, sql);

			// Release connection back to pool
			pool.Release(std::move(connection));

			if (!query_result.success) {
				// Surface SQL Server error with details
				throw InvalidInputException("MSSQL execution error: SQL Server error %d: %s", query_result.error_number,
											query_result.error_message);
			}

			// Return affected row count
			// For DDL and queries without affected rows, return 0
			return query_result.rows.empty() ? 0 : static_cast<int64_t>(query_result.rows.size());

		} catch (...) {
			// Release connection on error
			pool.Release(std::move(connection));
			throw;
		}
	});
}

ScalarFunction MSSQLExecScalarFunction::GetFunction() {
	ScalarFunction func(NAME, {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BIGINT, MSSQLExecExecute,
						MSSQLExecBind);
	func.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return func;
}

void RegisterMSSQLExecFunction(ExtensionLoader &loader) {
	auto func = MSSQLExecScalarFunction::GetFunction();
	loader.RegisterFunction(func);
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void RegisterMSSQLFunctions(ExtensionLoader &loader) {
	// mssql_scan(context_name VARCHAR, query VARCHAR)
	// -> dynamic return schema based on query result columns
	TableFunction mssql_scan("mssql_scan", {LogicalType::VARCHAR, LogicalType::VARCHAR}, MSSQLScanFunction,
							 MSSQLScanBind, MSSQLScanInitGlobal, MSSQLScanInitLocal);
	loader.RegisterFunction(mssql_scan);
}

}  // namespace duckdb

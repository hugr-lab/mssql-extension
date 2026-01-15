#include "mssql_functions.hpp"
#include "mssql_storage.hpp"
#include "query/mssql_query_executor.hpp"
#include "connection/mssql_pool_manager.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/database.hpp"
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

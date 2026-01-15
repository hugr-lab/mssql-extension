#include "mssql_functions.hpp"
#include "mssql_storage.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

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
		    "MSSQL Error: Unknown context '%s'. Attach a database first with: ATTACH '' AS %s TYPE mssql (SECRET ...)",
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
	return std::move(result);
}

bool MSSQLScanBindData::Equals(const FunctionData &other) const {
	auto &other_data = other.Cast<MSSQLScanBindData>();
	return context_name == other_data.context_name && query == other_data.query;
}

idx_t MSSQLScanGlobalState::MaxThreads() const {
	// Single-threaded for stub implementation
	return 1;
}

unique_ptr<FunctionData> MSSQLScanBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
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
		    "MSSQL Error: Unknown context '%s'. Attach a database first with: ATTACH '' AS %s TYPE mssql (SECRET ...)",
		    bind_data->context_name, bind_data->context_name);
	}

	// For stub implementation, return fixed schema: id INTEGER, name VARCHAR
	return_types.push_back(LogicalType::INTEGER);
	names.push_back("id");
	return_types.push_back(LogicalType::VARCHAR);
	names.push_back("name");

	bind_data->return_types = return_types;
	bind_data->column_names = names;

	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState> MSSQLScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<MSSQLScanGlobalState>();
	result->total_rows = 3;  // Stub returns 3 rows
	result->rows_returned = 0;
	return std::move(result);
}

unique_ptr<LocalTableFunctionState> MSSQLScanInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                       GlobalTableFunctionState *global_state) {
	auto result = make_uniq<MSSQLScanLocalState>();
	result->current_row = 0;
	return std::move(result);
}

void MSSQLScanFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &global_state = data.global_state->Cast<MSSQLScanGlobalState>();
	auto &local_state = data.local_state->Cast<MSSQLScanLocalState>();

	// Check if we're done
	idx_t remaining = global_state.total_rows - local_state.current_row;
	if (remaining == 0) {
		output.SetCardinality(0);
		return;
	}

	// Output all remaining rows (up to STANDARD_VECTOR_SIZE)
	idx_t to_output = MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE);
	output.SetCardinality(to_output);

	auto &id_vec = output.data[0];
	auto &name_vec = output.data[1];

	for (idx_t i = 0; i < to_output; i++) {
		idx_t row_idx = local_state.current_row + i + 1;  // 1-based

		// id column
		FlatVector::GetData<int32_t>(id_vec)[i] = static_cast<int32_t>(row_idx);

		// name column
		string name_val = StringUtil::Format("Name %llu", row_idx);
		FlatVector::GetData<string_t>(name_vec)[i] = StringVector::AddString(name_vec, name_val);
	}

	local_state.current_row += to_output;
	global_state.rows_returned += to_output;
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

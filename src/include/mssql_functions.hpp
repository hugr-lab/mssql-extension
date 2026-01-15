//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// mssql_functions.hpp
//
// Table functions: mssql_execute and mssql_scan
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

#include <atomic>

namespace duckdb {

//===----------------------------------------------------------------------===//
// mssql_execute - Execute raw SQL statement
//===----------------------------------------------------------------------===//

struct MSSQLExecuteBindData : public FunctionData {
	string context_name;
	string sql_statement;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

struct MSSQLExecuteGlobalState : public GlobalTableFunctionState {
	bool done = false;

	idx_t MaxThreads() const override {
		return 1;
	}
};

// Bind: validates arguments, sets return schema
unique_ptr<FunctionData> MSSQLExecuteBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names);

// Global init: sets up execution state
unique_ptr<GlobalTableFunctionState> MSSQLExecuteInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Execute: produces single-row result
void MSSQLExecuteFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output);

//===----------------------------------------------------------------------===//
// mssql_scan - Scan SQL Server data
//===----------------------------------------------------------------------===//

struct MSSQLScanBindData : public FunctionData {
	string context_name;
	string query;
	vector<LogicalType> return_types;
	vector<string> column_names;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

struct MSSQLScanGlobalState : public GlobalTableFunctionState {
	string context_name;
	string query;
	idx_t total_rows;
	atomic<idx_t> rows_returned;

	MSSQLScanGlobalState() : total_rows(3), rows_returned(0) {
	}

	idx_t MaxThreads() const override;
};

struct MSSQLScanLocalState : public LocalTableFunctionState {
	idx_t current_row = 0;
};

// Bind: validates arguments, determines return schema
unique_ptr<FunctionData> MSSQLScanBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names);

// Global init: sets up execution state
unique_ptr<GlobalTableFunctionState> MSSQLScanInitGlobal(ClientContext &context, TableFunctionInitInput &input);

// Local init: per-thread state
unique_ptr<LocalTableFunctionState> MSSQLScanInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                       GlobalTableFunctionState *global_state);

// Execute: produces output rows
void MSSQLScanFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output);

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

// Register all MSSQL table functions
void RegisterMSSQLFunctions(ExtensionLoader &loader);

}  // namespace duckdb

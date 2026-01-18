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
#include "query/mssql_result_stream.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

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

	// ID to retrieve pre-initialized result stream from registry
	// This avoids executing the query twice (once for schema, once for data)
	uint64_t result_stream_id = 0;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

//===----------------------------------------------------------------------===//
// MSSQLCatalogScanBindData - For catalog-based table scans
//===----------------------------------------------------------------------===//

struct MSSQLCatalogScanBindData : public FunctionData {
	string context_name;
	string schema_name;
	string table_name;

	// All columns from the table (for projection pushdown)
	// Query will be generated at InitGlobal time based on column_ids
	vector<LogicalType> all_types;      // Types for all columns
	vector<string> all_column_names;    // Names for all columns

	// Projected columns (set after InitGlobal based on column_ids)
	vector<LogicalType> return_types;
	vector<string> column_names;

	// ID to retrieve pre-initialized result stream from registry
	// Note: with projection pushdown, we can't pre-execute the query at bind time
	// because we don't know which columns are needed yet
	uint64_t result_stream_id = 0;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

//===----------------------------------------------------------------------===//
// Result Stream Registry - stores result streams between Bind and InitGlobal
//===----------------------------------------------------------------------===//

class MSSQLResultStreamRegistry {
public:
	static MSSQLResultStreamRegistry& Instance();

	// Register a result stream and get an ID
	uint64_t Register(std::unique_ptr<MSSQLResultStream> stream);

	// Retrieve and remove a result stream by ID
	std::unique_ptr<MSSQLResultStream> Retrieve(uint64_t id);

private:
	MSSQLResultStreamRegistry() = default;
	std::mutex mutex_;
	std::unordered_map<uint64_t, std::unique_ptr<MSSQLResultStream>> streams_;
	std::atomic<uint64_t> next_id_{1};
};

struct MSSQLScanGlobalState : public GlobalTableFunctionState {
	// Result stream from SQL Server
	std::unique_ptr<MSSQLResultStream> result_stream;

	// Context name for pool return
	string context_name;

	// Set when complete
	bool done = false;

	// Timing
	std::chrono::steady_clock::time_point scan_start;
	bool timing_started = false;

	MSSQLScanGlobalState() = default;
	~MSSQLScanGlobalState();

	idx_t MaxThreads() const override;
};

struct MSSQLScanLocalState : public LocalTableFunctionState {
	// No per-thread state needed - single-threaded streaming
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
// Catalog-based Table Scan Functions
//===----------------------------------------------------------------------===//

// Get a table function for scanning a catalog table entry
// This is called by MSSQLTableEntry::GetScanFunction
TableFunction GetMSSQLCatalogScanFunction();

// Bind function for catalog scan - generates query from table columns
unique_ptr<FunctionData> MSSQLCatalogScanBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names);

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

// Register all MSSQL table functions
void RegisterMSSQLFunctions(ExtensionLoader &loader);

}  // namespace duckdb

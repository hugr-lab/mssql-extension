//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// mssql_functions.hpp
//
// Table functions: mssql_scan
//===----------------------------------------------------------------------===//

#pragma once

#include "catalog/mssql_column_info.hpp"
#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "query/mssql_result_stream.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace duckdb {

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
	vector<LogicalType> all_types;	  // Types for all columns
	vector<string> all_column_names;  // Names for all columns

	// Extended column metadata for VARCHAR→NVARCHAR conversion (Spec 026)
	vector<MSSQLColumnInfo> mssql_columns;

	// Projected columns (set after InitGlobal based on column_ids)
	vector<LogicalType> return_types;
	vector<string> column_names;

	// ID to retrieve pre-initialized result stream from registry
	// Note: with projection pushdown, we can't pre-execute the query at bind time
	// because we don't know which columns are needed yet
	uint64_t result_stream_id = 0;

	// Complex filter expressions pushed down via pushdown_complex_filter callback
	// These are expressions like year(col) = 2024, BETWEEN, etc. that cannot be
	// represented as simple TableFilter objects
	mutable string complex_filter_where_clause;

	// ORDER BY pushdown (Spec 039)
	// Set by MSSQLOptimizer when ORDER BY can be pushed to SQL Server
	mutable string order_by_clause;
	// TOP N pushdown: when ORDER BY + LIMIT are both fully pushable
	// 0 = no TOP (default), >0 = SELECT TOP N
	int64_t top_n = 0;

	//===----------------------------------------------------------------------===//
	// RowId Support (Spec 001-pk-rowid-semantics)
	//===----------------------------------------------------------------------===//

	// Pointer to the table entry (for GetTable() / get_bind_info)
	// This allows DuckDB to discover virtual columns like rowid
	optional_ptr<TableCatalogEntry> table_entry;

	// Whether rowid was requested in the projection
	bool rowid_requested = false;

	// Primary key column names (for building SELECT with PK columns)
	vector<string> pk_column_names;

	// Primary key column types (for composite PK STRUCT construction)
	vector<LogicalType> pk_column_types;

	// Indices of PK columns in the SQL Server result set
	// Used to map from result columns to PK values for rowid construction
	vector<idx_t> pk_result_indices;

	// Whether the PK is composite (STRUCT) or scalar
	bool pk_is_composite = false;

	// The rowid type (scalar or STRUCT)
	LogicalType rowid_type;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

//===----------------------------------------------------------------------===//
// Result Stream Registry - stores result streams between Bind and InitGlobal
//===----------------------------------------------------------------------===//

class MSSQLResultStreamRegistry {
public:
	static MSSQLResultStreamRegistry &Instance();

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

	// Number of real (non-virtual) columns to fill in the output chunk
	// When 0 (e.g., COUNT(*)), we don't fill any columns but still count rows
	idx_t projected_column_count = 0;

	// Set when complete
	bool done = false;

	// Timing
	std::chrono::steady_clock::time_point scan_start;
	bool timing_started = false;

	//===----------------------------------------------------------------------===//
	// RowId Support (Spec 001-pk-rowid-semantics)
	//===----------------------------------------------------------------------===//

	// Whether rowid was requested in the projection
	bool rowid_requested = false;

	// Index of the rowid column in DuckDB output (if rowid_requested)
	idx_t rowid_output_idx = 0;

	// Indices of PK columns in the SQL Server result set
	// Used to map from result columns to PK values for rowid construction
	vector<idx_t> pk_result_indices;

	// Whether the PK is composite (STRUCT) or scalar
	bool pk_is_composite = false;

	// The rowid type (scalar or STRUCT)
	LogicalType rowid_type;

	// PK column types (for composite PK STRUCT construction)
	vector<LogicalType> pk_column_types;

	// Whether PK data should be written directly to rowid position
	// True when user projects only rowid (SELECT rowid FROM table)
	// and the PK is scalar (non-composite)
	bool pk_direct_to_rowid = false;

	// Whether we need to build STRUCT rowid from SQL columns directly
	// True when user projects only rowid and PK is composite
	// In this case, SQL columns are written directly to STRUCT children
	bool composite_pk_direct_to_struct = false;

	// Whether PK columns were added as extra SQL columns (not in user projection)
	// True when user selects rowid + other columns but NOT the PK column(s)
	// e.g., SELECT rowid, name FROM table (where id is the PK)
	bool pk_columns_added = false;

	// SQL result indices of PK columns (for reading PK data from result)
	vector<idx_t> pk_sql_indices;

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

// Note: Catalog scan functions have been moved to src/table_scan/table_scan.hpp
// Use mssql::GetCatalogScanFunction() from that module instead.

//===----------------------------------------------------------------------===//
// mssql_exec - Execute arbitrary T-SQL and return affected row count
//===----------------------------------------------------------------------===//

//! mssql_exec scalar function
//! Signature: mssql_exec(secret_name VARCHAR, sql VARCHAR) -> BIGINT
//! Returns the number of affected rows (or 0 for DDL statements)
struct MSSQLExecScalarFunction {
	static constexpr const char *NAME = "mssql_exec";

	//! Get the scalar function definition
	static ScalarFunction GetFunction();
};

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

// Register all MSSQL table functions
void RegisterMSSQLFunctions(ExtensionLoader &loader);

// Register mssql_exec scalar function
void RegisterMSSQLExecFunction(ExtensionLoader &loader);

}  // namespace duckdb

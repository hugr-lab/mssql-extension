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
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "query/mssql_result_stream.hpp"
#include "table_scan/table_scan_state.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace duckdb {

//===----------------------------------------------------------------------===//
// mssql_scan - Scan SQL Server data
//===----------------------------------------------------------------------===//

//! Spec 075: an sp_prepare handle lives in ONE session. In autocommit that
//! session is a pooled connection held from Bind to InitGlobal and returned to
//! the pool when the bind data dies -- the handle goes with the session reset,
//! nothing is sent. In a transaction the session is the pinned connection and
//! nothing is held here beyond the handle. Shared, because FunctionData::Copy
//! shares.
struct MSSQLPreparedSession {
	int32_t handle = 0;
	std::shared_ptr<tds::TdsConnection> connection;	 // empty inside a transaction
	weak_ptr<tds::ConnectionPool> pool_handle;
	bool reset_on_release = true;
	~MSSQLPreparedSession();
};

struct MSSQLScanBindData : public FunctionData {
	string context_name;
	string query;
	vector<LogicalType> return_types;
	vector<string> column_names;

	// UUID handle to retrieve pre-initialized result stream registered on the
	// owning MSSQLCatalog at Bind time (spec 047 / US3). Empty when there is
	// no pre-built stream (InitGlobal then re-executes the query).
	string result_stream_id;

	// Issue #316: inside an explicit transaction, Bind drains the result here and
	// closes the stream instead of registering it.
	//
	// mssql_scan executes its query at BIND time — it has to, the schema comes
	// from the batch's own COLMETADATA — and then holds the connection open until
	// execution drains it. In a transaction that connection is the ONE pinned
	// connection, so a second mssql_scan fails in its own Bind, before any
	// InitGlobal runs. The catalog scan's fix cannot reach that: materializing at
	// InitGlobal is too late, and the optimizer gate that decides it runs later
	// still.
	//
	// So the trigger here is "in a transaction", not "more than one scan": Bind
	// cannot know what else the plan will hold. That does buffer a lone
	// mssql_scan that would have streamed — but inside a transaction such a scan
	// could not have coexisted with anything else on that catalog anyway, so this
	// turns a failing case into a slower one rather than taking a working case
	// away.
	//
	// shared_ptr because FunctionData::Copy has to share it rather than duplicate
	// the rows.
	shared_ptr<ColumnDataCollection> materialized;

	// Spec 075 (W1/W2): Bind asks sp_describe_first_result_set for the shape --
	// or, with `prepared := true`, sp_prepare, whose answer carries it -- and
	// the query itself runs at InitGlobal. `execute_sql` is what InitGlobal
	// sends: the query, the sp_executesql batch of mssql_scan_params, or
	// `EXEC sp_execute <handle>`.
	string execute_sql;
	bool prepared = false;
	shared_ptr<MSSQLPreparedSession> prepared_session;
	// The F1 fallback: the describe could not settle the shape (a batch with a
	// temp table, a procedure, dynamic SQL) and Bind executed as it always had;
	// result_stream_id / materialized are then set exactly as before.
	bool executed_at_bind = false;

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

	// Drain this scan into a ColumnDataCollection at InitGlobal instead of
	// streaming it (issue #239).
	//
	// A scan normally holds its TDS connection open across GetData calls. That is
	// fine while each scan has its own pooled connection, and wrong the moment two
	// of them must share one — which is exactly what an explicit transaction does,
	// because the transaction pins a single connection and every read on that
	// catalog is routed to it. DuckDB does not promise to drain one source before
	// initializing the next; for a decorrelated subquery (LEFT_DELIM_JOIN +
	// DELIM_SCAN) it initializes both, and the second batch finds the connection
	// in Executing:
	//
	//     Cannot execute: connection not in Idle state (current: Executing)
	//
	// With threads > 1 the two run concurrently and the failure is a torn stream
	// instead. Materializing releases the connection before InitGlobal returns, so
	// the next scan finds it Idle.
	//
	// Set by MSSQLOptimizer, hence mutable — same as order_by_clause above.
	mutable bool requires_materialization = false;

	//===----------------------------------------------------------------------===//
	// RowId Support (Spec 001-pk-rowid-semantics)
	//===----------------------------------------------------------------------===//

	// Pointer to the table entry (for GetTable() / get_bind_info)
	// This allows DuckDB to discover virtual columns like rowid.
	// Spec 052 (Option D): lifetime of the underlying entry is guaranteed by
	// MSSQLBindAnchors (per ClientContext, released at QueryEnd) which is
	// populated on every LookupEntry call. No per-bind-data anchor needed.
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

struct MSSQLScanGlobalState : public GlobalTableFunctionState {
	// Result stream from SQL Server
	std::unique_ptr<MSSQLResultStream> result_stream;

	// Issue #239: when the bind data asked for materialization, InitGlobal drains
	// the stream into here and closes it, so the pinned connection is Idle again
	// before the next scan on the same catalog initializes. Execution then serves
	// chunks from the collection and never touches the connection.
	std::unique_ptr<ColumnDataCollection> materialized;

	// Issue #316: the raw mssql_scan materializes at BIND, not here, so its
	// collection is owned by the bind data and shared rather than moved — the
	// same bind data can init more than one global state.
	shared_ptr<ColumnDataCollection> materialized_shared;

	ColumnDataScanState materialized_scan;

	// Context name for pool return
	string context_name;

	// Number of real (non-virtual) columns to fill in the output chunk
	// When 0 (e.g., COUNT(*)), we don't fill any columns but still count rows
	idx_t projected_column_count = 0;

	// Set when complete
	bool done = false;

	// Filters the encoder refused to push, executed client-side per chunk
	// (table_scan.cpp; the type lives in table_scan_state.hpp).
	std::vector<mssql::ClientTableFilter> client_filters;

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
									   vector<LogicalType> &return_types, vector<Identifier> &names);

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

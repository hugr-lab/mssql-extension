// Table Scan Module Contract
// Feature: 013-table-scan-filter-refactor
// This is a design contract, not compilable code
//
// NAMING CONVENTION:
// - Namespace: duckdb::mssql (MSSQL-specific module)
// - Types in duckdb::mssql do NOT use MSSQL prefix (e.g., TableScanBindData, not MSSQLTableScanBindData)
// - Types directly in duckdb namespace MUST use MSSQL prefix (e.g., MSSQLCatalog)
// - This avoids name conflicts while maintaining code readability

#pragma once

#include <string>
#include <vector>
#include "duckdb.hpp"

namespace duckdb {
namespace mssql {

//------------------------------------------------------------------------------
// Bind Data Structure
//------------------------------------------------------------------------------

/**
 * Bind-time data for MSSQL catalog table scans.
 * Created in TableScanBind, used throughout scan lifecycle.
 *
 * Note: No MSSQL prefix as we're in duckdb::mssql namespace.
 */
struct TableScanBindData : public TableFunctionData {
    // Connection context
    std::string context_name;

    // Table identification
    std::string schema_name;
    std::string table_name;

    // Full table schema (all columns)
    std::vector<LogicalType> all_types;
    std::vector<std::string> all_column_names;

    // Projected schema (requested columns only)
    std::vector<LogicalType> return_types;
    std::vector<std::string> column_names;

    // Pre-executed result stream ID (for schema inference)
    uint64_t result_stream_id = 0;

    /**
     * Get the full table name as [schema].[table].
     */
    std::string GetFullTableName() const;

    /**
     * Check if a column index is valid for projection.
     */
    bool IsValidColumnIndex(idx_t idx) const;
};

//------------------------------------------------------------------------------
// Global State Structure
//------------------------------------------------------------------------------

/**
 * Global execution state for table scan.
 * One instance per scan operation.
 */
struct TableScanGlobalState : public GlobalTableFunctionState {
    // Result stream from SQL Server (MSSQLResultStream is in duckdb namespace)
    unique_ptr<MSSQLResultStream> result_stream;

    // Connection context name (for reconnection if needed)
    std::string context_name;

    // Number of projected columns
    idx_t projected_column_count = 0;

    // Scan completion flag
    bool done = false;

    // Timing for debug logging
    bool timing_started = false;
    std::chrono::steady_clock::time_point scan_start;

    // Filter pushdown state
    bool filter_pushdown_applied = false;
    bool needs_duckdb_filter = false;

    /**
     * Returns 1 (single-threaded execution).
     */
    idx_t MaxThreads() const override { return 1; }
};

//------------------------------------------------------------------------------
// Local State Structure
//------------------------------------------------------------------------------

/**
 * Per-thread local state (minimal for single-threaded scan).
 */
struct TableScanLocalState : public LocalTableFunctionState {
    idx_t current_chunk = 0;
};

//------------------------------------------------------------------------------
// Table Scan Function Interface
//------------------------------------------------------------------------------

/**
 * Get the table function for catalog-based MSSQL table scans.
 *
 * This function is called by MSSQLTableEntry::GetScanFunction().
 *
 * Contract:
 * - func.projection_pushdown = true
 * - func.filter_pushdown = true
 * - func.filter_prune = true
 * - MaxThreads() = 1 (single-threaded)
 */
TableFunction GetCatalogScanFunction();

/**
 * Bind function for catalog table scan.
 *
 * @param context Client context
 * @param input Bind input with table metadata
 * @param return_types Output: column types
 * @param names Output: column names
 * @return Bind data for execution
 *
 * Contract:
 * - Populates return_types and names from table metadata
 * - Does NOT execute any query
 * - Returns TableScanBindData
 */
unique_ptr<FunctionData> TableScanBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names
);

/**
 * Global init function for catalog table scan.
 *
 * @param context Execution context
 * @param input Init input with filters and projections
 * @param bind_data The bind data from TableScanBind
 * @return Global state for execution
 *
 * Contract:
 * - Receives input.filters (may be nullptr)
 * - Receives input.column_ids for projection
 * - Builds SELECT query with:
 *   - Column projection from column_ids
 *   - WHERE clause from filter encoder
 * - Executes query, stores result_stream in state
 * - Sets needs_duckdb_filter based on filter encoder result
 */
unique_ptr<GlobalTableFunctionState> TableScanInitGlobal(
    ClientContext &context,
    TableFunctionInitInput &input,
    const FunctionData *bind_data
);

/**
 * Local init function for catalog table scan.
 */
unique_ptr<LocalTableFunctionState> TableScanInitLocal(
    ExecutionContext &context,
    TableFunctionInitInput &input,
    GlobalTableFunctionState *global_state
);

/**
 * Execute function for catalog table scan.
 *
 * @param context Execution context
 * @param input Function input
 * @param output Output DataChunk to fill
 *
 * Contract:
 * - Calls result_stream->FillChunk(output)
 * - Sets output.size() = 0 when done
 * - Handles interruption (Ctrl+C)
 * - Logs timing if MSSQL_DEBUG is set
 */
void TableScanExecute(
    ClientContext &context,
    TableFunctionInput &input,
    DataChunk &output
);

//------------------------------------------------------------------------------
// Query Builder Interface
//------------------------------------------------------------------------------

/**
 * Build a SELECT query for the table scan.
 *
 * @param bind_data Table metadata
 * @param column_ids Projected column indices
 * @param filters Optional filter set
 * @param needs_duckdb_filter Output: true if DuckDB must re-filter
 * @return T-SQL SELECT query string
 *
 * Contract:
 * - Query uses bracketed identifiers for all names
 * - Query includes WHERE clause if filters can be pushed
 * - String literals use N'' prefix
 * - Returns valid T-SQL for SQL Server 2019+
 */
std::string BuildSelectQuery(
    const TableScanBindData& bind_data,
    const std::vector<idx_t>& column_ids,
    const TableFilterSet* filters,
    bool& needs_duckdb_filter
);

} // namespace mssql
} // namespace duckdb

// Table Scan State Structures
// Feature: 013-table-scan-filter-refactor
//
// NAMING CONVENTION:
// - Namespace: duckdb::mssql (MSSQL-specific module)
// - Types in duckdb::mssql do NOT use MSSQL prefix

#pragma once

#include <chrono>
#include <string>
#include "duckdb.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "query/mssql_result_stream.hpp"

namespace duckdb {
namespace mssql {

// A TableFilter the encoder could not push, re-applied client-side over the
// scan output. On 2.0 DuckDB does not re-check TableFilters behind a
// filter_pushdown scan, so a refused filter MUST be executed here or the scan
// returns wrong rows. The expression is the filter's ToExpression over
// BoundReference(0); the executor holds a reference into it, hence both live
// here together.
struct ClientTableFilter {
	idx_t out_col;
	unique_ptr<Expression> expr;
	unique_ptr<ExpressionExecutor> executor;
};

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

	// Filters refused by the encoder, executed client-side per chunk.
	std::vector<ClientTableFilter> client_filters;

	// Destructor - cleanup and logging
	~TableScanGlobalState() override;

	/**
	 * Returns 1 (single-threaded execution).
	 */
	idx_t MaxThreads() const override;
};

/**
 * Per-thread local state (minimal for single-threaded scan).
 */
struct TableScanLocalState : public LocalTableFunctionState {
	idx_t current_chunk = 0;
};

}  // namespace mssql
}  // namespace duckdb

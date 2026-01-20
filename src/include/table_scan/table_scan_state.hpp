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
#include "query/mssql_result_stream.hpp"

namespace duckdb {
namespace mssql {

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

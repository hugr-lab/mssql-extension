// MSSQL Table Scan Implementation
// Feature: 013-table-scan-filter-refactor

#include "table_scan/mssql_table_scan.hpp"
#include "table_scan/filter_encoder.hpp"
#include "table_scan/table_scan_bind.hpp"
#include "table_scan/table_scan_state.hpp"
#include "mssql_functions.hpp"  // For backward compatibility with MSSQLCatalogScanBindData
#include "connection/mssql_pool_manager.hpp"
#include "query/mssql_query_executor.hpp"
#include <cstdlib>

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define MSSQL_SCAN_DEBUG_LOG(level, fmt, ...)                                  \
	do {                                                                       \
		if (GetDebugLevel() >= level) {                                        \
			fprintf(stderr, "[MSSQL TABLE_SCAN] " fmt "\n", ##__VA_ARGS__);     \
		}                                                                      \
	} while (0)

namespace duckdb {
namespace mssql {

// Forward declarations for internal functions
static void TableScanExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output);

//------------------------------------------------------------------------------
// Bind Function
//------------------------------------------------------------------------------

static unique_ptr<FunctionData> TableScanBind(ClientContext &context, TableFunctionBindInput &input,
											  vector<LogicalType> &return_types, vector<string> &names) {
	// This bind function is not used for catalog scans - bind_data is set in GetScanFunction
	// from MSSQLTableEntry
	throw InternalException("TableScanBind should not be called directly");
}

//------------------------------------------------------------------------------
// Init Functions
//------------------------------------------------------------------------------

static unique_ptr<GlobalTableFunctionState> TableScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: START");

	// Use the existing MSSQLCatalogScanBindData for backward compatibility
	auto &bind_data = input.bind_data->Cast<MSSQLCatalogScanBindData>();
	auto result = make_uniq<MSSQLScanGlobalState>();
	result->context_name = bind_data.context_name;

	// Build column list based on projection pushdown (column_ids)
	// column_ids contains the indices of columns needed from the table
	// Note: column_ids may contain special identifiers like COLUMN_IDENTIFIER_ROW_ID or
	// COLUMN_IDENTIFIER_EMPTY for operations like COUNT(*). These are virtual column IDs
	// that start at 2^63, so any value >= that is a special identifier to skip.
	string column_list;
	const auto &column_ids = input.column_ids;

	// Virtual/special column identifiers start at 2^63
	constexpr column_t VIRTUAL_COL_START = UINT64_C(9223372036854775808);

	MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: projection has %zu columns (table has %zu)", column_ids.size(),
						 bind_data.all_column_names.size());

	// Filter out special column identifiers and collect valid column indices
	vector<column_t> valid_column_ids;
	for (const auto &col_idx : column_ids) {
		if (col_idx < VIRTUAL_COL_START && col_idx < bind_data.all_column_names.size()) {
			valid_column_ids.push_back(col_idx);
		} else {
			MSSQL_SCAN_DEBUG_LOG(2, "  skipping special/invalid column_id=%llu", (unsigned long long)col_idx);
		}
	}

	if (valid_column_ids.empty()) {
		// No valid columns projected (e.g., COUNT(*))
		// Select only the first column to minimize data transfer while still returning rows
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: no valid columns, selecting first column only for row counting");
		if (!bind_data.all_column_names.empty()) {
			column_list = "[" + FilterEncoder::EscapeBracketIdentifier(bind_data.all_column_names[0]) + "]";
		} else {
			// Fallback to constant if table has no columns (shouldn't happen)
			column_list = "1";
		}
	} else {
		// Build SELECT with only valid projected columns
		for (idx_t i = 0; i < valid_column_ids.size(); i++) {
			if (i > 0) {
				column_list += ", ";
			}
			column_t col_idx = valid_column_ids[i];
			column_list += "[" + FilterEncoder::EscapeBracketIdentifier(bind_data.all_column_names[col_idx]) + "]";
			MSSQL_SCAN_DEBUG_LOG(2, "  column[%llu] = %s", (unsigned long long)i,
								 bind_data.all_column_names[col_idx].c_str());
		}
	}

	// Generate the query: SELECT [col1], [col2], ... FROM [schema].[table]
	string full_table_name = "[" + FilterEncoder::EscapeBracketIdentifier(bind_data.schema_name) + "].[" +
							 FilterEncoder::EscapeBracketIdentifier(bind_data.table_name) + "]";
	string query = "SELECT " + column_list + " FROM " + full_table_name;

	// Build WHERE clause from filter pushdown using FilterEncoder
	bool filter_pushdown_applied = false;
	bool needs_duckdb_filter = false;

	if (input.filters && !input.filters->filters.empty()) {
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: filter pushdown with %zu filter(s)",
							 input.filters->filters.size());

		auto encode_result = FilterEncoder::Encode(
			input.filters.get(),
			column_ids,
			bind_data.all_column_names,
			bind_data.all_types
		);

		if (!encode_result.where_clause.empty()) {
			query += " WHERE " + encode_result.where_clause;
			filter_pushdown_applied = true;
			MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: added WHERE clause: %s", encode_result.where_clause.c_str());
		}

		needs_duckdb_filter = encode_result.needs_duckdb_filter;
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: needs_duckdb_filter=%s",
							 needs_duckdb_filter ? "true" : "false");
	}

	// Store filter state for potential future use (currently for logging only)
	(void)filter_pushdown_applied;
	(void)needs_duckdb_filter;

	MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: generated query = %s", query.c_str());

	// Execute the query
	MSSQLQueryExecutor executor(bind_data.context_name);
	result->result_stream = executor.Execute(context, query);

	// Set the number of columns to actually fill in the output chunk
	// When valid_column_ids is empty (e.g., COUNT(*)), we don't fill any columns
	result->projected_column_count = valid_column_ids.size();
	if (result->result_stream) {
		result->result_stream->SetColumnsToFill(valid_column_ids.size());
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: set columns_to_fill=%zu", valid_column_ids.size());
	}

	return std::move(result);
}

static unique_ptr<LocalTableFunctionState> TableScanInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
															  GlobalTableFunctionState *global_state) {
	return make_uniq<TableScanLocalState>();
}

//------------------------------------------------------------------------------
// Execute Function
//------------------------------------------------------------------------------

static void TableScanExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &global_state = data.global_state->Cast<MSSQLScanGlobalState>();

	// Start timing on first call
	if (!global_state.timing_started) {
		global_state.scan_start = std::chrono::steady_clock::now();
		global_state.timing_started = true;
		MSSQL_SCAN_DEBUG_LOG(1, "Execute: FIRST CALL - scan started");
	}

	// Check if we're done
	if (global_state.done || !global_state.result_stream) {
		auto scan_end = std::chrono::steady_clock::now();
		auto total_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(scan_end - global_state.scan_start).count();
		MSSQL_SCAN_DEBUG_LOG(1, "Execute: SCAN COMPLETE - total=%ldms", (long)total_ms);
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

//------------------------------------------------------------------------------
// Public Interface
//------------------------------------------------------------------------------

TableFunction GetCatalogScanFunction() {
	// Create table function without arguments - bind_data is set from TableEntry
	TableFunction func("mssql_catalog_scan", {}, TableScanExecute, TableScanBind, TableScanInitGlobal,
					   TableScanInitLocal);

	// Enable projection pushdown - allows DuckDB to tell us which columns are needed
	// The column_ids will be passed to InitGlobal via TableFunctionInitInput
	func.projection_pushdown = true;

	// Enable filter pushdown - allows DuckDB to push WHERE conditions to SQL Server
	// The filters will be passed to InitGlobal via TableFunctionInitInput
	func.filter_pushdown = true;

	// Note: We don't set filter_prune = true because that can cause issues with
	// the DataChunk column count when filter-only columns are excluded

	return func;
}

} // namespace mssql
} // namespace duckdb

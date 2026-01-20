// Table Scan Execute Implementation
// Feature: 013-table-scan-filter-refactor

#include "table_scan/table_scan_state.hpp"
#include "duckdb.hpp"
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

#define MSSQL_EXEC_DEBUG_LOG(level, fmt, ...)                                  \
	do {                                                                       \
		if (GetDebugLevel() >= level) {                                        \
			fprintf(stderr, "[MSSQL TABLE_SCAN EXEC] " fmt "\n", ##__VA_ARGS__); \
		}                                                                      \
	} while (0)

namespace duckdb {
namespace mssql {

void TableScanExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &global_state = data.global_state->Cast<TableScanGlobalState>();

	// Start timing on first call
	if (!global_state.timing_started) {
		global_state.scan_start = std::chrono::steady_clock::now();
		global_state.timing_started = true;
		MSSQL_EXEC_DEBUG_LOG(1, "FIRST CALL - scan started (needs_duckdb_filter=%s)",
							 global_state.needs_duckdb_filter ? "true" : "false");
	}

	// Check if we're done
	if (global_state.done || !global_state.result_stream) {
		auto scan_end = std::chrono::steady_clock::now();
		auto total_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(scan_end - global_state.scan_start).count();
		MSSQL_EXEC_DEBUG_LOG(1, "SCAN COMPLETE - total=%ldms", (long)total_ms);
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

} // namespace mssql
} // namespace duckdb

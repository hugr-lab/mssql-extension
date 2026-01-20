// Table Scan State Implementation
// Feature: 013-table-scan-filter-refactor

#include "table_scan/table_scan_state.hpp"
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

#define MSSQL_TABLE_SCAN_DEBUG_LOG(level, fmt, ...)                         \
	do {                                                                    \
		if (GetDebugLevel() >= level) {                                     \
			fprintf(stderr, "[MSSQL TABLE_SCAN] " fmt "\n", ##__VA_ARGS__); \
		}                                                                   \
	} while (0)

namespace duckdb {
namespace mssql {

TableScanGlobalState::~TableScanGlobalState() {
	// Connection is automatically returned to pool when shared_ptr in result_stream is released
	// This may trigger Cancel() if stream is still active
	result_stream.reset();

	// Log total scan time (from first call to destruction, including cancel/cleanup)
	if (timing_started) {
		auto end = std::chrono::steady_clock::now();
		auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - scan_start).count();
		MSSQL_TABLE_SCAN_DEBUG_LOG(1, "~TableScanGlobalState - total scan time: %ldms (including cancel)",
								   (long)total_ms);
	}
}

idx_t TableScanGlobalState::MaxThreads() const {
	// Single-threaded streaming
	return 1;
}

}  // namespace mssql
}  // namespace duckdb

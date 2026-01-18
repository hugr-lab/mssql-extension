#include "query/mssql_query_executor.hpp"
#include "connection/mssql_pool_manager.hpp"
#include "tds/tds_connection_pool.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/exception.hpp"
#include <chrono>
#include <cstdlib>

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetExecutorDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char* env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define MSSQL_EXEC_DEBUG_LOG(level, fmt, ...) \
	do { if (GetExecutorDebugLevel() >= level) { \
		fprintf(stderr, "[MSSQL EXEC] " fmt "\n", ##__VA_ARGS__); \
	} } while(0)

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLQueryExecutor Implementation
//===----------------------------------------------------------------------===//

MSSQLQueryExecutor::MSSQLQueryExecutor(const std::string& context_name)
    : context_name_(context_name) {
}

void MSSQLQueryExecutor::ValidateContext(ClientContext& context) {
	// Verify the context exists in the pool manager
	auto* pool = MssqlPoolManager::Instance().GetPool(context_name_);

	if (!pool) {
		throw InvalidInputException("MSSQL context '%s' not found. Create it with ATTACH first.",
		                            context_name_.c_str());
	}
}

unique_ptr<MSSQLResultStream> MSSQLQueryExecutor::Execute(ClientContext& context,
                                                          const std::string& sql) {
	MSSQL_EXEC_DEBUG_LOG(1, "Execute: START context='%s'", context_name_.c_str());
	auto total_start = std::chrono::steady_clock::now();

	ValidateContext(context);

	// Get pool from pool manager
	auto* pool = MssqlPoolManager::Instance().GetPool(context_name_);
	if (!pool) {
		throw IOException("Failed to get connection pool for context '%s'",
		                  context_name_.c_str());
	}

	// Log pool stats before acquire
	auto stats = pool->GetStats();
	MSSQL_EXEC_DEBUG_LOG(1, "Execute: pool stats - total=%d, active=%d, idle=%d",
	                     (int)stats.total_connections, (int)stats.active_connections, (int)stats.idle_connections);

	auto acquire_start = std::chrono::steady_clock::now();
	MSSQL_EXEC_DEBUG_LOG(1, "Execute: acquiring connection (timeout=%dms)...", acquire_timeout_ms_);
	auto connection = pool->Acquire(acquire_timeout_ms_);
	auto acquire_end = std::chrono::steady_clock::now();
	auto acquire_ms = std::chrono::duration_cast<std::chrono::milliseconds>(acquire_end - acquire_start).count();
	MSSQL_EXEC_DEBUG_LOG(1, "Execute: connection acquired in %ldms", (long)acquire_ms);

	if (!connection) {
		throw IOException("Failed to acquire connection from pool for context '%s'",
		                  context_name_.c_str());
	}

	// Create result stream with the shared connection
	// Note: result_stream takes ownership of connection - its destructor handles pool release
	auto result_stream = make_uniq<MSSQLResultStream>(std::move(connection), sql, context_name_);

	// Initialize the stream (sends query, waits for COLMETADATA)
	// If Initialize() throws, result_stream destructor will release connection back to pool
	auto init_start = std::chrono::steady_clock::now();
	MSSQL_EXEC_DEBUG_LOG(1, "Execute: initializing result stream...");
	if (!result_stream->Initialize()) {
		throw IOException("Failed to initialize query result stream");
	}
	auto init_end = std::chrono::steady_clock::now();
	auto init_ms = std::chrono::duration_cast<std::chrono::milliseconds>(init_end - init_start).count();
	MSSQL_EXEC_DEBUG_LOG(1, "Execute: result stream initialized in %ldms", (long)init_ms);

	auto total_end = std::chrono::steady_clock::now();
	auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start).count();
	MSSQL_EXEC_DEBUG_LOG(1, "Execute: END (total %ldms)", (long)total_ms);

	return result_stream;
}

}  // namespace duckdb

#pragma once

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/client_context.hpp"
#include "tds/tds_types.hpp"

namespace duckdb {

// Pool configuration structure
// Loaded from DuckDB settings at runtime
struct MSSQLPoolConfig {
	size_t connection_limit = tds::DEFAULT_CONNECTION_LIMIT;
	bool connection_cache = tds::DEFAULT_CONNECTION_CACHE;
	int connection_timeout = tds::DEFAULT_CONNECTION_TIMEOUT;
	int idle_timeout = tds::DEFAULT_IDLE_TIMEOUT;
	size_t min_connections = tds::DEFAULT_MIN_CONNECTIONS;
	int acquire_timeout = tds::DEFAULT_ACQUIRE_TIMEOUT;
};

//===----------------------------------------------------------------------===//
// Statistics Configuration
//===----------------------------------------------------------------------===//

// Default values for statistics settings
constexpr bool DEFAULT_STATISTICS_ENABLED = true;
constexpr int64_t DEFAULT_STATISTICS_LEVEL = 0;       // 0 = row count only
constexpr bool DEFAULT_STATISTICS_USE_DBCC = false;   // DBCC requires permissions
constexpr int64_t DEFAULT_STATISTICS_CACHE_TTL = 300; // 5 minutes

// Statistics configuration structure
struct MSSQLStatisticsConfig {
	bool enabled = DEFAULT_STATISTICS_ENABLED;
	int64_t level = DEFAULT_STATISTICS_LEVEL;          // 0=rowcount, 1=+histogram, 2=+NDV
	bool use_dbcc = DEFAULT_STATISTICS_USE_DBCC;       // Allow DBCC SHOW_STATISTICS
	int64_t cache_ttl_seconds = DEFAULT_STATISTICS_CACHE_TTL;
};

//===----------------------------------------------------------------------===//
// Registration and Loading
//===----------------------------------------------------------------------===//

// Register all MSSQL pool settings with DuckDB
void RegisterMSSQLSettings(ExtensionLoader &loader);

// Load current pool configuration from context settings
MSSQLPoolConfig LoadPoolConfig(ClientContext &context);

// Load catalog cache TTL setting (0 = manual refresh only)
int64_t LoadCatalogCacheTTL(ClientContext &context);

// Load statistics configuration from context settings
MSSQLStatisticsConfig LoadStatisticsConfig(ClientContext &context);

// Individual statistics setting loaders
bool LoadStatisticsEnabled(ClientContext &context);
int64_t LoadStatisticsLevel(ClientContext &context);
bool LoadStatisticsUseDBCC(ClientContext &context);
int64_t LoadStatisticsCacheTTL(ClientContext &context);

}  // namespace duckdb

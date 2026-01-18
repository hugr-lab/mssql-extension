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

// Register all MSSQL pool settings with DuckDB
void RegisterMSSQLSettings(ExtensionLoader &loader);

// Load current pool configuration from context settings
MSSQLPoolConfig LoadPoolConfig(ClientContext &context);

}  // namespace duckdb

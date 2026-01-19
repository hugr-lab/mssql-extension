#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "tds/tds_connection_pool.hpp"

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLTableStatistics - Cached statistics for a single table
//===----------------------------------------------------------------------===//

struct MSSQLTableStatistics {
	//! Approximate row count from sys.dm_db_partition_stats
	idx_t row_count = 0;

	//! When these statistics were last fetched
	std::chrono::steady_clock::time_point fetched_at;

	//! Whether the statistics are valid
	bool is_valid = false;
};

//===----------------------------------------------------------------------===//
// MSSQLStatisticsProvider - Provides table statistics from SQL Server DMVs
//
// This class fetches and caches table statistics (primarily row counts) from
// SQL Server for use by DuckDB's query optimizer. Statistics are cached with
// a configurable TTL to avoid excessive queries to SQL Server.
//===----------------------------------------------------------------------===//

class MSSQLStatisticsProvider {
public:
	//! Constructor
	//! @param cache_ttl_seconds TTL for cached statistics (0 = no caching)
	explicit MSSQLStatisticsProvider(int64_t cache_ttl_seconds = 300);

	//! Get row count for a table (uses cache if available and not expired)
	//! @param connection Connection to use for querying
	//! @param schema_name SQL Server schema name
	//! @param table_name SQL Server table name
	//! @return Approximate row count
	idx_t GetRowCount(tds::TdsConnection &connection, const string &schema_name, const string &table_name);

	//! Get base statistics for DuckDB optimizer
	//! @param connection Connection to use for querying
	//! @param schema_name SQL Server schema name
	//! @param table_name SQL Server table name
	//! @return BaseStatistics with cardinality estimate
	unique_ptr<BaseStatistics> GetTableStatistics(tds::TdsConnection &connection, const string &schema_name,
												  const string &table_name);

	//! Invalidate statistics for a specific table
	void InvalidateTable(const string &schema_name, const string &table_name);

	//! Invalidate statistics for all tables in a schema
	void InvalidateSchema(const string &schema_name);

	//! Invalidate all cached statistics
	void InvalidateAll();

	//! Set the cache TTL
	void SetCacheTTL(int64_t seconds);

	//! Get the cache TTL
	int64_t GetCacheTTL() const;

private:
	//! Build cache key from schema and table name
	static string BuildCacheKey(const string &schema_name, const string &table_name);

	//! Check if cached statistics are still valid
	bool IsCacheValid(const MSSQLTableStatistics &stats) const;

	//! Fetch row count from SQL Server
	idx_t FetchRowCount(tds::TdsConnection &connection, const string &schema_name, const string &table_name);

	//! Cache TTL in seconds
	int64_t cache_ttl_seconds_;

	//! Statistics cache (keyed by "schema.table")
	std::unordered_map<string, MSSQLTableStatistics> cache_;

	//! Mutex for thread-safe cache access
	mutable std::mutex mutex_;
};

}  // namespace duckdb

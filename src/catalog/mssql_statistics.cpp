#include "catalog/mssql_statistics.hpp"
#include "query/mssql_simple_query.hpp"

#include <sstream>

namespace duckdb {

//===----------------------------------------------------------------------===//
// SQL Query for Row Count from DMV
//===----------------------------------------------------------------------===//

// Query sys.dm_db_partition_stats to get approximate row count
// index_id IN (0, 1) captures both heaps (0) and clustered indexes (1)
static const char *ROW_COUNT_SQL_TEMPLATE = R"(
SELECT ISNULL(SUM(p.rows), 0) AS row_count
FROM sys.dm_db_partition_stats p
INNER JOIN sys.objects o ON p.object_id = o.object_id
INNER JOIN sys.schemas s ON o.schema_id = s.schema_id
WHERE s.name = '%s'
  AND o.name = '%s'
  AND p.index_id IN (0, 1)
)";

MSSQLStatisticsProvider::MSSQLStatisticsProvider(int64_t cache_ttl_seconds) : cache_ttl_seconds_(cache_ttl_seconds) {}

//===----------------------------------------------------------------------===//
// Public Methods
//===----------------------------------------------------------------------===//

idx_t MSSQLStatisticsProvider::GetRowCount(tds::TdsConnection &connection, const string &schema_name,
										   const string &table_name, int64_t ttl_seconds) {
	std::lock_guard<std::mutex> lock(mutex_);

	auto key = BuildCacheKey(schema_name, table_name);
	auto it = cache_.find(key);

	// Check if we have valid cached statistics
	if (it != cache_.end() && IsCacheValid(it->second, ttl_seconds, /*exempt_catalog_sourced=*/false)) {
		return it->second.row_count;
	}

	// Fetch fresh statistics
	idx_t row_count = FetchRowCount(connection, schema_name, table_name);

	// Update cache
	MSSQLTableStatistics stats;
	stats.row_count = row_count;
	stats.fetched_at = std::chrono::steady_clock::now();
	stats.is_valid = true;
	cache_[key] = stats;

	return row_count;
}

void MSSQLStatisticsProvider::InvalidateTable(const string &schema_name, const string &table_name) {
	std::lock_guard<std::mutex> lock(mutex_);

	auto key = BuildCacheKey(schema_name, table_name);
	cache_.erase(key);
}

void MSSQLStatisticsProvider::InvalidateSchema(const string &schema_name) {
	std::lock_guard<std::mutex> lock(mutex_);

	// Remove all entries for this schema
	string prefix = schema_name + ".";
	for (auto it = cache_.begin(); it != cache_.end();) {
		if (it->first.compare(0, prefix.length(), prefix) == 0) {
			it = cache_.erase(it);
		} else {
			++it;
		}
	}
}

void MSSQLStatisticsProvider::InvalidateAll() {
	std::lock_guard<std::mutex> lock(mutex_);
	cache_.clear();
}

void MSSQLStatisticsProvider::PreloadRowCount(const string &schema_name, const string &table_name, idx_t row_count) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto key = BuildCacheKey(schema_name, table_name);
	MSSQLTableStatistics stats;
	stats.row_count = row_count;
	stats.fetched_at = std::chrono::steady_clock::now();
	stats.is_valid = true;
	stats.from_catalog_metadata = true;	 // refreshed by invalidation, not by age
	cache_[key] = stats;
}

void MSSQLStatisticsProvider::SeedRowCountForTesting(const string &schema_name, const string &table_name,
													 idx_t row_count, bool from_catalog_metadata) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto key = BuildCacheKey(schema_name, table_name);
	MSSQLTableStatistics stats;
	stats.row_count = row_count;
	stats.fetched_at = std::chrono::steady_clock::now();
	stats.is_valid = true;
	stats.from_catalog_metadata = from_catalog_metadata;
	cache_[key] = stats;
}

bool MSSQLStatisticsProvider::TryGetCachedRowCount(const string &schema_name, const string &table_name,
												   int64_t ttl_seconds, idx_t &out_row_count,
												   bool exempt_catalog_sourced) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto key = BuildCacheKey(schema_name, table_name);
	auto it = cache_.find(key);
	if (it != cache_.end() && IsCacheValid(it->second, ttl_seconds, exempt_catalog_sourced)) {
		out_row_count = it->second.row_count;
		return true;
	}
	return false;
}

int64_t MSSQLStatisticsProvider::GetCacheTTL() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return cache_ttl_seconds_;
}

//===----------------------------------------------------------------------===//
// Private Methods
//===----------------------------------------------------------------------===//

string MSSQLStatisticsProvider::BuildCacheKey(const string &schema_name, const string &table_name) {
	return schema_name + "." + table_name;
}

bool MSSQLStatisticsProvider::IsCacheValid(const MSSQLTableStatistics &stats, int64_t ttl_seconds,
										   bool exempt_catalog_sourced) const {
	if (!stats.is_valid) {
		return false;
	}

	// A catalog-sourced count is not stale-by-age — it is refreshed when the
	// metadata is invalidated — but only the caller knows whether that matters
	// more than freshness. The listing path asks for the exemption because ageing
	// it out costs a connection + DMV query PER TABLE (job 1217); the planner does
	// not, so its TTL still means something (job 1230).
	if (exempt_catalog_sourced && stats.from_catalog_metadata) {
		return true;
	}

	// TTL of 0 means no caching (always fetch fresh)
	if (ttl_seconds <= 0) {
		return false;
	}

	auto now = std::chrono::steady_clock::now();
	auto age = std::chrono::duration_cast<std::chrono::seconds>(now - stats.fetched_at).count();

	return age < ttl_seconds;
}

idx_t MSSQLStatisticsProvider::FetchRowCount(tds::TdsConnection &connection, const string &schema_name,
											 const string &table_name) {
	// Escape single quotes in schema/table names to prevent SQL injection
	string safe_schema = schema_name;
	string safe_table = table_name;

	// Replace ' with '' for SQL escaping
	size_t pos = 0;
	while ((pos = safe_schema.find('\'', pos)) != string::npos) {
		safe_schema.replace(pos, 1, "''");
		pos += 2;
	}
	pos = 0;
	while ((pos = safe_table.find('\'', pos)) != string::npos) {
		safe_table.replace(pos, 1, "''");
		pos += 2;
	}

	// Build the query
	char sql_buffer[1024];
	snprintf(sql_buffer, sizeof(sql_buffer), ROW_COUNT_SQL_TEMPLATE, safe_schema.c_str(), safe_table.c_str());

	// Execute query and get result
	std::string result = MSSQLSimpleQuery::ExecuteScalar(connection, sql_buffer);

	if (result.empty()) {
		return 0;
	}

	try {
		return static_cast<idx_t>(std::stoull(result));
	} catch (...) {
		// If parsing fails, return 0
		return 0;
	}
}

}  // namespace duckdb

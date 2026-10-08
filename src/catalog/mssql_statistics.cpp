#include "catalog/mssql_statistics.hpp"
#include "duckdb/common/exception.hpp"
#include "query/mssql_simple_query.hpp"
#include "query/mssql_sql_params.hpp"

#include <sstream>

namespace duckdb {

//===----------------------------------------------------------------------===//
// SQL Query for Row Count from DMV
//===----------------------------------------------------------------------===//

// The table's row count, as the catalog's own metadata queries read it:
// OBJECTPROPERTYEX(id, 'Cardinality') answers out of object metadata (3 to ~110
// logical reads, specs 071 / 084), 0 for an empty table, NULL -> 0 for a view.
//
// Until spec 083 this read sys.dm_db_partition_stats, whose column is
// `row_count`, through `p.rows` (sys.partitions' name): every call failed with
// error 207, ExecuteScalar handed back "", and a 0 was cached -- since spec 008.
// The DMV was also the expensive source: a scan of sysrowsets whatever the
// filter (spec 071 F1).
static const char *ROW_COUNT_SQL_TEMPLATE = R"(
SELECT CAST(ISNULL(OBJECTPROPERTYEX(OBJECT_ID(QUOTENAME(@s) + N'.' + QUOTENAME(@t)), 'Cardinality'), 0) AS BIGINT)
    AS row_count
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
	// Spec 075 W4 (#334): the names travel as sp_executesql parameters, so one
	// plan serves every table; the quoting the old snprintf path did by hand
	// is NVarcharLiteral's.
	const tds::Request sql = mssql::BuildExecuteSqlRequest(ROW_COUNT_SQL_TEMPLATE, "@s sysname, @t sysname",
														   {{"s", schema_name}, {"t", table_name}});

	// A failed query throws rather than reading as 0: GetRowCount would cache
	// the 0 as a fresh count. GetStorageInfo catches and falls back to the
	// count the entry carries.
	auto result_set = MSSQLSimpleQuery::Execute(connection, sql);
	if (result_set.HasError()) {
		throw IOException("MSSQL: row count of %s.%s failed: %s", schema_name, table_name, result_set.DescribeError());
	}
	if (result_set.rows.empty() || result_set.rows[0].empty() || result_set.rows[0][0].empty()) {
		return 0;
	}
	const std::string &result = result_set.rows[0][0];

	try {
		return static_cast<idx_t>(std::stoull(result));
	} catch (...) {
		// A BIGINT that does not parse is a broken result, not an empty table.
		throw IOException("MSSQL: row count of %s.%s is not a number: '%s'", schema_name, table_name, result);
	}
}

}  // namespace duckdb

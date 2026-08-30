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

	//! Filled by PreloadRowCount from the CATALOG's own metadata load, not by a
	//! DMV query. Such an entry is not stale-by-age — it is refreshed when the
	//! metadata is invalidated — so the TTL does not apply to it (job 1217).
	//!
	//! Without this, `SET mssql_statistics_cache_ttl_seconds = 0` turned
	//! `SHOW ALL TABLES` into one pool acquire + one sys.dm_db_partition_stats
	//! round trip PER TABLE, on a catalog that had just loaded every count in a
	//! single query. "Always fresh" should not mean "N connections per listing".
	bool from_catalog_metadata = false;
};

//===----------------------------------------------------------------------===//
// MSSQLStatisticsProvider - Provides table statistics from SQL Server DMVs
//
// This class fetches and caches table statistics (primarily row counts) from
// SQL Server for use by DuckDB's query optimizer. Statistics are cached with
// a configurable TTL to avoid excessive queries to SQL Server.
//
// LIFETIME CONTRACT (spec 052 US3 T019):
// All public methods return by value (idx_t for row counts,
// unique_ptr<BaseStatistics> owned by caller) or via out-parameter. NO
// raw-pointer-handout pattern present in this surface, so the spec 052
// catalog-entry UAF class (binder holds raw pointer into a cache that
// another thread invalidates) cannot occur here. If a future method
// returns a `MSSQLTableStatistics *` or similar, audit it against this
// rule and either keep the by-value pattern or apply shared_ptr +
// graveyard like MSSQLTableSet did.
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
	//! @param ttl_seconds TTL to judge the cached entry against — the CALLER's
	//!        session value, not the provider's. See TryGetCachedRowCount below
	//!        for why the provider's own field must not be set from a session.
	//! @return Approximate row count
	idx_t GetRowCount(tds::TdsConnection &connection, const string &schema_name, const string &table_name,
					  int64_t ttl_seconds);

	//! Invalidate statistics for a specific table
	void InvalidateTable(const string &schema_name, const string &table_name);

	//! Invalidate statistics for all tables in a schema
	void InvalidateSchema(const string &schema_name);

	//! Invalidate all cached statistics
	void InvalidateAll();

	//! Pre-populate the cache with row counts (e.g. from BulkLoadAll)
	//! Avoids per-table DMV queries when cardinality is already known
	void PreloadRowCount(const string &schema_name, const string &table_name, idx_t row_count);

	//! Seed an entry with an explicit provenance. Exists so a test can create a
	//! DMV-SHAPED entry (`from_catalog_metadata = false`) without a connection —
	//! otherwise PreloadRowCount is the only reachable populator, every entry is
	//! catalog-sourced, and the `stats.from_catalog_metadata` half of the
	//! exemption predicate is unpinned: weakening it to `if (exempt_catalog_sourced)`
	//! alone leaves the suite green while making SHOW ALL TABLES serve a
	//! DMV-sourced count that never ages out (job 1259).
	void SeedRowCountForTesting(const string &schema_name, const string &table_name, idx_t row_count,
								bool from_catalog_metadata);

	//! Same, but judged against a CALLER-SUPPLIED TTL rather than the provider's.
	//!
	//! `mssql_statistics_cache_ttl_seconds` is a SESSION setting, and this
	//! provider is one object shared by the whole catalog (job 1203). Calling
	//! SetCacheTTL from a planning ClientContext therefore rewrote the TTL every
	//! OTHER session sees — one session's `SET ... = 3600` handed every other
	//! session a staleness window it never asked for, and `SET ... = 0` forced
	//! them all onto the per-table DMV path. Pass the value in at the read
	//! instead, so a session setting governs only that session's lookups.
	//! THE ONLY READ. Every caller passes its own TTL — the planner,
	//! GetStorageInfo and GetRowCount alike.
	//!
	//! The TTL-less overload and `SetCacheTTL` were REMOVED rather than left
	//! unused (job 1217): after the fix nothing called them, and a mutable
	//! catalog-wide TTL sitting next to a per-session setting is precisely the
	//! shape that invited this bug twice (jobs 1203, 1216). The type can no longer
	//! express it.
	//! @param exempt_catalog_sourced When true, an entry filled by PreloadRowCount
	//!        (the catalog's own metadata load, not a DMV query) ignores the TTL —
	//!        it is refreshed by invalidation, not by age. ONLY the table-listing
	//!        path wants this: it is what keeps `SET
	//!        mssql_statistics_cache_ttl_seconds = 0` from turning SHOW ALL TABLES
	//!        into one connection + DMV round trip per table.
	//!
	//!        Deliberately NOT the default (job 1230). Making it unconditional
	//!        exempted every entry the cache can hold in practice — PreloadRowCount
	//!        is its only populator on the reachable paths — so the TTL governed
	//!        nothing, GetRowCount's DMV re-read became unreachable, and the test
	//!        written to guard the caller-supplied-TTL invariant passed even with
	//!        that invariant reverted.
	bool TryGetCachedRowCount(const string &schema_name, const string &table_name, int64_t ttl_seconds,
							  idx_t &out_row_count, bool exempt_catalog_sourced = false);

	//! Get the cache TTL
	int64_t GetCacheTTL() const;

private:
	//! Build cache key from schema and table name
	static string BuildCacheKey(const string &schema_name, const string &table_name);

	//! Check if cached statistics are still valid
	//! Judged against the CALLER's TTL — there is deliberately no TTL-less form.
	bool IsCacheValid(const MSSQLTableStatistics &stats, int64_t ttl_seconds, bool exempt_catalog_sourced) const;

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

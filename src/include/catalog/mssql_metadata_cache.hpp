#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "catalog/mssql_catalog_filter.hpp"
#include "catalog/mssql_column_info.hpp"
#include "tds/tds_connection_pool.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Forward declarations
//===----------------------------------------------------------------------===//

class MSSQLCatalog;

//===----------------------------------------------------------------------===//
// ObjectType - Table or View
//===----------------------------------------------------------------------===//

enum class MSSQLObjectType : uint8_t {
	TABLE = 0,	// 'U' in sys.objects
	VIEW = 1	// 'V' in sys.objects
};

//===----------------------------------------------------------------------===//
// CacheLoadState - Per-level loading state for incremental cache
//===----------------------------------------------------------------------===//

enum class CacheLoadState : uint8_t {
	NOT_LOADED = 0,	 // Never loaded or invalidated
	LOADING = 1,	 // Currently being loaded (by another thread)
	LOADED = 2,		 // Successfully loaded and valid
	STALE = 3		 // TTL expired, needs refresh on next access
};

//===----------------------------------------------------------------------===//
// TableMetadata - Cached table/view metadata
//===----------------------------------------------------------------------===//

struct MSSQLTableMetadata {
	string name;					  // Table/view name
	MSSQLObjectType object_type;	  // TABLE or VIEW
	vector<MSSQLColumnInfo> columns;  // Column metadata
	idx_t approx_row_count;			  // Cardinality estimate from sys.partitions

	// Incremental cache state for columns.
	// Issue #178 (D6): all fields — including these states — are guarded by the
	// cache-wide MSSQLMetadataCache::mutex_; the former per-table load_mutex is gone.
	CacheLoadState columns_load_state = CacheLoadState::NOT_LOADED;
	std::chrono::steady_clock::time_point columns_last_refresh;

	// Default constructor
	MSSQLTableMetadata() : object_type(MSSQLObjectType::TABLE), approx_row_count(0) {}
};

//===----------------------------------------------------------------------===//
// SchemaMetadata - Cached schema metadata
//===----------------------------------------------------------------------===//

struct MSSQLSchemaMetadata {
	string name;									   // Schema name
	unordered_map<string, MSSQLTableMetadata> tables;  // Tables and views in schema

	// Incremental cache state for table list.
	// Issue #178 (D6): guarded by MSSQLMetadataCache::mutex_ (cache-wide); the
	// former per-schema load_mutex is gone — it did NOT exclude readers holding
	// the map mutex, which is exactly the race it pretended to prevent.
	CacheLoadState tables_load_state = CacheLoadState::NOT_LOADED;
	std::chrono::steady_clock::time_point tables_last_refresh;

	// Default constructor
	MSSQLSchemaMetadata() = default;

	// Explicit constructor with name
	explicit MSSQLSchemaMetadata(const string &n) : name(n) {}
};

//===----------------------------------------------------------------------===//
// CacheState - Metadata cache state machine (global cache state)
//===----------------------------------------------------------------------===//

enum class MSSQLCacheState : uint8_t {
	EMPTY = 0,	  // No metadata loaded
	LOADING = 1,  // Currently loading metadata
	LOADED = 2,	  // Metadata loaded and valid
	STALE = 3,	  // TTL expired, needs refresh
	INVALID = 4	  // Invalidated, must refresh before use
};

//===----------------------------------------------------------------------===//
// MSSQLMetadataCache - In-memory cache of schema/table/column metadata
//===----------------------------------------------------------------------===//

class MSSQLMetadataCache {
public:
	explicit MSSQLMetadataCache(int64_t ttl_seconds = 0);
	~MSSQLMetadataCache() = default;

	// Non-copyable
	MSSQLMetadataCache(const MSSQLMetadataCache &) = delete;
	MSSQLMetadataCache &operator=(const MSSQLMetadataCache &) = delete;

	//===----------------------------------------------------------------------===//
	// Cache Access (with lazy loading)
	//===----------------------------------------------------------------------===//

	// Get all schema names (triggers lazy loading of schema list)
	vector<string> GetSchemaNames(tds::TdsConnection &connection);

	// Get tables/views in a schema (triggers lazy loading of table list)
	vector<string> GetTableNames(tds::TdsConnection &connection, const string &schema_name);

	// Get table metadata: loads schemas (fast) + columns for the specific table in one query.
	// Does NOT load all tables in the schema. Returns false if the table doesn't exist.
	//
	// Issue #178 review: copies the metadata into out_meta UNDER the cache mutex.
	// The previous raw-pointer return escaped the lock and was dereferenced by
	// LoadSingleEntry while a concurrent Refresh / LoadAllTableMetadata /
	// TTL-expired reload freed the map node — a residual use-after-free of the
	// same class this cache's single-mutex rework eliminated.
	bool GetTableMetadata(tds::TdsConnection &connection, const string &schema_name, const string &table_name,
						  MSSQLTableMetadata &out_meta);

	// Load all table metadata for a schema in one bulk query.
	// If all tables already have columns loaded (e.g. from preload), returns from cache.
	// Otherwise loads everything with BULK_METADATA_SCHEMA_SQL_TEMPLATE (one round trip).
	void LoadAllTableMetadata(tds::TdsConnection &connection, const string &schema_name);

	// Check if schema exists (reads cached state only, no lazy loading)
	bool HasSchema(const string &schema_name);

	// Check if table exists (reads cached state only, no lazy loading)
	bool HasTable(const string &schema_name, const string &table_name);

	// T036: Get schema names without connection if already loaded
	// Returns true if schemas are loaded and names were populated, false otherwise
	bool TryGetCachedSchemaNames(vector<string> &out_names);

	//===----------------------------------------------------------------------===//
	// Bulk Catalog Preload (Spec 033: US5)
	//===----------------------------------------------------------------------===//

	// Load all metadata in a single SQL Server round trip
	// @param connection TDS connection to use
	// @param schema_name If non-empty, limit bulk load to this schema only
	// @param schema_count Output: number of schemas loaded
	// @param table_count Output: number of tables loaded
	// @param column_count Output: number of columns loaded
	void BulkLoadAll(tds::TdsConnection &connection, const string &schema_name, idx_t &schema_count, idx_t &table_count,
					 idx_t &column_count);

	//===----------------------------------------------------------------------===//
	// Cache Management
	//===----------------------------------------------------------------------===//

	// Full cache refresh from SQL Server
	void Refresh(tds::TdsConnection &connection, const string &database_collation);

	// Check if cache is expired (TTL > 0 and time exceeded)
	bool IsExpired() const;

	// Check if cache needs loading (empty or stale)
	bool NeedsRefresh() const;

	// Invalidate cache (marks as requiring refresh)
	void Invalidate();

	// Get cache state
	MSSQLCacheState GetState() const;

	// Set catalog filter (applied to schema/table discovery results)
	void SetFilter(const MSSQLCatalogFilter *filter);

	// Get catalog filter
	const MSSQLCatalogFilter *GetFilter() const;

	// Set TTL (0 = manual refresh only)
	void SetTTL(int64_t ttl_seconds);

	// Get TTL
	int64_t GetTTL() const;

	// Set metadata query timeout in seconds (0 = no timeout)
	void SetMetadataTimeout(int timeout_seconds);

	// Get metadata query timeout in milliseconds
	int GetMetadataTimeoutMs() const;

	// Set database default collation
	void SetDatabaseCollation(const string &collation);

	// Get database default collation.
	// Returns by VALUE: a reference would outlive the internal lock and race
	// with Refresh() overwriting the string (issue #178 D6 audit).
	string GetDatabaseCollation() const;

	//===----------------------------------------------------------------------===//
	// Incremental Cache Loading (Lazy Loading)
	//===----------------------------------------------------------------------===//

	// Ensure schema list is loaded (lazy loading with double-checked locking)
	void EnsureSchemasLoaded(tds::TdsConnection &connection);

	// Ensure table list for schema is loaded
	void EnsureTablesLoaded(tds::TdsConnection &connection, const string &schema_name);

	//===----------------------------------------------------------------------===//
	// Point Invalidation
	//===----------------------------------------------------------------------===//

	// Invalidate schema's table list (for CREATE/DROP TABLE)
	void InvalidateSchema(const string &schema_name);

	// Invalidate ONLY the schema's table list (existence), keeping every table's cached
	// column metadata. Used by per-table invalidation so a CREATE/DROP is reflected without
	// re-fetching the columns of every other table in the schema.
	void InvalidateSchemaTableList(const string &schema_name);

	// Invalidate table's column metadata (for ALTER TABLE)
	void InvalidateTable(const string &schema_name, const string &table_name);

	// Invalidate all cache (schema list, all tables, all columns)
	void InvalidateAll();

	//===----------------------------------------------------------------------===//
	// Cache State Queries (for testing/debugging)
	//===----------------------------------------------------------------------===//

	// Get schema list load state
	CacheLoadState GetSchemasState() const;

	// Get table list load state for schema
	CacheLoadState GetTablesState(const string &schema_name) const;

	// Iterate all loaded tables, calling callback(schema_name, table_name, approx_row_count)
	void ForEachTable(const std::function<void(const string &, const string &, idx_t)> &callback) const;

	// Iterate all loaded tables in a specific schema, calling callback(table_name, table_metadata)
	void ForEachTableInSchema(const string &schema_name,
							  const std::function<void(const string &, const MSSQLTableMetadata &)> &callback) const;

	// Get column metadata load state for table
	CacheLoadState GetColumnsState(const string &schema_name, const string &table_name) const;

private:
	//===----------------------------------------------------------------------===//
	// Internal Loading Methods
	//===----------------------------------------------------------------------===//

	// Load schemas from sys.schemas
	void LoadSchemas(tds::TdsConnection &connection);

	// Load tables and views from sys.objects
	void LoadTables(tds::TdsConnection &connection, const string &schema_name);

	// Load columns from sys.columns
	void LoadColumns(tds::TdsConnection &connection, const string &schema_name, const string &table_name,
					 MSSQLTableMetadata &table_metadata);

	// Execute metadata query with configured timeout (metadata_timeout_ms_)
	using MetadataRowCallback = std::function<void(const vector<string> &values)>;
	void ExecuteMetadataQuery(tds::TdsConnection &connection, const string &sql, MetadataRowCallback callback);

	//===----------------------------------------------------------------------===//
	// Member Variables
	//===----------------------------------------------------------------------===//

	// Issue #178 (D6): ONE cache-wide mutex. It guards state_, schemas_ (the map
	// AND everything reachable through it: tables, columns, load states),
	// last_refresh_, database_collation_, schemas_load_state_ and
	// schemas_last_refresh_. The previous split (`mutex_` for "global ops",
	// `schemas_mutex_` for the map) had Refresh() freeing the whole map under
	// one mutex while every reader iterated it under the other — a TSan-confirmed
	// use-after-free. Loads intentionally hold this mutex across their SQL round
	// trip so partial state is never visible; that also serialises concurrent
	// metadata loads (correctness over parallel-load throughput).
	mutable std::mutex mutex_;
	MSSQLCacheState state_;								  // Current cache state (backward compat)
	const MSSQLCatalogFilter *filter_ = nullptr;		  // Set once at catalog init, before any concurrency
	unordered_map<string, MSSQLSchemaMetadata> schemas_;  // Cached schemas
	std::chrono::steady_clock::time_point last_refresh_;  // Last refresh timestamp (backward compat)
	string database_collation_;							  // Database default collation

	// Atomics, NOT guarded by mutex_ (issue #178 D4): written by EnsureCacheLoaded
	// on every catalog lookup while loaders concurrently read them mid-query
	// (ExecuteMetadataQuery runs WITH mutex_ held, so these must be lock-free).
	std::atomic<int64_t> ttl_seconds_;		// Cache TTL (0 = manual only)
	std::atomic<int> metadata_timeout_ms_;	// Metadata query timeout in ms (default 5 min)

	// Incremental cache state for schema list (catalog-level) — guarded by mutex_
	CacheLoadState schemas_load_state_ = CacheLoadState::NOT_LOADED;
	std::chrono::steady_clock::time_point schemas_last_refresh_;
};

}  // namespace duckdb

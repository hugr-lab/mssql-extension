#pragma once

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

	// Incremental cache state for columns
	CacheLoadState columns_load_state = CacheLoadState::NOT_LOADED;
	std::chrono::steady_clock::time_point columns_last_refresh;
	mutable std::mutex load_mutex;	// Per-table loading synchronization

	// Default constructor
	MSSQLTableMetadata() : object_type(MSSQLObjectType::TABLE), approx_row_count(0) {}

	// Move constructor (mutex not movable)
	MSSQLTableMetadata(MSSQLTableMetadata &&other) noexcept;

	// Move assignment (mutex not movable)
	MSSQLTableMetadata &operator=(MSSQLTableMetadata &&other) noexcept;

	// Non-copyable (mutex not copyable)
	MSSQLTableMetadata(const MSSQLTableMetadata &) = delete;
	MSSQLTableMetadata &operator=(const MSSQLTableMetadata &) = delete;
};

//===----------------------------------------------------------------------===//
// SchemaMetadata - Cached schema metadata
//===----------------------------------------------------------------------===//

struct MSSQLSchemaMetadata {
	string name;									   // Schema name
	unordered_map<string, MSSQLTableMetadata> tables;  // Tables and views in schema

	// Incremental cache state for table list
	CacheLoadState tables_load_state = CacheLoadState::NOT_LOADED;
	std::chrono::steady_clock::time_point tables_last_refresh;
	mutable std::mutex load_mutex;	// Per-schema loading synchronization

	// Default constructor
	MSSQLSchemaMetadata() = default;

	// Explicit constructor with name
	explicit MSSQLSchemaMetadata(const string &n) : name(n) {}

	// Move constructor (mutex not movable)
	MSSQLSchemaMetadata(MSSQLSchemaMetadata &&other) noexcept;

	// Move assignment (mutex not movable)
	MSSQLSchemaMetadata &operator=(MSSQLSchemaMetadata &&other) noexcept;

	// Non-copyable (mutex not copyable)
	MSSQLSchemaMetadata(const MSSQLSchemaMetadata &) = delete;
	MSSQLSchemaMetadata &operator=(const MSSQLSchemaMetadata &) = delete;
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
	// Does NOT load all tables in the schema. Returns nullptr if the table doesn't exist.
	const MSSQLTableMetadata *GetTableMetadata(tds::TdsConnection &connection, const string &schema_name,
											   const string &table_name);

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
	void BulkLoadAll(tds::TdsConnection &connection, const string &schema_name,
					 idx_t &schema_count, idx_t &table_count, idx_t &column_count);

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

	// Get database default collation
	const string &GetDatabaseCollation() const;

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

	mutable std::mutex mutex_;							  // Thread-safety for global operations
	MSSQLCacheState state_;								  // Current cache state (backward compat)
	const MSSQLCatalogFilter *filter_ = nullptr;		  // Optional visibility filter (owned by MSSQLCatalog)
	unordered_map<string, MSSQLSchemaMetadata> schemas_;  // Cached schemas
	std::chrono::steady_clock::time_point last_refresh_;  // Last refresh timestamp (backward compat)
	int64_t ttl_seconds_;								  // Cache TTL (0 = manual only)
	int metadata_timeout_ms_ = 300000;					  // Metadata query timeout in ms (default 5 min)
	string database_collation_;							  // Database default collation

	// Incremental cache state for schema list (catalog-level)
	CacheLoadState schemas_load_state_ = CacheLoadState::NOT_LOADED;
	std::chrono::steady_clock::time_point schemas_last_refresh_;
	mutable std::mutex schemas_mutex_;	// Schema list loading synchronization
};

}  // namespace duckdb

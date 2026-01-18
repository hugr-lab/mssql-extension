#pragma once

#include "catalog/mssql_column_info.hpp"
#include "tds/tds_connection_pool.hpp"
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace duckdb {

//===----------------------------------------------------------------------===//
// Forward declarations
//===----------------------------------------------------------------------===//

class MSSQLCatalog;

//===----------------------------------------------------------------------===//
// ObjectType - Table or View
//===----------------------------------------------------------------------===//

enum class MSSQLObjectType : uint8_t {
	TABLE = 0,  // 'U' in sys.objects
	VIEW = 1    // 'V' in sys.objects
};

//===----------------------------------------------------------------------===//
// TableMetadata - Cached table/view metadata
//===----------------------------------------------------------------------===//

struct MSSQLTableMetadata {
	string name;                         // Table/view name
	MSSQLObjectType object_type;         // TABLE or VIEW
	vector<MSSQLColumnInfo> columns;     // Column metadata
	idx_t approx_row_count;              // Cardinality estimate from sys.partitions
};

//===----------------------------------------------------------------------===//
// SchemaMetadata - Cached schema metadata
//===----------------------------------------------------------------------===//

struct MSSQLSchemaMetadata {
	string name;                                          // Schema name
	unordered_map<string, MSSQLTableMetadata> tables;     // Tables and views in schema
};

//===----------------------------------------------------------------------===//
// CacheState - Metadata cache state machine
//===----------------------------------------------------------------------===//

enum class MSSQLCacheState : uint8_t {
	EMPTY = 0,      // No metadata loaded
	LOADING = 1,    // Currently loading metadata
	LOADED = 2,     // Metadata loaded and valid
	STALE = 3,      // TTL expired, needs refresh
	INVALID = 4     // Invalidated, must refresh before use
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
	// Cache Access
	//===----------------------------------------------------------------------===//

	// Get all schema names (loads if empty)
	vector<string> GetSchemaNames();

	// Get tables/views in a schema (loads if empty)
	vector<string> GetTableNames(const string &schema_name);

	// Get table metadata (loads if empty)
	const MSSQLTableMetadata *GetTableMetadata(const string &schema_name, const string &table_name);

	// Check if schema exists
	bool HasSchema(const string &schema_name);

	// Check if table exists
	bool HasTable(const string &schema_name, const string &table_name);

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

	// Set TTL (0 = manual refresh only)
	void SetTTL(int64_t ttl_seconds);

	// Get TTL
	int64_t GetTTL() const;

	// Set database default collation
	void SetDatabaseCollation(const string &collation);

	// Get database default collation
	const string &GetDatabaseCollation() const;

private:
	//===----------------------------------------------------------------------===//
	// Internal Loading Methods
	//===----------------------------------------------------------------------===//

	// Load schemas from sys.schemas
	void LoadSchemas(tds::TdsConnection &connection);

	// Load tables and views from sys.objects
	void LoadTables(tds::TdsConnection &connection, const string &schema_name);

	// Load columns from sys.columns
	void LoadColumns(tds::TdsConnection &connection, const string &schema_name,
	                 const string &table_name, MSSQLTableMetadata &table_metadata);

	//===----------------------------------------------------------------------===//
	// Member Variables
	//===----------------------------------------------------------------------===//

	mutable std::mutex mutex_;                                        // Thread-safety
	MSSQLCacheState state_;                                           // Current cache state
	unordered_map<string, MSSQLSchemaMetadata> schemas_;              // Cached schemas
	std::chrono::steady_clock::time_point last_refresh_;              // Last refresh timestamp
	int64_t ttl_seconds_;                                             // Cache TTL (0 = manual only)
	string database_collation_;                                       // Database default collation
};

}  // namespace duckdb

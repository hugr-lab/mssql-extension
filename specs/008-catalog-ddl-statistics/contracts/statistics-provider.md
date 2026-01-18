# Contract: Statistics Provider

**Feature**: 008-catalog-ddl-statistics
**Module**: `mssql_statistics`

## Purpose

Provides table and column statistics from SQL Server to DuckDB's query optimizer.

## Interface

```cpp
namespace duckdb {

//! Statistics level for MSSQL tables
enum class MSSQLStatisticsLevel : uint8_t {
    ROW_COUNT_ONLY = 0,    //! Only fetch row count from DMV
    WITH_HISTOGRAM = 1,    //! Include min/max from histogram (requires DBCC)
    WITH_NDV = 2           //! Include distinct value estimate
};

//! Cached statistics for a single table
struct MSSQLTableStatistics {
    //! Approximate row count from DMV
    idx_t row_count;
    //! When statistics were fetched
    timestamp_t last_updated;
    //! Per-column statistics (keyed by column name)
    unordered_map<string, MSSQLColumnStatistics> column_stats;
    //! Whether this entry is still valid
    bool is_valid;
};

//! Column-level statistics
struct MSSQLColumnStatistics {
    //! Minimum value (for numeric/temporal types)
    Value min_value;
    //! Maximum value (for numeric/temporal types)
    Value max_value;
    //! Approximate number of distinct values
    idx_t distinct_count;
    //! Number of NULL values
    idx_t null_count;
    //! Whether SQL Server has statistics for this column
    bool has_stats;
};

//! Provides statistics from SQL Server for DuckDB optimizer
class MSSQLStatisticsProvider {
public:
    //! Constructor
    //! @param catalog Parent catalog for connection access
    explicit MSSQLStatisticsProvider(MSSQLCatalog &catalog);

    //! Get statistics for a table
    //! @param context Client context for settings access
    //! @param schema_name Schema containing the table
    //! @param table_name Table name
    //! @return Pointer to statistics or nullptr if unavailable
    unique_ptr<BaseStatistics> GetTableStatistics(ClientContext &context,
                                                  const string &schema_name,
                                                  const string &table_name);

    //! Get row count for a table (fast path)
    //! @param context Client context
    //! @param schema_name Schema containing the table
    //! @param table_name Table name
    //! @return Row count or 0 if unavailable
    idx_t GetRowCount(ClientContext &context,
                      const string &schema_name,
                      const string &table_name);

    //! Invalidate cached statistics for a table
    //! @param schema_name Schema containing the table
    //! @param table_name Table name
    void InvalidateTable(const string &schema_name, const string &table_name);

    //! Invalidate all cached statistics
    void InvalidateAll();

    //! Check if statistics are enabled
    //! @param context Client context for settings
    //! @return True if statistics collection is enabled
    static bool IsEnabled(ClientContext &context);

    //! Get configured statistics level
    //! @param context Client context for settings
    //! @return Current statistics level
    static MSSQLStatisticsLevel GetLevel(ClientContext &context);

    //! Get cache TTL in seconds
    //! @param context Client context for settings
    //! @return TTL in seconds
    static int64_t GetCacheTTL(ClientContext &context);

private:
    //! Parent catalog
    MSSQLCatalog &catalog_;

    //! Statistics cache: schema.table -> statistics
    unordered_map<string, MSSQLTableStatistics> cache_;

    //! Cache mutex
    mutex cache_mutex_;

    //! Fetch row count from sys.dm_db_partition_stats
    idx_t FetchRowCount(const string &schema_name, const string &table_name);

    //! Fetch column statistics via DBCC SHOW_STATISTICS
    MSSQLColumnStatistics FetchColumnStats(const string &schema_name,
                                           const string &table_name,
                                           const string &column_name);

    //! Build cache key from schema and table
    static string BuildCacheKey(const string &schema_name, const string &table_name);

    //! Check if cached entry is still valid
    bool IsCacheValid(const MSSQLTableStatistics &stats, int64_t ttl_seconds);
};

} // namespace duckdb
```

## SQL Queries

### Row Count Query

```sql
SELECT SUM(row_count) AS row_count
FROM sys.dm_db_partition_stats
WHERE object_id = OBJECT_ID(N'[schema].[table]')
  AND (index_id = 0 OR index_id = 1);
```

### Column Statistics Query (DBCC)

```sql
-- Find statistics name
SELECT TOP 1 s.name AS stats_name
FROM sys.stats s
JOIN sys.stats_columns sc ON s.stats_id = sc.stats_id AND s.object_id = sc.object_id
JOIN sys.columns c ON sc.column_id = c.column_id AND sc.object_id = c.object_id
WHERE s.object_id = OBJECT_ID(N'[schema].[table]')
  AND c.name = N'column_name';

-- Get histogram (requires VIEW DATABASE STATE permission)
DBCC SHOW_STATISTICS (N'[schema].[table]', N'stats_name') WITH HISTOGRAM;
```

## Configuration Settings

| Setting | Type | Default | Description |
| ------- | ---- | ------- | ----------- |
| mssql_enable_statistics | bool | true | Enable statistics collection |
| mssql_statistics_level | int | 0 | 0=rowcount, 1=+histogram, 2=+NDV |
| mssql_statistics_use_dbcc | bool | false | Allow DBCC commands |
| mssql_statistics_cache_ttl_seconds | int | 300 | Statistics cache TTL |

## Error Handling

- **Permission Denied**: If row count query fails, return nullptr (graceful fallback)
- **DBCC Not Allowed**: If `mssql_statistics_use_dbcc = false`, skip column stats
- **Invalid Statistics**: If DBCC fails, log warning and continue with row count only

## Thread Safety

- Cache access is protected by `cache_mutex_`
- Concurrent reads are allowed when cache is valid
- Cache writes are serialized

## Cache Invalidation

Statistics are invalidated when:
1. TTL expires (`mssql_statistics_cache_ttl_seconds`)
2. DDL operation affects the table
3. `InvalidateTable()` is called explicitly
4. `InvalidateAll()` is called (e.g., on schema reload)

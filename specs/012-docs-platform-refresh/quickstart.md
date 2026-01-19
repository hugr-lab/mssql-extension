# Quickstart: mssql_refresh_cache()

**Branch**: `012-docs-platform-refresh` | **Date**: 2026-01-19

## Overview

The `mssql_refresh_cache()` function allows you to manually refresh the metadata cache for an attached SQL Server database without detaching and reattaching.

## When to Use

Use this function when:
- Schema changes have been made on SQL Server (new tables, altered columns)
- You want to see updated metadata without reconnecting
- You're developing against a rapidly changing schema

## Basic Usage

```sql
-- 1. Attach SQL Server database
ATTACH 'Server=localhost,1433;Database=mydb;User Id=sa;Password=pass'
    AS sqlserver (TYPE mssql);

-- 2. Query tables (initial cache load)
SELECT table_name FROM duckdb_tables() WHERE database_name = 'sqlserver';

-- 3. After schema changes on SQL Server, refresh the cache
SELECT mssql_refresh_cache('sqlserver');
-- Returns: true

-- 4. Query again to see new tables
SELECT table_name FROM duckdb_tables() WHERE database_name = 'sqlserver';
```

## Function Reference

### mssql_refresh_cache()

Manually refreshes the metadata cache for an attached MSSQL catalog.

**Signature**: `mssql_refresh_cache(catalog_name VARCHAR) -> BOOLEAN`

**Parameters**:
| Parameter | Type | Description |
|-----------|------|-------------|
| catalog_name | VARCHAR | Name of the attached MSSQL catalog |

**Returns**: `true` on successful cache refresh

**Errors**:
| Condition | Error Type |
|-----------|------------|
| Empty or null argument | InvalidInputException |
| Catalog not found | BinderException |
| Catalog is not MSSQL type | BinderException |
| Connection failure | IOException |

## Examples

### Example 1: Basic Refresh

```sql
-- Attach and refresh
ATTACH '' AS ms (TYPE mssql, SECRET my_secret);
SELECT mssql_refresh_cache('ms');
```

### Example 2: After DDL on SQL Server

```sql
-- Someone created a new table on SQL Server
-- Refresh to see it in DuckDB
SELECT mssql_refresh_cache('production_db');

-- Now the new table appears
SELECT * FROM production_db.dbo.new_table LIMIT 5;
```

### Example 3: Error Handling

```sql
-- Non-existent catalog
SELECT mssql_refresh_cache('nonexistent');
-- Error: mssql_refresh_cache: catalog 'nonexistent' not found

-- Empty argument
SELECT mssql_refresh_cache('');
-- Error: mssql_refresh_cache: catalog name is required
```

## Behavior Notes

1. **Connection Pool**: Uses existing connection pool; respects `mssql_acquire_timeout` setting
2. **Cache TTL**: Manual refresh overrides TTL-based auto-refresh when `mssql_catalog_cache_ttl > 0`
3. **Thread Safety**: Safe to call concurrently; uses existing cache locking mechanisms
4. **Performance**: Network-bound; time depends on SQL Server response and schema size

## Related Settings

| Setting | Description |
|---------|-------------|
| `mssql_catalog_cache_ttl` | Auto-refresh TTL in seconds (0 = manual only) |
| `mssql_acquire_timeout` | Connection acquire timeout in seconds |

## See Also

- [Catalog Integration](../../README.md#catalog-integration)
- [Connection Configuration](../../README.md#connection-configuration)

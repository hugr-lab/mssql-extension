# API Contract: MSSQLMetadataCache

**Feature**: 029-incremental-catalog-cache
**Version**: 1.0.0
**Date**: 2026-02-05

## Overview

Extended `MSSQLMetadataCache` API supporting incremental lazy loading with per-level TTL and point invalidation.

## Public Methods

### Configuration

#### `SetTTL(int64_t seconds)`

Set TTL for all cache levels (from `mssql_catalog_cache_ttl` setting).

**Parameters**:
- `seconds`: TTL in seconds. 0 = no automatic expiration (manual/DDL invalidation only).

**Thread Safety**: Acquires `schemas_mutex_`.

---

#### `GetTTL() const -> int64_t`

Get current TTL.

**Returns**: TTL in seconds (0 = disabled).

---

### Schema Operations (Lazy)

#### `GetSchemaNames(tds::TdsConnection &conn) -> vector<string>`

Get all schema names, loading from SQL Server if needed.

**Parameters**:
- `conn`: Active TDS connection for metadata queries.

**Returns**: Vector of schema names.

**Behavior**:
1. If `schemas_load_state_ == NOT_LOADED` or expired: Load schema list
2. Return cached schema names

**Thread Safety**: Acquires `schemas_mutex_` for loading.

**SQL Executed**: `SELECT name FROM sys.schemas WHERE ...` (if loading)

---

#### `HasSchema(const string &name) const -> bool`

Check if schema exists in cache.

**Parameters**:
- `name`: Schema name (case-sensitive).

**Returns**: `true` if schema is in cache.

**Note**: Does NOT trigger lazy loading. Returns `false` if schemas not loaded yet.

**Thread Safety**: Acquires `schemas_mutex_` for read.

---

### Table Operations (Lazy)

#### `GetTableNames(tds::TdsConnection &conn, const string &schema) -> vector<string>`

Get table names in a schema, loading from SQL Server if needed.

**Parameters**:
- `conn`: Active TDS connection.
- `schema`: Schema name.

**Returns**: Vector of table names, or empty vector if schema not found.

**Behavior**:
1. Ensure schemas loaded (may trigger schema list load)
2. If schema's `tables_load_state == NOT_LOADED` or expired: Load table list
3. Return cached table names

**Thread Safety**: Acquires schema's `load_mutex` for loading.

**SQL Executed**: `SELECT name, type, rows FROM sys.objects ... WHERE SCHEMA_NAME(...) = '{schema}'` (if loading)

---

#### `GetTableMetadata(tds::TdsConnection &conn, const string &schema, const string &table) -> const MSSQLTableMetadata *`

Get full table metadata including columns, loading if needed.

**Parameters**:
- `conn`: Active TDS connection.
- `schema`: Schema name.
- `table`: Table name.

**Returns**: Pointer to cached metadata, or `nullptr` if not found.

**Behavior**:
1. Ensure tables loaded for schema
2. If table's `columns_load_state == NOT_LOADED` or expired: Load columns
3. Return pointer to cached metadata

**Thread Safety**: Acquires table's `load_mutex` for column loading.

**SQL Executed**: `SELECT ... FROM sys.columns ... WHERE object_id = OBJECT_ID(...)` (if loading columns)

---

#### `HasTable(const string &schema, const string &table) const -> bool`

Check if table exists in cache.

**Note**: Does NOT trigger lazy loading.

---

### Column Operations (Lazy)

#### `EnsureColumnsLoaded(tds::TdsConnection &conn, const string &schema, const string &table)`

Ensure column metadata is loaded for a specific table.

**Parameters**:
- `conn`: Active TDS connection.
- `schema`: Schema name.
- `table`: Table name.

**Throws**: `IOException` if table not found or query fails.

**Behavior**:
1. Lookup table in cache (must exist)
2. If `columns_load_state == NOT_LOADED` or expired: Load columns
3. Update `columns_last_refresh`

---

### Cache Management

#### `Refresh(tds::TdsConnection &conn, const string &collation)`

Force full cache refresh (backward compatible).

**Parameters**:
- `conn`: Active TDS connection.
- `collation`: Database default collation.

**Behavior**:
1. Clear all cached data
2. Load all schemas, tables, and columns eagerly
3. Update all timestamps

**Note**: This is the existing behavior, preserved for `mssql_refresh_cache()` function.

---

#### `InvalidateSchema(const string &schema)`

Invalidate a schema's table list (point invalidation).

**Parameters**:
- `schema`: Schema name to invalidate.

**Behavior**:
- Sets schema's `tables_load_state = NOT_LOADED`
- Does NOT clear existing table/column data (allows stale reads until refresh)
- Does NOT affect other schemas

**Use Case**: After CREATE TABLE, DROP TABLE in this schema.

---

#### `InvalidateTable(const string &schema, const string &table)`

Invalidate a table's column metadata (point invalidation).

**Parameters**:
- `schema`: Schema name.
- `table`: Table name.

**Behavior**:
- Sets table's `columns_load_state = NOT_LOADED`
- Does NOT clear existing column data
- Does NOT affect other tables

**Use Case**: After ALTER TABLE.

---

#### `InvalidateAll()`

Invalidate entire cache (full invalidation).

**Behavior**:
- Sets `schemas_load_state_ = NOT_LOADED`
- Sets all schemas' `tables_load_state = NOT_LOADED`
- Sets all tables' `columns_load_state = NOT_LOADED`

**Use Case**: After CREATE SCHEMA, DROP SCHEMA, or explicit `mssql_refresh_cache()`.

---

### State Queries

#### `GetSchemasState() const -> CacheLoadState`

Get loading state of schema list.

---

#### `GetTablesState(const string &schema) const -> CacheLoadState`

Get loading state of a schema's table list.

**Returns**: `NOT_LOADED` if schema doesn't exist.

---

#### `GetColumnsState(const string &schema, const string &table) const -> CacheLoadState`

Get loading state of a table's columns.

**Returns**: `NOT_LOADED` if schema/table doesn't exist.

---

## Error Handling

### Exceptions

| Exception | Condition |
|-----------|-----------|
| `IOException` | SQL Server metadata query fails |
| `IOException` | Connection lost during loading |

### Recovery

On loading error:
1. State remains/reverts to `NOT_LOADED`
2. Existing cached data (if any) preserved
3. Exception propagates to caller
4. Next access will retry loading

---

## Backward Compatibility

| Existing Method | Change |
|-----------------|--------|
| `GetSchemaNames()` | Now requires connection parameter |
| `GetTableNames(schema)` | Now requires connection parameter |
| `GetTableMetadata(schema, table)` | Now requires connection parameter |
| `Refresh(conn, collation)` | Unchanged (full refresh) |
| `NeedsRefresh()` | Checks catalog-level state only |
| `IsExpired()` | Checks catalog-level TTL only |
| `Invalidate()` | Maps to `InvalidateAll()` |

**Migration Note**: Callers must provide `TdsConnection` for lazy loading. The `MSSQLCatalog` class handles connection acquisition.

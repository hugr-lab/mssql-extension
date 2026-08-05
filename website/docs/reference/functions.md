---
title: Functions
sidebar_position: 2
---

# Function Reference

### mssql_version()

Returns the extension version string (e.g. `0.2.0`).

**Signature:** `mssql_version() -> VARCHAR`

```sql
SELECT mssql_version();
-- Returns: '0.2.0'
```

### mssql_scan()

Stream SELECT query results from SQL Server. Supports multi-statement batches where only one statement returns a result set.

**Signature:** `mssql_scan(context VARCHAR, query VARCHAR) -> TABLE(...)`

```sql
-- Simple query
SELECT * FROM mssql_scan('sqlserver', 'SELECT TOP 10 * FROM sys.tables');

-- Multi-statement batch with temp table
FROM mssql_scan('sqlserver', 'SELECT * INTO #t FROM dbo.src; SELECT * FROM #t');
```

The return schema is dynamic based on the query result columns. Multi-statement batches support intermediate DML/DDL statements that don't return results, but only one result-producing statement is allowed per call.

### mssql_exec()

Execute a SQL statement and return affected row count. Use this for SQL Server-specific DDL or statements that don't return results.

**Signature:** `mssql_exec(context VARCHAR, sql VARCHAR) -> BIGINT`

```sql
-- Execute DDL
SELECT mssql_exec('sqlserver', 'CREATE TABLE dbo.my_table (id INT PRIMARY KEY)');

-- Execute DML
SELECT mssql_exec('sqlserver', 'UPDATE dbo.users SET status = 1 WHERE id = 5');
-- Returns: number of affected rows
```

### mssql_open() — `[DEPRECATED]`

> **Deprecated** (spec 047 FR-010). Prefer ATTACH + the catalog-bound functions (`mssql_scan`, `mssql_exec`, `mssql_pool_stats`) which integrate with the catalog lifecycle and the per-catalog connection pool. The handle-manager singleton backing `mssql_open` / `mssql_close` / `mssql_ping` / `mssql_close_all` is the last extension-internal process-wide state and will be removed alongside these functions in a future major release. Use `mssql_close_all()` as the bulk shutdown hook.

Open a diagnostic connection to SQL Server.

**Signature:** `mssql_open(connection_string VARCHAR) -> BIGINT`

```sql
SELECT mssql_open('Server=localhost,1433;Database=master;User Id=sa;Password=...');
-- Returns: 12345 (connection handle)
```

### mssql_close() — `[DEPRECATED]`

> Same deprecation group as `mssql_open` (FR-010).

Close a diagnostic connection. Idempotent — closing an already-closed handle returns true.

**Signature:** `mssql_close(handle BIGINT) -> BOOLEAN`

```sql
SELECT mssql_close(12345);
-- Returns: true
```

### mssql_close_all() — `[DEPRECATED]`

> Same deprecation group as `mssql_open` (FR-010 / FR-013). Lives here as a deterministic shutdown hook so hosts using the diagnostic API can release every open handle in one call without tracking IDs individually.

Closes every diagnostic connection opened via `mssql_open()` in one shot. Returns the count of handles closed. Idempotent — a second call after a full close returns 0.

**Signature:** `mssql_close_all() -> INTEGER`

```sql
SELECT mssql_close_all();
-- Returns: 3 (count of handles closed on this call)

SELECT mssql_close_all();
-- Returns: 0 (idempotent)
```

### mssql_ping() — `[DEPRECATED]`

> Same deprecation group as `mssql_open` (FR-010).

Test if a connection is alive.

**Signature:** `mssql_ping(handle BIGINT) -> BOOLEAN`

```sql
SELECT mssql_ping(12345);
-- Returns: true (connection alive) or false (connection dead)
```

### mssql_pool_stats()

Get connection pool statistics.

**Signature:** `mssql_pool_stats(context? VARCHAR) -> TABLE(...)`

```sql
SELECT * FROM mssql_pool_stats('sqlserver');
```

**Return columns:**

| Column                  | Type   | Description                        |
| ----------------------- | ------ | ---------------------------------- |
| `db`                    | VARCHAR | Attached database context name     |
| `total_connections`     | BIGINT | Current pool size                  |
| `idle_connections`      | BIGINT | Available connections              |
| `active_connections`    | BIGINT | Currently in use                   |
| `connections_created`   | BIGINT | Lifetime connections created       |
| `connections_closed`    | BIGINT | Lifetime connections closed        |
| `acquire_count`         | BIGINT | Times connections acquired         |
| `acquire_timeout_count` | BIGINT | Times acquisition timed out        |
| `pinned_count`          | BIGINT | Connections pinned to transactions (per-pool atomic; spec 047 T005) |

### mssql_refresh_cache()

Manually refresh the metadata cache for an attached MSSQL catalog. This forces a reload of schema, table, and column information from SQL Server without requiring detach/reattach.

**Signature:** `mssql_refresh_cache(catalog_name VARCHAR) -> BOOLEAN`

```sql
-- Refresh metadata cache for attached catalog
SELECT mssql_refresh_cache('sqlserver');
-- Returns: true (cache successfully refreshed)
```

**Error conditions:**

- Empty or NULL catalog name throws an error
- Non-existent catalog throws an error
- Catalog that is not an MSSQL type throws an error

### mssql_invalidate_cache()

Lazily invalidate the metadata cache at a chosen granularity, without an eager reload (reload happens on next access). Unlike `mssql_refresh_cache()`, this is point-scoped, so it can drop a single table or schema while keeping the rest of a large preloaded cache intact.

**Signature:** `mssql_invalidate_cache(catalog_name VARCHAR [, schema VARCHAR [, table VARCHAR]]) -> BOOLEAN`

```sql
-- Whole catalog (lazy; equivalent to what mssql_exec() DDL triggers automatically)
SELECT mssql_invalidate_cache('sqlserver');

-- One schema
SELECT mssql_invalidate_cache('sqlserver', 'dbo');

-- One table — re-fetches this table's columns + re-checks its existence,
-- keeping every other table's cached column metadata
SELECT mssql_invalidate_cache('sqlserver', 'dbo', 'orders');
```

Use this after changing schema out of band (e.g. via `mssql_exec()` with `mssql_exec_invalidate_cache = false`, or from another client) instead of paying for a full `mssql_refresh_cache()` reload.

### mssql_preload_catalog()

Bulk-load all metadata (schemas, tables, columns) for an attached MSSQL catalog in a single operation. This is useful for large databases where you want to avoid per-table metadata queries during subsequent queries.

**Signature:** `mssql_preload_catalog(catalog_name VARCHAR [, schema_name VARCHAR]) -> VARCHAR`

```sql
-- Preload all schemas
SELECT mssql_preload_catalog('sqlserver');
-- Returns: 'Preloaded 5 schemas, 120 tables, 890 columns'

-- Preload a specific schema only
SELECT mssql_preload_catalog('sqlserver', 'dbo');
-- Returns: 'Preloaded schema 'dbo': 80 tables, 650 columns'
```

The function loads metadata per-schema to avoid SQL Server tempdb sort spills on large databases. Statistics (approximate row counts) are also pre-populated to avoid per-table DMV queries.


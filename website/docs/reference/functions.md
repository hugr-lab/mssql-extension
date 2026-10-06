---
title: Functions
sidebar_position: 2
---

# Function Reference

### mssql_version()

Returns the extension version string (e.g. `0.2.3`).

**Signature:** `mssql_version() -> VARCHAR`

```sql
SELECT mssql_version();
-- Returns: '0.2.3'
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

The shape is learned without running the statement: bind asks SQL Server to describe it (`sp_describe_first_result_set`), and the query runs when the scan starts. So a `DESCRIBE` or `EXPLAIN` of a batch that inserts before it selects inserts nothing, and a query with a side effect runs exactly once. A statement the server cannot describe — a batch that reads a `#temp` table it creates, a stored procedure — is run at bind instead, as every statement was before 0.3.0.

Inside a transaction the rows are read in full when the scan starts, so the transaction's single connection is free for the next scan or for a `COPY` / `INSERT` in the same statement.

**`prepared := true`** compiles the statement once (`sp_prepare`) and executes it by handle; the default path describes at bind and compiles again at execution, which SQL Server's plan cache makes cheap unless it will not keep the plan (`optimize for ad hoc workloads`, a large batch):

```sql
FROM mssql_scan('sqlserver', 'SELECT id, name FROM dbo.users WHERE id > 100', prepared := true);
```

### mssql_scan_params()

`mssql_scan` with parameters. The STRUCT's keys become `@name` variables, declared from the DuckDB types and passed through `sp_executesql` as an RPC call with typed values, so SQL Server keeps one compiled plan for the statement and reuses it for every call, from every session.

**Signature:** `mssql_scan_params(context VARCHAR, statement VARCHAR, params STRUCT [, declarations VARCHAR] [, prepared := false]) -> TABLE(...)`

```sql
FROM mssql_scan_params('sqlserver',
    'SELECT id, name FROM dbo.users WHERE id > @id AND created > @since',
    {'id': 42, 'since': TIMESTAMP '2024-01-02 00:00:00'});

-- Your own declarations when the derived ones are not the column's type
FROM mssql_scan_params('sqlserver',
    'SELECT * FROM dbo.events WHERE at = @ts AND code = @c',
    {'ts': TIMESTAMP '2024-01-02 03:04:05', 'c': 'AB'},
    '@ts datetime, @c varchar(8)');
```

Derived declarations: `BOOLEAN` → `bit`, `TINYINT`/`SMALLINT` → `smallint`, `UTINYINT` → `tinyint`, `INTEGER` → `int`, `BIGINT` → `bigint`, `HUGEINT` → `decimal(38,0)`, `FLOAT` → `real`, `DOUBLE` → `float`, `DECIMAL(p,s)` → `decimal(p,s)`, `DATE` → `date`, `TIME` → `time(6)`, `TIMESTAMP` → `datetime2(6)` (`_S`/`_MS`/`_NS` → `datetime2(0/3/7)`), `TIMESTAMP WITH TIME ZONE` → `datetimeoffset(6)`, `BLOB` → `varbinary(max)`, `UUID` → `uniqueidentifier`, `VARCHAR` → `nvarchar(4000)` (or `nvarchar(max)` past 4000 bytes), `MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)` → `varchar(n)` / `nvarchar(n)`. A bare `NULL` has no type — cast it (`NULL::INTEGER`); a LIST or STRUCT value cannot travel as a scalar parameter; a key must be a T-SQL identifier (letters, digits, `_`).

### mssql_scan_unsafe() / mssql_scan_params_unsafe()

`mssql_scan` / `mssql_scan_params` with the result's shape **given** instead of described. Bind returns the shape you pass and asks the server nothing — no connection, no `sp_describe_first_result_set` — so it binds inside a transaction, on a pool of one connection, and for a batch the server cannot describe (one that reads a `#temp` table it creates) exactly like any other statement. When the statement runs, its rows are checked against the shape; a stream that does not fit fails the statement, naming it. "Unsafe" means you vouch for the shape.

**Signatures:**
`mssql_scan_unsafe(context VARCHAR, query VARCHAR, columns := {'name': 'TYPE', ...}) -> TABLE(...)`
`mssql_scan_params_unsafe(context VARCHAR, statement VARCHAR, params STRUCT [, declarations VARCHAR], columns := {'name': 'TYPE', ...}) -> TABLE(...)`

```sql
FROM mssql_scan_unsafe('sqlserver', 'SELECT id, name FROM dbo.users',
    columns := {'id': 'INTEGER', 'name': 'VARCHAR'});

FROM mssql_scan_params_unsafe('sqlserver',
    'SELECT COUNT_BIG(*) AS n FROM dbo.users WHERE created > @since',
    {'since': TIMESTAMP '2024-01-02 00:00:00'},
    columns := {'n': 'BIGINT'});
```

`columns` is a STRUCT, as `read_csv` takes it: the field names are the result's column names, in order; each value is a type name. Declare each column as the catalog reads its SQL Server type. The check is strict on the kind of each column and nothing is converted; a string column's length and collation are not checked (any string type reads any string):

| SQL Server | declare |
|---|---|
| `bit` | `BOOLEAN` |
| `tinyint` / `smallint` / `int` / `bigint` | `UTINYINT` / `SMALLINT` / `INTEGER` / `BIGINT` (`COUNT(*)` is `int`, `COUNT_BIG(*)` is `bigint`) |
| `real` / `float` | `FLOAT` / `DOUBLE` |
| `decimal(p,s)` / `numeric(p,s)` | `DECIMAL(p,s)`, the same precision and scale |
| `money` / `smallmoney` | `DECIMAL(19,4)` / `DECIMAL(10,4)` |
| `char` / `varchar` / `nchar` / `nvarchar` / `text` / `ntext` / `xml` | `VARCHAR`, or `MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)` |
| `binary` / `varbinary` / `image` / `rowversion` | `BLOB` |
| `date` / `time` | `DATE` / `TIME` |
| `datetime` / `smalldatetime` | `TIMESTAMP` |
| `datetime2(n)` | any TIMESTAMP variant; the one of its scale keeps full precision (`TIMESTAMP_NS` for `datetime2(7)`) |
| `datetimeoffset` | `TIMESTAMP WITH TIME ZONE` |
| `uniqueidentifier` | `UUID` |
| `geometry` | `GEOMETRY`, sent as WKB: `SELECT shape.STAsBinary() AS shape …` |
| `sql_variant`, `hierarchyid`, other UDTs | not readable as such: cast them in the T-SQL (`CAST(v AS nvarchar(4000))`, `h.ToString()`) |

A type no SQL Server column is read as (`TINYINT`, a LIST, a STRUCT) is refused at bind. Casting each column in the T-SQL to the type you declare keeps the shape from drifting when a table changes. `EXPLAIN` shows `Shape: given (columns :=)`. The remote-pushdown rewriter uses these two for every pushed statement whose column types it knows.

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

### mssql_exec_params()

`mssql_exec` with parameters — the same contract as `mssql_scan_params`. One round trip per row; the statement compiles once on the server and the plan serves every row and every session.

**Signature:** `mssql_exec_params(context VARCHAR, statement VARCHAR, params STRUCT [, declarations VARCHAR]) -> BIGINT`

```sql
-- One statement, many rows, one plan
SELECT mssql_exec_params('sqlserver',
    'UPDATE dbo.users SET status = @s WHERE id = @id',
    {'s': new_status, 'id': user_id})
FROM pending_changes;
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
| `creation_failures`     | BIGINT | Times the pool tried to open a connection and could not (issue #302) |
| `last_create_error`     | VARCHAR | What the last failed attempt said; cleared by a creation success, never by a reuse |
| `last_create_error_age_ms` | BIGINT | How long ago that error was recorded; NULL when there is none. A pool at its limit recovers by reuse, so the error can outlive the failure — the age is how you tell |

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

### Authentication Test Functions

Connectivity diagnostics that exercise the auth path without a full query.
Details on the auth pages: [Azure AD](../connection/azure.md), [Kerberos / SSPI](../connection/kerberos.md).

| Function | Description |
|---|---|
| `mssql_azure_auth_test(secret [, tenant])` | Test Azure AD token acquisition |
| `mssql_kerberos_auth_test(host [, port])` | POSIX Kerberos: returns OK + SPN / principal / token size, or the verbatim GSSAPI error |
| `mssql_kerberos_auth_test_secret(secret_name)` | Same, reading keytab / SPN override from an MSSQL secret |
| `mssql_winsspi_auth_test(host [, port])` | Windows SSPI peer; `mssql_winsspi_auth_test_spn(spn)` takes an explicit SPN |

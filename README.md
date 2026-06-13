<!-- METRICS-BADGES:START -->

[![CI](https://github.com/hugr-lab/mssql-extension/actions/workflows/ci.yml/badge.svg)](https://github.com/hugr-lab/mssql-extension/actions/workflows/ci.yml)
[![DuckDB](https://img.shields.io/static/v1?label=duckdb&message=v1.4.1%2B&color=blue)](https://github.com/duckdb/duckdb/releases)
[![Latest release](https://img.shields.io/github/v/release/hugr-lab/mssql-extension?label=release&color=blue)](https://github.com/hugr-lab/mssql-extension/releases/latest)
[![Community downloads per week](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fcommunity-extensions.duckdb.org%2Fdownloads-last-week.json&query=%24.mssql&label=downloads%2Fweek&color=brightgreen)](https://duckdb.org/community_extensions/download_metrics)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![GitHub stars](https://img.shields.io/github/stars/hugr-lab/mssql-extension?style=flat&color=informational)](https://github.com/hugr-lab/mssql-extension/stargazers)

<!-- METRICS-BADGES:END -->

# DuckDB MSSQL Extension

A DuckDB extension for connecting to Microsoft SQL Server databases using native TDS protocol - no ODBC, JDBC, or external drivers required.

<!-- METRICS-CHART:START -->

### 📈 Community Extension Downloads

![Weekly downloads of the mssql DuckDB community extension](docs/assets/download-metrics.svg)

📊 **[Interactive chart](https://hugr-lab.github.io/mssql-extension/)** — queried live in your browser with DuckDB-Wasm.

> Latest published version **v0.2.1** · **4,441** downloads in the trailing 7 days (snapshot 2026-06-09 UTC). Counts are a Cloudflare estimate of `INSTALL mssql FROM community` events, aggregated across DuckDB versions and platforms. Source: [DuckDB Community Extensions download metrics](https://duckdb.org/community_extensions/download_metrics).

<!-- METRICS-CHART:END -->

> **Experimental**: This extension is under active development. APIs and behavior may change between releases. We welcome contributions, bug reports, and testing feedback!

## Features

- Native TDS protocol implementation (no external dependencies)
- Stream query results directly into DuckDB without buffering
- Full DuckDB catalog integration with three-part naming and lazy metadata loading
- Row identity (`rowid`) support for tables with primary keys
- Connection pooling with configurable limits and automatic session reset
- TLS/SSL encrypted connections
- Full DML support: INSERT (with RETURNING), UPDATE, DELETE
- CREATE TABLE AS SELECT (CTAS) with streaming and type mapping
- High-performance COPY TO via TDS BulkLoadBCP protocol
- Transaction support: BEGIN/COMMIT/ROLLBACK with connection pinning
- Multi-statement SQL batches via `mssql_scan()` (e.g., temp table workflows)
- DuckDB secret management for secure credential storage
- [Azure AD authentication](AZURE.md) (service principal, CLI, interactive device code flow)
- [Kerberos / Windows SSPI integrated authentication](Kerberos.md) — POSIX (`kinit` / keytab / raw credentials) and Windows (current logon session via `secur32.dll`)
- Custom `Application Name` propagated to SQL Server `APP_NAME()` / `sys.dm_exec_sessions.program_name`
- ATTACH-time credential validation (opt-out via `lazy_validation true`)
- **Experimental**: ORDER BY pushdown to SQL Server (opt-in via `mssql_order_pushdown` setting)

## Support MSSQL Extension

MSSQL Extension is an open-source DuckDB extension maintained in spare time.

If it helps you or your organization, consider sponsoring its development through GitHub Sponsors.

Your support helps fund maintenance, bug fixes, testing, and new features.

## Quick Start

### Prerequisites

- DuckDB v1.4.1 or later (minimum supported version)
- SQL Server 2019 or later accessible on network

### Step 1: Install Extension

```sql
INSTALL mssql FROM community;
LOAD mssql;
```

### Step 2: Connect to SQL Server

#### Option A: Using a Secret (Recommended)

```sql
CREATE SECRET my_sqlserver (
    TYPE mssql,
    host 'localhost',
    port 1433,
    database 'master',
    user 'sa',
    password 'YourPassword123'
);

ATTACH '' AS sqlserver (TYPE mssql, SECRET my_sqlserver);
```

#### Option B: Using Connection String

```sql
ATTACH 'Server=localhost,1433;Database=master;User Id=sa;Password=YourPassword123'
    AS sqlserver (TYPE mssql);
```

### Step 3: Query Data

```sql
-- List schemas
SELECT schema_name FROM duckdb_schemas() WHERE database_name = 'sqlserver';

-- List tables in dbo schema
SELECT table_name FROM duckdb_tables() WHERE database_name = 'sqlserver' AND schema_name = 'dbo';

-- Query a table
FROM sqlserver.dbo.my_table LIMIT 10;
```

### Step 4: Disconnect

```sql
DETACH sqlserver;
DROP SECRET my_sqlserver;
```

## Connection Configuration

### Using Secrets

Create a secret to store connection credentials securely:

```sql
CREATE SECRET secret_name (
    TYPE mssql,
    host 'hostname',
    port 1433,
    database 'database_name',
    user 'username',
    password 'password',
    use_encrypt true  -- TLS enabled by default
);
```

#### Secret Fields

| Field         | Type    | Required | Description                          |
| ------------- | ------- | -------- | ------------------------------------ |
| `host`        | VARCHAR | Yes      | SQL Server hostname or IP address    |
| `port`        | INTEGER | Yes      | TCP port (1-65535, default: 1433)    |
| `database`    | VARCHAR | Yes      | Database name                        |
| `user`        | VARCHAR | Yes\*    | SQL Server username (\*not required for `authenticator='krb5'` ccache mode or Azure AD) |
| `password`    | VARCHAR | Yes\*    | Password (hidden in `duckdb_secrets()`; required only for SQL auth + Kerberos raw mode) |
| `use_encrypt` | BOOLEAN | No       | Enable TLS encryption (default: true) |
| `catalog`     | BOOLEAN | No       | Enable catalog integration (default: true). Set to false for serverless/restricted databases that don't support catalog queries |
| `schema_filter` | VARCHAR | No     | Regex pattern to filter visible schemas (case-insensitive partial match) |
| `table_filter`  | VARCHAR | No     | Regex pattern to filter visible tables/views (case-insensitive partial match) |
| `azure_secret`  | VARCHAR | No     | Name of an Azure secret (DuckDB Azure extension) for Azure AD auth — see [AZURE.md](AZURE.md) |
| `access_token`  | VARCHAR | No     | Pre-acquired Azure AD JWT (hidden in `duckdb_secrets()`) — see [AZURE.md](AZURE.md) |
| `authenticator` | VARCHAR | No     | `krb5` (POSIX) or `winsspi` (Windows; pending) — Kerberos / SSPI integrated auth, see [Kerberos.md](Kerberos.md) |
| `krb5_configfile`    | VARCHAR | No | Per-secret `/etc/krb5.conf` override (Linux only) |
| `krb5_keytabfile`    | VARCHAR | No | Path to a keytab — selects keytab credential mode (Linux only) |
| `krb5_credcachefile` | VARCHAR | No | ccache path override (Linux only) |
| `krb5_realm`         | VARCHAR | No | AD realm (UPPERCASE) — required for keytab and raw modes |
| `service_principal_name` | VARCHAR | No | SPN override, e.g. `MSSQLSvc/sqlhost.example.com:1433` |
| `application_name`   | VARCHAR | No | LOGIN7 `program_name` propagated to SQL Server (visible via `APP_NAME()` / `sys.dm_exec_sessions.program_name`). Empty → `"DuckDB MSSQL Extension"` default. Clamped client-side to 128 UTF-16 code units. Fallback secret key: `applicationname`. |

Attach using the secret:

```sql
ATTACH '' AS context_name (TYPE mssql, SECRET secret_name);
```

### Using Connection Strings

#### ADO.NET Format

```sql
ATTACH 'Server=host,port;Database=db;User Id=user;Password=pass;Encrypt=yes'
    AS context_name (TYPE mssql);
```

#### Key Aliases (case-insensitive)

| Key                         | Aliases                              |
| --------------------------- | ------------------------------------ |
| `Server`                    | `Data Source`                        |
| `Database`                  | `Initial Catalog`                    |
| `User Id`                   | `Uid`, `User`                        |
| `Password`                  | `Pwd`                                |
| `Encrypt`                   | `Use Encryption for Data`, `TrustServerCertificate` |
| `Trusted_Connection`        | `Trusted Connection`, `TrustedConnection` (yes/true/SSPI/1 -> Kerberos on POSIX, SSPI on Windows; see [Kerberos.md](Kerberos.md)) |
| `Integrated Security`       | `IntegratedSecurity`, `Integrated_Security` (same resolution as `Trusted_Connection`) |
| `authenticator`             | `krb5` or `winsspi` (see [Kerberos.md](Kerberos.md)) |
| `krb5-keytabfile`           | `krb5_keytabfile` (path to keytab; selects keytab mode, Linux only) |
| `krb5-configfile`           | `krb5_configfile` (per-connection `/etc/krb5.conf` override, Linux only) |
| `krb5-credcachefile`        | `krb5_credcachefile` (ccache path override, Linux only) |
| `krb5-realm`                | `krb5_realm` (AD realm, UPPERCASE) |
| `service_principal_name`    | `service-principal-name`, `serviceprincipalname` (SPN override) |
| `Application Name`          | `ApplicationName`, `App Name`, `application_name` (LOGIN7 `program_name`; visible as `APP_NAME()`. URI query form: `applicationname`. Empty → `"DuckDB MSSQL Extension"`. Clamped to 128 UTF-16 code units.) |

#### URI Format

```sql
ATTACH 'mssql://user:password@host:port/database?encrypt=true'
    AS context_name (TYPE mssql);
```

URI format supports URL-encoded components for special characters in credentials.

### Integrated Authentication (Kerberos / SSPI)

POSIX users with an Active-Directory-joined SQL Server can authenticate via
Kerberos after running `kinit`. The simplest form (pyodbc-compatible alias):

```sql
ATTACH 'Server=sqlhost.corp.example.com;Database=YourDB;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes'
    AS db (TYPE mssql);
```

Or the explicit `microsoft/go-mssqldb` form:

```sql
ATTACH 'Server=sqlhost.corp.example.com;Database=YourDB;authenticator=krb5;Encrypt=yes'
    AS db (TYPE mssql);
```

Three credential modes are supported on POSIX:

- **Credential cache** (default) — uses a `kinit` ticket. Works on Linux and macOS.
- **Keytab** — `krb5-keytabfile=/path` + `User Id=svc@REALM`. Linux only.
- **Raw credentials** — username + password + realm via `CREATE SECRET` only (not connection string, to keep cleartext out of logs). Linux only.

On Windows, **SSPI** (`authenticator=winsspi` or `Trusted_Connection=yes`) authenticates with the current Windows logon session via `secur32.dll`'s Negotiate package — no `kinit` needed. The connection-string surface is identical to POSIX; `Trusted_Connection=yes` / `Integrated Security=SSPI` resolve to `winsspi` automatically on Windows hosts.

See [Kerberos.md](Kerberos.md) for prerequisites, full connection-string
reference, the bundled docker-compose test stack (no real AD required),
troubleshooting (including WSL2 specifics), and SPN verification.

### TLS/SSL Configuration

To enable encrypted connections:

#### Using Secret

```sql
CREATE SECRET secure_conn (
    TYPE mssql,
    host 'sql-server.example.com',
    port 1433,
    database 'MyDatabase',
    user 'sa',
    password 'Password123',
    use_encrypt true
);
```

#### Using Connection String

```sql
ATTACH 'Server=sql-server.example.com,1433;Database=MyDatabase;User Id=sa;Password=Password123;Encrypt=yes'
    AS db (TYPE mssql);
```

#### Using URI

```sql
ATTACH 'mssql://sa:Password123@sql-server.example.com:1433/MyDatabase?encrypt=true'
    AS db (TYPE mssql);
```

> **Note**: TLS is enabled by default for security. Use `use_encrypt=false` or `Encrypt=no` to disable. TLS support is available in both static and loadable extension builds (using OpenSSL).

#### TrustServerCertificate Parameter

For compatibility with ADO.NET connection strings, `TrustServerCertificate` is supported as an alias for `Encrypt`:

```sql
-- Using TrustServerCertificate (equivalent to Encrypt=yes)
ATTACH 'Server=localhost,1433;Database=master;User Id=sa;Password=pass;TrustServerCertificate=true'
    AS db (TYPE mssql);
```

> **Note**: If both `Encrypt` and `TrustServerCertificate` are specified with conflicting values (e.g., `Encrypt=true;TrustServerCertificate=false`), ATTACH will fail with an error. Either omit one parameter or ensure they have the same value.

### Catalog-Free Mode

For serverless databases (like Azure SQL Serverless) or databases with restricted permissions where catalog queries fail, disable catalog integration:

#### Using Secret

```sql
CREATE SECRET serverless_db (
    TYPE mssql,
    host 'myserver.database.windows.net',
    port 1433,
    database 'mydb',
    user 'sa',
    password 'Password123',
    catalog false  -- Disable catalog integration
);

ATTACH '' AS serverless (TYPE mssql, SECRET serverless_db);
```

#### Using Connection String

```sql
ATTACH 'Server=myserver.database.windows.net,1433;Database=mydb;User Id=sa;Password=Password123;Catalog=false'
    AS serverless (TYPE mssql);
```

With catalog disabled:
- `mssql_scan()` and `mssql_exec()` work normally for raw SQL queries
- Schema browsing via `duckdb_schemas()`, `duckdb_tables()` is not available
- Three-part naming (`db.schema.table`) is not available
- Use `mssql_scan()` for all queries instead

### Catalog Filters

For large databases with thousands of schemas or tables, you can filter which objects are visible to DuckDB using regex patterns. This significantly reduces metadata loading time and memory usage.

#### Using Secret

```sql
CREATE SECRET erp_db (
    TYPE mssql,
    host 'erp-server.example.com',
    port 1433,
    database 'ERP',
    user 'readonly',
    password 'Password123',
    schema_filter '^(dbo|sales|inventory)$',  -- Only these schemas
    table_filter '^(Order|Product|Customer)'   -- Tables starting with these prefixes
);

ATTACH '' AS erp (TYPE mssql, SECRET erp_db);
```

#### Using Connection String

```sql
ATTACH 'Server=erp-server,1433;Database=ERP;User Id=sa;Password=pass;SchemaFilter=^dbo$;TableFilter=^Order'
    AS erp (TYPE mssql);
```

#### Filter Behavior

- Filters use case-insensitive regex partial match (C++ `std::regex_search`)
- Use `^` and `$` anchors for exact matching: `^dbo$` matches only "dbo"
- Without anchors, `dbo` matches "dbo", "dbo_archive", "test_dbo", etc.
- Filters apply to catalog browsing, schema scans, and metadata loading
- `mssql_scan()` and `mssql_exec()` bypass filters (raw SQL access)

### Connection Validation

The extension validates connections at ATTACH time, providing immediate feedback on configuration errors:

```sql
-- Invalid hostname - fails immediately with clear error
ATTACH 'Server=nonexistent.host,1433;Database=master;User Id=sa;Password=pass'
    AS db (TYPE mssql);
-- Error: MSSQL connection validation failed: Cannot resolve hostname 'nonexistent.host'

-- Invalid credentials - fails immediately
ATTACH 'Server=localhost,1433;Database=master;User Id=wrong;Password=wrong'
    AS db (TYPE mssql);
-- Error: MSSQL connection validation failed: Authentication failed for user 'wrong'
```

This fail-fast behavior ensures that:

1. **No orphaned catalogs**: Failed ATTACH operations do not create catalog entries
2. **Clear error messages**: Connection errors are reported immediately with specific details
3. **Faster debugging**: Invalid configurations are caught at ATTACH time, not during first query
4. **Password never leaks**: error messages never include the password (audited)

**Opt out per-ATTACH** for container/orchestrator startup where the SQL Server may not yet be reachable:

```sql
ATTACH 'Server=...' AS db (TYPE mssql, lazy_validation true);
```

With `lazy_validation true`, ATTACH succeeds without the TCP+LOGIN7 round trip; the first query then pays the connection-establishment cost (pre-spec-047 behaviour). The eager-validation ceiling is bounded by `mssql_attach_validation_timeout` (default `0` inherits `mssql_connection_timeout`).

### ATTACH Options Reference

In addition to options propagated from the secret / connection string, the following ATTACH options are accepted directly:

| Option              | Type    | Description                                                                  |
| ------------------- | ------- | ---------------------------------------------------------------------------- |
| `SECRET`            | VARCHAR | Name of an MSSQL secret holding connection parameters                        |
| `azure_secret`      | VARCHAR | Override / supply Azure secret name for Azure AD auth                        |
| `access_token`      | VARCHAR | Pre-acquired Azure AD JWT (see [AZURE.md](AZURE.md))                         |
| `catalog`           | BOOLEAN | Enable catalog integration (default `true`)                                  |
| `schema_filter`     | VARCHAR | Override secret schema_filter for this ATTACH                                |
| `table_filter`      | VARCHAR | Override secret table_filter for this ATTACH                                 |
| `order_pushdown`    | BOOLEAN | Per-ATTACH ORDER BY pushdown override (overrides `mssql_order_pushdown` setting) |
| `lazy_validation`   | BOOLEAN | Skip the eager ATTACH-time credential check (default `false`)                |
| `application_name`  | VARCHAR | Override LOGIN7 `program_name` for this ATTACH (also accepts `applicationname`) |

## Catalog Integration

### Attaching and Detaching

```sql
-- Attach with secret
ATTACH '' AS sqlserver (TYPE mssql, SECRET my_secret);

-- Attach with connection string
ATTACH 'Server=localhost,1433;Database=master;User Id=sa;Password=pass'
    AS sqlserver (TYPE mssql);

-- Detach when done
DETACH sqlserver;
```

### Schema Browsing

```sql
-- List all schemas
SELECT schema_name FROM duckdb_schemas() WHERE database_name = 'sqlserver';

-- List tables in a schema
SELECT table_name FROM duckdb_tables() WHERE database_name = 'sqlserver' AND schema_name = 'dbo';

-- Describe table structure (list columns)
SELECT column_name, data_type, is_nullable
FROM duckdb_columns()
WHERE database_name = 'sqlserver' AND schema_name = 'dbo' AND table_name = 'my_table';
```

### Three-Part Naming

Access SQL Server tables using `context.schema.table` naming:

```sql
SELECT id, name, created_at
FROM sqlserver.dbo.customers
WHERE status = 'active'
LIMIT 100;
```

### Cross-Catalog Joins

Join SQL Server tables with local DuckDB tables:

```sql
-- Create local table
CREATE TABLE local_data (customer_id INTEGER, extra_info VARCHAR);

-- Join with SQL Server
SELECT c.id, c.name, l.extra_info
FROM sqlserver.dbo.customers c
JOIN local_data l ON c.id = l.customer_id;
```

## Query Execution

### Streaming SELECT

Results are streamed directly into DuckDB without buffering the entire result set:

```sql
SELECT * FROM sqlserver.dbo.large_table;
```

### Filter and Projection Pushdown

The extension pushes filters and column selections to SQL Server:

```sql
-- Only 'id' and 'name' columns are fetched, filter applied server-side
SELECT id, name FROM sqlserver.dbo.customers WHERE status = 'active';
```

Supported filter operations for pushdown:

- Equality: `column = value`
- Comparisons: `>`, `<`, `>=`, `<=`, `<>`
- IN clause: `column IN (val1, val2, ...)`
- NULL checks: `IS NULL`, `IS NOT NULL`
- Conjunctions: `AND`, `OR`
- Date/timestamp comparisons: `date_col >= '2024-01-01'`
- Boolean comparisons: `is_active = true` (converted to `= 1`)
- Datetime functions: `year(date_col) = 2024`, `month(date_col) = 6`, `day(date_col) = 15`

**Not pushed down** (applied locally by DuckDB):

- LIKE patterns with leading wildcards: `LIKE '%pattern'`, `LIKE '%pattern%'`
- ILIKE (case-insensitive LIKE)
- Most function expressions: `lower(name) = 'test'`, `length(col) > 5`
- DuckDB-specific functions: `list_contains()`, `regexp_matches()`

Note: `LIKE 'prefix%'` patterns are optimized by DuckDB into range comparisons which ARE pushed down.

### ORDER BY Pushdown (Experimental)

When enabled, ORDER BY clauses on simple column references and supported functions are pushed to SQL Server, avoiding a local sort in DuckDB. Combined ORDER BY + LIMIT is pushed as `SELECT TOP N ... ORDER BY ...`.

This feature is **disabled by default** and must be explicitly enabled:

```sql
-- Enable globally
SET mssql_order_pushdown = true;

-- Or per-database via ATTACH option
ATTACH 'Server=...' AS db (TYPE mssql, order_pushdown true);
```

**Setting precedence:** The global setting is checked first; if `true`, pushdown is enabled. The ATTACH option is checked second; `true` enables pushdown, `false` is a no-op (does not override global `true`).

**Supported expressions:**
- Simple column references: `ORDER BY name ASC`, `ORDER BY id DESC`
- Single-argument functions: `ORDER BY year(date_col)`
- Multi-column: `ORDER BY category ASC, name DESC`
- Combined with LIMIT: `ORDER BY id ASC LIMIT 10` → `SELECT TOP 10 ... ORDER BY [id] ASC`

**Limitations:**
- NULL ordering must match SQL Server defaults (ASC = NULLS FIRST, DESC = NULLS LAST); mismatched null ordering falls back to DuckDB
- Only prefix pushdown: stops at first non-pushable column
- Expressions like `ORDER BY col * 2` are not pushed

### Row Identity (rowid)

Tables with primary keys expose a virtual `rowid` column that provides stable row identification:

```sql
-- Query rowid alongside other columns
SELECT rowid, name, value FROM sqlserver.dbo.products LIMIT 5;
```

**rowid Type Mapping:**

| Primary Key Type | rowid Type | Example |
|------------------|------------|---------|
| Single column (INT) | `INTEGER` | `42` |
| Single column (BIGINT) | `BIGINT` | `9223372036854775807` |
| Single column (VARCHAR) | `VARCHAR` | `'ABC-001'` |
| Single column (UNIQUEIDENTIFIER) | `UUID` | `a1b2c3d4-e5f6-...` |
| Composite (multiple columns) | `STRUCT` | `{'region_id': 1, 'product_id': 100}` |

**Usage Examples:**

```sql
-- Scalar primary key (INT)
SELECT rowid, name FROM sqlserver.dbo.customers;
-- rowid: 1, 2, 3, ...

-- Composite primary key (VARCHAR + INT)
SELECT rowid, quantity FROM sqlserver.dbo.order_items;
-- rowid: {'tenant_code': 'ACME', 'item_id': 1}, ...

-- Filter using rowid (composite key)
SELECT * FROM sqlserver.dbo.order_items
WHERE rowid = {'tenant_code': 'ACME', 'item_id': 1};
```

**Limitations:**

- Tables without primary keys do not expose `rowid`
- Views do not support `rowid`
- `rowid` is read-only (cannot be used in INSERT/UPDATE)

## Transactions

The extension supports DuckDB transactions mapped to SQL Server transactions with connection pinning.

### Basic Transaction Usage

```sql
BEGIN;
INSERT INTO sqlserver.dbo.orders (customer_id, amount) VALUES (1, 99.99);
UPDATE sqlserver.dbo.customers SET order_count = order_count + 1 WHERE id = 1;
COMMIT;
```

All statements within a transaction execute on the same SQL Server connection. If any statement fails, use `ROLLBACK` to undo changes.

### Transaction Behavior

- **Autocommit (default)**: Each statement is independent with its own implicit transaction
- **Explicit transactions**: `BEGIN` pins a connection; all subsequent operations reuse it until `COMMIT` or `ROLLBACK`
- **Isolation level**: SQL Server default (READ COMMITTED). Use `mssql_exec()` to change if needed
- **Connection reset**: After commit/rollback, the connection's session state is reset via TDS RESET_CONNECTION flag before pool reuse

### Multi-Statement SQL Batches

`mssql_scan()` supports multi-statement batches where intermediate statements don't return result sets:

```sql
-- Temp table workflow: create, populate, query
FROM mssql_scan('sqlserver', '
    SELECT * INTO #temp FROM dbo.large_table WHERE region = ''US'';
    SELECT * FROM #temp ORDER BY created_at
');
```

**Constraint**: Only one statement in the batch may produce a result set. Batches with multiple SELECTs will return a clear error message.

## CREATE TABLE AS SELECT (CTAS)

Create SQL Server tables directly from DuckDB query results.

### Basic CTAS

```sql
-- Create table from DuckDB query
CREATE TABLE sqlserver.dbo.summary AS
SELECT region, COUNT(*) AS order_count, SUM(amount) AS total
FROM sqlserver.dbo.orders
GROUP BY region;

-- Create from local DuckDB table
CREATE TABLE sqlserver.dbo.imported_data AS
SELECT * FROM read_csv('data.csv');

-- Create from generate_series
CREATE TABLE sqlserver.dbo.sequence AS
SELECT i AS id, 'item_' || i::VARCHAR AS name
FROM generate_series(1, 1000) t(i);
```

### CREATE OR REPLACE

Replace an existing table with new data:

```sql
-- Overwrites existing table (non-atomic: DROP then CREATE)
CREATE OR REPLACE TABLE sqlserver.dbo.daily_report AS
SELECT * FROM sqlserver.dbo.transactions WHERE date = CURRENT_DATE;
```

### Type Mapping

DuckDB types are automatically mapped to SQL Server types:

| DuckDB Type | SQL Server Type |
|-------------|-----------------|
| `BOOLEAN` | `BIT` |
| `TINYINT` | `TINYINT` |
| `SMALLINT` | `SMALLINT` |
| `INTEGER` | `INT` |
| `BIGINT` | `BIGINT` |
| `FLOAT` | `REAL` |
| `DOUBLE` | `FLOAT` |
| `DECIMAL(p,s)` | `DECIMAL(p,s)` (max 38) |
| `VARCHAR` | `NVARCHAR(MAX)` |
| `BLOB` | `VARBINARY(MAX)` |
| `DATE` | `DATE` |
| `TIME` | `TIME(7)` |
| `TIMESTAMP` | `DATETIME2(7)` |
| `TIMESTAMP WITH TIME ZONE` | `DATETIMEOFFSET(7)` |
| `UUID` | `UNIQUEIDENTIFIER` |

**Unsupported types** (will error with clear message):
- `HUGEINT`, `UHUGEINT` - Consider casting to `DECIMAL(38,0)`
- `INTERVAL` - No SQL Server equivalent
- `LIST`, `STRUCT`, `MAP`, `ARRAY` - No SQL Server equivalent

### CTAS Settings

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `mssql_ctas_use_bcp` | BOOLEAN | `true` | Use BCP protocol for data transfer (2-10x faster than INSERT) |
| `mssql_ctas_text_type` | VARCHAR | `NVARCHAR` | Text column type: `NVARCHAR` or `VARCHAR` |
| `mssql_ctas_drop_on_failure` | BOOLEAN | `false` | Drop table if data transfer phase fails |

```sql
-- Disable BCP for legacy INSERT mode (slower, but compatible)
SET mssql_ctas_use_bcp = false;

-- Use VARCHAR instead of NVARCHAR for text columns
SET mssql_ctas_text_type = 'VARCHAR';

-- Auto-cleanup on failure (for production pipelines)
SET mssql_ctas_drop_on_failure = true;
```

### CTAS Behavior

- **BCP mode (default)**: Uses TDS BulkLoadBCP protocol for 2-10x faster data transfer
- **Two-phase execution**: CREATE TABLE DDL, then data transfer via BCP or INSERT
- **Streaming**: Large result sets are streamed without full buffering
- **Non-atomic**: DDL commits immediately; data transfer respects transactions
- **Schema validation**: Target schema must exist before CTAS
- **Legacy INSERT mode**: Set `mssql_ctas_use_bcp = false` to use batched INSERT statements

## COPY TO (Bulk Load)

High-performance bulk data transfer using the native TDS BulkLoadBCP protocol. Significantly faster than INSERT for large datasets.

### Target Formats

The COPY TO command supports two target formats:

| Format | Syntax | Example |
|--------|--------|---------|
| **URL** | `mssql://catalog/schema/table` | `mssql://sqlserver/dbo/my_table` |
| **Catalog** | `catalog.schema.table` | `sqlserver.dbo.my_table` |

Both formats are equivalent and can be used interchangeably.

#### Empty Schema Syntax for Temp Tables

Temp tables can use an empty schema notation for clarity:

| Format | Standard Syntax | Empty Schema Syntax |
|--------|-----------------|---------------------|
| **URL** | `mssql://catalog/#temp` | `mssql://catalog//#temp` |
| **Catalog** | `catalog.#temp` | `catalog..#temp` |

Both syntaxes are equivalent for temp tables. The empty schema syntax (`//` or `..`) explicitly shows there's no schema component.

### Basic COPY TO

```sql
-- Copy DuckDB table to SQL Server (URL format)
COPY my_local_table TO 'mssql://sqlserver/dbo/target_table' (FORMAT 'bcp');

-- Copy DuckDB table to SQL Server (catalog format)
COPY my_local_table TO 'sqlserver.dbo.target_table' (FORMAT 'bcp');

-- Copy query results to SQL Server
COPY (SELECT * FROM source WHERE year = 2024) TO 'mssql://sqlserver/dbo/target_table' (FORMAT 'bcp');

-- Generate data and copy to SQL Server
COPY (SELECT i AS id, 'row_' || i AS name FROM range(1000000) t(i))
  TO 'sqlserver.dbo.million_rows' (FORMAT 'bcp');
```

### COPY TO Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `CREATE_TABLE` | BOOLEAN | true | Auto-create target table if it doesn't exist |
| `REPLACE` | BOOLEAN | false | Drop and recreate table (replaces existing data) |
| `FLUSH_ROWS` | BIGINT | 100000 | Rows before flushing to SQL Server (overrides setting) |
| `TABLOCK` | BOOLEAN | false | Use TABLOCK hint for faster bulk load (overrides setting) |

```sql
-- Auto-create table (default: true)
COPY data TO 'mssql://sqlserver/dbo/new_table' (FORMAT 'bcp', CREATE_TABLE true);

-- Replace existing table (drop and recreate)
COPY data TO 'mssql://sqlserver/dbo/existing_table' (FORMAT 'bcp', REPLACE true);

-- Control flush frequency (rows before committing to SQL Server)
COPY data TO 'sqlserver.dbo.table' (FORMAT 'bcp', FLUSH_ROWS 500000);

-- Disable TABLOCK hint (allows concurrent access, slower)
COPY data TO 'sqlserver.dbo.table' (FORMAT 'bcp', TABLOCK false);
```

### Temporary Tables

Temp tables are prefixed with `#` (local) or `##` (global). They require a transaction context to remain accessible.

```sql
-- Local temp table using URL format (session-scoped, requires transaction)
BEGIN;
COPY data TO 'mssql://sqlserver/#temp_table' (FORMAT 'bcp');
SELECT * FROM mssql_scan('sqlserver', 'SELECT * FROM #temp_table');
COMMIT;

-- Local temp table using catalog format
BEGIN;
COPY data TO 'sqlserver.#temp_table' (FORMAT 'bcp');
SELECT * FROM mssql_scan('sqlserver', 'SELECT * FROM #temp_table');
COMMIT;

-- Empty schema syntax (equivalent alternatives)
BEGIN;
COPY data TO 'mssql://sqlserver//#temp_table' (FORMAT 'bcp');  -- URL with empty schema
COPY data TO 'sqlserver..#temp_table' (FORMAT 'bcp');          -- Catalog with empty schema
COMMIT;

-- Global temp table (visible to all sessions)
COPY data TO 'mssql://sqlserver/##global_temp' (FORMAT 'bcp');
```

> **Note**: Temp tables have no schema component. Use `catalog.#table`, `catalog..#table`, `mssql://catalog/#table`, or `mssql://catalog//#table` format.

### COPY TO Settings

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `mssql_copy_flush_rows` | BIGINT | 100000 | Rows before flushing to SQL Server (0 = flush at end only) |
| `mssql_copy_tablock` | BOOLEAN | false | Use TABLOCK hint for 15-30% better performance (blocks concurrent access) |

### Performance Characteristics

- **Protocol**: Uses TDS BulkLoadBCP (packet type 0x07) for maximum throughput
- **Streaming**: Bounded memory usage regardless of dataset size
- **Throughput**: ~300K rows/s for simple rows, ~10K rows/s for wide rows (500+ chars × 10 columns)
- **TABLOCK**: Enables table-level locking and minimal logging for faster inserts

### COPY TO Behavior

- **Auto-create**: Tables are created automatically with inferred schema (can be disabled)
- **Type mapping**: DuckDB types mapped to SQL Server equivalents (VARCHAR→NVARCHAR, etc.)
- **No RETURNING**: Use INSERT for cases requiring returned values
- **Transaction support**: Works within transactions; temp tables require transaction context

### Column Mapping (Existing Tables)

When copying to an existing table with `CREATE_TABLE false`, columns are matched **by name** (case-insensitive), not by position:

```sql
-- Target table has columns: id INT, name VARCHAR(50), value FLOAT

-- Source can have different column order
CREATE TABLE source AS SELECT 1.5::DOUBLE AS value, 1 AS id;

-- Copies successfully: id→id, value→value, name→NULL
COPY source TO 'mssql://db/dbo/target' (FORMAT 'bcp', CREATE_TABLE false);
```

**Column Mapping Rules:**

| Scenario | Behavior |
|----------|----------|
| Same columns, same order | Direct mapping (backward compatible) |
| Same columns, different order | Mapped by name |
| Source has fewer columns | Missing target columns receive NULL |
| Source has extra columns | Extra columns are ignored |
| No matching columns | Error: "No matching columns" |
| Case mismatch (id vs ID) | Matched case-insensitively |

> **Note**: Target columns that don't have matching source columns must allow NULL values.

## Data Modification (INSERT)

### Basic INSERT

```sql
-- Single row
INSERT INTO sqlserver.dbo.my_table (name, value)
VALUES ('test', 42);

-- Multiple rows
INSERT INTO sqlserver.dbo.my_table (name, value)
VALUES ('first', 1), ('second', 2), ('third', 3);
```

### INSERT from SELECT

```sql
INSERT INTO sqlserver.dbo.target_table (name, value)
SELECT name, value FROM local_source_table;
```

### INSERT with RETURNING

Get inserted values back (uses SQL Server's OUTPUT INSERTED):

```sql
INSERT INTO sqlserver.dbo.my_table (name)
VALUES ('test')
RETURNING id, name;
```

```sql
INSERT INTO sqlserver.dbo.my_table (name, value)
VALUES ('a', 1), ('b', 2)
RETURNING *;
```

### Batch Configuration

Large inserts are automatically batched. Configure batch size:

```sql
-- Set batch size (default: 1000, SQL Server limit)
SET mssql_insert_batch_size = 500;

-- Maximum SQL statement size (default: 8MB)
SET mssql_insert_max_sql_bytes = 4194304;
```

### Identity Columns

Identity (auto-increment) columns are automatically excluded from INSERT statements. The generated values are returned via RETURNING clause.

## Data Modification (UPDATE)

UPDATE operations are supported for tables with primary keys. The extension uses rowid-based targeting for efficient updates.

### Basic UPDATE

```sql
-- Update single row
UPDATE sqlserver.dbo.products SET price = 19.99 WHERE id = 1;

-- Update multiple rows
UPDATE sqlserver.dbo.products SET status = 'discontinued' WHERE category = 'legacy';

-- Update with expressions
UPDATE sqlserver.dbo.products SET price = price * 1.10 WHERE category = 'premium';
```

### UPDATE with Multiple Columns

```sql
UPDATE sqlserver.dbo.customers
SET name = 'John Doe', email = 'john@example.com', updated_at = NOW()
WHERE id = 42;
```

### Batch Configuration

Large updates are automatically batched:

```sql
-- Set batch size (default: 1000)
SET mssql_update_batch_size = 500;
```

### Limitations

- **RETURNING clause is not supported** for UPDATE operations
- Tables must have a primary key (uses rowid for row identification)
- Updates are executed as batched DELETE + INSERT internally for composite PKs

## Data Modification (DELETE)

DELETE operations are supported for tables with primary keys.

### Basic DELETE

```sql
-- Delete single row
DELETE FROM sqlserver.dbo.products WHERE id = 1;

-- Delete multiple rows
DELETE FROM sqlserver.dbo.products WHERE status = 'discontinued';

-- Delete all rows (use with caution)
DELETE FROM sqlserver.dbo.products;
```

### DELETE with Complex Conditions

```sql
DELETE FROM sqlserver.dbo.order_items
WHERE order_id IN (SELECT id FROM sqlserver.dbo.orders WHERE status = 'cancelled');
```

### Batch Configuration

Large deletes are automatically batched:

```sql
-- Set batch size (default: 1000)
SET mssql_delete_batch_size = 500;
```

### Limitations

- **RETURNING clause is not supported** for DELETE operations
- Tables must have a primary key (uses rowid for row identification)

## DDL Operations

The extension supports standard DuckDB DDL syntax for common operations, which are translated to T-SQL and executed on SQL Server. For advanced operations (indexes, constraints), use `mssql_exec()`.

### Create Table

```sql
-- Standard DuckDB syntax - automatically translated to T-SQL
CREATE TABLE sqlserver.dbo.users (
    id INTEGER,
    username VARCHAR,
    email VARCHAR,
    created_at TIMESTAMP
);
```

DuckDB types are mapped to SQL Server types (INTEGER → INT, VARCHAR → NVARCHAR(MAX), TIMESTAMP → DATETIME2).

For SQL Server-specific features (IDENTITY, constraints, defaults), use `mssql_exec()`:

```sql
SELECT mssql_exec('sqlserver', '
    CREATE TABLE dbo.products (
        id INT IDENTITY(1,1) PRIMARY KEY,
        name NVARCHAR(100) NOT NULL,
        price DECIMAL(10,2) DEFAULT 0.00
    )
');
```

### Drop Table

```sql
-- Standard DuckDB syntax
DROP TABLE sqlserver.dbo.users;

-- With IF EXISTS (via mssql_exec)
SELECT mssql_exec('sqlserver', 'DROP TABLE IF EXISTS dbo.old_table');
```

### Alter Table

```sql
-- Add a column
ALTER TABLE sqlserver.dbo.users ADD COLUMN status VARCHAR;

-- Drop a column
ALTER TABLE sqlserver.dbo.users DROP COLUMN status;

-- Rename a column
ALTER TABLE sqlserver.dbo.users RENAME COLUMN email TO email_address;
```

For constraints, use `mssql_exec()`:

```sql
SELECT mssql_exec('sqlserver', 'ALTER TABLE dbo.users ADD CONSTRAINT UQ_email UNIQUE (email)');
```

### Rename Table

```sql
ALTER TABLE sqlserver.dbo.old_name RENAME TO new_name;
```

### Create and Drop Schema

```sql
-- Create schema
CREATE SCHEMA sqlserver.sales;

-- Drop schema (must be empty)
DROP SCHEMA sqlserver.sales;
```

### Indexes (via mssql_exec)

Index operations are not supported via DuckDB DDL syntax. Use `mssql_exec()`:

```sql
-- Create index
SELECT mssql_exec('sqlserver', 'CREATE INDEX IX_users_email ON dbo.users (email)');

-- Create unique index
SELECT mssql_exec('sqlserver', 'CREATE UNIQUE INDEX IX_users_username ON dbo.users (username)');

-- Drop index
SELECT mssql_exec('sqlserver', 'DROP INDEX IX_users_email ON dbo.users');
```

> **Note**: Schema-changing statements run through `mssql_exec()` (`CREATE`/`DROP`/`ALTER`/`TRUNCATE`/`RENAME`/`EXEC`) invalidate the metadata cache automatically, the same as standard DuckDB DDL. To preserve a large manually-preloaded cache, set `mssql_exec_invalidate_cache = false` and invalidate yourself with `mssql_invalidate_cache('sqlserver' [, schema [, table]])` (lazy, point-scoped) or `mssql_refresh_cache('sqlserver')` (eager full reload). You also need a manual invalidate to pick up changes made entirely out of band (e.g. by another client) while `mssql_catalog_cache_ttl` is `0`. See [Cache & invalidation](DATAMODEL.md#cache-invalidation) in `DATAMODEL.md` for how the two-layer cache works.

## Function Reference

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

## Type Mapping

### Numeric Types

| SQL Server Type   | DuckDB Type    | Notes                        |
| ----------------- | -------------- | ---------------------------- |
| `TINYINT`         | `UTINYINT`     | Unsigned 0-255               |
| `SMALLINT`        | `SMALLINT`     | -32768 to 32767              |
| `INT`             | `INTEGER`      | Standard 32-bit integer      |
| `BIGINT`          | `BIGINT`       | 64-bit integer               |
| `BIT`             | `BOOLEAN`      | 0 or 1                       |
| `REAL`            | `FLOAT`        | 32-bit floating point        |
| `FLOAT`           | `DOUBLE`       | 64-bit floating point        |
| `DECIMAL(p,s)`    | `DECIMAL(p,s)` | Preserves precision/scale    |
| `NUMERIC(p,s)`    | `DECIMAL(p,s)` | Preserves precision/scale    |
| `MONEY`           | `DECIMAL(19,4)`| Fixed precision              |
| `SMALLMONEY`      | `DECIMAL(10,4)`| Fixed precision              |

### String Types

| SQL Server Type   | DuckDB Type    | Notes                        |
| ----------------- | -------------- | ---------------------------- |
| `CHAR(n)`         | `VARCHAR`      | Fixed-length, trailing spaces trimmed |
| `VARCHAR(n)`      | `VARCHAR`      | Variable-length              |
| `NCHAR(n)`        | `VARCHAR`      | UTF-16LE decoded             |
| `NVARCHAR(n)`     | `VARCHAR`      | UTF-16LE decoded             |

### Binary Types

| SQL Server Type   | DuckDB Type    | Notes                        |
| ----------------- | -------------- | ---------------------------- |
| `BINARY(n)`       | `BLOB`         | Fixed-length binary          |
| `VARBINARY(n)`    | `BLOB`         | Variable-length binary       |

### Date/Time Types

| SQL Server Type     | DuckDB Type     | Notes                        |
| ------------------- | --------------- | ---------------------------- |
| `DATE`              | `DATE`          | Date only                    |
| `TIME`              | `TIME`          | Up to 100ns precision        |
| `DATETIME`          | `TIMESTAMP`     | 3.33ms precision             |
| `SMALLDATETIME`     | `TIMESTAMP`     | 1 minute precision           |
| `DATETIME2`         | `TIMESTAMP`     | Up to 100ns precision        |
| `DATETIMEOFFSET`    | `TIMESTAMP_TZ`  | Timezone-aware               |

### Special Types

| SQL Server Type     | DuckDB Type    | Notes                        |
| ------------------- | -------------- | ---------------------------- |
| `UNIQUEIDENTIFIER`  | `UUID`         | 128-bit GUID                 |
| `XML`               | `VARCHAR`      | PLP encoding, UTF-16LE decoded to UTF-8, up to 2 GB |

**XML type notes:**
- **SELECT**: XML columns are read via the same PLP + UTF-16LE code path as NVARCHAR(MAX)
- **COPY TO (BCP)**: Supported — XML is remapped to NVARCHAR(MAX) on the wire (SQL Server auto-converts)
- **CTAS**: Supported via BCP protocol
- **INSERT/UPDATE via SQL literals**: Supported for small values (up to 4096 bytes). Larger XML values error with a recommendation to use COPY TO with BCP protocol

### Unsupported Types

The following SQL Server types are not currently supported:

- `UDT` (User-Defined Types)
- `SQL_VARIANT`
- `IMAGE` (deprecated)
- `TEXT` (deprecated)
- `NTEXT` (deprecated)

Queries involving unsupported types will raise an error.

## Configuration Reference

### Connection Pool Settings

| Setting                    | Type    | Default | Range | Description                              |
| -------------------------- | ------- | ------- | ----- | ---------------------------------------- |
| `mssql_connection_limit`   | BIGINT  | 64      | ≥1    | Max connections per attached database    |
| `mssql_connection_cache`   | BOOLEAN | true    | -     | Enable connection pooling and reuse      |
| `mssql_connection_timeout` | BIGINT  | 30      | ≥0    | TCP connection timeout (seconds)         |
| `mssql_idle_timeout`       | BIGINT  | 300     | ≥0    | Idle connection timeout (seconds, 0=none)|
| `mssql_min_connections`    | BIGINT  | 0       | ≥0    | Minimum connections to maintain          |
| `mssql_acquire_timeout`    | BIGINT  | 30      | ≥0    | Connection acquire timeout (seconds)     |
| `mssql_query_timeout`      | BIGINT  | 30      | ≥0    | Query execution timeout (seconds, 0=infinite) |
| `mssql_metadata_timeout`   | BIGINT  | 300     | ≥0    | Metadata query timeout (seconds, 0=no timeout) |
| `mssql_catalog_cache_ttl`  | BIGINT  | 0       | ≥0    | Metadata cache TTL (seconds, 0=manual)   |
| `mssql_exec_invalidate_cache` | BOOLEAN | true | true/false | Auto-invalidate the catalog cache after DDL run via `mssql_exec()`. Set `false` to keep a large preloaded cache and invalidate manually with `mssql_invalidate_cache()`. |
| `mssql_attach_validation_timeout` | BIGINT | 0 | ≥0 | ATTACH-time eager-validation timeout (seconds). `0` inherits `mssql_connection_timeout`. Spec 047 FR-011. |

### Statistics Settings

| Setting                            | Type    | Default | Range | Description                           |
| ---------------------------------- | ------- | ------- | ----- | ------------------------------------- |
| `mssql_enable_statistics`          | BOOLEAN | true    | -     | Enable statistics collection          |
| `mssql_statistics_level`           | BIGINT  | 0       | ≥0    | Detail: 0=rowcount, 1=+histogram, 2=+NDV |
| `mssql_statistics_use_dbcc`        | BOOLEAN | false   | -     | Use DBCC SHOW_STATISTICS (requires permissions) |
| `mssql_statistics_cache_ttl_seconds` | BIGINT | 300    | ≥0    | Statistics cache TTL (seconds)        |

### ORDER BY Pushdown Settings (Experimental)

| Setting                            | Type    | Default | Range | Description                           |
| ---------------------------------- | ------- | ------- | ----- | ------------------------------------- |
| `mssql_order_pushdown`             | BOOLEAN | false   | -     | Enable ORDER BY pushdown to SQL Server |

The `order_pushdown` ATTACH option provides per-database control. See [ORDER BY Pushdown](#order-by-pushdown-experimental) for details.

### INSERT Settings

| Setting                            | Type    | Default  | Range  | Description                           |
| ---------------------------------- | ------- | -------- | ------ | ------------------------------------- |
| `mssql_insert_batch_size`          | BIGINT  | 1000     | ≥1     | Rows per INSERT (SQL Server limit: 1000) |
| `mssql_insert_max_rows_per_statement` | BIGINT | 1000   | ≥1     | Hard cap on rows per INSERT           |
| `mssql_insert_max_sql_bytes`       | BIGINT  | 8388608  | ≥1024  | Max SQL statement size (8MB)          |
| `mssql_insert_use_returning_output`| BOOLEAN | true     | -      | Use OUTPUT INSERTED for RETURNING     |

### UPDATE/DELETE Settings

| Setting                            | Type    | Default  | Range  | Description                           |
| ---------------------------------- | ------- | -------- | ------ | ------------------------------------- |
| `mssql_dml_batch_size`             | BIGINT  | 500      | ≥1     | Rows per UPDATE/DELETE batch          |
| `mssql_dml_max_parameters`         | BIGINT  | 2000     | ≥1     | Max parameters per statement (~2100 limit) |
| `mssql_dml_use_prepared`           | BOOLEAN | true     | -      | Use prepared statements for DML       |

### Usage Examples

```sql
-- Increase connection pool for high-concurrency workloads
SET mssql_connection_limit = 20;

-- Reduce batch size for tables with large rows
SET mssql_insert_batch_size = 100;

-- Enable detailed statistics for query optimization
SET mssql_statistics_level = 2;

-- Disable connection caching for debugging
SET mssql_connection_cache = false;
```

## Performance Tuning

### Bulk Data Loading

For loading large datasets into SQL Server, use COPY TO with BCP protocol:

```sql
-- Fastest: TABLOCK + large flush threshold
SET mssql_copy_tablock = true;  -- 15-30% faster, but blocks concurrent access
SET mssql_copy_flush_rows = 500000;  -- Fewer flushes = better throughput

COPY large_dataset TO 'mssql://db/dbo/target' (FORMAT 'bcp');
```

| Scenario | Recommended Settings | Notes |
|----------|---------------------|-------|
| Single-user batch load | `TABLOCK=true`, `FLUSH_ROWS=500000` | Maximum throughput |
| Multi-user environment | `TABLOCK=false` (default) | Allows concurrent access |
| Memory-constrained | `FLUSH_ROWS=50000` | Lower memory on both sides |
| Maximum reliability | `FLUSH_ROWS=100000` (default) | Balanced throughput/memory |

### Connection Pool Tuning

```sql
-- High-concurrency workloads
SET mssql_connection_limit = 100; -- More connections (default: 64)
SET mssql_min_connections = 5;    -- Pre-warm more connections (default: 0)

-- Long-running analytics
SET mssql_query_timeout = 0;      -- No timeout (default: 30s)
SET mssql_idle_timeout = 600;     -- Keep connections longer (default: 300s)

-- Debugging connection issues
SET mssql_connection_cache = false;  -- Disable pooling for isolation
```

### INSERT vs COPY Performance

| Method | Rows/sec | Best For |
|--------|----------|----------|
| Single INSERT | ~1K | Small single-row operations |
| Batched INSERT | ~50K | INSERT with RETURNING clause |
| COPY TO (BCP) | ~300K | Bulk loading without RETURNING |
| COPY TO + TABLOCK | ~400K | Single-user bulk loading |

```sql
-- For bulk loads without RETURNING, always prefer COPY
-- Instead of:
INSERT INTO db.dbo.target SELECT * FROM large_source;

-- Use:
COPY (SELECT * FROM large_source) TO 'db.dbo.target' (FORMAT 'bcp');
```

### Query Optimization

```sql
-- Enable filter pushdown verification
SET mssql_enable_statistics = true;  -- Default

-- For complex queries, use mssql_scan with explicit SQL
-- DuckDB will still optimize joins with local tables
FROM mssql_scan('db', 'SELECT id, name FROM dbo.large_table WHERE region = ''US''')
JOIN local_lookup USING (id);
```

### Memory Management

| Setting | Impact | Recommendation |
|---------|--------|----------------|
| `mssql_copy_flush_rows` | SQL Server buffer memory | Increase for throughput, decrease for memory |
| `mssql_insert_batch_size` | DuckDB batch memory | Keep at 1000 (SQL Server limit) |
| `mssql_dml_batch_size` | UPDATE/DELETE memory | Decrease for wide tables |

## Contributing

We welcome contributions! Whether it's bug reports, feature requests, documentation improvements, or code contributions - your help makes this extension better for everyone.

- **Report bugs**: Open an issue with reproduction steps
- **Request features**: Describe your use case and proposed solution
- **Submit PRs**: Fork, branch, and submit a pull request
- **Test on your platform**: Help us validate on different environments

## Development

For building from source, testing, and contributing, see the [Development Guide](DEVELOPMENT.md).

Quick start:

```bash
git clone --recurse-submodules <repository-url>
cd mssql-extension
make        # Build release
make test   # Run tests
```

## Building with DuckDB Extension CI Tools

This extension is compatible with DuckDB Community Extensions CI.

### Setup

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/hugr-lab/mssql-extension.git
cd mssql-extension

# Or initialize submodules after clone
git submodule update --init --recursive
```

### CI Build (Community Extensions compatible)

```bash
# Set DuckDB version (required by Community CI)
DUCKDB_GIT_VERSION=v1.4.3 make set_duckdb_version

# Build release
make release

# Run tests
make test
```

### Local Development Build

```bash
# Bootstrap vcpkg (required for TLS/OpenSSL support)
make vcpkg-setup

# Build
make release   # or: make debug

# Load extension in DuckDB
./build/release/duckdb
> LOAD mssql;
```

### Running Integration Tests

```bash
# Start SQL Server container
make docker-up

# Run integration tests
make integration-test

# Stop container when done
make docker-down
```

### Available Build Targets

Run `make help` to see all available targets:

| Target               | Description                                          |
| -------------------- | ---------------------------------------------------- |
| `release`            | Build release version                                |
| `debug`              | Build debug version                                  |
| `test`               | Run unit tests                                       |
| `set_duckdb_version` | Set DuckDB version (use `DUCKDB_GIT_VERSION=v1.x.x`) |
| `vcpkg-setup`        | Bootstrap vcpkg (required for TLS support)           |
| `integration-test`   | Run integration tests (requires SQL Server)          |
| `test-all`           | Run all tests                                        |
| `docker-up`          | Start SQL Server test container                      |
| `docker-down`        | Stop SQL Server test container                       |
| `docker-status`      | Check SQL Server container status                    |

## Troubleshooting

### Connection Refused

```text
Error: Failed to connect to SQL Server: Connection refused
```

**Solutions:**

- Verify SQL Server hostname and port are correct
- Check firewall allows TCP connections on port 1433
- Ensure SQL Server is configured for TCP/IP connections (SQL Server Configuration Manager)
- Test connectivity: `telnet hostname 1433`

### Login Failed

```text
Error: Login failed for user 'username'
```

**Solutions:**

- Verify username and password are correct
- Ensure SQL Server authentication mode is enabled (not Windows-only)
- Check user has access to the specified database
- Verify user account is not locked or disabled

### TLS Required

```text
Error: Server requires encryption but TLS is not available
```

**Solutions:**

- Enable encryption in connection: `use_encrypt true` or `Encrypt=yes`
- Ensure extension was built with OpenSSL (default for vcpkg builds)

### TLS Handshake Failed

```text
Error: TLS handshake failed
```

**Solutions:**

- Verify server certificate is valid
- Check TLS version compatibility (TLS 1.2+ required)
- Set `MSSQL_DEBUG=1` for detailed TLS debugging output
- Verify server hostname matches certificate

### Type Conversion Error

```text
Error: Unsupported SQL Server type 'UDT' (0xF0) for column 'col_name'
```

**Solutions:**

- Check the [Type Mapping](#type-mapping) section for supported types
- Cast unsupported columns to supported types in your query
- Exclude unsupported columns from SELECT

### Slow Query Performance

**Solutions:**

- Verify filter pushdown is working (check query plan)
- Reduce result set size with LIMIT or WHERE clauses
- Increase connection pool size for concurrent queries
- Check network latency to SQL Server
- Consider using `mssql_scan()` for complex queries with explicit SQL

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| macOS ARM64 | Primary development | Active development and testing |
| Linux x86_64 | CI-validated | Automated builds and tests in CI |
| Linux ARM64 | CI-validated | Automated builds and tests in CI |
| Windows x64 | CI-validated | Automated builds and tests in CI |

## Roadmap

The following features are planned for future releases:

| Feature | Description | Status |
|---------|-------------|--------|
| **Row Identity** | `rowid` pseudo-column mapping to primary keys | ✅ Implemented |
| **UPDATE/DELETE** | DML support with PK-based row identification, batched execution | ✅ Implemented |
| **Transactions** | BEGIN/COMMIT/ROLLBACK with connection pinning | ✅ Implemented |
| **Multi-Statement Batches** | Temp table workflows via `mssql_scan()` with session reset | ✅ Implemented |
| **CTAS** | CREATE TABLE AS SELECT with two-phase execution (DDL + INSERT) | ✅ Implemented |
| **BCP/COPY** | High-throughput bulk insert via TDS BCP protocol (10M+ rows) | ✅ Implemented |
| **Integrated Auth** | Kerberos on POSIX (kinit / keytab / raw); SSPI on Windows | ✅ Implemented |
| **Custom Application Name** | LOGIN7 `program_name` from connection string / URI / secret / ATTACH option | ✅ Implemented |
| **ATTACH credential validation** | Fail-fast on wrong password / unreachable host (`lazy_validation true` to opt out) | ✅ Implemented |
| **BCP throughput** | LOGIN7 32 KB packet size, single-connection encoder/sender pipelining, column-batch encoding, parallel multi-connection BCP for heap targets | Planned |
| **CTAS quality** | Bounded `NVARCHAR(N)` text defaults, `PAGE` compression, primary-key propagation, per-query overrides via COPY options | Planned |
| **TRUNCATE optimization** | Auto-detect `DELETE FROM t` without `WHERE` and emit `TRUNCATE TABLE` when safe (no triggers/FK/CCI) | Planned |
| **COPY TO TRUNCATE mode** | Atomic replace via TRUNCATE + BCP (distinct from current `OVERWRITE` = DROP+CREATE) | Planned |

### Under consideration

Items being designed; surface and implementation strategy not committed yet.

| Topic | What we're thinking about |
|-------|----------------------------|
| **Direct DML pushdown** | Skip the `rowid` round trip on UPDATE/DELETE when the entire `WHERE` filter is pushable — emit a single `UPDATE target SET ... WHERE <pushdown>` / `DELETE FROM target WHERE <pushdown>` statement instead. |
| **MERGE INTO** | Native SQL Server `MERGE` push-down. Pipelined BCP upload of the USING source to a session `#tmp`, then emit `MERGE INTO target USING #tmp WITH (HOLDLOCK)`. RETURNING via `OUTPUT $action`. |
| **VARIANT fallback for unsupported types** | UDT / `SQL_VARIANT` / legacy `IMAGE` / `TEXT` / `NTEXT` columns mapped to DuckDB `VARIANT` instead of raising. Opt-in via a setting. |

### Feature Details

**Row Identity**: Tables with primary keys expose a virtual `rowid` column. Scalar PKs map to their native type; composite PKs map to DuckDB STRUCT. This enables UPDATE/DELETE support.

**UPDATE/DELETE**: Supports `UPDATE ... SET ... WHERE` and `DELETE FROM ... WHERE` through DuckDB catalog integration. Uses `rowid` for row identification. Batched execution for large operations. Note: RETURNING clause is not supported for UPDATE/DELETE (only for INSERT).

**Transactions**: DML transactions with connection pinning. Each explicit transaction pins a single TDS connection for the transaction's duration, using SQL Server's 8-byte transaction descriptor in ALL_HEADERS. Connections are flagged for session reset (RESET_CONNECTION) on pool return.

**Multi-Statement Batches**: `mssql_scan()` supports batches where intermediate statements (DML/DDL) don't return result sets. Only one result-producing statement per batch is allowed. Session state (temp tables, variables) is reset via TDS RESET_CONNECTION flag when connections return to the pool.

**CTAS**: `CREATE TABLE mssql.schema.table AS SELECT ...` with two-phase execution: CREATE TABLE DDL followed by batched INSERT. Supports CREATE OR REPLACE, configurable text type (NVARCHAR/VARCHAR), and streaming for large result sets. Type mapping from DuckDB to SQL Server with clear errors for unsupported types.

**BCP/COPY**: Binary bulk copy protocol for maximum throughput. Streaming execution with bounded memory. No RETURNING support (use regular INSERT for that).

## Limitations

### Unsupported Features

- **RETURNING for UPDATE/DELETE**: Only INSERT supports RETURNING clause; UPDATE/DELETE do not
- **UPDATE/DELETE without PK**: Tables must have primary keys for UPDATE/DELETE operations
- **Updating primary key columns**: UPDATE cannot modify primary key columns (used for row identification)
- **Keytab / raw Kerberos credentials on macOS**: macOS's `GSS.framework` lacks MIT extensions for `gss_acquire_cred_from` keytab and raw-password paths. macOS supports credential-cache mode only (via `kinit`); keytab and raw-credentials modes are Linux-only. See [Kerberos.md](Kerberos.md) for the WSL2 / Docker testing path.
- **Multiple result sets**: Only one result-producing statement per `mssql_scan()` batch is allowed
- **Stored Procedures with Output Parameters**: Use `mssql_scan()` for stored procedures
- **rowid for views/tables without PK**: Only tables with primary keys expose `rowid`

### VARCHAR Encoding

VARCHAR and CHAR columns with non-UTF8 collations (e.g., `Latin1_General_CI_AS`) are automatically converted to NVARCHAR in generated queries to ensure proper UTF-8 decoding in DuckDB. This allows extended ASCII characters (é, ñ, ü, etc.) to be correctly returned.

**Behavior:**

| VARCHAR Length | Converted To | Notes |
|----------------|--------------|-------|
| VARCHAR(n) where n ≤ 4000 | NVARCHAR(n) | Full data preserved |
| VARCHAR(n) where n > 4000 | NVARCHAR(4000) | Data may be truncated to 4000 characters |
| VARCHAR(MAX) | NVARCHAR(MAX) | Converted by default; disable with `mssql_convert_varchar_max = false` |

**VARCHAR Encoding Setting:**

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `mssql_convert_varchar_max` | BOOLEAN | true | Convert VARCHAR(MAX) to NVARCHAR(MAX) in catalog queries |

When `mssql_convert_varchar_max = true` (default), VARCHAR(MAX) columns with non-UTF8 collations are converted to NVARCHAR(MAX), enabling proper UTF-8 handling for extended ASCII characters. This may halve the effective buffer capacity (from ~4096 to ~2048 characters) due to NVARCHAR's 2-byte encoding.

To preserve the full buffer capacity at the cost of potential encoding errors with extended ASCII:

```sql
SET mssql_convert_varchar_max = false;
```

**Notes:**

1. **Catalog queries only**: This conversion applies only to catalog-based queries (three-part naming like `db.schema.table`). When using `mssql_scan()` with raw SQL, you must manually add CAST expressions:

```sql
-- Without CAST: may fail with UTF-8 validation error for extended ASCII
FROM mssql_scan('db', 'SELECT name FROM dbo.customers');

-- With CAST: properly handles extended ASCII characters
FROM mssql_scan('db', 'SELECT CAST(name AS NVARCHAR(100)) AS name FROM dbo.customers');
```

2. **VARCHAR(MAX) buffer limits**: SQL Server has TDS buffer limits (~4096 bytes) for MAX types. When converted to NVARCHAR(MAX), the effective character count is halved since NVARCHAR uses 2 bytes per character. Use `SET mssql_convert_varchar_max = false` if you need maximum buffer capacity with ASCII-only data.

### Unicode Transcoding (simdutf)

DuckDB strings are UTF-8 internally; the TDS wire protocol carries character data as UTF-16LE. Every NVARCHAR / NCHAR / NTEXT / XML value the extension reads from SQL Server is decoded from UTF-16LE to UTF-8 before being handed to DuckDB, and every string the extension sends back (LOGIN7 fields, INSERT/UPDATE parameters, T-SQL identifiers in BCP metadata, etc.) is encoded the other direction. On a SELECT that returns millions of rows, that's a lot of bytes — and the codec sits squarely in the hot path.

The extension uses [**simdutf**](https://github.com/simdutf/simdutf) for that conversion. simdutf is a SIMD-accelerated Unicode validation and transcoding library by Daniel Lemire and contributors; on modern x86_64 (AVX-512 / AVX2 / SSE) and ARM64 (NEON / SVE) cores it typically transcodes UTF-8 ↔ UTF-16 at 2–5 GB/s, an order of magnitude faster than the hand-rolled byte-at-a-time loops it replaced. The library is dual-licensed MIT / Apache-2.0; the extension links it statically under the MIT terms.

**Where simdutf is called** (`src/tds/encoding/utf16.cpp`):

| Direction | simdutf entry points | Used by |
|-----------|----------------------|---------|
| UTF-8 → UTF-16LE (encode) | `simdutf::validate_utf8`, `simdutf::utf16_length_from_utf8`, `simdutf::convert_valid_utf8_to_utf16le` | LOGIN7 client/host/app/server/library/language/database/SSPI fields; T-SQL batches built by INSERT/UPDATE/DELETE; BCP column metadata; identifier quoting |
| UTF-16LE → UTF-8 (decode) | `simdutf::validate_utf16le`, `simdutf::utf8_length_from_utf16le`, `simdutf::convert_valid_utf16le_to_utf8` | NVARCHAR / NCHAR / NTEXT / SQL_VARIANT-string / XML column results; ENVCHANGE token payloads; ERROR/INFO token messages |

The codec validates the input first and falls back to a slower scalar implementation only when input is malformed — so well-formed UTF-8/UTF-16 (the overwhelming majority of real traffic) hits the SIMD fast path every time. There is a microbenchmark (`make bench-utf16`) and an end-to-end before/after benchmark (`test/bench/bench_codec_e2e.sh`) recorded in `bench_results.md`.

**Dependency surface:** simdutf is pulled in statically via `vcpkg.json` (`"simdutf"` dependency, version resolved from the `builtin-baseline` pin). Release binaries embed it; downstream builds that don't use vcpkg need to provide `simdutfConfig.cmake` some other way (e.g., the `test/kerberos/test-client` Docker image builds it from upstream at a pinned tag).

### Known Issues

- Queries with unsupported types (UDT, SQL_VARIANT, etc.) will fail
- XML columns in INSERT/UPDATE are limited to 4096 bytes per value — use COPY TO with BCP protocol for larger documents
- Very large DECIMAL values may lose precision at extreme scales
- Connection pool statistics reset when all connections close
- VARCHAR columns >4000 characters with non-UTF8 collations are truncated when queried via catalog (see VARCHAR Encoding above)
- NVARCHAR(max) and VARCHAR(max) columns are not supported in the Microsoft Fabric Warehouse

## Third-Party Licenses

The hugr-lab/mssql-extension release binaries statically link the
following third-party components:

- **OpenSSL** (TLS / cryptography), licensed under the Apache
  License, Version 2.0. © The OpenSSL Project Authors and
  contributors. Version pinned via `vcpkg.json`.
  License text: [LICENSES/openssl-LICENSE.txt](LICENSES/openssl-LICENSE.txt)
- The **simdutf** library (Unicode transcoding via SIMD), used
  under the MIT License option of its dual Apache-2.0/MIT
  licensing. © Daniel Lemire and simdutf contributors.
  License text: [LICENSES/simdutf-LICENSE-MIT.txt](LICENSES/simdutf-LICENSE-MIT.txt)

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

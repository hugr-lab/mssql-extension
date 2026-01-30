# DuckDB MSSQL Extension

A DuckDB extension for connecting to Microsoft SQL Server databases using native TDS protocol - no ODBC, JDBC, or external drivers required.

> **Experimental**: This extension is under active development. APIs and behavior may change between releases. We welcome contributions, bug reports, and testing feedback!

## Features

- Native TDS protocol implementation (no external dependencies)
- Stream query results directly into DuckDB without buffering
- Full DuckDB catalog integration with three-part naming (`database.schema.table`)
- Row identity (`rowid`) support for tables with primary keys
- Connection pooling with configurable limits and automatic session reset
- TLS/SSL encrypted connections
- Full DML support: INSERT (with RETURNING), UPDATE, DELETE
- CREATE TABLE AS SELECT (CTAS) with streaming and type mapping
- High-performance COPY TO via TDS BulkLoadBCP protocol
- Transaction support: BEGIN/COMMIT/ROLLBACK with connection pinning
- Multi-statement SQL batches via `mssql_scan()` (e.g., temp table workflows)
- DuckDB secret management for secure credential storage

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
| `user`        | VARCHAR | Yes      | SQL Server username                  |
| `password`    | VARCHAR | Yes      | Password (hidden in duckdb_secrets)  |
| `use_encrypt` | BOOLEAN | No       | Enable TLS encryption (default: true) |
| `catalog`     | BOOLEAN | No       | Enable catalog integration (default: true). Set to false for serverless/restricted databases that don't support catalog queries |

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

#### URI Format

```sql
ATTACH 'mssql://user:password@host:port/database?encrypt=true'
    AS context_name (TYPE mssql);
```

URI format supports URL-encoded components for special characters in credentials.

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
| `mssql_ctas_text_type` | VARCHAR | `NVARCHAR` | Text column type: `NVARCHAR` or `VARCHAR` |
| `mssql_ctas_drop_on_failure` | BOOLEAN | `false` | Drop table if INSERT phase fails |

```sql
-- Use VARCHAR instead of NVARCHAR for text columns
SET mssql_ctas_text_type = 'VARCHAR';

-- Auto-cleanup on failure (for production pipelines)
SET mssql_ctas_drop_on_failure = true;
```

### CTAS Behavior

- **Two-phase execution**: CREATE TABLE DDL, then batched INSERT statements
- **Streaming**: Large result sets are streamed without full buffering
- **Non-atomic**: DDL commits immediately; INSERT respects transactions
- **Schema validation**: Target schema must exist before CTAS
- **Batching**: Uses `mssql_insert_batch_size` setting for INSERT batches

## COPY TO (Bulk Load)

High-performance bulk data transfer using the native TDS BulkLoadBCP protocol. Significantly faster than INSERT for large datasets.

### Target Formats

The COPY TO command supports two target formats:

| Format | Syntax | Example |
|--------|--------|---------|
| **URL** | `mssql://catalog/schema/table` | `mssql://sqlserver/dbo/my_table` |
| **Catalog** | `catalog.schema.table` | `sqlserver.dbo.my_table` |

Both formats are equivalent and can be used interchangeably.

### Basic COPY TO

```sql
-- Copy DuckDB table to SQL Server (URL format)
COPY my_local_table TO 'mssql://sqlserver/dbo/target_table' (FORMAT bcp);

-- Copy DuckDB table to SQL Server (catalog format)
COPY my_local_table TO 'sqlserver.dbo.target_table' (FORMAT bcp);

-- Copy query results to SQL Server
COPY (SELECT * FROM source WHERE year = 2024) TO 'mssql://sqlserver/dbo/target_table' (FORMAT bcp);

-- Generate data and copy to SQL Server
COPY (SELECT i AS id, 'row_' || i AS name FROM range(1000000) t(i))
  TO 'sqlserver.dbo.million_rows' (FORMAT bcp);
```

### COPY TO Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `CREATE_TABLE` | BOOLEAN | true | Auto-create target table if it doesn't exist |
| `REPLACE` | BOOLEAN | false | Drop and recreate table (replaces existing data) |
| `FLUSH_ROWS` | BIGINT | 100000 | Rows before flushing to SQL Server (overrides setting) |
| `TABLOCK` | BOOLEAN | true | Use TABLOCK hint for faster bulk load (overrides setting) |

```sql
-- Auto-create table (default: true)
COPY data TO 'mssql://sqlserver/dbo/new_table' (FORMAT bcp, CREATE_TABLE true);

-- Replace existing table (drop and recreate)
COPY data TO 'mssql://sqlserver/dbo/existing_table' (FORMAT bcp, REPLACE true);

-- Control flush frequency (rows before committing to SQL Server)
COPY data TO 'sqlserver.dbo.table' (FORMAT bcp, FLUSH_ROWS 500000);

-- Disable TABLOCK hint (allows concurrent access, slower)
COPY data TO 'sqlserver.dbo.table' (FORMAT bcp, TABLOCK false);
```

### Temporary Tables

Temp tables are prefixed with `#` (local) or `##` (global). They require a transaction context to remain accessible.

```sql
-- Local temp table using URL format (session-scoped, requires transaction)
BEGIN;
COPY data TO 'mssql://sqlserver/#temp_table' (FORMAT bcp);
SELECT * FROM mssql_scan('sqlserver', 'SELECT * FROM #temp_table');
COMMIT;

-- Local temp table using catalog format
BEGIN;
COPY data TO 'sqlserver.#temp_table' (FORMAT bcp);
SELECT * FROM mssql_scan('sqlserver', 'SELECT * FROM #temp_table');
COMMIT;

-- Global temp table (visible to all sessions)
COPY data TO 'mssql://sqlserver/##global_temp' (FORMAT bcp);
```

> **Note**: Temp tables have no schema component. Use `catalog.#table` or `mssql://catalog/#table` format.

### COPY TO Settings

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `mssql_copy_flush_rows` | BIGINT | 100000 | Rows before flushing to SQL Server (0 = flush at end only) |
| `mssql_copy_tablock` | BOOLEAN | true | Use TABLOCK hint for 15-30% better performance |

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

> **Note**: After DDL operations via `mssql_exec()`, use `mssql_refresh_cache('sqlserver')` to update the metadata cache. Standard DuckDB DDL operations automatically refresh the cache.

## Function Reference

### mssql_version()

Returns the extension version (DuckDB commit hash).

**Signature:** `mssql_version() -> VARCHAR`

```sql
SELECT mssql_version();
-- Returns: 'abc123def...'
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

### mssql_open()

Open a diagnostic connection to SQL Server.

**Signature:** `mssql_open(secret VARCHAR) -> BIGINT`

```sql
SELECT mssql_open('my_secret');
-- Returns: 12345 (connection handle)
```

### mssql_close()

Close a diagnostic connection.

**Signature:** `mssql_close(handle BIGINT) -> BOOLEAN`

```sql
SELECT mssql_close(12345);
-- Returns: true
```

### mssql_ping()

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
| `pinned_connections`    | BIGINT | Connections pinned to transactions |

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

### Unsupported Types

The following SQL Server types are not currently supported:

- `XML`
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
| `mssql_connection_limit`   | BIGINT  | 10      | ≥1    | Max connections per attached database    |
| `mssql_connection_cache`   | BOOLEAN | true    | -     | Enable connection pooling and reuse      |
| `mssql_connection_timeout` | BIGINT  | 30      | ≥0    | TCP connection timeout (seconds)         |
| `mssql_idle_timeout`       | BIGINT  | 300     | ≥0    | Idle connection timeout (seconds, 0=none)|
| `mssql_min_connections`    | BIGINT  | 2       | ≥0    | Minimum connections to maintain          |
| `mssql_acquire_timeout`    | BIGINT  | 30      | ≥0    | Connection acquire timeout (seconds)     |
| `mssql_query_timeout`      | BIGINT  | 30      | ≥0    | Query execution timeout (seconds, 0=infinite) |
| `mssql_catalog_cache_ttl`  | BIGINT  | 0       | ≥0    | Metadata cache TTL (seconds, 0=manual)   |

### Statistics Settings

| Setting                            | Type    | Default | Range | Description                           |
| ---------------------------------- | ------- | ------- | ----- | ------------------------------------- |
| `mssql_enable_statistics`          | BOOLEAN | true    | -     | Enable statistics collection          |
| `mssql_statistics_level`           | BIGINT  | 0       | ≥0    | Detail: 0=rowcount, 1=+histogram, 2=+NDV |
| `mssql_statistics_use_dbcc`        | BOOLEAN | false   | -     | Use DBCC SHOW_STATISTICS (requires permissions) |
| `mssql_statistics_cache_ttl_seconds` | BIGINT | 300    | ≥0    | Statistics cache TTL (seconds)        |

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
| `mssql_dml_use_prepared`           | BOOLEAN | false    | -      | Use prepared statements for DML       |

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
Error: Unsupported SQL Server type: XML
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
| Linux ARM64 | Not tested | Not built in CD pipeline |
| Windows x64 | Not tested | Not built in CD pipeline |

## Roadmap

The following features are planned for future releases:

| Feature | Description | Status |
|---------|-------------|--------|
| **Row Identity** | `rowid` pseudo-column mapping to primary keys | ✅ Implemented |
| **UPDATE/DELETE** | DML support with PK-based row identification, batched execution | ✅ Implemented |
| **Transactions** | BEGIN/COMMIT/ROLLBACK with connection pinning | ✅ Implemented |
| **Multi-Statement Batches** | Temp table workflows via `mssql_scan()` with session reset | ✅ Implemented |
| **CTAS** | CREATE TABLE AS SELECT with two-phase execution (DDL + INSERT) | ✅ Implemented |
| **MERGE/UPSERT** | Insert-or-update operations using SQL Server MERGE statement | Planned |
| **BCP/COPY** | High-throughput bulk insert via TDS BCP protocol (10M+ rows) | ✅ Implemented |

### Feature Details

**Row Identity**: Tables with primary keys expose a virtual `rowid` column. Scalar PKs map to their native type; composite PKs map to DuckDB STRUCT. This enables UPDATE/DELETE support.

**UPDATE/DELETE**: Supports `UPDATE ... SET ... WHERE` and `DELETE FROM ... WHERE` through DuckDB catalog integration. Uses `rowid` for row identification. Batched execution for large operations. Note: RETURNING clause is not supported for UPDATE/DELETE (only for INSERT).

**Transactions**: DML transactions with connection pinning. Each explicit transaction pins a single TDS connection for the transaction's duration, using SQL Server's 8-byte transaction descriptor in ALL_HEADERS. Connections are flagged for session reset (RESET_CONNECTION) on pool return.

**Multi-Statement Batches**: `mssql_scan()` supports batches where intermediate statements (DML/DDL) don't return result sets. Only one result-producing statement per batch is allowed. Session state (temp tables, variables) is reset via TDS RESET_CONNECTION flag when connections return to the pool.

**CTAS**: `CREATE TABLE mssql.schema.table AS SELECT ...` with two-phase execution: CREATE TABLE DDL followed by batched INSERT. Supports CREATE OR REPLACE, configurable text type (NVARCHAR/VARCHAR), and streaming for large result sets. Type mapping from DuckDB to SQL Server with clear errors for unsupported types.

**MERGE/UPSERT**: Batched upsert using SQL Server `MERGE` statement. Supports primary key or user-specified key columns.

**BCP/COPY**: Binary bulk copy protocol for maximum throughput. Streaming execution with bounded memory. No RETURNING support (use regular INSERT for that).

## Limitations

### Unsupported Features

- **RETURNING for UPDATE/DELETE**: Only INSERT supports RETURNING clause; UPDATE/DELETE do not
- **UPDATE/DELETE without PK**: Tables must have primary keys for UPDATE/DELETE operations
- **Windows Authentication**: Only SQL Server authentication is supported
- **Multiple result sets**: Only one result-producing statement per `mssql_scan()` batch is allowed
- **Stored Procedures with Output Parameters**: Use `mssql_scan()` for stored procedures
- **rowid for views/tables without PK**: Only tables with primary keys expose `rowid`

### Known Issues

- Queries with unsupported types (XML, UDT, etc.) will fail
- Very large DECIMAL values may lose precision at extreme scales
- Connection pool statistics reset when all connections close

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

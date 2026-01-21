# DuckDB MSSQL Extension

A DuckDB extension for connecting to Microsoft SQL Server databases using native TDS protocol - no ODBC, JDBC, or external drivers required.

> **Experimental**: This extension is under active development. APIs and behavior may change between releases. We welcome contributions, bug reports, and testing feedback!

## Features

- Native TDS protocol implementation (no external dependencies)
- Stream query results directly into DuckDB without buffering
- Full DuckDB catalog integration with three-part naming (`database.schema.table`)
- Connection pooling with configurable limits
- TLS/SSL encrypted connections
- INSERT support with RETURNING clause and automatic batching
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
    use_encrypt false
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
| `use_encrypt` | BOOLEAN | No       | Enable TLS encryption (default: false) |

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

| Key                         | Aliases                    |
| --------------------------- | -------------------------- |
| `Server`                    | `Data Source`              |
| `Database`                  | `Initial Catalog`          |
| `User Id`                   | `Uid`, `User`              |
| `Password`                  | `Pwd`                      |
| `Encrypt`                   | `Use Encryption for Data`  |

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

> **Note**: TLS support is available in both static and loadable extension builds (using OpenSSL).

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

Stream SELECT query results from SQL Server.

**Signature:** `mssql_scan(context VARCHAR, query VARCHAR) -> TABLE(...)`

```sql
SELECT * FROM mssql_scan('sqlserver', 'SELECT TOP 10 * FROM sys.tables');
```

The return schema is dynamic based on the query result columns.

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
| `context_name`          | VARCHAR | Attached database context name     |
| `total_connections`     | BIGINT | Current pool size                  |
| `idle_connections`      | BIGINT | Available connections              |
| `active_connections`    | BIGINT | Currently in use                   |
| `connections_created`   | BIGINT | Lifetime connections created       |
| `connections_closed`    | BIGINT | Lifetime connections closed        |
| `acquire_count`         | BIGINT | Times connections acquired         |
| `acquire_timeout_count` | BIGINT | Times acquisition timed out        |
| `acquire_wait_total_ms` | BIGINT | Total milliseconds spent waiting   |

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
| **UPDATE/DELETE** | DML support with PK-based row identification, batched execution | Planned |
| **Transactions** | BEGIN/COMMIT/ROLLBACK, savepoints, connection pinning | Planned |
| **CTAS** | CREATE TABLE AS SELECT with two-phase execution (DDL + INSERT) | Planned |
| **MERGE/UPSERT** | Insert-or-update operations using SQL Server MERGE statement | Planned |
| **BCP/COPY** | High-throughput bulk insert via TDS BCP protocol (10M+ rows) | Planned |

### Feature Details

**UPDATE/DELETE**: Will support `UPDATE ... SET ... WHERE` and `DELETE FROM ... WHERE` through DuckDB catalog integration. Requires primary key for row identification. Batched execution using `UPDATE ... FROM (VALUES ...)` pattern.

**Transactions**: DML-only transactions with connection pinning. Savepoints via `SAVE TRANSACTION`. DDL executes outside transactions (auto-commit).

**CTAS**: `CREATE TABLE mssql.schema.table AS SELECT ...` implemented as DDL creation followed by bulk INSERT (no RETURNING).

**MERGE/UPSERT**: Batched upsert using SQL Server `MERGE` statement. Supports primary key or user-specified key columns.

**BCP/COPY**: Binary bulk copy protocol for maximum throughput. Streaming execution with bounded memory. No RETURNING support (use regular INSERT for that).

## Limitations

### Unsupported Features

- **UPDATE/DELETE**: Use `mssql_exec()` for data modification other than INSERT
- **Windows Authentication**: Only SQL Server authentication is supported
- **Transactions**: Multi-statement transactions are not supported
- **Stored Procedures with Output Parameters**: Use `mssql_scan()` for stored procedures

### Known Issues

- Queries with unsupported types (XML, UDT, etc.) will fail
- Very large DECIMAL values may lose precision at extreme scales
- Connection pool statistics reset when all connections close

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

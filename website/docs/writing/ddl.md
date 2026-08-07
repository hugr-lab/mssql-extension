---
title: DDL Operations
sidebar_position: 5
---

# DDL Operations

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

> **Note**: After **schema-changing** DDL run through `mssql_exec()` (`CREATE`/`DROP`/`ALTER`/`TRUNCATE`/`RENAME`/`EXEC`), invalidate the metadata cache yourself — the same as the Postgres extension's `postgres_execute`. By default (`mssql_exec_invalidate_cache = false`) `mssql_exec()` does **not** touch the cache, so use `mssql_invalidate_cache('sqlserver' [, schema [, table]])` (lazy, point-scoped) or `mssql_refresh_cache('sqlserver')` (eager full reload). Standard DuckDB-catalog DDL (e.g. `CREATE TABLE db.dbo.t`) still invalidates automatically. Set `mssql_exec_invalidate_cache = true` to make `mssql_exec()` DDL auto-invalidate too. The same manual step picks up changes made entirely out of band (e.g. by another client) while `mssql_catalog_cache_ttl` is `0`. See [Cache & invalidation](https://github.com/hugr-lab/mssql-extension/blob/main/DATAMODEL.md#cache-invalidation) in `DATAMODEL.md` for how the two-layer cache works.


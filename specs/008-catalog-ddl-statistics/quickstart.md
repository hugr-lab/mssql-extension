# Quickstart: Catalog-Driven DDL and Statistics

**Feature**: 008-catalog-ddl-statistics
**Date**: 2026-01-18

## Prerequisites

1. DuckDB with MSSQL extension loaded
2. SQL Server 2019+ accessible
3. User has DDL permissions on target database

## Setup

```sql
-- Load extension
LOAD mssql;

-- Create secret with connection info
CREATE SECRET mssql_secret (
    TYPE mssql,
    HOST 'sqlserver.example.com',
    PORT 1433,
    DATABASE 'mydb',
    USER 'myuser',
    PASSWORD 'mypassword'
);

-- Attach database (read-write mode, default)
ATTACH '' AS mssql (TYPE mssql, SECRET mssql_secret);

-- Or attach in read-only mode (blocks all DDL and mssql_exec)
ATTACH '' AS mssql (TYPE mssql, SECRET mssql_secret, READ_ONLY);
```

## Schema Operations

```sql
-- Create a new schema
CREATE SCHEMA mssql.analytics;

-- Drop an empty schema
DROP SCHEMA mssql.analytics;
```

## Table Operations

```sql
-- Create a table
CREATE TABLE mssql.dbo.users (
    id INTEGER NOT NULL,
    name VARCHAR(100),
    email VARCHAR(255) NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Drop a table
DROP TABLE mssql.dbo.users;

-- Rename a table
ALTER TABLE mssql.dbo.users RENAME TO customers;
```

## Column Operations

```sql
-- Add a column
ALTER TABLE mssql.dbo.users ADD COLUMN phone VARCHAR(20);

-- Rename a column
ALTER TABLE mssql.dbo.users RENAME COLUMN phone TO phone_number;

-- Drop a column
ALTER TABLE mssql.dbo.users DROP COLUMN phone_number;

-- Change column type
ALTER TABLE mssql.dbo.users ALTER COLUMN name TYPE VARCHAR(200);

-- Change nullability
ALTER TABLE mssql.dbo.users ALTER COLUMN email SET NOT NULL;
ALTER TABLE mssql.dbo.users ALTER COLUMN email DROP NOT NULL;
```

## Direct T-SQL Execution

```sql
-- Execute arbitrary T-SQL
SELECT mssql_exec('mssql_secret', 'CREATE INDEX idx_users_email ON dbo.users(email)');

-- Insert data
SELECT mssql_exec('mssql_secret', 'INSERT INTO dbo.users (id, name) VALUES (1, ''Alice'')');

-- Execute stored procedure
SELECT mssql_exec('mssql_secret', 'EXEC dbo.refresh_stats');
```

## Statistics Configuration

```sql
-- Enable/disable statistics (default: true)
SET mssql_enable_statistics = true;

-- Set statistics level
-- 0 = row count only (default)
-- 1 = row count + histogram min/max
-- 2 = row count + histogram + NDV
SET mssql_statistics_level = 0;

-- Enable DBCC commands for column stats (default: false)
SET mssql_statistics_use_dbcc = false;

-- Cache TTL in seconds (default: 300)
SET mssql_statistics_cache_ttl_seconds = 300;
```

## Working with Views

```sql
-- Views are automatically detected and read-only
SELECT * FROM mssql.dbo.active_users;  -- Works

-- Write operations on views are blocked
INSERT INTO mssql.dbo.active_users VALUES (...);
-- Error: MSSQL view 'dbo.active_users' is read-only
```

## Inspecting Metadata

```sql
-- List tables
SHOW TABLES FROM mssql.dbo;

-- List views
SHOW VIEWS FROM mssql.dbo;

-- Describe table structure
DESCRIBE mssql.dbo.users;

-- Check query plan (statistics used by optimizer)
EXPLAIN SELECT * FROM mssql.dbo.users WHERE id = 1;
```

## Read-Only Mode

```sql
-- Attach in read-only mode for safe analytics
ATTACH '' AS mssql_prod (TYPE mssql, SECRET prod_secret, READ_ONLY);

-- SELECT works normally
SELECT COUNT(*) FROM mssql_prod.dbo.orders;

-- DDL is blocked
CREATE TABLE mssql_prod.dbo.test (id INT);
-- Error: Cannot modify MSSQL catalog 'mssql_prod': attached in read-only mode

-- mssql_exec is also blocked
SELECT mssql_exec('prod_secret', 'INSERT INTO dbo.logs VALUES (...)');
-- Error: Cannot execute mssql_exec: catalog 'mssql_prod' is attached in read-only mode
```

## Type Mapping Reference

| DuckDB Type | SQL Server Type |
| ----------- | --------------- |
| BOOLEAN | BIT |
| INTEGER | INT |
| BIGINT | BIGINT |
| DOUBLE | FLOAT |
| VARCHAR | NVARCHAR |
| BLOB | VARBINARY(MAX) |
| DATE | DATE |
| TIMESTAMP | DATETIME2(6) |
| UUID | UNIQUEIDENTIFIER |

## Error Handling

DDL errors include SQL Server details:

```
MSSQL DDL error (CREATE_TABLE): [dbo].[users]
SQL Server error 2714, state 6, class 16: There is already an object named 'users' in the database.
```

## Best Practices

1. **Use READ_ONLY for production analytics** - Prevents accidental schema changes
2. **Set appropriate statistics level** - Level 0 is fast and sufficient for most queries
3. **Cache metadata** - Default TTL is 5 minutes; adjust based on schema volatility
4. **Use secrets** - Never hardcode credentials; use DuckDB's secret manager
5. **Test DDL in dev first** - Verify SQL Server compatibility before production

# Quickstart: MSSQL rowid Semantics

**Feature**: 001-pk-rowid-semantics
**Date**: 2026-01-25

## Overview

This feature exposes SQL Server primary keys as DuckDB's `rowid` pseudo-column, enabling row-level identification for future UPDATE/DELETE operations.

## Usage Examples

### Scalar Primary Key

```sql
-- Attach MSSQL database
ATTACH 'Server=localhost,1433;Database=TestDB;User Id=sa;Password=xxx' AS mssql (TYPE mssql);

-- Table with INT primary key
-- SQL Server: CREATE TABLE customers (id INT PRIMARY KEY, name NVARCHAR(100))

-- Query with rowid
SELECT rowid, id, name FROM mssql.dbo.customers LIMIT 5;
-- rowid | id | name
-- ------|----|----------
-- 1     | 1  | Alice
-- 2     | 2  | Bob
-- 3     | 3  | Charlie

-- rowid equals PK value for scalar PKs
SELECT rowid = id AS rowid_matches_pk FROM mssql.dbo.customers;
-- rowid_matches_pk
-- ----------------
-- true
-- true
-- ...
```

### Composite Primary Key

```sql
-- Table with composite PK
-- SQL Server: CREATE TABLE orders (tenant_id INT, order_id BIGINT, amount DECIMAL(10,2),
--             PRIMARY KEY (tenant_id, order_id))

-- Query with rowid (returns STRUCT)
SELECT rowid, amount FROM mssql.dbo.orders LIMIT 3;
-- rowid                        | amount
-- -----------------------------|--------
-- {'tenant_id': 1, 'order_id': 100} | 99.99
-- {'tenant_id': 1, 'order_id': 101} | 149.50
-- {'tenant_id': 2, 'order_id': 100} | 75.00

-- Access struct fields
SELECT rowid.tenant_id, rowid.order_id, amount FROM mssql.dbo.orders;
```

### Tables Without Primary Key

```sql
-- Table without PK
-- SQL Server: CREATE TABLE logs (timestamp DATETIME, message NVARCHAR(MAX))

-- Regular queries work fine
SELECT * FROM mssql.dbo.logs LIMIT 10;  -- OK
SELECT COUNT(message) FROM mssql.dbo.logs;  -- OK

-- rowid query fails with clear error
SELECT rowid, * FROM mssql.dbo.logs;
-- Error: MSSQL: rowid requires a primary key
```

### Views

```sql
-- SQL Server view
-- CREATE VIEW active_customers AS SELECT * FROM customers WHERE active = 1

-- Regular queries work
SELECT * FROM mssql.dbo.active_customers;  -- OK

-- rowid not supported for views
SELECT rowid FROM mssql.dbo.active_customers;
-- Error: MSSQL: rowid not supported for views
```

## Query Patterns

### Efficient: rowid Not Requested

```sql
-- Only requested columns are fetched
SELECT name, email FROM mssql.dbo.customers;
-- Generated SQL: SELECT [name], [email] FROM [dbo].[customers]
-- PK column NOT included (no overhead)
```

### With rowid Projection

```sql
-- rowid requested - PK columns added to SELECT
SELECT rowid, name FROM mssql.dbo.customers;
-- Generated SQL: SELECT [id], [name] FROM [dbo].[customers]
-- PK column [id] added for rowid construction
```

## Testing Your Setup

### Prerequisites

1. SQL Server with test database
2. Extension built: `make`
3. Test container running: `make docker-up`

### Quick Test

```sql
-- Load extension
LOAD 'build/release/extension/mssql/mssql.duckdb_extension';

-- Attach test database
ATTACH 'Server=localhost,1433;Database=TestDB;User Id=sa;Password=TestPassword1' AS mssql (TYPE mssql);

-- Test scalar PK (TestSimplePK has INT id as PK)
SELECT rowid, * FROM mssql.dbo.TestSimplePK LIMIT 3;

-- Test composite PK (TestCompositePK has multi-column PK)
SELECT rowid, * FROM mssql.dbo.TestCompositePK LIMIT 3;

-- Test no-PK error (NullableTypes has no PK)
SELECT rowid FROM mssql.dbo.NullableTypes;  -- Should error
```

## Debugging

Enable debug logging to see PK discovery:

```bash
export MSSQL_DEBUG=1
./build/release/duckdb
```

Expected log output for PK discovery:
```
[MSSQL PK] Discovering primary key for [dbo].[customers]
[MSSQL PK] Found PK with 1 column(s): id (INTEGER)
[MSSQL PK] rowid type: INTEGER (scalar)
```

For composite PK:
```
[MSSQL PK] Discovering primary key for [dbo].[orders]
[MSSQL PK] Found PK with 2 column(s): tenant_id (INTEGER), order_id (BIGINT)
[MSSQL PK] rowid type: STRUCT(tenant_id INTEGER, order_id BIGINT)
```

## Common Issues

| Issue | Cause | Solution |
|-------|-------|----------|
| "rowid requires a primary key" | Table has no PK constraint | Add PK in SQL Server or don't use rowid |
| "rowid not supported for views" | Querying rowid from a view | Query base table instead |
| Wrong rowid order in STRUCT | — | Fields are ordered by PK ordinal, not column_id |
| rowid always NULL | — | Should not happen; PK columns are NOT NULL by definition |

## Next Steps

With rowid semantics implemented, the next spec (05.04b) will enable:
- `UPDATE mssql.dbo.table SET col = value WHERE rowid = x`
- `DELETE FROM mssql.dbo.table WHERE rowid = x`

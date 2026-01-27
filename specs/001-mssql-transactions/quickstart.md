# Quickstart: MSSQL Transactions

**Feature**: 001-mssql-transactions
**Date**: 2026-01-26

## Overview

This feature adds DuckDB transaction support for the MSSQL extension. When you use `BEGIN`, `COMMIT`, or `ROLLBACK` in DuckDB, those transaction boundaries are mapped to SQL Server transactions.

## Basic Usage

### Atomic Multi-Statement DML

```sql
-- Attach SQL Server database
ATTACH 'Server=localhost,1433;Database=TestDB;User Id=sa;Password=YourPassword' AS sqlsrv (TYPE mssql);

-- Start a transaction
BEGIN;

-- All DML runs on the same SQL Server connection
INSERT INTO sqlsrv.dbo.orders (id, customer) VALUES (1, 'Alice');
INSERT INTO sqlsrv.dbo.order_items (order_id, product_id, qty) VALUES (1, 100, 2);
INSERT INTO sqlsrv.dbo.order_items (order_id, product_id, qty) VALUES (1, 200, 1);

-- Commit atomically
COMMIT;
```

### Rollback on Error

```sql
BEGIN;

INSERT INTO sqlsrv.dbo.orders (id, customer) VALUES (2, 'Bob');

-- Oops, realized this is wrong
ROLLBACK;

-- The INSERT is undone; order 2 does not exist
```

## Read-Your-Writes

Within a transaction, you can verify your changes using `mssql_scan()`:

```sql
BEGIN;

INSERT INTO sqlsrv.dbo.products (id, name, price) VALUES (999, 'New Product', 49.99);

-- Verify the insert (uses the same SQL Server session)
SELECT * FROM mssql_scan('sqlsrv', 'SELECT * FROM dbo.products WHERE id = 999');
-- Shows: 999 | New Product | 49.99

ROLLBACK;

-- After rollback, product 999 is gone
SELECT * FROM mssql_scan('sqlsrv', 'SELECT * FROM dbo.products WHERE id = 999');
-- Returns empty
```

## Using mssql_exec in Transactions

Execute arbitrary T-SQL within the transaction:

```sql
BEGIN;

-- Create a temp table (session-scoped)
SELECT mssql_exec('sqlsrv', 'CREATE TABLE #staging (id INT, data NVARCHAR(100))');

-- Populate it
SELECT mssql_exec('sqlsrv', 'INSERT INTO #staging VALUES (1, ''test'')');

-- Use it
SELECT * FROM mssql_scan('sqlsrv', 'SELECT * FROM #staging');

-- Complex update
SELECT mssql_exec('sqlsrv', 'UPDATE dbo.products SET price = price * 1.1 WHERE category = ''Electronics''');

COMMIT;
-- Temp table is dropped when connection is released
```

## Important Limitations (MVP)

### Catalog Scans Are Not Allowed in Transactions

To ensure consistency, direct table scans via catalog syntax are blocked inside transactions:

```sql
BEGIN;

-- This will ERROR:
SELECT * FROM sqlsrv.dbo.products;
-- Error: MSSQL: reading attached tables/views inside DuckDB transactions is disabled (MVP). Use mssql_scan() instead.

-- Use this instead:
SELECT * FROM mssql_scan('sqlsrv', 'SELECT * FROM dbo.products');

COMMIT;
```

### Why This Restriction?

The MVP implementation uses a single pinned connection per transaction. Catalog scans would need to coordinate with DuckDB's parallel execution model, which introduces complexity around:
- Multiple concurrent connections vs. single transaction session
- Read consistency between catalog scans and DML operations
- Filter pushdown and result streaming on the pinned connection

Future versions may lift this restriction.

## Autocommit Mode (Default)

When not in an explicit transaction, each DML statement auto-commits immediately:

```sql
-- These are independent, auto-committed operations
INSERT INTO sqlsrv.dbo.logs (msg) VALUES ('Event 1');
INSERT INTO sqlsrv.dbo.logs (msg) VALUES ('Event 2');

-- If the second INSERT fails, the first is still committed
```

## DDL Behavior

DDL operations (CREATE, ALTER, DROP) execute on a separate connection, even within a transaction:

```sql
BEGIN;

-- This DDL runs on a separate auto-commit connection
CREATE TABLE sqlsrv.dbo.new_table (id INT PRIMARY KEY, name NVARCHAR(100));

-- DML still runs on the pinned transaction connection
INSERT INTO sqlsrv.dbo.new_table (id, name) VALUES (1, 'First');

COMMIT;
```

## Error Handling

If an error occurs during DML, the transaction remains active until you explicitly rollback:

```sql
BEGIN;

INSERT INTO sqlsrv.dbo.products (id, name) VALUES (1, 'Duplicate');

-- Constraint violation (duplicate key)
INSERT INTO sqlsrv.dbo.products (id, name) VALUES (1, 'Duplicate');
-- Error: Violation of PRIMARY KEY constraint...

-- Transaction is in error state; must rollback
ROLLBACK;
```

## Quick Reference

| Operation | In Transaction | In Autocommit |
|-----------|----------------|---------------|
| `INSERT INTO sqlsrv...` | Uses pinned connection | Acquires from pool |
| `UPDATE sqlsrv...` | Uses pinned connection | Acquires from pool |
| `DELETE FROM sqlsrv...` | Uses pinned connection | Acquires from pool |
| `SELECT FROM sqlsrv...` | **ERROR** (MVP restriction) | Acquires from pool |
| `mssql_scan(...)` | Uses pinned connection | Acquires from pool |
| `mssql_exec(...)` | Uses pinned connection | Acquires from pool |
| DDL (CREATE, etc.) | Separate auto-commit connection | Separate auto-commit connection |

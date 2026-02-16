# Quickstart: ORDER BY Pushdown

**Branch**: `039-order-pushdown`

## Enable ORDER BY Pushdown

```sql
-- Option 1: Enable globally for all MSSQL databases
SET mssql_order_pushdown = true;
ATTACH 'Server=myserver;Database=mydb;User Id=sa;Password=pass' AS db (TYPE mssql);

-- Option 2: Enable per-database via ATTACH
ATTACH 'Server=myserver;Database=mydb;User Id=sa;Password=pass' AS db (TYPE mssql, order_pushdown true);
```

## Basic Usage

```sql
-- Simple ORDER BY — pushed to SQL Server
SELECT * FROM db.dbo.Orders ORDER BY order_date DESC;

-- Multi-column ORDER BY
SELECT * FROM db.dbo.Orders ORDER BY customer_id ASC, order_date DESC;

-- TOP N — ORDER BY + LIMIT combined as SELECT TOP N
SELECT * FROM db.dbo.Orders ORDER BY order_date DESC LIMIT 10;

-- ORDER BY with function expression
SELECT * FROM db.dbo.Orders ORDER BY YEAR(order_date) DESC;
```

## What Gets Pushed Down

- Simple column references: `ORDER BY col1, col2`
- Simple function expressions: `ORDER BY YEAR(date_col)`
- Multi-column with mixed directions: `ORDER BY col1 ASC, col2 DESC`

## What Does NOT Get Pushed Down

- Complex expressions, nested functions, casts
- Columns with non-default NULL ordering on nullable columns
- Expressions referencing columns not in the table scan

When some ORDER BY columns can't be pushed, the pushable subset is still sent to SQL Server for pre-sorting (partial pushdown). DuckDB performs the final sort.

## Verify Pushdown is Working

Enable debug logging to see generated SQL:

```bash
MSSQL_DEBUG=1 duckdb
```

Look for the generated SQL query in stderr output — it should contain `ORDER BY` and/or `TOP N`.

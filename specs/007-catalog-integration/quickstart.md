# Quickstart: Catalog Integration & Read-Only SELECT

**Branch**: `007-catalog-integration`
**Date**: 2026-01-18

---

## Prerequisites

1. DuckDB with mssql extension loaded
2. SQL Server 2019+ with TCP connectivity
3. SQL Server credentials with SELECT permission on:
   - Target tables and views
   - `sys.schemas`, `sys.tables`, `sys.views`, `sys.columns`, `sys.types`

---

## 1. Create Secret

```sql
-- Create secret with SQL Server credentials
CREATE SECRET my_mssql (
    TYPE mssql,
    HOST 'localhost',
    PORT 1433,
    DATABASE 'AdventureWorks',
    USER 'myuser',
    PASSWORD 'mypassword'
);

-- For encrypted connection (recommended)
CREATE SECRET my_mssql_tls (
    TYPE mssql,
    HOST 'sql.example.com',
    PORT 1433,
    DATABASE 'production',
    USER 'reader',
    PASSWORD 'secret',
    USE_ENCRYPT true
);
```

---

## 2. Attach Database

```sql
-- Attach SQL Server as a DuckDB catalog
ATTACH '' AS sales (TYPE mssql, SECRET my_mssql);
```

---

## 3. Explore Catalog

```sql
-- List schemas
SHOW SCHEMAS FROM sales;
-- Returns: dbo, hr, finance, ...

-- List tables in a schema
SHOW TABLES FROM sales.dbo;
-- Returns: customers, orders, products, ...

-- Describe table structure
DESCRIBE sales.dbo.customers;
-- Returns columns with DuckDB types
```

---

## 4. Query Data

```sql
-- Simple SELECT
SELECT * FROM sales.dbo.customers LIMIT 10;

-- Projection pushdown (only fetches requested columns)
SELECT id, name, email FROM sales.dbo.customers;

-- Filter pushdown (WHERE clause sent to SQL Server)
SELECT * FROM sales.dbo.orders
WHERE status = 'shipped' AND total > 100;

-- Complex filters (AND, OR, IN, BETWEEN, LIKE)
SELECT * FROM sales.dbo.products
WHERE category IN ('Electronics', 'Books')
  AND price BETWEEN 10 AND 500
  AND name LIKE 'Widget%';

-- Joins across schemas
SELECT c.name, o.order_date, o.total
FROM sales.dbo.customers c
JOIN sales.dbo.orders o ON c.id = o.customer_id
WHERE o.status = 'completed';
```

---

## 5. Cache Management

```sql
-- Default: no automatic refresh (TTL = 0)
-- Manually refresh after schema changes
CALL mssql_refresh_catalog('sales');

-- Enable automatic refresh (5-minute TTL)
SET mssql_catalog_cache_ttl = 300;

-- Disable automatic refresh
SET mssql_catalog_cache_ttl = 0;
```

---

## 6. Use Default Schema

```sql
-- Set default catalog and schema
USE sales.dbo;

-- Now queries resolve to sales.dbo automatically
SELECT * FROM customers;  -- Same as: sales.dbo.customers
SELECT * FROM orders;     -- Same as: sales.dbo.orders
```

---

## 7. Detach When Done

```sql
DETACH sales;
```

---

## Common Errors

| Error | Cause | Solution |
|-------|-------|----------|
| `Write operations not supported` | INSERT/UPDATE/DELETE attempted | Use only SELECT queries |
| `Catalog 'x' not found` | Invalid catalog name in refresh | Check ATTACH name |
| `Connection failed` | Network/auth error | Verify host, port, credentials |
| `Object not found` | Table doesn't exist or no permission | Check schema.table name, permissions |

---

## Performance Tips

1. **Use projection**: Only SELECT columns you need
2. **Push filters**: Use supported operators (=, <, >, IN, LIKE) for pushdown
3. **Avoid functions on columns**: `WHERE UPPER(name) = 'X'` evaluates locally
4. **LIMIT early**: Add LIMIT to avoid fetching entire tables
5. **Refresh cache**: After schema changes, call `mssql_refresh_catalog()`

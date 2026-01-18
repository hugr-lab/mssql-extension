# Contract: DuckDB SQL Functions

**Branch**: `007-catalog-integration`
**Date**: 2026-01-18

This contract defines the SQL-level functions exposed by the MSSQL extension for catalog management.

---

## Extension Settings

### mssql_catalog_cache_ttl

**Type**: INTEGER
**Default**: 0
**Range**: 0 to MAX_INT64

Controls automatic metadata cache refresh.

```sql
-- Disable automatic refresh (default)
SET mssql_catalog_cache_ttl = 0;

-- Enable 5-minute TTL
SET mssql_catalog_cache_ttl = 300;

-- Query current value
SELECT current_setting('mssql_catalog_cache_ttl');
```

**Behavior**:
- When 0: No automatic refresh; use `mssql_refresh_catalog()` for manual refresh
- When > 0: Cache automatically refreshes when older than TTL seconds

---

## Table Functions

### mssql_refresh_catalog

**Signature**: `mssql_refresh_catalog(catalog_name VARCHAR) → TABLE(success BOOLEAN, message VARCHAR)`

Explicitly refreshes metadata cache for an attached MSSQL catalog.

```sql
-- Refresh single catalog
CALL mssql_refresh_catalog('sales');

-- Or use as table function
SELECT * FROM mssql_refresh_catalog('sales');
-- Returns: success=true, message='Refreshed catalog sales: 5 schemas, 42 tables'
```

**Parameters**:

| Name | Type | Description |
|------|------|-------------|
| `catalog_name` | `VARCHAR` | Name of attached MSSQL catalog |

**Returns**:

| Column | Type | Description |
|--------|------|-------------|
| `success` | `BOOLEAN` | TRUE if refresh succeeded |
| `message` | `VARCHAR` | Status message with counts or error |

**Errors**:
- `Catalog 'name' not found` - No attached catalog with that name
- `Catalog 'name' is not an MSSQL catalog` - Catalog exists but is not MSSQL type
- `Failed to refresh: <error>` - SQL Server connection/query error

---

## Standard DuckDB Catalog Commands

These standard DuckDB commands work with attached MSSQL catalogs:

### ATTACH

```sql
-- Attach SQL Server database using secret
ATTACH '' AS sales (TYPE mssql, SECRET my_mssql_secret);

-- Attach with inline connection (if supported)
ATTACH 'Server=localhost;Database=sales;User Id=sa;Password=pwd' AS sales (TYPE mssql);
```

**Behavior**:
- Creates catalog entry in DuckDB
- Queries database default collation
- Does NOT load schemas/tables (lazy loading)

### DETACH

```sql
DETACH sales;
```

**Behavior**:
- Returns connections to pool
- Clears metadata cache
- Removes catalog entry

### USE

```sql
USE sales;
USE sales.dbo;
```

**Behavior**:
- Sets default catalog and/or schema for unqualified table references

### SHOW SCHEMAS

```sql
SHOW SCHEMAS FROM sales;
```

**Returns**:

| Column | Type | Description |
|--------|------|-------------|
| `schema_name` | `VARCHAR` | Schema name (e.g., "dbo", "hr") |

**Behavior**:
- Triggers lazy load of schema list if not cached
- Filters to schemas with at least one accessible table/view

### SHOW TABLES

```sql
SHOW TABLES FROM sales.dbo;
SHOW ALL TABLES FROM sales.dbo;  -- Include views
```

**Returns**:

| Column | Type | Description |
|--------|------|-------------|
| `name` | `VARCHAR` | Table or view name |

**Behavior**:
- Triggers lazy load of table list if not cached
- `SHOW TABLES` returns both tables and views
- `SHOW ALL TABLES` same behavior (views always included)

### DESCRIBE / SHOW

```sql
DESCRIBE sales.dbo.customers;
SHOW sales.dbo.customers;
```

**Returns**:

| Column | Type | Description |
|--------|------|-------------|
| `column_name` | `VARCHAR` | Column name |
| `column_type` | `VARCHAR` | DuckDB type |
| `null` | `VARCHAR` | "YES" or "NO" |
| `key` | `VARCHAR` | (empty - PK not exposed in this spec) |
| `default` | `VARCHAR` | (empty - defaults not exposed) |
| `extra` | `VARCHAR` | (empty) |

**Extended metadata** (accessible via system tables):
- `collation_name` - SQL Server collation
- `sql_type_name` - Original SQL Server type

---

## Query Syntax

### SELECT with Full Qualification

```sql
-- Fully qualified table reference
SELECT * FROM sales.dbo.customers;

-- With projection
SELECT id, name FROM sales.dbo.orders WHERE status = 'shipped';

-- Join across schemas
SELECT c.name, o.total
FROM sales.dbo.customers c
JOIN sales.hr.employees e ON c.rep_id = e.id;
```

### SELECT with USE

```sql
USE sales.dbo;
SELECT * FROM customers;  -- Resolves to sales.dbo.customers
```

---

## Write Operations (Blocked)

All write operations fail with clear error:

```sql
-- INSERT
INSERT INTO sales.dbo.customers (name) VALUES ('test');
-- Error: Write operations not supported for MSSQL catalog

-- UPDATE
UPDATE sales.dbo.customers SET name = 'test' WHERE id = 1;
-- Error: Write operations not supported for MSSQL catalog

-- DELETE
DELETE FROM sales.dbo.customers WHERE id = 1;
-- Error: Write operations not supported for MSSQL catalog

-- DDL
CREATE TABLE sales.dbo.newtable (id INT);
-- Error: Write operations not supported for MSSQL catalog

DROP TABLE sales.dbo.customers;
-- Error: Write operations not supported for MSSQL catalog
```

---

## Filter Pushdown Behavior

### Pushed Filters

These filter patterns are pushed to SQL Server:

```sql
-- Equality (all types)
WHERE id = 123
WHERE name = 'Smith'
WHERE created_at = '2024-01-01'

-- Comparison operators
WHERE amount > 100.50
WHERE age >= 18 AND age <= 65

-- NULL checks
WHERE middle_name IS NULL
WHERE phone IS NOT NULL

-- Boolean logic
WHERE status = 'active' AND region = 'US'
WHERE category = 'A' OR category = 'B'

-- BETWEEN
WHERE price BETWEEN 10 AND 100

-- IN lists (up to 100 items)
WHERE status IN ('new', 'pending', 'shipped')

-- LIKE patterns
WHERE name LIKE 'John%'
WHERE email LIKE '%@example.com'
WHERE code LIKE 'ABC_123'

-- ILIKE on case-insensitive columns only
WHERE name ILIKE 'smith'  -- Pushed if column collation is CI
```

### Local Filters (Not Pushed)

These are evaluated locally in DuckDB:

```sql
-- ILIKE on case-sensitive columns
WHERE name ILIKE 'smith'  -- Local if column collation is CS

-- Complex functions
WHERE UPPER(name) = 'SMITH'  -- Local (function on column)
WHERE LENGTH(name) > 10     -- Local (function result comparison)

-- IN lists exceeding limit
WHERE id IN (1, 2, 3, ..., 150)  -- Partial push or local

-- Regex
WHERE name ~ '^[A-Z]'  -- Local (no SQL Server equivalent)

-- JSON operations
WHERE data->>'key' = 'value'  -- Local

-- Expressions with multiple columns
WHERE first_name || ' ' || last_name = 'John Smith'  -- Local
```

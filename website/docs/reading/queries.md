---
title: Query Execution
sidebar_position: 2
---

# Query Execution

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


# Quickstart: Fix Catalog Scan & Object Visibility Filters

## Problem

When attaching to SQL Server databases with many tables (65K+), the extension issues one column-discovery query per table during catalog operations. This makes the extension unusable for large databases.

## Solution Overview

Three complementary fixes:

1. **Deferred column loading** — Catalog scans load only table names; columns are loaded on-demand when a specific table is accessed
2. **Regex filters** — `schema_filter` and `table_filter` parameters limit visible objects
3. **Bulk preload** — `mssql_preload_catalog()` loads all metadata in a single SQL query

## Usage Examples

### Basic (no changes needed — just faster)

```sql
-- Attach as before — now won't trigger 65K column queries
ATTACH 'Server=erp;Database=erp_db;User Id=reader;Password=pass' AS erp (TYPE mssql);

-- Single-table query: only loads columns for this one table
SELECT * FROM erp.dbo.Orders LIMIT 10;
```

### With Filters (for large databases)

```sql
-- Only expose dbo schema, tables starting with Sales or Inv
ATTACH 'Server=erp;Database=erp_db;User Id=reader;Password=pass' AS erp (
    TYPE mssql,
    schema_filter '^dbo$',
    table_filter '^(Sales|Inv).*'
);

-- SHOW TABLES now only lists matching tables
SHOW ALL TABLES;
```

### With Bulk Preload (for IDE/tool integration)

```sql
-- Attach normally
ATTACH 'Server=erp;Database=erp_db;User Id=reader;Password=pass' AS erp (TYPE mssql);

-- One-time bulk load of ALL metadata (single SQL round trip)
SELECT mssql_preload_catalog('erp');

-- All subsequent queries use cached metadata — no additional SQL Server queries
SELECT * FROM erp.dbo.Orders LIMIT 10;
SHOW ALL TABLES;
```

### Filters in Secrets

```sql
CREATE SECRET erp_secret (
    TYPE mssql,
    host 'erp-server',
    database 'erp_db',
    user 'reader',
    password 'pass',
    schema_filter '^dbo$',
    table_filter '^(Orders|Products|Customers)$'
);

ATTACH '' AS erp (TYPE mssql, SECRET erp_secret);
```

## Backward Compatibility

- No configuration changes needed for existing users
- All existing queries continue to work
- Performance improves automatically for the default case (deferred column loading)
- Filters and preload are opt-in features

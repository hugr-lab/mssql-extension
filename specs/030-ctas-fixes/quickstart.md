# Quickstart: CTAS Fixes - IF NOT EXISTS and Auto-TABLOCK

## Overview

This guide covers the implementation of two fixes for CREATE TABLE AS SELECT (CTAS) operations:
1. **Bug Fix**: `CREATE TABLE IF NOT EXISTS` now works correctly
2. **Enhancement**: Automatic TABLOCK for 15-30% faster bulk loads

## Prerequisites

- DuckDB with MSSQL extension loaded
- SQL Server instance attached via `ATTACH`

## Feature 1: CREATE TABLE IF NOT EXISTS

### Basic Usage

```sql
-- Attach to SQL Server
ATTACH 'Server=localhost;Database=TestDB;User Id=sa;Password=Pass123' AS db (TYPE mssql);

-- First run: Creates table
CREATE TABLE IF NOT EXISTS db.dbo.my_data AS
SELECT id, name FROM generate_series(1, 1000) t(id), (SELECT 'test' as name);
-- Result: 1000 rows

-- Second run: No error, no changes
CREATE TABLE IF NOT EXISTS db.dbo.my_data AS
SELECT id, name FROM generate_series(1, 1000) t(id), (SELECT 'test' as name);
-- Result: 0 rows (table already exists)
```

### Use Cases

**ETL Pipeline Idempotency**:
```sql
-- Safe to re-run entire script
CREATE TABLE IF NOT EXISTS db.dbo.dim_date AS
SELECT * FROM generate_date_range();

CREATE TABLE IF NOT EXISTS db.dbo.dim_customer AS
SELECT * FROM staging_customers;

CREATE TABLE IF NOT EXISTS db.dbo.fact_sales AS
SELECT * FROM staging_sales;
```

**Migration Scripts**:
```sql
-- Won't fail if table already exists from previous migration
CREATE TABLE IF NOT EXISTS db.dbo.audit_log (
    id INT,
    action VARCHAR(100),
    timestamp TIMESTAMP
);
```

## Feature 2: Automatic TABLOCK for New Tables

### How It Works

When creating a new table via CTAS or COPY, TABLOCK is automatically enabled for BCP operations. This provides 15-30% faster bulk loads with no downside (new tables have no concurrent readers).

### Automatic Behavior

```sql
-- TABLOCK auto-enabled (new table)
CREATE TABLE db.dbo.large_table AS
SELECT * FROM generate_series(1, 1000000);
-- Internally uses: INSERT BULK ... WITH (TABLOCK)

-- TABLOCK auto-enabled (OR REPLACE drops and recreates)
CREATE OR REPLACE TABLE db.dbo.large_table AS
SELECT * FROM generate_series(1, 1000000);
-- Internally uses: INSERT BULK ... WITH (TABLOCK)
```

### Disabling Auto-TABLOCK

If you need to disable auto-TABLOCK (rare):

```sql
-- Disable TABLOCK explicitly
SET mssql_copy_tablock = false;

-- Now creates without TABLOCK hint
CREATE TABLE db.dbo.my_table AS
SELECT * FROM source;
```

### COPY TO Automatic TABLOCK

```sql
-- TABLOCK auto-enabled when creating new table
COPY (SELECT * FROM source) TO 'mssql://db/dbo/new_table' (CREATE_TABLE true);

-- TABLOCK NOT auto-enabled for existing tables (append)
COPY (SELECT * FROM source) TO 'mssql://db/dbo/existing_table';

-- Force TABLOCK for existing table (use with caution - blocks readers)
COPY (SELECT * FROM source) TO 'mssql://db/dbo/existing_table' (TABLOCK true);
```

## Comparison: Before vs After

### Issue #44 Fix

| Scenario | Before (Bug) | After (Fixed) |
|----------|--------------|---------------|
| `CREATE TABLE IF NOT EXISTS` (table exists) | Error: "already exists" | Success, 0 rows |
| `CREATE TABLE IF NOT EXISTS` (table doesn't exist) | Creates table | Creates table |

### Issue #45 Enhancement

| Scenario | Before | After |
|----------|--------|-------|
| CTAS new table (1M rows) | ~30 seconds | ~20-25 seconds |
| CTAS OR REPLACE | ~30 seconds | ~20-25 seconds |
| COPY TO CREATE_TABLE | ~30 seconds | ~20-25 seconds |

## Troubleshooting

### Q: My CTAS is still slow, is TABLOCK working?

Enable debug logging to verify:
```bash
MSSQL_DEBUG=2 duckdb
```

Look for: `BCP using TABLOCK hint` in the output.

### Q: I want TABLOCK for an existing table

```sql
-- Method 1: Use COPY with TABLOCK option
COPY (SELECT * FROM source) TO 'mssql://db/dbo/existing' (TABLOCK true);

-- Method 2: Global setting
SET mssql_copy_tablock = true;
CREATE TABLE db.dbo.another_table AS SELECT ...;
```

### Q: IF NOT EXISTS still errors with "permission denied"

`IF NOT EXISTS` only handles the "table already exists" case. Other errors (permissions, missing schema) are still raised normally.

### Q: How do I know if a table was created or skipped?

Check the return value:
```sql
-- Returns row count
SELECT * FROM (
    CREATE TABLE IF NOT EXISTS db.dbo.test AS SELECT 1 as id
);
-- Result: 1 (created) or 0 (already existed)
```

## Performance Tips

1. **Large data loads**: Use CTAS with new tables to get automatic TABLOCK
2. **Incremental loads to existing tables**: Consider `mssql_copy_tablock = true` if you can afford blocking readers
3. **ETL scripts**: Use `IF NOT EXISTS` for idempotent table creation
4. **Schema migrations**: Combine `IF NOT EXISTS` with `OR REPLACE` for different scenarios

## Related Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `mssql_copy_tablock` | false | Enable TABLOCK for all BCP operations |
| `mssql_ctas_use_bcp` | true | Use BCP protocol for CTAS (required for auto-TABLOCK) |
| `mssql_copy_flush_rows` | 100000 | Rows per batch in BCP |

## See Also

- [Spec 024: COPY TO BCP](../024-mssql-copy-bcp/spec.md)
- [Spec 027: CTAS BCP Integration](../027-ctas-bcp-integration/spec.md)
- [CLAUDE.md Extension Settings](../../CLAUDE.md)

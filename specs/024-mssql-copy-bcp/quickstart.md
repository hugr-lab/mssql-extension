# Quickstart: COPY TO MSSQL via BCP

**Feature**: 024-mssql-copy-bcp
**Date**: 2026-01-29

## Prerequisites

1. MSSQL extension loaded
2. SQL Server attached via `ATTACH`
3. Write access to target database

## Basic Usage

### 1. Attach SQL Server Database

```sql
ATTACH 'Server=localhost,1433;Database=TestDB;User Id=sa;Password=YourPassword'
AS db (TYPE mssql);
```

### 2. COPY Local Data to SQL Server

```sql
-- Create local test data
CREATE TABLE local_data AS
SELECT i AS id, 'Item ' || i AS name, random() * 1000 AS amount
FROM range(100000) t(i);

-- COPY to SQL Server (creates table if not exists)
COPY (SELECT * FROM local_data)
TO 'mssql://db/dbo/imported_data' (FORMAT 'bcp');
```

### 3. Verify Data

```sql
SELECT COUNT(*) FROM mssql_scan('db', 'SELECT * FROM dbo.imported_data');
-- Returns: 100000
```

## Common Patterns

### Append to Existing Table

```sql
COPY (SELECT * FROM new_records)
TO 'mssql://db/dbo/existing_table' (FORMAT 'bcp', CREATE_TABLE false);
```

### Replace Existing Table

```sql
COPY (SELECT * FROM fresh_data)
TO 'mssql://db/dbo/target' (FORMAT 'bcp', OVERWRITE true);
```

### COPY to Temp Table

```sql
-- Create temp table with staging data
COPY (SELECT id, amount FROM orders WHERE status = 'pending')
TO 'mssql://db/#pending_orders' (FORMAT 'bcp');

-- Use temp table in subsequent queries (same session)
SELECT mssql_exec('db', 'UPDATE o SET o.status = ''processing'' FROM Orders o INNER JOIN #pending_orders p ON o.id = p.id');
```

### Catalog Syntax

```sql
-- Using catalog.schema.table notation
COPY (SELECT * FROM local_data)
TO db.dbo.target_table (FORMAT 'bcp');
```

## Configuration Settings

```sql
-- Adjust batch size (default: 10000 rows)
SET mssql_copy_batch_rows = 50000;

-- Adjust max batch bytes (default: 32MB)
SET mssql_copy_max_batch_bytes = '64MB';
```

## Debugging

```bash
# Enable debug output
export MSSQL_DEBUG=1  # Basic: COPY start/end
export MSSQL_DEBUG=2  # Verbose: batch details
export MSSQL_DEBUG=3  # Trace: packet-level
```

## Error Handling

```sql
-- Table exists error
COPY (SELECT * FROM data) TO 'mssql://db/dbo/existing' (FORMAT 'bcp');
-- Error: Table [dbo].[existing] already exists. Use OVERWRITE=true to replace.

-- Table doesn't exist with CREATE_TABLE=false
COPY (SELECT * FROM data) TO 'mssql://db/dbo/missing' (FORMAT 'bcp', CREATE_TABLE false);
-- Error: Table [dbo].[missing] does not exist and CREATE_TABLE=false

-- Connection streaming error
BEGIN;
SELECT * FROM db.dbo.some_table;  -- starts streaming
COPY (SELECT * FROM local) TO 'mssql://db/#temp' (FORMAT 'bcp');
-- Error: cannot start COPY/BCP on a connection that is currently streaming results
ROLLBACK;
```

## Performance Tips

1. **Large datasets**: Use default batch size (10000 rows) or tune with `mssql_copy_batch_rows`
2. **Wide tables**: Reduce `mssql_copy_batch_rows` to stay under `mssql_copy_max_batch_bytes`
3. **Network latency**: Larger batches reduce round-trips
4. **Memory**: Bounded by batch size, not total row count

## Type Mapping Reference

| DuckDB | SQL Server |
|--------|------------|
| BOOLEAN | bit |
| INTEGER | int |
| BIGINT | bigint |
| DOUBLE | float |
| DECIMAL(p,s) | decimal(p,s) |
| VARCHAR | nvarchar(max) |
| UUID | uniqueidentifier |
| BLOB | varbinary(max) |
| DATE | date |
| TIMESTAMP | datetime2(7) |
| TIMESTAMP_TZ | datetimeoffset(7) |

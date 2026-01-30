# Quickstart: BCP Improvements

**Feature**: 025-bcp-improvements
**Date**: 2026-01-30

## Overview

This feature adds four improvements to the COPY TO functionality:

1. **Empty Schema Syntax** - Use `db..#temp` or `mssql://db//#temp` for temp tables
2. **Connection Leak Fix** - Proper connection cleanup on errors
3. **Better Type Errors** - Clear messages for column type mismatches
4. **INSERT Method** - Optional INSERT fallback via `METHOD 'insert'`

## Quick Examples

### Empty Schema for Temp Tables

```sql
-- Attach to SQL Server
ATTACH 'Server=localhost;Database=mydb;User Id=sa;Password=pass' AS db (TYPE mssql);

-- Create local data
CREATE TABLE local_data AS SELECT i AS id FROM range(100) t(i);

-- These all work for temp tables:
BEGIN;
COPY local_data TO 'db.#my_temp' (FORMAT 'bcp');           -- Current syntax
COPY local_data TO 'db..#my_temp' (FORMAT 'bcp');          -- New: empty schema
COPY local_data TO 'mssql://db/#my_temp' (FORMAT 'bcp');   -- URL current
COPY local_data TO 'mssql://db//#my_temp' (FORMAT 'bcp');  -- URL new: empty schema
COMMIT;
```

### INSERT Method for Debugging

```sql
-- Use INSERT statements instead of BCP protocol
COPY local_data TO 'db.dbo.my_table' (FORMAT 'bcp', METHOD 'insert');

-- Useful when:
-- - Debugging data issues
-- - Testing smaller datasets
-- - BCP-specific errors occur
```

### Handling Type Mismatches

```sql
-- If source types don't match target:
COPY varchar_data TO 'db.dbo.int_table' (FORMAT 'bcp');
-- Error: Column 'id' type mismatch: target expects INT, source provides VARCHAR
```

## Testing the Features

### Test Empty Schema

```sql
-- Setup
CREATE TABLE test_data AS SELECT 1 AS val;

BEGIN;
-- Test URL empty schema
COPY test_data TO 'mssql://db//#test_empty' (FORMAT 'bcp');
SELECT * FROM mssql_scan('db', 'SELECT * FROM #test_empty');
COMMIT;
```

### Test Connection Leak Prevention

```sql
-- Check initial pool stats
SELECT * FROM mssql_pool_stats('db');

-- Intentionally cause error (type mismatch)
CREATE TABLE bad_data AS SELECT 'text' AS num;
COPY bad_data TO 'db.dbo.int_table' (FORMAT 'bcp', CREATE_TABLE false);
-- Error expected

-- Verify no connection leak
SELECT * FROM mssql_pool_stats('db');
-- active_connections should not increase
```

### Test INSERT Method

```sql
-- Create data
CREATE TABLE insert_test AS SELECT i AS id, 'row' || i AS name FROM range(10) t(i);

-- Use INSERT method
COPY insert_test TO 'db.dbo.insert_target' (FORMAT 'bcp', METHOD 'insert', CREATE_TABLE true);

-- Verify
SELECT COUNT(*) FROM db.dbo.insert_target;
-- Should return 10
```

## Configuration

### Relevant Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `mssql_insert_batch_size` | 1000 | Rows per INSERT (used by INSERT method) |
| `mssql_copy_flush_rows` | 100000 | Rows per BCP flush |

### Debug Logging

```bash
# Enable COPY debug logging
export MSSQL_DEBUG=2

# See connection acquire/release
export MSSQL_DEBUG=3
```

## Migration Notes

### Backward Compatibility

All existing COPY syntax continues to work unchanged:
- `db.schema.table` - unchanged
- `db.#temp` - unchanged
- `mssql://db/schema/table` - unchanged
- `mssql://db/#temp` - unchanged

### New Syntax

Additional syntax now supported:
- `db..#temp` - empty schema for temp tables
- `mssql://db//#temp` - URL empty schema for temp tables

### Error Message Changes

Type mismatch errors now include more detail:
- Before: Generic SQL Server error
- After: "Column 'name' type mismatch: target expects TYPE1, source provides TYPE2"

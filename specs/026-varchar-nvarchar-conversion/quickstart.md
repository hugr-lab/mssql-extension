# Quickstart: VARCHAR to NVARCHAR Conversion

**Date**: 2026-02-02
**Feature**: 026-varchar-nvarchar-conversion

## Problem

Querying SQL Server tables with non-UTF8 VARCHAR columns fails with:
```
Invalid Input Error: Failed to append: Invalid unicode (byte sequence mismatch) detected in segment statistics update
```

## Solution

The extension now automatically wraps non-UTF8 VARCHAR/CHAR columns in `CAST(... AS NVARCHAR(n))` when generating table scan queries.

## What Changes

### For Users

**Nothing changes in usage**. Table scans work transparently:

```sql
-- This just works now (even with Latin1 collation)
SELECT * FROM mssql.dbo.products WHERE name LIKE 'Café%';
```

### Generated SQL

Before:
```sql
SELECT [id], [name], [description] FROM [dbo].[products]
```

After:
```sql
SELECT [id], CAST([name] AS NVARCHAR(100)) AS [name], CAST([description] AS NVARCHAR(MAX)) AS [description] FROM [dbo].[products]
```

## Limitations

1. **VARCHAR >4000 truncation**: VARCHAR columns defined >4000 characters are truncated to 4000 when read.
   - Only the column *definition* matters, not actual data length

2. **VARCHAR(MAX) buffer capacity**: VARCHAR(MAX) columns are converted to NVARCHAR(MAX) by default, which may halve the effective buffer capacity due to NVARCHAR's 2-byte encoding.
   - Disable with `SET mssql_convert_varchar_max = false` if you need maximum buffer capacity

3. **Raw SQL queries**: `mssql_scan(context, 'SELECT ...')` does NOT apply conversion.
   - Users must add CAST manually if needed

## Settings

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `mssql_convert_varchar_max` | BOOLEAN | true | Convert VARCHAR(MAX) to NVARCHAR(MAX) in catalog queries |

## Implementation Files

| File | Change |
|------|--------|
| `src/include/mssql_functions.hpp` | Add `mssql_columns` to bind data |
| `src/table_scan/table_scan.cpp` | Wrap columns in CAST during query generation |
| `src/catalog/mssql_table_entry.cpp` | Pass mssql_columns to bind data |
| `test/sql/catalog/varchar_encoding.test` | New integration test |
| `README.md` | Document limitations |

## Testing

Run integration tests (requires SQL Server):
```bash
make docker-up
make integration-test
```

Or run specific test:
```bash
./build/release/test/unittest "test/sql/catalog/varchar_encoding.test"
```

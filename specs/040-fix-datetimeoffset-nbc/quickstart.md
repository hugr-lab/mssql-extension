# Quickstart: Fix DATETIMEOFFSET in NBC Row Reader

**Phase 1 Output** | **Date**: 2026-02-18

## The Fix (1 minute)

Add the missing `case TDS_TYPE_DATETIMEOFFSET` to `ReadValueNBC()` in `src/tds/tds_row_reader.cpp`, between the `TDS_TYPE_DATETIME2` case (line ~726) and `TDS_TYPE_UNIQUEIDENTIFIER` case (line ~728):

```cpp
// DATETIMEOFFSET - has 1-byte length prefix in NBC rows
case TDS_TYPE_DATETIMEOFFSET: {
    if (length < 1)
        return 0;
    uint8_t data_length = data[0];
    if (length < 1 + data_length)
        return 0;
    value.assign(data + 1, data + 1 + data_length);
    return 1 + data_length;
}
```

## Build & Test

```bash
# Build
GEN=ninja make

# Unit tests (no SQL Server needed)
./build/release/test/unittest

# Start SQL Server container
make docker-up

# Integration tests
make integration-test
```

## Verify the fix

```sql
-- Load extension
LOAD mssql;

-- Attach SQL Server
ATTACH 'Server=localhost,1433;Database=TestDB;User Id=sa;Password=TestPassword1' AS db (TYPE mssql);

-- This query should now work (previously threw "Unsupported type in NBC RowReader: DATETIMEOFFSET")
SELECT * FROM db.dbo.NullableDatetimeScales;
```

## Files to modify

1. `src/tds/tds_row_reader.cpp` — Add DATETIMEOFFSET case to ReadValueNBC()
2. `docker/init/init.sql` — Add NullableDatetimeScales test table
3. `test/sql/catalog/datetimeoffset_nbc.test` — New integration test for NBCROW path
4. `test/sql/integration/datetime_scales.test` — Extend with TIME and DATETIMEOFFSET scale tests

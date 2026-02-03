# Quickstart: Testing ANSI Connection Options Fix

**Feature**: 028-ansi-connection-options
**Date**: 2026-02-03

## Prerequisites

1. SQL Server container running:
   ```bash
   make docker-up
   ```

2. Extension built with the fix:
   ```bash
   make
   ```

## Quick Verification

Test the fix by executing a DDL command that previously failed:

```bash
# Source test environment
set -a && source .env && set +a

# Run DDL command via mssql_exec
./build/release/duckdb -c "
ATTACH 'Server=${MSSQL_TEST_HOST},${MSSQL_TEST_PORT};Database=TestDB;User Id=${MSSQL_TEST_USER};Password=${MSSQL_TEST_PASS}' AS db (TYPE mssql);
SELECT mssql_exec('db', 'SELECT @@OPTIONS');
"
```

Expected: Query succeeds and returns the options bitmask (should include bits for ANSI settings).

## Detailed Testing

### Test 1: Verify ANSI Options Are Set

```sql
ATTACH 'Server=localhost,1433;Database=TestDB;User Id=sa;Password=YourPassword' AS db (TYPE mssql);

-- Check @@OPTIONS bitmask
-- Expected bits set: 16 (ANSI_WARNINGS), 32 (ANSI_PADDING), 64 (ANSI_NULLS),
--                    512 (QUOTED_IDENTIFIER), 8192 (CONCAT_NULL_YIELDS_NULL)
-- Sum = 8816 (minimum expected)
SELECT * FROM mssql_scan('db', 'SELECT @@OPTIONS AS options_mask');
```

### Test 2: DDL Command Execution

```sql
-- This previously failed with SET options error
SELECT mssql_exec('db', 'ALTER DATABASE CURRENT SET RECOVERY SIMPLE');
-- Expected: Returns 0 (affected rows) without error
```

### Test 3: Connection Pool Reuse

```sql
-- Execute multiple DDL commands to test pool behavior
SELECT mssql_exec('db', 'SELECT 1');
SELECT mssql_exec('db', 'ALTER DATABASE CURRENT SET RECOVERY SIMPLE');
SELECT mssql_exec('db', 'SELECT 2');
SELECT mssql_exec('db', 'ALTER DATABASE CURRENT SET RECOVERY FULL');
```

### Test 4: With Debug Logging

```bash
set -a && source .env && set +a
MSSQL_DEBUG=1 ./build/release/duckdb -c "
ATTACH 'Server=${MSSQL_TEST_HOST},${MSSQL_TEST_PORT};Database=TestDB;User Id=${MSSQL_TEST_USER};Password=${MSSQL_TEST_PASS}' AS db (TYPE mssql);
SELECT mssql_exec('db', 'ALTER DATABASE CURRENT SET RECOVERY SIMPLE');
"
```

Look for log lines indicating ANSI initialization:
```
[MSSQL CONN] DoLogin7: initializing ANSI session options
[MSSQL CONN] InitializeAnsiSettings: success
```

## Running Integration Tests

```bash
# Run all tests
make test-all

# Or run specific integration test file (once it exists)
set -a && source .env && set +a
./build/release/test/unittest "test/sql/integration/ddl_ansi_settings.test" --force-reload
```

## Troubleshooting

### Error: SET options have incorrect settings

If you still see this error after applying the fix, check:

1. **Connection pool reuse**: The connection may have been reset. Ensure ANSI settings are re-applied on `RESET_CONNECTION`.

2. **Debug logging**: Enable `MSSQL_DEBUG=1` to see if `InitializeAnsiSettings` is being called.

3. **SQL Server response**: The SET command may have failed silently. Check for error tokens in debug output.

### Error: Connection failed during ANSI initialization

If connections fail after LOGIN7:

1. Check SQL Server logs for permission errors
2. Verify the SET statements are valid for your SQL Server version
3. Check network stability during the initialization phase

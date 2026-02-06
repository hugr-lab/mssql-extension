# Quickstart: Testing & Debugging Connection/FEDAUTH Changes

**Branch**: `031-connection-fedauth-refactor` | **Date**: 2026-02-06

## Debug Environment Setup

### Enable Debug Logging

```bash
# Level 1: Basic operations (pool acquire/release, auth steps)
export MSSQL_DEBUG=1

# Level 2: Verbose details (packet contents, cache state)
export MSSQL_DEBUG=2

# Level 3: Trace-level (per-row, memory ops)
export MSSQL_DEBUG=3
```

### Debug Prefixes

| Prefix | Component | Level |
|--------|-----------|-------|
| `[MSSQL POOL]` | Connection pool | 1 |
| `[MSSQL CONN]` | TDS connection | 1 |
| `[MSSQL BCP]` | Bulk copy | 1 |
| `[MSSQL STORAGE]` | ATTACH/DETACH | 1 |
| `[MSSQL CACHE]` | Metadata cache | 2 |

---

## Test Scenarios

### Bug 0.1: BCP State Corruption

**Setup**: Use `future-specs/setup_test.sql` for Azure Warehouse connection.

> **IMPORTANT**: Microsoft Fabric Data Warehouse does NOT support the TDS `INSERT BULK` protocol.
> BCP operations will always fail on Fabric with "INSERT is not a supported statement type".
> This test verifies that BCP failures don't corrupt connection pool state.

```sql
-- 1. Attach Azure Warehouse with FEDAUTH
ATTACH 'Server=...fabric.microsoft.com;Database=...' AS azurewh (
    TYPE mssql,
    AZURE_SECRET 'azure_cli'
);

-- 2. Trigger BCP failure (CTAS to Fabric which doesn't support INSERT BULK)
SET mssql_ctas_use_bcp=true;  -- Force BCP mode
CREATE TABLE azurewh.dbo.test_bcp AS
    SELECT 1 AS id, 'test' AS name;
-- Expected: Error "INSERT is not a supported statement type" (Fabric limitation)

-- 3. Verify recovery - this should NOT fail
SHOW ALL TABLES;
-- Expected: Lists tables without "Connection is not in idle state" error

-- 4. Check pool stats
FROM mssql_pool_stats();
-- Expected: active_connections = 0 (no stuck connections)
```

**Success Criteria**:
- Step 2 fails with clear error (expected - Fabric limitation)
- Step 3 succeeds (no state corruption from BCP failure)
- Step 4 shows `active_connections = 0`

---

### Bug 0.7: Fabric BCP Detection (NEW)

**Purpose**: Verify graceful handling of BCP on Fabric endpoints.

```sql
-- 1. Attach Fabric Warehouse
ATTACH 'Server=xyz.datawarehouse.fabric.microsoft.com;Database=warehouse' AS fabric (
    TYPE mssql,
    AZURE_SECRET 'azure_cli'
);

-- 2. CTAS with BCP (should auto-fallback or show clear error)
CREATE TABLE fabric.dbo.test_fallback AS
    SELECT 1 AS id, 'test' AS name;
-- Expected (after fix): Either succeeds with INSERT fallback, or clear error

-- 3. CTAS without BCP (should always work)
SET mssql_ctas_use_bcp=false;
CREATE TABLE fabric.dbo.test_insert AS
    SELECT 2 AS id, 'test2' AS name;
-- Expected: Succeeds

-- 4. Verify data
SELECT * FROM fabric.dbo.test_insert;
-- Expected: Returns row (2, 'test2')
```

**Success Criteria**:
- Clear error message explaining Fabric BCP limitation, OR
- Automatic fallback to INSERT mode with warning log

---

### Bug 0.2: Excessive Connection Acquires

**Setup**: Enable debug logging.

```bash
export MSSQL_DEBUG=1
```

```sql
-- 1. Attach and warm cache
ATTACH 'Server=localhost;Database=test;...' AS mssql (TYPE mssql);
SELECT * FROM mssql.dbo.SomeTable LIMIT 1;

-- 2. Run CTAS and count acquire messages
CREATE TABLE mssql.dbo.ctas_test AS
    SELECT * FROM mssql.dbo.SomeTable LIMIT 10;
```

**Count `[MSSQL POOL] Acquire` messages**:
- Before fix: 9+ acquires
- After fix: ≤3 acquires

---

### Bug 0.3: INSERT in Transaction

```sql
-- 1. Attach FEDAUTH database
ATTACH '...' AS azurewh (TYPE mssql, AZURE_SECRET 'azure_cli');

-- 2. Create test table
CREATE TABLE azurewh.dbo.tx_test (id INT, name VARCHAR(100));

-- 3. Test transaction INSERT
BEGIN;
INSERT INTO azurewh.dbo.tx_test VALUES (1, 'test');
COMMIT;
-- Expected: Success (no "Failed to acquire connection for schema lookup")

-- 4. Verify data
SELECT * FROM azurewh.dbo.tx_test;
```

---

### Bug 0.4: Token Expiration

**Note**: This requires waiting ~1 hour or mocking token expiration.

```sql
-- 1. Attach Azure database
ATTACH '...' AS azurewh (TYPE mssql, AZURE_SECRET 'azure_cli');

-- 2. Verify initial connection works
SELECT 1 FROM azurewh.dbo.SomeTable;

-- 3. Wait for token expiration (~1 hour) or mock expiration

-- 4. Run query - should auto-refresh token
SELECT 1 FROM azurewh.dbo.SomeTable;
-- Expected: Success after auto-refresh

-- 5. Detach and re-attach should get fresh token
DETACH azurewh;
ATTACH '...' AS azurewh (TYPE mssql, AZURE_SECRET 'azure_cli');
SELECT 1 FROM azurewh.dbo.SomeTable;
-- Expected: Success with fresh token
```

---

### Bug 0.5: FEDAUTH ATTACH Validation

```sql
-- Test 1: Invalid credentials should fail at ATTACH
CREATE SECRET bad_azure (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID 'invalid-tenant',
    CLIENT_ID 'invalid-client',
    CLIENT_SECRET 'invalid-secret'
);

ATTACH '...' AS test (TYPE mssql, AZURE_SECRET 'bad_azure');
-- Expected: Immediate authentication error (not deferred)

-- Test 2: Valid credentials should validate with SELECT 1
ATTACH '...' AS azurewh (TYPE mssql, AZURE_SECRET 'azure_cli');
-- Expected: Connection validated, no error
```

---

### Bug 0.6: Debug Output Control

```bash
# Without MSSQL_DEBUG - should be silent
unset MSSQL_DEBUG
./build/release/duckdb -c "ATTACH '...' AS mssql; SELECT 1 FROM mssql.dbo.t;"
# Expected: No [MSSQL POOL] messages

# With MSSQL_DEBUG=1 - should show pool activity
export MSSQL_DEBUG=1
./build/release/duckdb -c "ATTACH '...' AS mssql; SELECT 1 FROM mssql.dbo.t;"
# Expected: [MSSQL POOL] Acquire/Release messages visible
```

---

## Azure Test Environment

### Load Credentials

```bash
source .env  # Contains AZURE_APP_ID, AZURE_DIRECTORY_ID, etc.
```

### Test SQL Template

See `future-specs/setup_test.sql` for complete Azure connection examples:

```sql
-- Service Principal
CREATE SECRET azure_sp (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID '${AZURE_DIRECTORY_ID}',
    CLIENT_ID '${AZURE_APP_ID}',
    CLIENT_SECRET '${AZURE_APP_SECRET}'
);

-- Azure CLI (interactive, has full permissions)
CREATE SECRET azure_cli (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'cli'
);
```

---

## Build & Test Commands

```bash
# Build debug
make debug

# Run unit tests (no SQL Server needed)
make test

# Start SQL Server container
make docker-up

# Run integration tests
make integration-test

# Load extension in CLI
./build/release/duckdb
> INSTALL mssql FROM local_build_debug;
> LOAD mssql;
```

---

## Pool Stats Interpretation

```sql
FROM mssql_pool_stats();
```

| Column | Healthy Value | Problem Indicator |
|--------|---------------|-------------------|
| `active_connections` | 0 (when idle) | > 0 when no query running |
| `idle_connections` | 1-N | 0 if all connections busy |
| `acquire_timeout_count` | 0 | > 0 indicates pool exhaustion |
| `pinned_connections` | 0 (outside txn) | > 0 outside transaction |

---

## Troubleshooting

### "Connection is not in idle state"

1. Check pool stats for stuck active connections
2. Enable `MSSQL_DEBUG=1` and look for BCP errors without state reset
3. After fix: Connection should be closed on error, not returned to pool

### "Failed to acquire connection for schema lookup"

1. Check if running inside a transaction
2. Enable `MSSQL_DEBUG=1` to see if pinned connection is being used
3. After fix: Schema lookup should use transaction's pinned connection

### Token expiration failures

1. Check when token was last acquired (not currently logged)
2. Try `DETACH` + `ATTACH` to force new token
3. After fix: Auto-refresh should occur on auth failure

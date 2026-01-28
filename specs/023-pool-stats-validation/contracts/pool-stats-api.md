# Pool Statistics API Contract

**Feature**: 023-pool-stats-validation
**Date**: 2026-01-28

## Function: mssql_pool_stats()

### Signature

```sql
-- All pools
SELECT * FROM mssql_pool_stats();

-- Specific context
SELECT * FROM mssql_pool_stats('context_name');
```

### Output Schema

| Column | Type | Description |
|--------|------|-------------|
| `db` | VARCHAR | Database context name |
| `total_connections` | BIGINT | Total connections in pool (created - closed) |
| `idle_connections` | BIGINT | Connections available in idle queue |
| `active_connections` | BIGINT | Connections currently borrowed from pool |
| `pinned_connections` | BIGINT | **NEW**: Connections pinned to active transactions |
| `connections_created` | BIGINT | Lifetime count of connections created |
| `connections_closed` | BIGINT | Lifetime count of connections closed |
| `acquire_count` | BIGINT | Total number of acquire operations |
| `acquire_timeout_count` | BIGINT | Acquire operations that timed out |

### Invariants

```
pinned_connections <= active_connections
active_connections + idle_connections <= total_connections
total_connections = connections_created - connections_closed
```

### Example Output

```sql
SELECT * FROM mssql_pool_stats();
```

| db | total_connections | idle_connections | active_connections | pinned_connections | connections_created | connections_closed | acquire_count | acquire_timeout_count |
|----|-------------------|------------------|--------------------|--------------------|---------------------|--------------------|---------------|----------------------|
| sqlserver | 5 | 3 | 2 | 1 | 10 | 5 | 150 | 0 |

### Semantics

#### active_connections
- Connections that have been acquired via `Acquire()` but not yet released via `Release()`
- Includes connections actively executing queries
- Includes connections pinned to transactions

#### pinned_connections
- Subset of `active_connections` that are pinned to a transaction
- A connection becomes pinned when `MSSQLTransaction::SetPinnedConnection(conn)` is called
- A connection is unpinned when `SetPinnedConnection(nullptr)` is called (at commit/rollback)
- Pinned connections are held for the duration of the DuckDB transaction

#### idle_connections
- Connections in the idle queue waiting to be acquired
- Subject to idle timeout cleanup
- Not pinned, not actively executing

---

## Usage Examples

### Monitor Connection Usage

```sql
-- Check for potential connection leaks
SELECT
    db,
    active_connections,
    pinned_connections,
    active_connections - pinned_connections AS executing_queries
FROM mssql_pool_stats()
WHERE active_connections > 10;
```

### Verify Transaction Behavior

```sql
-- Start transaction
BEGIN;

-- Check pinned count (should increase)
SELECT pinned_connections FROM mssql_pool_stats('mydb');
-- Returns: 1

-- Query within transaction
SELECT * FROM mydb.dbo.mytable;

-- Check pinned count (still pinned)
SELECT pinned_connections FROM mssql_pool_stats('mydb');
-- Returns: 1

-- Commit
COMMIT;

-- Check pinned count (should decrease)
SELECT pinned_connections FROM mssql_pool_stats('mydb');
-- Returns: 0
```

### Health Check

```sql
-- Connection pool health summary
SELECT
    db,
    total_connections,
    CASE
        WHEN active_connections = total_connections THEN 'EXHAUSTED'
        WHEN idle_connections = 0 THEN 'BUSY'
        ELSE 'HEALTHY'
    END AS pool_status,
    acquire_timeout_count AS timeouts
FROM mssql_pool_stats();
```

---

## Behavior on Empty/Non-existent Context

```sql
-- Non-existent context returns empty result
SELECT * FROM mssql_pool_stats('nonexistent');
-- Returns: 0 rows
```

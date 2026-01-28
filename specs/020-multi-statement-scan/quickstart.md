# Quickstart: Support Multi-Statement SQL in mssql_scan

## What Changed

Two changes:
1. `MSSQLResultStream::Initialize()` now checks the `DONE_MORE` flag on DONE tokens. If more results follow, it continues reading instead of stopping — allowing multi-statement batches where earlier statements don't return columns.
2. Connections are reset via `sp_reset_connection` before being returned to the pool, clearing temp tables, session variables, and SET options. Only autocommit connections are reset on return; transaction-pinned connections are reset after commit/rollback.

## Files Modified

| File | Change |
| ---- | ------ |
| `src/query/mssql_result_stream.cpp` | Handle non-final DONE tokens in `Initialize()` — check `IsFinal()` and continue loop if more results expected |
| `src/connection/mssql_connection_provider.cpp` | Add `sp_reset_connection` before pool return in `ReleaseConnection()` (autocommit) and after commit/rollback (transactions) |

## Implementation

### Part 1: Multi-Statement Token Handling

In `src/query/mssql_result_stream.cpp`, modify the `ParsedTokenType::Done` case in `Initialize()`:

```cpp
case tds::ParsedTokenType::Done: {
    auto done = parser_.GetDone();
    // Check for errors accumulated from previous ERROR tokens
    if (!errors_.empty()) {
        state_ = MSSQLResultStreamState::Error;
        auto &err = errors_[0];
        throw InvalidInputException("SQL Server error [%d, severity %d]: %s",
                                    err.number, err.severity, err.message);
    }
    // If more results follow, continue looking for COLMETADATA
    if (!done.IsFinal()) {
        break;  // Continue the token loop
    }
    // Final DONE with no columns — empty result set
    state_ = MSSQLResultStreamState::Complete;
    return true;
}
```

### Part 2: Connection Reset on Pool Return

In `src/connection/mssql_connection_provider.cpp`, add reset before pool release in `ReleaseConnection()`:

```cpp
// Autocommit mode — reset connection state before returning to pool
if (!txn || is_autocommit) {
    // Reset session state (temp tables, variables, SET options)
    if (conn->IsAlive()) {
        conn->ExecuteAndDrain("exec sp_reset_connection");
    }
    auto &pool = catalog.GetConnectionPool();
    pool.Release(conn);
    return;
}
```

After commit/rollback in the transaction manager, before pool release:

```cpp
// Reset connection after transaction completes
if (pinned_conn && pinned_conn->IsAlive()) {
    pinned_conn->ExecuteAndDrain("exec sp_reset_connection");
}
pool.Release(pinned_conn);
```

## Verification

1. Build: `GEN=ninja make release`
2. Run tests: `make test && make integration-test`
3. Manual test:
   ```sql
   ATTACH 'mssql://...' AS db (TYPE mssql);
   FROM mssql_scan('db', 'select * into #t from dbo.test; select * from #t');
   ```

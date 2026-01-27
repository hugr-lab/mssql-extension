# Data Model: MSSQL Transactions

**Feature**: 001-mssql-transactions
**Date**: 2026-01-26

## Entities

### MSSQLTransaction (Modified)

Represents a DuckDB transaction with MSSQL-specific state.

**Current State**:
```cpp
class MSSQLTransaction : public Transaction {
    MSSQLCatalog &catalog_;
};
```

**New State**:
```cpp
class MSSQLTransaction : public Transaction {
    MSSQLCatalog &catalog_;

    // --- New fields for transaction support ---

    // Pinned SQL Server connection for this transaction
    // nullptr when not in transaction or autocommit mode
    std::shared_ptr<tds::TdsConnection> pinned_connection_;

    // Mutex for serializing concurrent operations on pinned connection
    mutable std::mutex connection_mutex_;

    // True if BEGIN TRANSACTION has been sent to SQL Server
    bool sql_server_transaction_active_ = false;

    // Counter for generating unique savepoint names
    uint32_t savepoint_counter_ = 0;
};
```

**Relationships**:
- 1:1 with `TdsConnection` (when pinned)
- 1:1 with `MSSQLCatalog`
- Managed by `MSSQLTransactionManager`

**State Transitions**:

```text
                    StartTransaction()
[No Transaction] ─────────────────────────> [Transaction Active]
                                                   │
                     ┌─────────────────────────────┼─────────────────────────────┐
                     │                             │                             │
                     ▼                             ▼                             ▼
             [First DML/Scan]             [Commit Requested]           [Rollback Requested]
                     │                             │                             │
                     │ Pin connection              │ COMMIT TRANSACTION          │ ROLLBACK TRANSACTION
                     │ BEGIN TRANSACTION           │ Release connection          │ Release connection
                     │                             │                             │
                     ▼                             ▼                             ▼
        [SQL Server Txn Active] ──────────> [No Transaction] <──────────────────┘
```

---

### MSSQLTransactionManager (Modified)

Manages transaction lifecycle for the MSSQL catalog.

**Current Implementation**:
- `StartTransaction()`: Creates `MSSQLTransaction`, no SQL Server interaction
- `CommitTransaction()`: No-op, removes transaction from map
- `RollbackTransaction()`: No-op, removes transaction from map

**New Implementation**:
- `StartTransaction()`: Creates `MSSQLTransaction`, defers SQL Server transaction start until first operation
- `CommitTransaction()`: If SQL Server transaction active, sends `COMMIT TRANSACTION`, returns connection to pool
- `RollbackTransaction()`: If SQL Server transaction active, sends `ROLLBACK TRANSACTION`, returns connection to pool

---

### ConnectionProvider (New)

Utility class for acquiring connections based on transaction context.

```cpp
namespace duckdb::mssql {

class ConnectionProvider {
public:
    // Acquire connection (pinned or from pool)
    static std::shared_ptr<tds::TdsConnection> GetConnection(
        ClientContext &context,
        MSSQLCatalog &catalog,
        int timeout_ms = -1
    );

    // Release connection (no-op if pinned, returns to pool otherwise)
    static void ReleaseConnection(
        ClientContext &context,
        MSSQLCatalog &catalog,
        std::shared_ptr<tds::TdsConnection> conn
    );

    // Check if in active MSSQL transaction
    static bool IsInTransaction(
        ClientContext &context,
        MSSQLCatalog &catalog
    );
};

}  // namespace duckdb::mssql
```

**Behavior Matrix**:

| Context State | GetConnection() | ReleaseConnection() |
|---------------|-----------------|---------------------|
| No transaction | Acquire from pool | Return to pool |
| Transaction, no pinned conn | Pin new connection from pool, BEGIN TRANSACTION | No-op (keeps pinned) |
| Transaction, conn pinned | Return pinned connection | No-op (keeps pinned) |

---

## Integration Points

### DML Executors

All DML executors (`MSSQLInsertExecutor`, `MSSQLUpdateExecutor`, `MSSQLDeleteExecutor`) must be modified to:

1. Replace direct `pool->Acquire()` with `ConnectionProvider::GetConnection()`
2. Replace `pool->Release()` with `ConnectionProvider::ReleaseConnection()`

**Change Pattern**:
```cpp
// Before:
auto conn = connection_pool_->Acquire(timeout_ms);
// ... execute ...
connection_pool_->Release(conn);

// After:
auto conn = ConnectionProvider::GetConnection(context_, catalog, timeout_ms);
// ... execute ...
ConnectionProvider::ReleaseConnection(context_, catalog, conn);
```

### mssql_scan Function

The `MSSQLQueryExecutor::Execute()` method must be modified to:

1. Use `ConnectionProvider::GetConnection()` instead of direct pool access
2. Pass the connection to `MSSQLResultStream` (already done)
3. Use `ConnectionProvider::ReleaseConnection()` in stream destructor

### mssql_exec Function

The `mssql_exec` scalar function in `mssql_functions.cpp` must be modified to:

1. Use `ConnectionProvider::GetConnection()` for connection acquisition
2. Use `ConnectionProvider::ReleaseConnection()` after execution

### Catalog Scan Bind

The `MSSQLCatalogScanBind()` function must add a check:

```cpp
// At start of bind function:
if (ConnectionProvider::IsInTransaction(context, catalog)) {
    throw BinderException(
        "MSSQL: reading attached tables/views inside DuckDB transactions "
        "is disabled (MVP). Use mssql_scan() instead."
    );
}
```

---

## Validation Rules

### Transaction State Invariants

1. **Connection exclusivity**: A pinned connection MUST NOT be used by any other transaction
2. **Transaction count**: `@@TRANCOUNT` on SQL Server MUST be 1 while transaction is active
3. **Clean return**: `@@TRANCOUNT` MUST be 0 before returning connection to pool
4. **Mutex protection**: All operations on pinned connection MUST acquire `connection_mutex_` first

### Error State Rules

1. **SQL Server error**: If SQL Server returns error, transaction remains active until explicit rollback
2. **Connection error**: If connection drops, transaction is implicitly rolled back (SQL Server behavior)
3. **Timeout**: Connection acquisition timeout throws exception, no transaction state change

---

## Memory Management

### Connection Ownership

```text
Pool owns:
  └── Idle connections (ConnectionMetadata.connection)

MSSQLTransaction owns (when pinned):
  └── pinned_connection_ (shared_ptr)
      └── Shared with executing operators temporarily

Executing Operator holds:
  └── shared_ptr copy during execution
      └── Released after operation completes
```

### Lifecycle

1. **Pool creates** connection on first `Acquire()` when pool is empty
2. **Transaction pins** connection on first MSSQL operation
3. **Transaction unpins** connection on commit/rollback
4. **Pool reclaims** connection when `Release()` is called
5. **Pool destroys** connection on idle timeout or shutdown

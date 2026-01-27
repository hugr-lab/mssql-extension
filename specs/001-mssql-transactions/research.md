# Research: MSSQL Transactions

**Feature**: 001-mssql-transactions
**Date**: 2026-01-26

## R1: DuckDB Transaction Lifecycle Hooks

### Decision
Use `MSSQLTransactionManager::StartTransaction()`, `CommitTransaction()`, and `RollbackTransaction()` as the integration points for SQL Server transaction management.

### Rationale
- DuckDB's `TransactionManager` provides well-defined callbacks for transaction lifecycle
- The existing `MSSQLTransactionManager` already implements these methods (currently as no-ops for read-only catalog)
- These callbacks are guaranteed to be invoked by DuckDB before/after transaction boundaries

### Alternatives Considered
1. **Hook into DML operators directly**: Rejected because it would require tracking "first DML" to start transaction, and there's no guaranteed callback for commit/rollback
2. **Create separate transaction tracking**: Rejected because it duplicates DuckDB's existing infrastructure

### Implementation Details
- `StartTransaction()`: Acquire connection from pool, execute `BEGIN TRANSACTION`, store in `MSSQLTransaction`
- `CommitTransaction()`: Execute `COMMIT TRANSACTION`, verify `@@TRANCOUNT = 0`, return connection to pool
- `RollbackTransaction()`: Execute `ROLLBACK TRANSACTION`, verify `@@TRANCOUNT = 0`, return connection to pool

---

## R2: Pinned Connection Storage

### Decision
Store the pinned connection as a `shared_ptr<TdsConnection>` member in `MSSQLTransaction`, along with a `std::mutex` for serialization.

### Rationale
- `MSSQLTransaction` is already bound to the DuckDB transaction lifecycle
- `shared_ptr` enables safe sharing with executing operators while transaction holds ownership
- Mutex ensures thread-safe access when multiple DML operations are in flight

### Alternatives Considered
1. **Store in thread-local**: Rejected because DuckDB may execute operators on different threads within the same transaction context
2. **Store in separate map keyed by ClientContext**: Rejected because `MSSQLTransaction` already has per-context semantics

### Data Model Addition
```cpp
class MSSQLTransaction : public Transaction {
private:
    // Existing
    MSSQLCatalog &catalog_;

    // New for transaction support
    std::shared_ptr<tds::TdsConnection> pinned_connection_;
    std::mutex connection_mutex_;
    bool sql_server_transaction_started_ = false;
};
```

---

## R3: Connection Provider Pattern

### Decision
Introduce `MSSQLConnectionProvider` utility class that returns either the pinned connection (if transaction active) or acquires from pool (autocommit mode).

### Rationale
- All DML executors, `mssql_scan`, and `mssql_exec` need the same logic: "use pinned if in transaction, else acquire from pool"
- Centralizing this logic prevents duplication and ensures consistent behavior
- The provider doesn't own connections; it returns shared_ptr that callers use temporarily

### Alternatives Considered
1. **Modify ConnectionPool interface**: Rejected because pool is context-agnostic and shouldn't know about DuckDB transactions
2. **Duplicate logic in each executor**: Rejected due to code duplication and consistency risks

### API Design
```cpp
namespace duckdb {
namespace mssql {

class ConnectionProvider {
public:
    // Get connection for the current context
    // Returns pinned connection if in transaction, or acquires from pool
    static std::shared_ptr<tds::TdsConnection> GetConnection(
        ClientContext &context,
        MSSQLCatalog &catalog,
        int timeout_ms = -1
    );

    // Release connection back to pool (no-op if connection is pinned)
    static void ReleaseConnection(
        ClientContext &context,
        MSSQLCatalog &catalog,
        std::shared_ptr<tds::TdsConnection> conn
    );

    // Check if context is in an active MSSQL transaction
    static bool IsInTransaction(ClientContext &context, MSSQLCatalog &catalog);
};

}  // namespace mssql
}  // namespace duckdb
```

---

## R4: Catalog Scan Restriction Enforcement

### Decision
Check for active DuckDB transaction in the table scan bind phase (`MSSQLCatalogScanBind`) and throw an error if detected.

### Rationale
- Bind phase is early enough to fail fast before any SQL Server query is prepared
- The bind context has access to `ClientContext` which provides transaction state
- Clear error message guides users to use `mssql_scan()` instead

### Alternatives Considered
1. **Check in InitGlobal**: Rejected because bind phase is earlier and fails faster
2. **Check in physical operator**: Rejected because planning has already completed by then

### Error Message
```
MSSQL: reading attached tables/views inside DuckDB transactions is disabled (MVP). Use mssql_scan() instead.
```

---

## R5: SQL Server Transaction Commands

### Decision
Use explicit T-SQL transaction commands: `BEGIN TRANSACTION`, `COMMIT TRANSACTION`, `ROLLBACK TRANSACTION`.

### Rationale
- Explicit commands are clearer and more debuggable than implicit modes
- SQL Server's default isolation level (READ COMMITTED) is acceptable for MVP
- These commands are standard T-SQL and work reliably over TDS

### Transaction State Verification
Before returning connection to pool, verify clean state:
```sql
SELECT @@TRANCOUNT
```
- If result > 0, issue `ROLLBACK TRANSACTION` and log warning
- This prevents pool corruption from orphaned transactions

---

## R6: Savepoint Support Strategy (MVP)

### Decision
Implement savepoints as a SHOULD requirement; if not implemented in initial MVP, throw clear error.

### Rationale
- DuckDB may use savepoints internally for error recovery
- SQL Server supports savepoints via `SAVE TRANSACTION <name>` and `ROLLBACK TRANSACTION <name>`
- Can be stubbed initially with graceful degradation

### Savepoint Mapping
- DuckDB savepoint → `SAVE TRANSACTION sp_<counter>`
- Rollback to savepoint → `ROLLBACK TRANSACTION sp_<counter>`
- Release savepoint → No-op (SQL Server has no explicit release)

### Stub Error (if deferred)
```
MSSQL: Savepoints are not yet supported in transactions (MVP). Use explicit COMMIT or ROLLBACK instead.
```

---

## R7: DDL Behavior in Transactions

### Decision
Execute DDL operations on a separate autocommit connection, not the pinned transaction connection.

### Rationale
- SQL Server DDL can cause implicit commits or locking issues
- Keeping DDL separate prevents unexpected transaction commits
- This matches the spec requirement (FR-014)

### Cache Invalidation
After DDL execution, call `catalog.GetMetadataCache().Invalidate()` to refresh table/view metadata.

---

## R8: Error Handling Strategy

### Decision
On SQL Server errors during transaction operations:
1. Propagate error to DuckDB
2. Keep connection pinned (user must explicitly rollback)
3. Mark transaction state as "error pending rollback"

### Rationale
- Users should see the actual SQL Server error
- Automatic rollback could hide errors or cause data loss expectations
- Explicit rollback requirement matches PostgreSQL and other DBMS behavior

### Connection State After Error
- Transaction remains active on SQL Server until explicit `ROLLBACK`
- Pinned connection stays pinned until DuckDB rollback
- Subsequent DML on the connection will fail (SQL Server transaction is in error state)

---

## Summary of Decisions

| Area | Decision |
|------|----------|
| Transaction hooks | Use existing `MSSQLTransactionManager` callbacks |
| Pinned storage | `shared_ptr<TdsConnection>` + `mutex` in `MSSQLTransaction` |
| Connection logic | New `ConnectionProvider` utility class |
| Catalog restriction | Check in table scan bind phase |
| SQL commands | Explicit `BEGIN/COMMIT/ROLLBACK TRANSACTION` |
| Savepoints | Implement or stub with clear error |
| DDL behavior | Separate autocommit connection |
| Error handling | Propagate error, keep pinned until explicit rollback |

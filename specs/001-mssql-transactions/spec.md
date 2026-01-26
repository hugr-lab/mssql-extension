# Feature Specification: MSSQL Transactions (DuckDB-Compatible MVP)

**Feature Branch**: `001-mssql-transactions`
**Created**: 2026-01-26
**Status**: Draft
**Input**: User description: "Spec 05.05 — MSSQL Transactions: Provide a correct and predictable transaction model for the DuckDB MSSQL extension by mapping DuckDB transactions to SQL Server transactions using a pinned connection strategy."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - DML Transaction Commit (Priority: P1)

A user wants to perform multiple related INSERT, UPDATE, and DELETE operations on SQL Server tables and have them all commit atomically, ensuring data consistency.

**Why this priority**: Atomic DML operations are the core value proposition of transaction support. Without this, users cannot safely perform multi-statement data modifications.

**Independent Test**: Can be fully tested by running `BEGIN; INSERT...; UPDATE...; DELETE...; COMMIT;` and verifying all changes persist atomically.

**Acceptance Scenarios**:

1. **Given** a DuckDB session with an attached MSSQL database, **When** the user executes `BEGIN; INSERT INTO mssql.dbo.orders VALUES (1, 'A'); INSERT INTO mssql.dbo.order_items VALUES (1, 100); COMMIT;`, **Then** both rows are visible in SQL Server after commit.

2. **Given** a DuckDB transaction is active, **When** the user executes multiple DML statements on the same SQL Server table, **Then** all statements execute on a single pinned SQL Server connection/session.

3. **Given** a DuckDB transaction with pending DML changes, **When** COMMIT is executed, **Then** SQL Server's `COMMIT TRANSACTION` is sent before the connection is returned to the pool.

---

### User Story 2 - DML Transaction Rollback (Priority: P1)

A user wants to undo all DML changes made within a transaction when an error occurs or when explicitly rolling back.

**Why this priority**: Rollback capability is essential for error recovery and maintaining data integrity.

**Independent Test**: Can be fully tested by running `BEGIN; INSERT...; ROLLBACK;` and verifying the inserted row does not exist.

**Acceptance Scenarios**:

1. **Given** a DuckDB transaction with inserted rows, **When** the user executes `ROLLBACK`, **Then** none of the inserted rows are visible in SQL Server.

2. **Given** a DuckDB transaction with UPDATE and DELETE operations, **When** ROLLBACK is executed, **Then** all modified data returns to its original state.

3. **Given** a pinned connection with an active transaction, **When** ROLLBACK completes, **Then** `@@TRANCOUNT` on SQL Server equals 0 before returning the connection to the pool.

---

### User Story 3 - Read-Your-Writes via mssql_scan (Priority: P2)

A user executing DML within a DuckDB transaction wants to verify their changes immediately using `mssql_scan()` before committing.

**Why this priority**: Read-your-writes capability enables data validation within transactions, which is critical for complex workflows but secondary to basic DML atomicity.

**Independent Test**: Can be fully tested by `BEGIN; INSERT...; SELECT * FROM mssql_scan(...);` and verifying the inserted row is visible in the scan results.

**Acceptance Scenarios**:

1. **Given** a DuckDB transaction with an INSERT executed, **When** the user calls `mssql_scan('ctx', 'SELECT * FROM dbo.table WHERE id = @inserted_id')`, **Then** the newly inserted row is visible.

2. **Given** a DuckDB transaction with uncommitted DML, **When** `mssql_scan` is executed, **Then** the scan runs on the same pinned SQL Server connection as the DML.

3. **Given** a DuckDB transaction with uncommitted changes visible via mssql_scan, **When** ROLLBACK is executed, **Then** subsequent mssql_scan (outside the transaction) no longer shows those changes.

---

### User Story 4 - Execute DDL/DML via mssql_exec in Transaction (Priority: P2)

A user wants to execute arbitrary T-SQL statements (DDL or DML) via `mssql_exec()` within a DuckDB transaction and have them participate in the transaction.

**Why this priority**: Enables advanced use cases like temp table creation and complex T-SQL that cannot be expressed through catalog-integrated DML.

**Independent Test**: Can be fully tested by `BEGIN; SELECT mssql_exec(..., 'UPDATE...'); COMMIT;` and verifying the update persists.

**Acceptance Scenarios**:

1. **Given** a DuckDB transaction is active, **When** `mssql_exec('ctx', 'CREATE TABLE #temp (id INT)')` is executed, **Then** the temp table exists on the pinned connection and is accessible by subsequent operations.

2. **Given** a DuckDB transaction with mssql_exec-created temp table, **When** COMMIT is executed, **Then** the temp table is dropped when the connection is returned (SQL Server session ends).

3. **Given** a DuckDB transaction is active, **When** `mssql_exec('ctx', 'UPDATE dbo.t SET col=1 WHERE id=5')` is executed, **Then** the update participates in the transaction (visible to mssql_scan, rolled back on ROLLBACK).

---

### User Story 5 - Catalog Scan Restriction (MVP) (Priority: P2)

The system prevents users from executing catalog-based table scans (`SELECT * FROM mssql.schema.table`) inside DuckDB transactions to avoid consistency issues.

**Why this priority**: Clear error messaging prevents user confusion and data consistency problems in the MVP.

**Independent Test**: Can be fully tested by `BEGIN; SELECT * FROM mssql.dbo.table;` and verifying an error is thrown.

**Acceptance Scenarios**:

1. **Given** a DuckDB transaction is active, **When** the user executes `SELECT * FROM mssql.dbo.sometable`, **Then** an error is thrown: "MSSQL: reading attached tables/views inside DuckDB transactions is disabled (MVP). Use mssql_scan() instead."

2. **Given** a DuckDB transaction is active, **When** the user attempts to read from an MSSQL view via catalog syntax, **Then** the same restriction error is thrown.

3. **Given** no DuckDB transaction is active (autocommit mode), **When** the user executes `SELECT * FROM mssql.dbo.sometable`, **Then** the query succeeds normally.

---

### User Story 6 - Autocommit Mode Behavior (Priority: P3)

When not in an explicit transaction, DML operations execute immediately with SQL Server's implicit autocommit.

**Why this priority**: Autocommit is the default mode and must continue working; it's lower priority because it's existing behavior that should be preserved.

**Independent Test**: Can be fully tested by executing `INSERT INTO mssql.dbo.table VALUES(...)` without BEGIN and verifying immediate persistence.

**Acceptance Scenarios**:

1. **Given** no explicit DuckDB transaction, **When** an INSERT is executed, **Then** the row is immediately committed on SQL Server.

2. **Given** autocommit mode, **When** multiple DML statements are executed in sequence, **Then** each statement may use a different connection from the pool.

3. **Given** autocommit mode, **When** SELECT queries are executed on catalog tables, **Then** they succeed without restriction.

---

### Edge Cases

- What happens when a transaction is abandoned (e.g., session disconnect)?
  - The pinned connection detects the orphaned state and issues a ROLLBACK before returning to the pool.

- What happens when COMMIT fails (e.g., constraint violation)?
  - The error is propagated to DuckDB; the connection remains pinned until the user issues ROLLBACK.

- What happens when concurrent DML operations are attempted on the pinned connection?
  - Operations are serialized using a mutex to prevent corruption.

- What happens when the pool is exhausted during transaction start?
  - Connection acquisition blocks (up to configured timeout), then throws an error if timeout expires.

- What happens when a savepoint rollback fails?
  - Error is propagated; the transaction remains active; user must issue full ROLLBACK or retry.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST acquire and pin a SQL Server connection from the pool when a DuckDB transaction begins (`BEGIN`).

- **FR-002**: System MUST execute `BEGIN TRANSACTION` on the pinned SQL Server connection when the DuckDB transaction starts.

- **FR-003**: System MUST execute `COMMIT TRANSACTION` on the pinned connection when DuckDB commits, then return the connection to the pool.

- **FR-004**: System MUST execute `ROLLBACK TRANSACTION` on the pinned connection when DuckDB rolls back, then return the connection to the pool.

- **FR-005**: System MUST verify `@@TRANCOUNT = 0` before returning a connection to the pool; if not zero, MUST issue ROLLBACK and log an error.

- **FR-006**: System MUST use the pinned connection for all DML operations (INSERT, UPDATE, DELETE) executed during the transaction.

- **FR-007**: System MUST use the pinned connection for `mssql_scan()` calls executed during a DuckDB transaction.

- **FR-008**: System MUST use the pinned connection for `mssql_exec()` calls executed during a DuckDB transaction.

- **FR-009**: System MUST throw an error when catalog-based table/view scans are attempted during an active DuckDB transaction.

- **FR-010**: System MUST allow catalog-based table/view scans when no DuckDB transaction is active (autocommit mode).

- **FR-011**: System MUST serialize concurrent operations on a pinned connection using a mutex.

- **FR-012**: System SHOULD support savepoints (`SAVE TRANSACTION`, `ROLLBACK TRANSACTION <name>`) for DuckDB savepoint operations.

- **FR-013**: System SHOULD reset session state (if modified) before returning a connection to the pool.

- **FR-014**: System MUST execute DDL operations (CREATE, ALTER, DROP) using a separate autocommit connection, not the pinned transaction connection.

- **FR-015**: System MUST invalidate metadata caches after DDL execution during an active transaction.

### Key Entities *(include if feature involves data)*

- **MSSQLTransaction**: Extension transaction object bound to DuckDB's transaction lifecycle; stores the pinned connection and mutex for serialization.

- **PinnedConnection**: A SQL Server TDS connection acquired from the pool and exclusively assigned to a DuckDB transaction for its lifetime.

- **ConnectionPool**: Existing pool implementation that provides connection acquisition/release with timeout support.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Multi-statement DML transactions (INSERT, UPDATE, DELETE) commit atomically—either all changes persist or none do.

- **SC-002**: Transaction rollback undoes all changes made within the transaction.

- **SC-003**: `mssql_scan()` within a transaction returns results consistent with uncommitted DML operations (read-your-writes).

- **SC-004**: `mssql_exec()` within a transaction participates in the transaction scope.

- **SC-005**: Catalog-based scans inside a transaction produce a clear, actionable error message.

- **SC-006**: Connection pool remains healthy after transaction completion—no leaked connections or orphaned transactions.

- **SC-007**: Concurrent DML operations within a transaction do not cause data corruption or crashes.

- **SC-008**: Existing autocommit behavior remains unchanged for non-transactional operations.

## Assumptions

- DuckDB's transaction lifecycle callbacks (StartTransaction, CommitTransaction, RollbackTransaction) are reliable and always invoked.
- The existing connection pool can handle the "pinned" pattern where a connection is held for the duration of a transaction.
- SQL Server's `BEGIN TRANSACTION`, `COMMIT TRANSACTION`, and `ROLLBACK TRANSACTION` commands work reliably over TDS.
- Single-threaded execution within a DuckDB transaction is acceptable for the MVP (MaxThreads = 1).
- Savepoints may be stubbed with clear error messages in the MVP if full implementation is deferred.

## Out of Scope (MVP)

- Transaction-level read consistency for catalog scans (requires complex multi-connection coordination).
- Parallel MSSQL scans inside a DuckDB transaction (requires multiple pinned connections).
- Cross-connection distributed transactions (XA/2PC).
- Configurable isolation levels (READ COMMITTED, SERIALIZABLE, etc.).
- Nested transactions beyond savepoints.

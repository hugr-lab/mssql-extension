# Feature Specification: Support Multi-Statement SQL in mssql_scan

**Feature Branch**: `020-multi-statement-scan`
**Created**: 2026-01-27
**Status**: Draft
**Input**: User description: "Support multi-statement SQL in mssql_scan table function"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Temp Table Queries via mssql_scan (Priority: P1)

A DuckDB user wants to execute multi-statement SQL Server batches through `mssql_scan()` where earlier statements prepare data (e.g., populating temp tables) and a later statement returns the result set. Currently, if the first statement in the batch does not produce columns (e.g., `SELECT INTO`, `INSERT`, `CREATE TABLE`), the function fails with an internal error: "Table function must return at least one column." The user expects the function to skip non-result-producing statements and return the columns from the first statement that produces a result set.

**Why this priority**: This is the core use case — temp table patterns are extremely common in SQL Server workflows. Without this, users cannot use `mssql_scan` for any multi-step data preparation query.

**Independent Test**: Execute `FROM mssql_scan('db', 'select * into #t from dbo.test; select * from #t')` and verify it returns the expected rows and columns.

**Acceptance Scenarios**:

1. **Given** an attached MSSQL database, **When** a user runs `mssql_scan` with a batch containing `SELECT INTO` followed by `SELECT`, **Then** the result set from the `SELECT` statement is returned with correct columns and data.
2. **Given** an attached MSSQL database, **When** a user runs `mssql_scan` with a batch containing `CREATE TABLE #t (...); INSERT INTO #t ...; SELECT * FROM #t`, **Then** the result set from the final `SELECT` is returned.
3. **Given** an attached MSSQL database, **When** a user runs `mssql_scan` with a single `SELECT` statement (no multi-statement), **Then** behavior is identical to before — no regression.

---

### User Story 2 - DML Followed by SELECT (Priority: P2)

A DuckDB user runs a batch where earlier statements perform DML operations (INSERT, UPDATE, DELETE) and a later statement queries the results. The function skips the DML statements and returns the result set from the query.

**Why this priority**: DML-then-query is a common pattern but less frequent than temp table usage.

**Independent Test**: Execute `FROM mssql_scan('db', 'UPDATE dbo.test SET col=1 WHERE id=5; SELECT * FROM dbo.test WHERE id=5')` and verify it returns the updated row.

**Acceptance Scenarios**:

1. **Given** an attached MSSQL database, **When** a user runs `mssql_scan` with an `UPDATE` followed by a `SELECT`, **Then** the result set from the `SELECT` is returned.
2. **Given** an attached MSSQL database, **When** a user runs `mssql_scan` with an `INSERT` followed by a `SELECT`, **Then** the result set from the `SELECT` is returned.

---

### User Story 3 - Error in Intermediate Statement (Priority: P1)

A DuckDB user runs a multi-statement batch where an intermediate statement fails (e.g., referencing a non-existent table). The system reports the error clearly rather than silently skipping it or returning unexpected results.

**Why this priority**: Error handling is critical for correctness — users must know when part of their batch fails.

**Independent Test**: Execute `FROM mssql_scan('db', 'SELECT * FROM nonexistent_table; SELECT 1 AS x')` and verify an error is reported.

**Acceptance Scenarios**:

1. **Given** an attached MSSQL database, **When** a user runs `mssql_scan` with a batch where an intermediate statement produces an error, **Then** the error is reported to the user with the SQL Server error message.
2. **Given** an attached MSSQL database, **When** all statements in the batch fail, **Then** the first error is reported to the user.

---

### User Story 4 - No Result Set in Entire Batch (Priority: P2)

A DuckDB user runs a multi-statement batch where no statement produces a result set (e.g., all DML or DDL). The system returns gracefully rather than hanging or crashing.

**Why this priority**: Edge case handling — users should get a clear response even for pure DML batches.

**Independent Test**: Execute `FROM mssql_scan('db', 'CREATE TABLE #t (x INT); DROP TABLE #t')` and verify the function returns gracefully.

**Acceptance Scenarios**:

1. **Given** an attached MSSQL database, **When** a user runs `mssql_scan` with a batch where no statement returns columns, **Then** the function completes without error and returns an empty result set (zero columns, zero rows).

---

### User Story 5 - Connection Cleanup on Pool Return (Priority: P1)

When a connection in autocommit mode is returned to the pool after executing a query, session-scoped artifacts (temp tables, session variables, SET options, open cursors) persist on the connection. The next user of that pooled connection would inherit these artifacts, causing unexpected behavior. The system must reset the connection state before returning it to the pool.

**Important**: Connection reset MUST only happen on pool return in autocommit mode. Connections pinned to an explicit transaction MUST NOT be reset — they retain their session state until the transaction is committed or rolled back. The reset happens after commit/rollback, when the connection is finally released back to the pool.

**Why this priority**: Without cleanup, temp tables from one query leak into the next user's session — a correctness and security issue. The standard SQL Server mechanism for this is a session reset command used by ADO.NET, JDBC, and ODBC drivers.

**Independent Test**: Execute `mssql_scan('db', 'select * into #t from dbo.test; select * from #t')`, then on the next query using the same pooled connection, verify that `#t` does not exist.

**Acceptance Scenarios**:

1. **Given** a pooled connection in autocommit mode that just executed a batch creating temp tables, **When** the connection is returned to the pool, **Then** session state is reset (temp tables dropped, variables cleared, SET options restored to defaults).
2. **Given** a pooled connection in autocommit mode that just executed a batch with `SET NOCOUNT ON`, **When** a different user acquires that connection from the pool, **Then** `NOCOUNT` is back to its default (OFF).
3. **Given** a connection pinned to an explicit transaction, **When** the connection is used for multiple queries within the transaction, **Then** session state (temp tables, variables) is preserved throughout the transaction.
4. **Given** a connection pinned to an explicit transaction, **When** the transaction is committed or rolled back and the connection is returned to the pool, **Then** session state is reset before the connection becomes available to other users.
5. **Given** a pooled connection, **When** the reset operation itself fails, **Then** the connection is discarded from the pool rather than returned in a dirty state.

---

### Edge Cases

- What happens when the result-producing statement is in the middle of the batch (not the last statement)? The system should return columns from the first result-producing statement.
- What happens with very large batches (10+ statements)? The system should handle them without timeout issues for the non-result statements.
- What happens when a single-statement DML query (e.g., `UPDATE ...`) is passed to `mssql_scan`? Behavior should remain unchanged — empty result, no error.
- What happens when the batch includes `SET NOCOUNT ON`? The system should handle it correctly since `SET NOCOUNT ON` suppresses row count messages.
- What happens when the connection reset itself fails (e.g., broken connection)? The connection should be discarded from the pool, not returned dirty.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: When the system receives a multi-statement batch, it MUST continue processing protocol responses beyond the first non-result-producing statement if more results are expected.
- **FR-002**: The system MUST return columns and data from the first statement in the batch that produces a result set (has column metadata).
- **FR-003**: If an error occurs in any statement before a result set is found, the system MUST report that error to the user with the server-provided error message and error number.
- **FR-004**: If no statement in the batch produces a result set and no errors occur, the system MUST return an empty result without crashing or hanging.
- **FR-005**: Single-statement queries that return columns MUST behave identically to before this change — zero regression.
- **FR-006**: Single-statement DML queries (INSERT/UPDATE/DELETE) that return no columns MUST behave identically to before this change.
- **FR-007**: The system MUST correctly handle intermediate protocol messages (informational messages, environment changes, row counts) between statements without treating them as errors.
- **FR-008**: When a connection in autocommit mode is returned to the pool, the system MUST reset the connection's session state to clear temp tables, session variables, SET options, and other session-scoped artifacts. Connections pinned to an explicit transaction MUST NOT be reset until after commit or rollback.
- **FR-009**: If the connection reset fails, the system MUST discard the connection from the pool rather than returning it in a dirty state.
- **FR-010**: After an explicit transaction is committed or rolled back, the connection MUST be reset before being returned to the pool.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can execute `SELECT INTO` followed by `SELECT` via `mssql_scan` and receive correct results — zero failures for this common pattern.
- **SC-002**: All existing tests continue to pass with zero regressions (108 test cases, 2741 assertions).
- **SC-003**: Errors in intermediate statements are reported with the original SQL Server error message — users see actionable error information.
- **SC-004**: Batches with no result-producing statements complete without crashes or hangs.
- **SC-005**: Temp tables created in one query do not leak to subsequent queries on the same pooled connection — session state is fully reset between uses.

## Assumptions

- SQL Server sends a completion marker with a "more results" flag between statements in a batch. The final statement's completion marker does not have this flag.
- The existing protocol parser already distinguishes between final and non-final completion markers.
- The `mssql_exec` function (for executing DML without returning results) is not affected by this change, but may benefit from the same underlying improvement.
- Temp tables created in a batch are visible to subsequent statements in the same batch because they share the same connection and session.

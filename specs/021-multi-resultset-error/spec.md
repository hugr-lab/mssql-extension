# Feature Specification: Improve Multi-Resultset Error Messages in mssql_scan

**Feature Branch**: `021-multi-resultset-error`
**Created**: 2026-01-28
**Status**: Draft
**Input**: User description: "Multistatement SQL through mssql_scan is failing with an internal error when multiple statements return different columns"

## Clarifications

### Session 2026-01-28

- Q: Should rows from subsequent identical-schema result sets be appended, or should only the first result set be returned? → A: Return only the first result set; throw a clear error if any subsequent result set is encountered, regardless of schema match.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Clear Error on Multiple Result Sets (Priority: P1)

A DuckDB user executes a multi-statement SQL batch via `mssql_scan()` where more than one statement produces a result set. Currently, if the result sets have different column schemas, the system crashes with an internal error (`Expected vector of type INT16, but found vector of type INT32`). The user expects a clear, actionable error message explaining that the batch produced multiple result sets and only the first result set can be returned. The system should reject any batch that produces more than one result set, regardless of whether the schemas match.

**Why this priority**: This is the primary bug — an internal crash with no useful error message is the worst user experience. Users need to understand what went wrong and how to fix their query.

**Independent Test**: Execute `FROM mssql_scan('db', 'SELECT * FROM dbo.test; SELECT ''hello''')` and verify the system returns a clear error message instead of an internal crash.

**Acceptance Scenarios**:

1. **Given** an attached MSSQL database, **When** a user runs `mssql_scan` with a batch where two or more statements return result sets (regardless of schema), **Then** the system reports a clear error message indicating that the batch produced multiple result sets and only one result-producing statement is allowed.
2. **Given** an attached MSSQL database, **When** a user runs `mssql_scan` with a batch where an intermediate non-result statement is followed by a result statement (e.g., `SELECT INTO #t ...; SELECT * FROM #t; SELECT 'extra'`), **Then** the system returns the first result set's data and reports an error about the subsequent result set.
3. **Given** an attached MSSQL database, **When** a user runs `mssql_scan` with a batch where only one statement produces a result set (others are DML/DDL), **Then** the system works correctly without error — no regression.

---

### User Story 2 - Connection Remains Clean After Error (Priority: P2)

A DuckDB user triggers the multiple-result-set error. After the error, the connection must remain in a clean state so subsequent queries on the same pooled connection work correctly.

**Why this priority**: Connection state corruption would affect all subsequent queries, not just the failing one.

**Independent Test**: Execute a multi-result-set batch that triggers the error, then run a normal query on the same connection and verify it succeeds.

**Acceptance Scenarios**:

1. **Given** an attached MSSQL database, **When** a user triggers the multiple-result-set error and then runs a subsequent query, **Then** the subsequent query succeeds without connection errors.
2. **Given** an attached MSSQL database, **When** the error is triggered, **Then** remaining TDS tokens from the batch are drained before the connection is returned to the pool.

---

### Edge Cases

- What happens when two result sets have the same schema? The system should still report an error — only one result-producing statement is allowed per batch.
- What happens when a batch produces many result sets (5+)? The system should detect the second result set and report an error immediately.
- What happens when a DML statement (no result set) appears between two result-producing statements? The DML's DONE token should be skipped correctly (spec 020), and the error is raised when the second COLMETADATA is encountered.
- What happens when the second result set's COLMETADATA arrives mid-chunk (during FillChunk)? The system should detect it and raise the error cleanly.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: When the system encounters a second COLMETADATA token (indicating a new result set) during row streaming, it MUST report a clear error — never crash with an internal type assertion.
- **FR-002**: The error message MUST clearly state that the SQL batch produced multiple result sets and only one result-producing statement is allowed per batch.
- **FR-003**: When a second result set is detected, the system MUST drain remaining TDS tokens from the connection to leave it in a clean state for reuse.
- **FR-004**: Single-statement queries and multi-statement batches with only one result-producing statement MUST continue to work identically to before — zero regression.
- **FR-005**: The system MUST reject any batch producing more than one result set, regardless of whether the result sets have identical or different schemas.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users who execute multi-statement batches with multiple result sets see a descriptive error message instead of an internal crash — zero internal assertion failures for this scenario.
- **SC-002**: All existing tests continue to pass with zero regressions.
- **SC-003**: The error message includes guidance that helps the user fix the query (e.g., ensuring only one statement in the batch returns results).
- **SC-004**: After a multiple-result-set error, the connection remains usable for subsequent queries — no connection state corruption.

## Assumptions

- The TDS protocol sends a new COLMETADATA token before rows of each result set in a batch. This is the signal that a new result set is starting.
- The `FillChunk` method in `MSSQLResultStream` processes rows and may encounter a second COLMETADATA token mid-stream if a subsequent statement returns results.
- The current crash occurs because `ProcessRow` uses the original column metadata (types, sizes) to write into DuckDB vectors, but the second result set's row data has different types/sizes, causing a type assertion failure in DuckDB's vector system.
- Only one result-producing statement per batch is supported. Users who need multiple result sets should use separate `mssql_scan` calls.

# Feature Specification: ANSI Connection Options Fix

**Feature Branch**: `028-ansi-connection-options`
**Created**: 2026-02-03
**Status**: Draft
**Input**: User description: "Fix ANSI SET options for DDL commands - GitHub issue #41"
**GitHub Issue**: https://github.com/hugr-lab/mssql-extension/issues/41

## Problem Statement

When executing DDL commands (ALTER DATABASE, DBCC, BACKUP LOG) via `mssql_scan()` or `mssql_exec()`, SQL Server returns error 1934 about incorrect SET options:

```
SELECT failed because the following SET options have incorrect settings:
'CONCAT_NULL_YIELDS_NULL, ANSI_WARNINGS'
```

This occurs because the TDS connection does not initialize the required ANSI session options that SQL Server expects for certain operations.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Execute DDL Commands Successfully (Priority: P1)

As a database administrator using DuckDB with the MSSQL extension, I want to execute administrative DDL commands (ALTER DATABASE, DBCC, etc.) so that I can manage SQL Server databases from DuckDB.

**Why this priority**: This is the core bug fix - without it, users cannot perform database administration tasks through the extension.

**Independent Test**: Can be fully tested by executing `ALTER DATABASE CURRENT SET RECOVERY SIMPLE` via `mssql_exec()` and verifying it completes without SET options error.

**Acceptance Scenarios**:

1. **Given** a connected MSSQL database, **When** user executes `SELECT mssql_exec('db', 'ALTER DATABASE CURRENT SET RECOVERY SIMPLE')`, **Then** the command completes successfully without SET options error
2. **Given** a connected MSSQL database, **When** user executes `SELECT mssql_exec('db', 'DBCC SHRINKFILE (2, 64)')`, **Then** the command completes successfully
3. **Given** a connected MSSQL database, **When** user runs any query against indexed views or computed columns, **Then** the query executes without SET options errors

---

### User Story 2 - Connection Pool Maintains ANSI Settings (Priority: P2)

As a user executing multiple queries, I want connection pool connections to maintain proper ANSI settings even after being returned to the pool and reused.

**Why this priority**: Ensures consistent behavior across connection reuse, which is critical for production workloads.

**Independent Test**: Can be tested by executing DDL commands, returning connection to pool, then executing another DDL command on a reused connection.

**Acceptance Scenarios**:

1. **Given** a connection that was used and returned to pool, **When** the connection is reused for a DDL command, **Then** ANSI settings are still active and DDL succeeds
2. **Given** a connection with RESET_CONNECTION flag applied, **When** the connection is used for a DDL command, **Then** ANSI settings are re-applied and DDL succeeds

---

### Edge Cases

- What happens when SQL Server rejects a SET statement (permissions, version incompatibility)? The connection should fail with a clear error message.
- How does the system handle connection timeout during ANSI initialization? Standard connection timeout handling applies.
- What happens if ANSI settings are partially applied (network interruption mid-batch)? Connection is marked as failed and not returned to pool.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST send ANSI SET statements after successful TDS LOGIN7 authentication
- **FR-002**: System MUST set the following options on every new connection:
  - `SET CONCAT_NULL_YIELDS_NULL ON`
  - `SET ANSI_WARNINGS ON`
  - `SET ANSI_NULLS ON`
  - `SET ANSI_PADDING ON`
  - `SET QUOTED_IDENTIFIER ON`
- **FR-003**: System MUST apply ANSI settings before the connection is made available for queries
- **FR-004**: System MUST handle ANSI initialization failures gracefully and report clear errors
- **FR-005**: System MUST maintain ANSI settings across connection pool reuse (or re-apply them after RESET_CONNECTION)

### Key Entities

- **TdsConnection**: The TDS protocol connection class that handles LOGIN7 and query execution
- **ConnectionPool**: Manages connection lifecycle and reuse
- **Session Options**: SQL Server session-level settings that affect query behavior

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: DDL commands (ALTER DATABASE, DBCC, BACKUP LOG) complete without SET options errors
- **SC-002**: All existing tests continue to pass (no regression)
- **SC-003**: New integration test validates DDL execution works correctly
- **SC-004**: Connection pool behavior remains unchanged for normal queries
- **SC-005**: Extension startup time increase is negligible (less than 50ms per connection)

## Assumptions

- SQL Server 2016+ is the minimum supported version (all ANSI SET options available)
- The ANSI SET options specified are compatible with all SQL Server editions (Standard, Enterprise, Azure SQL)
- Setting ANSI options does not require elevated permissions beyond normal connection access
- The fix is applied in `DoLogin7()` after successful authentication, before connection enters pool

## Out of Scope

- Making ANSI options configurable via extension settings (future enhancement)
- Supporting pre-SQL Server 2016 versions with different option requirements
- Modifying LOGIN7 packet option flags (using post-login SQL_BATCH approach instead)

## Technical Context (For Planning Reference)

The fix should be implemented in `src/tds/tds_connection.cpp::DoLogin7()` after successful authentication. The pattern is:

1. After successful authentication response parsed
2. Execute SQL_BATCH with ANSI SET statements
3. Receive and validate response
4. Set connection state to Idle
5. Return success

This ensures all new connections get ANSI settings before entering the pool.

## Documentation Updates Required

- Update architecture documentation with ANSI connection initialization details
- Add test documentation for the new integration test
- Update README with information about ANSI-compliant connection behavior

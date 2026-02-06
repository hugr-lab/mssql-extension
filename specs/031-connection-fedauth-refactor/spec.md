# Feature Specification: Connection & FEDAUTH Refactoring

**Feature Branch**: `031-connection-fedauth-refactor`
**Created**: 2026-02-06
**Status**: Draft
**Input**: User description: "Review and refactor the TDS connection layer and FEDAUTH (Azure AD) authentication implementation to reduce code duplication, improve maintainability, and establish clear architectural patterns."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Azure Warehouse Operations Work Reliably (Priority: P1)

As a user with Azure Fabric Warehouse attached via FEDAUTH, I need BCP/CTAS/COPY operations to recover gracefully from errors so that subsequent operations don't fail with "Connection is not in idle state" errors.

**Why this priority**: This is a blocking bug that makes Azure Warehouse unusable after any BCP failure. Users cannot continue working without restarting DuckDB.

**Independent Test**: Can be fully tested by attaching an Azure Warehouse, triggering a BCP failure, then running any metadata query (e.g., `SHOW ALL TABLES`). Success means the subsequent query works.

**Acceptance Scenarios**:

1. **Given** an Azure Warehouse attached via FEDAUTH, **When** a CTAS operation fails during BCP bulk load, **Then** subsequent operations (SHOW TABLES, SELECT queries) succeed without requiring restart
2. **Given** a BCP failure has occurred, **When** checking pool statistics, **Then** `active_connections` shows 0 (no stuck connections)
3. **Given** a COPY TO MSSQL operation fails, **When** the user runs another query, **Then** the query succeeds without "connection not in idle state" error

---

### User Story 2 - INSERT in Transaction Works with FEDAUTH (Priority: P1)

As a user running transactions on Azure-attached databases, I need INSERT operations within transactions to succeed so that I can use standard transaction patterns with Azure SQL/Fabric.

**Why this priority**: Transactions are fundamental to data integrity. This bug prevents standard SQL workflows.

**Independent Test**: Can be tested by running `BEGIN; INSERT INTO azurewh.dbo.table SELECT ...; COMMIT;` on a FEDAUTH-attached database.

**Acceptance Scenarios**:

1. **Given** a FEDAUTH-attached database with an active transaction, **When** executing INSERT with subquery, **Then** the operation completes successfully
2. **Given** an explicit transaction is started, **When** INSERT requires schema lookup, **Then** the operation uses the pinned connection (no "Failed to acquire connection" error)

---

### User Story 3 - FEDAUTH Connections Survive Token Expiration (Priority: P1)

As a user with long-running DuckDB sessions connected to Azure, I need connections to automatically refresh expired tokens so that queries continue working after the ~1 hour token TTL.

**Why this priority**: Token expiration causes complete session failure with no recovery path except restart.

**Independent Test**: Can be tested by attaching Azure database, waiting for token expiration (~1 hour), then running a query.

**Acceptance Scenarios**:

1. **Given** an attached FEDAUTH database with expired token, **When** executing a query, **Then** the system automatically refreshes the token and query succeeds
2. **Given** token has expired, **When** detaching and re-attaching the database, **Then** a fresh token is acquired (no cached stale token used)

---

### User Story 4 - FEDAUTH ATTACH Fails Fast on Invalid Credentials (Priority: P2)

As a user attaching Azure databases, I need ATTACH to validate the connection immediately so that I discover credential problems at ATTACH time, not on first query.

**Why this priority**: Fail-fast behavior prevents confusion and wasted time troubleshooting auth issues.

**Independent Test**: Can be tested by attempting ATTACH with invalid Azure secret and verifying immediate error.

**Acceptance Scenarios**:

1. **Given** an Azure secret with invalid credentials, **When** running ATTACH, **Then** the operation fails immediately with a clear authentication error
2. **Given** a valid Azure secret, **When** running ATTACH with FEDAUTH, **Then** the connection is validated (SELECT 1 executed internally)
3. **Given** Azure routing redirects to a different server, **When** ATTACH with FEDAUTH, **Then** routing failures are reported immediately at ATTACH time

---

### User Story 5 - Connection Pool Operations Are Silent by Default (Priority: P2)

As a user running queries, I should not see debug messages like `[MSSQL POOL] Acquire called` in normal operation so that output remains clean and professional.

**Why this priority**: Debug noise confuses users and clutters output.

**Independent Test**: Can be tested by running any query without MSSQL_DEBUG set and verifying no pool messages appear.

**Acceptance Scenarios**:

1. **Given** MSSQL_DEBUG is not set, **When** running queries, **Then** no pool-related messages are printed
2. **Given** MSSQL_DEBUG=1, **When** running queries, **Then** pool acquire/release messages are visible
3. **Given** MSSQL_DEBUG=2, **When** running queries, **Then** detailed pool state information is visible

---

### User Story 6 - Efficient Connection Usage for CTAS (Priority: P2)

As a user running CTAS operations, I need the system to use cached metadata efficiently so that operations don't trigger excessive connection acquires.

**Why this priority**: 9+ connection acquires per operation indicates inefficiency and potential performance issues.

**Independent Test**: Can be tested by running CTAS with MSSQL_DEBUG=1 and counting acquire calls.

**Acceptance Scenarios**:

1. **Given** a table with cached metadata, **When** running CTAS, **Then** at most 3 connection acquires occur
2. **Given** MSSQL_DEBUG=1 is set, **When** running operations, **Then** cache hits/misses are logged for debugging

---

### User Story 7 - Unified Authentication Architecture (Priority: P3)

As a maintainer of this extension, I need authentication code paths to be consolidated so that bugs are fixed once (not in 4 places) and new auth methods are easier to add.

**Why this priority**: Technical debt reduction improves long-term maintainability but is not user-facing.

**Independent Test**: Can be tested by verifying all existing auth tests pass after refactoring.

**Acceptance Scenarios**:

1. **Given** the refactored codebase, **When** SQL Server auth is used, **Then** all existing tests pass
2. **Given** the refactored codebase, **When** Azure Service Principal auth is used, **Then** all existing tests pass
3. **Given** the refactored codebase, **When** Azure CLI auth is used, **Then** all existing tests pass
4. **Given** the refactored codebase, **When** Azure Device Code auth is used, **Then** all existing tests pass

---

### User Story 8 - Clear Error Messages with Context (Priority: P3)

As a user encountering connection errors, I need error messages that include context (what was happening, what server, what operation) so that I can diagnose issues.

**Why this priority**: Better errors reduce support burden and improve user experience.

**Independent Test**: Can be tested by triggering various error conditions and verifying messages include actionable context.

**Acceptance Scenarios**:

1. **Given** a connection failure, **When** viewing the error, **Then** the message includes server/port and what operation was attempted
2. **Given** an authentication failure, **When** viewing the error, **Then** the message indicates whether it was SQL auth or FEDAUTH and which step failed

---

### User Story 9 - Fabric Warehouse BCP Handling (Priority: P2)

As a user working with Microsoft Fabric Data Warehouse, I need clear feedback when BCP/INSERT BULK is not supported so that I can use appropriate alternatives.

**Why this priority**: Fabric is increasingly popular, and BCP failures cause connection corruption (Bug 0.1). Clear handling prevents confusion.

**Background**: Microsoft Fabric Data Warehouse does NOT support the TDS `INSERT BULK` protocol (BulkLoadBCP). This is a documented platform limitation. See: https://github.com/dotnet/SqlClient/issues/2995

**Independent Test**: Can be tested by attaching Fabric Warehouse and attempting CTAS with `mssql_ctas_use_bcp=true`.

**Acceptance Scenarios**:

1. **Given** a Fabric Warehouse connection, **When** attempting BCP operation, **Then** clear error message explains the limitation
2. **Given** a Fabric endpoint is detected, **When** running CTAS, **Then** system automatically uses INSERT mode (not BCP)
3. **Given** MSSQL_DEBUG=1, **When** Fabric endpoint is detected, **Then** log message indicates "Fabric detected, BCP disabled"

---

### Edge Cases

- What happens when token expires mid-operation (during BCP bulk load)?
- How does system handle Azure routing that redirects multiple times (routing loops)?
- What happens when detach is called while a transaction is active?
- How does the pool behave when all connections are in corrupted state?
- What happens when Azure AD is temporarily unavailable during token refresh?

## Requirements *(mandatory)*

### Functional Requirements

**Phase 0: Critical Bug Fixes**

- **FR-001**: System MUST reset connection state properly after BCP failure (no connections left in Executing state)
- **FR-002**: System MUST validate connection state before accepting it back into the pool
- **FR-003**: System MUST use pinned transaction connection for schema lookups during INSERT
- **FR-004**: System MUST automatically refresh expired FEDAUTH tokens on authentication failure
- **FR-005**: System MUST clear cached tokens when detaching FEDAUTH databases
- **FR-006**: System MUST validate FEDAUTH connections at ATTACH time (execute SELECT 1)
- **FR-007**: System MUST NOT print debug messages unless MSSQL_DEBUG environment variable is set
- **FR-008**: System MUST use metadata cache effectively (reduce connection acquires for cached tables)
- **FR-013**: System MUST detect Fabric endpoints and either disable BCP with clear error OR auto-fallback to INSERT mode

**Phase 1-5: Refactoring**

- **FR-009**: System MUST handle all authentication flows through a unified entry point
- **FR-010**: System MUST provide structured errors with error codes and context
- **FR-011**: System MUST validate state transitions before allowing operations
- **FR-012**: System MUST maintain transaction descriptor in a single location

### Key Entities

- **TdsConnection**: Represents a connection to SQL Server with state (Disconnected, Authenticating, Idle, Executing)
- **ConnectionPool**: Manages reusable connections per attached database context
- **AuthenticationStrategy**: Abstraction for different auth methods (SQL, Service Principal, CLI, Device Code)
- **FedAuthToken**: Azure AD token with expiration tracking for FEDAUTH connections
- **TransactionDescriptor**: 8-byte identifier for SQL Server transactions (used for connection pinning)

## Success Criteria *(mandatory)*

### Measurable Outcomes

**Phase 0 (Critical)**

- **SC-001**: After any BCP/COPY failure, subsequent queries succeed 100% of the time (currently 0%)
- **SC-002**: INSERT in transaction succeeds on FEDAUTH connections (currently fails)
- **SC-003**: FEDAUTH connections remain usable after token expiration without restart (currently requires restart)
- **SC-004**: FEDAUTH ATTACH with invalid credentials fails within 5 seconds (currently may silently succeed)
- **SC-005**: No debug output visible in normal operation (currently shows `[MSSQL POOL]` messages)
- **SC-006**: CTAS operations trigger 3 or fewer connection acquires for cached tables (currently 9+)
- **SC-011**: Fabric Warehouse CTAS operations succeed using INSERT fallback (currently fails with BCP error)

**Phase 1-5 (Refactoring)**

- **SC-007**: All existing integration tests pass after refactoring
- **SC-008**: No performance regression in connection acquisition (< 5% increase)
- **SC-009**: New authentication methods can be added without modifying core connection code
- **SC-010**: Error messages include operation context (server, port, auth type, operation)

## Assumptions

- Azure token TTL is approximately 1 hour (standard Azure AD behavior)
- Users have `future-specs/setup_test.sql` and `.env` available for manual FEDAUTH testing
- SQL Server 2019+ is the minimum supported version
- Phase 0 bugs must be fixed before starting refactoring phases
- Each phase will be a separate PR for easier review
- Microsoft Fabric Data Warehouse does NOT support TDS `INSERT BULK` protocol (documented platform limitation)
- Fabric endpoints can be detected via hostname patterns (`*.datawarehouse.fabric.microsoft.com`, `*.pbidedicated.windows.net`)

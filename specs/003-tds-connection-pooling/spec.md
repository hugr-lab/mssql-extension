# Feature Specification: TDS Connection, Authentication, and Pooling

**Feature Branch**: `003-tds-connection-pooling`
**Created**: 2026-01-15
**Status**: Draft
**Input**: Implement native SQL Server connectivity via TDS protocol with TCP transport, PRELOGIN/LOGIN7 handshake, SQL Server authentication, explicit connection state management, and a reusable connection pool. Expose diagnostic functions and DuckDB variables for pool configuration.

## Clarifications

### Session 2026-01-15

- Q: Which TDS protocol version to target? → A: TDS 7.4 only (SQL Server 2019+ support)
- Q: What concurrency model for the connection pool? → A: Thread-safe pool with mutex-protected acquire/release
- Q: How should mssql_ping verify connection health? → A: TDS-level ping (send minimal TDS packet and verify response)

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Open and Authenticate Connection (Priority: P1)

As a DuckDB extension developer, I want to open a TCP connection to SQL Server and authenticate using SQL Server authentication so that I can establish connectivity for query execution.

**Why this priority**: Opening and authenticating a connection is the foundational capability. All other functionality (pooling, query execution) depends on establishing a working connection first.

**Independent Test**: Can be fully tested by calling `mssql_open` with valid credentials and verifying the connection reaches the `idle` state, then calling `mssql_ping` to confirm the connection is alive.

**Acceptance Scenarios**:

1. **Given** valid SQL Server credentials (host, port, user, password, database), **When** I call `mssql_open(secret_name)`, **Then** a connection handle is returned and the connection state transitions through `disconnected` → `authenticating` → `idle`.
2. **Given** invalid credentials (wrong password), **When** I call `mssql_open(secret_name)`, **Then** authentication fails with a clear error message from SQL Server and the connection transitions to `disconnected`.
3. **Given** an unreachable host, **When** I call `mssql_open(secret_name)`, **Then** the connection attempt times out based on the configured timeout and returns an appropriate error.

---

### User Story 2 - Close Connection (Priority: P1)

As a DuckDB extension developer, I want to explicitly close a connection so that TCP resources are properly released.

**Why this priority**: Proper connection cleanup prevents resource leaks. This completes the basic connection lifecycle needed before pooling can be implemented.

**Independent Test**: Can be fully tested by opening a connection, closing it with `mssql_close`, and verifying the connection handle is invalidated and TCP socket is released.

**Acceptance Scenarios**:

1. **Given** an open connection in `idle` state, **When** I call `mssql_close(handle)`, **Then** the TCP connection is closed and the handle becomes invalid.
2. **Given** a connection in `executing` state, **When** I call `mssql_close(handle)`, **Then** the active request is cancelled first, then the connection is closed.
3. **Given** an invalid or already-closed handle, **When** I call `mssql_close(handle)`, **Then** the call returns gracefully without error (idempotent behavior).

---

### User Story 3 - Ping Connection (Priority: P1)

As a DuckDB extension developer, I want to verify a connection is still alive so that I can detect stale connections before using them.

**Why this priority**: Connection health checking is essential for reliable pool management. Dead connections must be detected and removed.

**Independent Test**: Can be fully tested by pinging an open connection (expect success) and pinging after server-side disconnect (expect failure).

**Acceptance Scenarios**:

1. **Given** an open connection in `idle` state, **When** I call `mssql_ping(handle)`, **Then** the function returns true indicating the connection is alive.
2. **Given** a connection where the server has closed the TCP socket, **When** I call `mssql_ping(handle)`, **Then** the function returns false and the connection state becomes `disconnected`.
3. **Given** a connection in `executing` state, **When** I call `mssql_ping(handle)`, **Then** the function returns true (connection is active and in use).

---

### User Story 4 - Configure Connection Pool Settings (Priority: P2)

As a DuckDB user, I want to configure connection pool behavior through DuckDB variables so that I can tune performance for my workload.

**Why this priority**: Pool configuration allows users to optimize for their specific use case. Not blocking for basic functionality but important for production use.

**Independent Test**: Can be fully tested by setting pool configuration variables and verifying the pool respects those settings when acquiring/releasing connections.

**Acceptance Scenarios**:

1. **Given** DuckDB is running with the mssql extension loaded, **When** I execute `SET mssql_connection_limit = 10`, **Then** the pool will maintain at most 10 connections per attached database.
2. **Given** a configured `mssql_connection_cache = false`, **When** connections are acquired and released, **Then** connections are closed immediately rather than returned to the pool.
3. **Given** a configured `mssql_connection_timeout = 30`, **When** a connection attempt exceeds 30 seconds, **Then** the attempt fails with a timeout error.

---

### User Story 5 - Automatic Connection Pooling (Priority: P2)

As a DuckDB user, I want connections to be automatically pooled and reused so that query performance is improved by avoiding repeated connection handshakes.

**Why this priority**: Connection pooling significantly improves performance for workloads with multiple queries. Builds on the connection lifecycle foundation.

**Independent Test**: Can be fully tested by executing multiple sequential queries and observing that connections are reused rather than reopened (can verify via pool statistics).

**Acceptance Scenarios**:

1. **Given** an attached mssql database with pooling enabled, **When** multiple queries are executed sequentially, **Then** connections are reused from the pool rather than reopened.
2. **Given** a pool with one idle connection, **When** a query is executed, **Then** the idle connection is acquired (state → `executing`) and returned to idle after completion.
3. **Given** pool connections are idle for longer than `mssql_idle_timeout`, **When** the idle timeout expires, **Then** excess connections beyond `mssql_min_connections` are closed.

---

### User Story 6 - Connection Recovery After Cancellation (Priority: P2)

As a DuckDB extension developer, I want connections to be reusable after a query cancellation so that the pool is not depleted by cancelled operations.

**Why this priority**: Query cancellation is common in interactive use. Connections must be recoverable to maintain pool efficiency.

**Independent Test**: Can be fully tested by starting a query, cancelling it, and verifying the connection returns to `idle` state and can be reused for subsequent queries.

**Acceptance Scenarios**:

1. **Given** a connection in `executing` state, **When** the query is cancelled, **Then** the connection transitions to `cancelling` → `idle` and remains in the pool.
2. **Given** a connection that was cancelled, **When** I execute a new query on that connection, **Then** the query executes successfully (connection is fully reusable).
3. **Given** a connection where cancellation fails to complete cleanly, **When** cancellation times out, **Then** the connection is forcefully closed and removed from the pool.

---

### User Story 7 - View Pool Statistics (Priority: P3)

As a DuckDB user, I want to view connection pool statistics so that I can monitor pool health and tune configuration.

**Why this priority**: Observability is important for production systems but not required for core functionality.

**Independent Test**: Can be fully tested by executing queries, then calling the statistics function and verifying counts match expected values.

**Acceptance Scenarios**:

1. **Given** an attached mssql database with active pool, **When** I call `mssql_pool_stats(context_name)`, **Then** I receive a table with statistics including total connections, idle connections, active connections, and connections created/closed counts.
2. **Given** no attached databases, **When** I call `mssql_pool_stats('nonexistent')`, **Then** I receive an error indicating the context does not exist.

---

### Edge Cases

- What happens when the connection pool is exhausted and a new query is requested? → Query waits up to `mssql_acquire_timeout` for a connection to become available, then fails with a timeout error.
- How does the system handle SQL Server closing a pooled connection (server-side timeout)? → Connection is detected as dead on next use attempt and removed from pool; a new connection is transparently created.
- What happens when TCP connection is lost mid-query? → Query fails with a network error, connection transitions to `disconnected` and is removed from pool.
- How does the system handle multiple concurrent requests on the same connection handle? → Only one active request per connection is allowed; subsequent requests wait or fail based on configuration.
- What happens when pool settings are changed while connections exist? → New settings apply to newly created connections; existing connections are not affected until they are closed and reopened.
- How does the system handle PRELOGIN failures (e.g., protocol version mismatch)? → Connection attempt fails with a descriptive error before LOGIN7 is attempted.
- What happens when LOGIN7 authentication fails due to account lockout vs wrong password? → Both return authentication failure with the SQL Server error message distinguishing the cause.

## Requirements *(mandatory)*

### Functional Requirements

#### TCP Transport

- **FR-001**: System MUST establish TCP connections to SQL Server on the specified host and port.
- **FR-002**: System MUST respect the configured connection timeout when establishing TCP connections.
- **FR-003**: System MUST properly close TCP sockets when connections are closed or fail.

#### TDS Protocol

- **FR-004**: System MUST implement TDS packet framing with correct packet type, status, length, SPID, packet ID, and window fields.
- **FR-005**: System MUST implement the PRELOGIN handshake to negotiate protocol version and encryption.
- **FR-006**: System MUST implement LOGIN7 packet for SQL Server authentication.
- **FR-007**: System MUST support SQL Server authentication (username/password); Windows authentication and other methods are out of scope.
- **FR-008**: System MUST NOT support SSL/TLS encryption in this phase; all connections are unencrypted.

#### Connection States

- **FR-009**: System MUST track connection state with explicit values: `disconnected`, `authenticating`, `idle`, `executing`, `cancelling`.
- **FR-010**: Connections MUST transition through states in valid sequences only:
  - `disconnected` → `authenticating` (on open)
  - `authenticating` → `idle` (on successful auth) or `disconnected` (on failed auth)
  - `idle` → `executing` (on query start) or `disconnected` (on close)
  - `executing` → `idle` (on query complete) or `cancelling` (on cancel request)
  - `cancelling` → `idle` (on successful cancel) or `disconnected` (on cancel failure/timeout)
- **FR-011**: Only one active request MUST be allowed per connection at any time.
- **FR-012**: Connections MUST be reusable after successful cancellation (return to `idle` state).

#### Diagnostic Functions

- **FR-013**: System MUST provide `mssql_open(secret_name)` scalar function that opens a connection using credentials from the named mssql secret and returns a connection handle (BIGINT).
- **FR-014**: System MUST provide `mssql_close(handle)` scalar function that closes a connection and releases resources; returns BOOLEAN.
- **FR-015**: System MUST provide `mssql_ping(handle)` scalar function that tests if a connection is alive by sending a minimal TDS packet and verifying response; returns BOOLEAN.
- **FR-016**: System SHOULD provide `mssql_pool_stats(context_name)` table function that returns pool statistics as a table.

#### Connection Pool

- **FR-017**: System MUST maintain a connection pool per attached mssql database context.
- **FR-018**: Pool MUST reuse idle connections for subsequent queries when `mssql_connection_cache` is enabled.
- **FR-019**: Pool MUST respect `mssql_connection_limit` as the maximum number of connections per context.
- **FR-020**: Pool MUST close connections that have been idle longer than `mssql_idle_timeout` (beyond minimum).
- **FR-021**: Pool MUST maintain at least `mssql_min_connections` connections when the context is attached.
- **FR-022**: Pool MUST wait up to `mssql_acquire_timeout` for a connection when pool is exhausted before failing.
- **FR-029**: Pool MUST be thread-safe; acquire and release operations MUST be protected by mutex to support concurrent callers.

#### DuckDB Variables (Settings)

- **FR-023**: System MUST provide `mssql_connection_limit` variable (default: 64) controlling maximum connections per context.
- **FR-024**: System MUST provide `mssql_connection_cache` variable (default: true) enabling/disabling connection reuse.
- **FR-025**: System MUST provide `mssql_connection_timeout` variable (default: 30 seconds) controlling TCP connection timeout.
- **FR-026**: System MUST provide `mssql_idle_timeout` variable (default: 300 seconds) controlling how long idle connections are kept.
- **FR-027**: System MUST provide `mssql_min_connections` variable (default: 0) controlling minimum connections to maintain.
- **FR-028**: System MUST provide `mssql_acquire_timeout` variable (default: 30 seconds) controlling how long to wait for a connection from exhausted pool.

### Key Entities

- **TdsConnection**: Represents a single TDS connection to SQL Server. Contains TCP socket, current state, SPID, and protocol state. Tracks state transitions and enforces single-active-request constraint.
- **ConnectionPool**: Manages a collection of TdsConnections for a single database context. Handles acquisition, release, idle timeout, and statistics tracking.
- **TdsPacket**: Represents a TDS protocol packet with header fields (type, status, length, SPID, packet ID, window) and payload data.
- **ConnectionState**: Enumeration of valid connection states (disconnected, authenticating, idle, executing, cancelling).
- **PoolStatistics**: Statistics about a connection pool including total/idle/active counts and creation/closure metrics.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A connection can be opened and authenticated to SQL Server in under 2 seconds on a local network.
- **SC-002**: Connection pool reduces connection overhead by 90% for workloads executing 100+ sequential queries (measured by eliminating reconnection handshakes).
- **SC-003**: Pool can maintain and manage up to the configured limit of concurrent connections without resource leaks.
- **SC-004**: Cancelled queries allow connection recovery within 5 seconds, returning the connection to reusable state.
- **SC-005**: All connection state transitions are logged and observable for debugging purposes.
- **SC-006**: Pool statistics accurately reflect the actual state of connections (idle, active, total counts match reality).
- **SC-007**: Connection timeout and pool acquisition timeout prevent indefinite hangs in error scenarios.

## Scope Boundaries

### In Scope

- TCP socket management (connect, close, read, write)
- TDS packet framing (header construction, packet splitting)
- PRELOGIN handshake (version negotiation, no encryption)
- LOGIN7 authentication (SQL Server auth only)
- Connection state machine with explicit states
- Connection pooling with configurable limits
- DuckDB variables for pool configuration
- Diagnostic functions (open, close, ping, pool_stats)
- Connection reuse after cancellation

### Out of Scope (Non-Goals)

- Query execution (SQL batch/RPC packets) - separate specification
- Result set decoding (TDS response parsing) - separate specification
- SSL/TLS encryption - future enhancement
- Windows authentication (SSPI/Kerberos) - future enhancement
- Azure Active Directory authentication - future enhancement
- Multiple Active Result Sets (MARS) - not planned
- Connection string parsing (use secrets instead)

## Assumptions

- SQL Server is reachable via TCP/IP on the standard or configured port.
- SQL Server has TCP/IP protocol enabled and listening.
- SQL Server authentication mode is enabled (mixed mode or SQL auth only).
- Target SQL Server version is 2019 or later (TDS 7.4 protocol only; no fallback to older versions).
- DuckDB's secret manager provides credentials in the format defined by spec 002.
- Single-threaded connection use is acceptable (no MARS requirement).
- Unencrypted connections are acceptable for this phase (development/testing environments).

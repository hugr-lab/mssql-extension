# Feature Specification: Fix Windows Winsock Initialization

**Feature Branch**: `019-fix-winsock-init`
**Created**: 2026-01-27
**Status**: Draft
**Input**: User description: "Fix Windows Winsock initialization (WSAStartup) for TCP socket connections"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Connect to SQL Server from Windows (Priority: P1)

A DuckDB user on Windows loads the mssql extension and attaches to a locally installed SQL Server named instance using a TCP connection string. Currently this fails with the error: `IO Error: MSSQL connection validation failed: Connection failed to 100.100.249.81:1434: Failed to connect to 100.100.249.81:1434`. The same connection string works from Linux. SSMS and PyTDS also connect to the same server and port from Windows without issues. The root cause is that the Winsock library is never initialized.

**Why this priority**: This is the core bug — the extension is completely non-functional on Windows for TCP connections. Without this fix, zero Windows users can connect to SQL Server.

**Independent Test**: Can be fully tested by loading the extension on Windows and running `ATTACH 'Server=host,port;...' AS db (TYPE mssql)`. Delivers the fundamental ability to use the extension on Windows.

**Acceptance Scenarios**:

1. **Given** a Windows machine with DuckDB and the mssql extension loaded, **When** a user attaches to a SQL Server instance using a TCP connection string with IP and port, **Then** the connection succeeds and queries return results.
2. **Given** a Windows machine with DuckDB and the mssql extension loaded, **When** a user attaches to a SQL Server instance using hostname and port, **Then** the connection succeeds (hostname resolution works).
3. **Given** a Windows machine with DuckDB and the mssql extension loaded, **When** a user attaches using `localhost,1434`, **Then** the connection succeeds for loopback connections.

---

### User Story 2 - TLS Connections from Windows (Priority: P1)

A DuckDB user on Windows connects to a SQL Server instance with TLS encryption enabled (`encrypt=yes`). The TLS handshake completes successfully and encrypted queries work. TLS also depends on the underlying socket layer being initialized.

**Why this priority**: TLS connections depend on Winsock initialization. Many production SQL Server deployments require encrypted connections.

**Independent Test**: Can be tested by attaching with `encrypt=yes;trustServerCertificate=yes` on Windows and running queries over the encrypted connection.

**Acceptance Scenarios**:

1. **Given** a Windows machine with DuckDB and the mssql extension, **When** a user attaches with `encrypt=yes;trustServerCertificate=yes`, **Then** the TLS handshake succeeds and queries return results over the encrypted connection.

---

### User Story 3 - Multiple Concurrent Connections from Windows (Priority: P2)

A DuckDB user on Windows opens multiple connections to SQL Server (e.g., via connection pool or multiple attached databases). All connections work correctly because the socket library initialization is performed only once and is thread-safe.

**Why this priority**: Connection pooling is a core feature. The initialization must be safe for concurrent use, but this is a lower priority than basic single-connection functionality.

**Independent Test**: Can be tested by attaching multiple MSSQL databases simultaneously and running parallel queries.

**Acceptance Scenarios**:

1. **Given** the socket library has been initialized by a first connection, **When** additional connections are created concurrently, **Then** all connections succeed without re-initialization errors or race conditions.

---

### User Story 4 - No Regression on Linux/macOS (Priority: P1)

Existing Linux and macOS users experience no changes in behavior. The Windows-specific initialization code is completely inert on non-Windows platforms.

**Why this priority**: Must not break existing working platforms while fixing Windows.

**Independent Test**: Run the existing full integration test suite on Linux/macOS and verify all tests pass with zero changes in behavior.

**Acceptance Scenarios**:

1. **Given** a Linux or macOS machine, **When** the extension is loaded and used normally, **Then** all existing functionality works identically to before the change.

---

### Edge Cases

- What happens when socket library initialization fails (e.g., required system library not available on a stripped-down Windows environment)? The extension should return a clear error message indicating the failure.
- What happens when the extension is loaded but no connections are ever made? Initialization should not cause errors or side effects if no sockets are created.
- What happens when DuckDB is shut down while connections are active? Cleanup should not interfere with connection teardown order.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: On Windows, the system MUST initialize the socket library before any socket operation is invoked (connect, send, receive, DNS resolution).
- **FR-002**: Socket library initialization MUST be performed exactly once per process, regardless of how many connections are created or destroyed.
- **FR-003**: Socket library initialization MUST be thread-safe (safe for concurrent first-connection scenarios from multiple threads).
- **FR-004**: The system MUST clean up the socket library when the process shuts down or the extension is unloaded.
- **FR-005**: On non-Windows platforms (Linux, macOS), the initialization code MUST compile to a no-op with zero runtime overhead.
- **FR-006**: If socket library initialization fails, the system MUST return a clear, user-facing error message including the platform-specific error code.
- **FR-007**: The fix MUST NOT change any existing socket, TLS, or connection logic — only add the missing initialization/cleanup step.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users on Windows can successfully connect to SQL Server using TCP connection strings (IP:port or hostname:port) — zero connection failures due to missing socket initialization.
- **SC-002**: Users on Windows can successfully connect with TLS encryption enabled.
- **SC-003**: All existing integration tests continue to pass on Linux and macOS with no regressions.
- **SC-004**: Windows CI builds (both MSVC and MinGW toolchains) compile and link successfully.
- **SC-005**: Concurrent connection creation on Windows does not produce initialization race conditions or errors.

## Assumptions

- DuckDB itself does not guarantee that the socket library is initialized before extension code runs. The extension must handle its own initialization.
- The extension targets Windows versions that ship with the socket library (Windows 7+, all modern Windows).
- A "lazy initialization on first use" pattern (initializing when the first connection is attempted) is acceptable and preferred over initializing at extension load time.

# Feature Specification: DuckDB Surface API

**Feature Branch**: `002-duckdb-surface-api`
**Created**: 2026-01-15
**Status**: Draft
**Input**: Define and stabilize the public DuckDB interface of the extension before implementing protocol and execution logic. Includes secrets, attach/detach, and user-facing functions.

## Clarifications

### Session 2026-01-15

- Q: What security posture should be used for credential/password handling at runtime? → A: Defer to DuckDB's secret manager (use whatever it provides)
- Q: What should mssql_scan return as stub behavior (no real network traffic)? → A: Return hardcoded sample rows (e.g., 3 rows with dummy values)
- Q: What happens when DETACH is called while a query is in progress? → A: Immediately abort query and detach

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Secret Creation (Priority: P1)

As a DuckDB user, I want to securely store SQL Server connection credentials so that I can connect to remote databases without exposing sensitive information in queries.

**Why this priority**: Secrets are the foundation for all other functionality. Without secure credential storage, users cannot establish any connection to SQL Server databases.

**Independent Test**: Can be fully tested by creating a secret with all required fields and verifying it persists across sessions and cannot be modified after creation.

**Acceptance Scenarios**:

1. **Given** DuckDB is running with the mssql extension loaded, **When** I create a secret with `CREATE SECRET my_secret (TYPE mssql, host 'server.example.com', port 1433, database 'mydb', user 'admin', password 'secret')`, **Then** the secret is stored and accessible by name.
2. **Given** an mssql secret already exists, **When** I attempt to modify any of its fields, **Then** the operation fails with an error indicating secrets are immutable.
3. **Given** I am creating an mssql secret, **When** I omit any required field (host, port, database, user, or password), **Then** the creation fails with a clear error message indicating which field is missing.

---

### User Story 2 - Attach SQL Server Database (Priority: P1)

As a DuckDB user, I want to attach a SQL Server database as a named connection context so that I can query it using familiar DuckDB syntax.

**Why this priority**: Attach is the primary mechanism for establishing a connection context. Without it, users cannot access SQL Server data through DuckDB.

**Independent Test**: Can be fully tested by attaching a database with a secret, verifying the named context is created, and confirming no network connection is opened until data is actually accessed.

**Acceptance Scenarios**:

1. **Given** an mssql secret named `my_secret` exists, **When** I execute `ATTACH '' AS mydb (TYPE mssql, SECRET my_secret)`, **Then** a named connection context `mydb` is created.
2. **Given** an attached database context exists, **When** no queries have been executed against it, **Then** no actual network connection to SQL Server has been opened (lazy connection).
3. **Given** I attempt to attach without a valid secret, **When** the attach command executes, **Then** it fails with a clear error message.

---

### User Story 3 - Detach SQL Server Database (Priority: P2)

As a DuckDB user, I want to detach a previously attached SQL Server database so that resources are properly released and the connection context is removed.

**Why this priority**: Detach completes the connection lifecycle. While not needed for initial functionality, proper cleanup is essential for production use.

**Independent Test**: Can be fully tested by detaching an attached database and verifying the context is removed and any open connections are closed.

**Acceptance Scenarios**:

1. **Given** an attached database context `mydb` exists, **When** I execute `DETACH mydb`, **Then** the connection context is removed.
2. **Given** an attached database had an active network connection, **When** I detach it, **Then** the network connection is properly closed.
3. **Given** I attempt to detach a non-existent context, **When** the detach command executes, **Then** it fails with a clear error message.

---

### User Story 4 - Execute Raw SQL (Priority: P2)

As a DuckDB user, I want to execute arbitrary SQL statements against SQL Server so that I can perform operations that may not be supported through DuckDB's native syntax.

**Why this priority**: Provides an escape hatch for advanced users and operations not covered by standard query translation. Important for real-world usability but not blocking for MVP.

**Independent Test**: Can be fully tested by executing a raw SQL statement and verifying the return includes success status, affected row count, and result message.

**Acceptance Scenarios**:

1. **Given** an attached database context `mydb` exists, **When** I execute `SELECT * FROM mssql_execute('mydb', 'UPDATE users SET active = 1 WHERE id = 5')`, **Then** I receive a result with success flag, affected row count, and message.
2. **Given** I execute a statement that fails on SQL Server, **When** `mssql_execute` runs, **Then** the result includes success=false and an error message from SQL Server.
3. **Given** I reference a non-existent database context, **When** `mssql_execute` runs, **Then** it fails with a clear error indicating the context does not exist.

---

### User Story 5 - Scan SQL Server Data (Priority: P2)

As a DuckDB user, I want to execute SELECT queries against SQL Server and receive results as a DuckDB relation so that I can analyze remote data using DuckDB's query engine.

**Why this priority**: Core query functionality for reading data. Equally important as execute but focused on data retrieval rather than mutations.

**Independent Test**: Can be fully tested by executing a SELECT query and verifying a relation is returned with appropriate schema and data.

**Acceptance Scenarios**:

1. **Given** an attached database context `mydb` exists, **When** I execute `SELECT * FROM mssql_scan('mydb', 'SELECT id, name FROM users')`, **Then** I receive a relation with the query results.
2. **Given** a scan query returns no rows, **When** `mssql_scan` executes, **Then** an empty relation with the correct schema is returned.
3. **Given** I reference a non-existent database context, **When** `mssql_scan` runs, **Then** it fails with a clear error indicating the context does not exist.

---

### Edge Cases

- What happens when a secret is created with an invalid port number (e.g., negative, zero, or > 65535)?
- How does the system handle attach with an empty database name in the secret?
- What happens when detach is called while a query is in progress? → Immediately abort the query and proceed with detach.
- How does mssql_execute handle very large affected row counts that may exceed integer limits?
- What happens when mssql_scan receives a query that is not a SELECT statement?
- How does the system handle special characters in passwords (quotes, backslashes)?
- What happens when attempting to create a secret with the same name as an existing one?

## Requirements *(mandatory)*

### Functional Requirements

#### Secrets

- **FR-001**: System MUST support a `SECRET TYPE mssql` for storing SQL Server connection credentials.
- **FR-002**: Mssql secrets MUST require all of the following fields: host, port, database, user, password.
- **FR-003**: Mssql secrets MUST be immutable after creation; any attempt to modify fields MUST fail.
- **FR-004**: System MUST validate port is a valid TCP port number (1-65535) during secret creation.
- **FR-005**: System MUST validate that all required fields are non-empty during secret creation.
- **FR-006**: System MUST prevent creation of a secret with a name that already exists.

#### Attach/Detach

- **FR-007**: System MUST support `ATTACH ... TYPE mssql` syntax for creating named connection contexts.
- **FR-008**: Attach MUST require a valid mssql secret to be specified.
- **FR-009**: Network connections MUST be opened lazily (only when data is actually accessed).
- **FR-010**: `DETACH` MUST remove the named connection context.
- **FR-011**: `DETACH` MUST close any open network connections associated with the context.
- **FR-012**: `DETACH` MUST clean up all internal metadata associated with the connection context.
- **FR-012a**: `DETACH` MUST immediately abort any in-progress query on the context before proceeding with cleanup.

#### Functions

- **FR-013**: System MUST provide `mssql_execute(context_name, sql_statement)` function for executing raw SQL.
- **FR-014**: `mssql_execute` MUST return a result containing: success flag (boolean), affected row count (integer), and message (string).
- **FR-015**: System MUST provide `mssql_scan(context_name, select_query)` function for executing SELECT queries.
- **FR-016**: `mssql_scan` MUST return a DuckDB relation containing the query results.
- **FR-017**: Both functions MUST validate that the specified context name exists before execution.
- **FR-018**: Both functions MUST propagate meaningful error messages from SQL Server when queries fail.

### Key Entities

- **MssqlSecret**: Stores connection credentials for SQL Server. Contains host (string), port (integer), database (string), user (string), password (string). Immutable after creation.
- **ConnectionContext**: Named reference to an attached SQL Server database. Links to an MssqlSecret. Manages lazy network connection lifecycle.
- **ExecuteResult**: Result of mssql_execute. Contains success (boolean), affected_rows (integer), message (string).

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can create an mssql secret and attach a database in under 10 seconds with no prior knowledge beyond reading documentation.
- **SC-002**: All five required secret fields are validated at creation time with clear error messages identifying any missing or invalid fields.
- **SC-003**: Detaching a database releases all associated resources within 1 second.
- **SC-004**: Error messages from invalid operations clearly identify the problem and suggest corrective action.
- **SC-005**: The public API (secret type, attach/detach, functions) remains stable and backward-compatible for future releases.
- **SC-006**: All functions handle edge cases gracefully without crashes or undefined behavior.

## Scope Boundaries

### In Scope

- SECRET TYPE mssql definition and validation
- ATTACH/DETACH mechanics for connection context management
- mssql_execute function signature and return structure
- mssql_scan function signature and return type
- Error handling and validation for all public interfaces

### Out of Scope (Non-Goals)

- Actual network traffic to SQL Server
- TDS protocol implementation
- Catalog integration (schema discovery, table listing)
- Query translation from DuckDB SQL to T-SQL
- Connection pooling
- Transaction management

## Assumptions

- DuckDB's existing secret management infrastructure is available and follows standard patterns.
- Password/credential security (storage, memory handling) is delegated entirely to DuckDB's secret manager; no custom encryption layer is implemented.
- The extension can register custom secret types through DuckDB's extension API.
- Lazy connection opening is achievable through DuckDB's attach mechanism.
- During this phase (no real network), mssql_scan returns hardcoded sample rows (e.g., 3 rows with dummy values) to validate the data flow through DuckDB.
- The DuckDB main branch API for extensions is stable enough for this implementation.

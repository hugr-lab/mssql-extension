# Feature Specification: Attach Connection Validation

**Feature Branch**: `001-attach-connection-validation`
**Created**: 2026-01-25
**Status**: Draft
**Input**: User description: "When the SQL Server credential is wrong the attach command doesn't return error and take a lot time. The next catalog queries return IO Error: Failed to acquire connection for metadata refresh. Check the connection can be established at attach statement, if not return error and don't create a catalog"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Immediate Feedback on Invalid Credentials (Priority: P1)

As a user connecting to a SQL Server database via ATTACH, I want to receive an immediate error when my credentials are wrong, so that I can correct them without waiting for subsequent query failures.

**Why this priority**: This is the core value of this feature - preventing delayed failure and wasted time. Users currently experience silent failures that only manifest later during catalog queries.

**Independent Test**: Can be fully tested by attempting ATTACH with invalid credentials and verifying immediate error response. Delivers value by saving user time and providing actionable feedback.

**Acceptance Scenarios**:

1. **Given** a user provides invalid username/password, **When** they execute the ATTACH command, **Then** the system returns an authentication error within 5 seconds
2. **Given** a user provides incorrect server hostname, **When** they execute the ATTACH command, **Then** the system returns a connection error indicating the server cannot be reached
3. **Given** a user provides invalid database name, **When** they execute the ATTACH command, **Then** the system returns an error indicating the database does not exist or is inaccessible

---

### User Story 2 - No Catalog Created on Connection Failure (Priority: P1)

As a user, when my connection attempt fails, I want the system to NOT create a partial catalog entry, so that I don't have stale/broken catalog references in my DuckDB session.

**Why this priority**: Equal priority with P1 because a partial catalog with no valid connection leads to confusing subsequent errors and requires manual cleanup.

**Independent Test**: Can be tested by attempting ATTACH with invalid credentials, then verifying no catalog entry exists via `SHOW DATABASES` or `duckdb_databases()`.

**Acceptance Scenarios**:

1. **Given** invalid credentials cause ATTACH to fail, **When** the user checks available databases, **Then** no catalog entry for the failed connection exists
2. **Given** a connection timeout occurs during ATTACH, **When** the user queries the system catalogs, **Then** no orphaned catalog entries exist

---

### User Story 3 - Clear Error Messages (Priority: P2)

As a user, when connection validation fails, I want to receive a clear, actionable error message that tells me specifically what went wrong, so that I can fix the issue without guessing.

**Why this priority**: Enhances user experience but the feature still delivers value with basic error handling.

**Independent Test**: Can be tested by triggering various connection failure scenarios and verifying error messages are specific and actionable.

**Acceptance Scenarios**:

1. **Given** authentication fails due to wrong password, **When** the error is displayed, **Then** the message indicates "Authentication failed" or similar, not just "IO Error"
2. **Given** the server is unreachable, **When** the error is displayed, **Then** the message indicates the server could not be reached (connection refused, timeout, DNS failure)
3. **Given** TLS handshake fails, **When** the error is displayed, **Then** the message indicates a TLS/SSL error with relevant details

---

### User Story 4 - TrustServerCertificate Parameter Alias (Priority: P3)

As a user familiar with ADO.NET/ODBC connection strings, I want to use `TrustServerCertificate` as an alias for `use_encrypt`, so that I can use familiar parameter names from other SQL Server tools.

**Why this priority**: Quality-of-life improvement for users migrating from other tools. The feature works without this, but it improves usability.

**Independent Test**: Can be tested by using `TrustServerCertificate=true` in ATTACH options and verifying TLS behavior matches `use_encrypt=true`.

**Acceptance Scenarios**:

1. **Given** a user specifies `TrustServerCertificate=true`, **When** they execute the ATTACH command, **Then** the system behaves identically to `use_encrypt=true`
2. **Given** a user specifies `TrustServerCertificate=false`, **When** they execute the ATTACH command, **Then** the system behaves identically to `use_encrypt=false`
3. **Given** a user specifies both `TrustServerCertificate` and `use_encrypt` with conflicting values, **When** they execute the ATTACH command, **Then** the system returns an error indicating conflicting options

---

### Edge Cases

- What happens when the server is reachable but very slow to respond? (Reasonable timeout should apply)
- How does the system handle intermittent network failures during validation?
- What happens when credentials are valid but the user lacks permission to access the specified database?
- How does the system handle connection validation when TLS is required but not configured correctly?
- What happens when the SQL Server is in single-user mode or being restored?
- What happens when both `TrustServerCertificate` and `use_encrypt` are specified with the same value? (Should succeed)

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST attempt to establish a connection to SQL Server during the ATTACH statement execution
- **FR-002**: System MUST validate authentication credentials before creating any catalog entry
- **FR-003**: System MUST NOT create a catalog entry if connection validation fails
- **FR-004**: System MUST return an error from the ATTACH statement if connection cannot be established
- **FR-005**: System MUST complete connection validation within a reasonable timeout period (default: 30 seconds)
- **FR-006**: Error messages MUST indicate the specific type of failure (authentication, network, TLS, database access)
- **FR-007**: System MUST release any partially acquired resources if connection validation fails
- **FR-008**: System MUST support connection validation for both TLS and non-TLS connections
- **FR-009**: System MUST accept `TrustServerCertificate` as an alias for the `use_encrypt` parameter
- **FR-010**: System MUST reject ATTACH if both `TrustServerCertificate` and `use_encrypt` are specified with conflicting values

### Assumptions

- Connection timeout defaults follow existing extension settings (or 30 seconds if not configured)
- The validation uses the existing connection pool infrastructure
- A single successful connection is sufficient to validate credentials (no need to validate pool capacity)
- Database access permissions are validated as part of the connection test (the connection will fail if the user can't access the database)
- `TrustServerCertificate` and `use_encrypt` are interchangeable; when both are specified with the same value, no error occurs

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: ATTACH with invalid credentials fails within 5 seconds (excluding network timeout scenarios)
- **SC-002**: ATTACH with unreachable server fails within the configured timeout period (default 30 seconds)
- **SC-003**: 100% of failed ATTACH attempts result in no catalog entry being created
- **SC-004**: Error messages for connection failures are specific enough that users can identify the cause without debugging
- **SC-005**: No regression in ATTACH performance for valid connections (less than 1 second overhead for validation)

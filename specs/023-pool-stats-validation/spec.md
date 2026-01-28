# Feature Specification: Extend Pool Stats and Connection Validation

**Feature Branch**: `023-pool-stats-validation`
**Created**: 2026-01-28
**Status**: Draft
**Input**: User description: "Extend pool stats and connection validation with TLS validation, query timeout, catalog-free mode, active/pinned connection counts, and TLS-by-default behavior"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - TLS Validation at Attach Time (Priority: P1)

As a database administrator, I want connection TLS settings validated at ATTACH time so that misconfigurations are detected immediately rather than causing silent timeouts or cryptic errors during query execution.

**Why this priority**: Critical for user experience - silent timeouts and delayed error discovery waste debugging time and can cause production issues. Fail-fast behavior is essential.

**Independent Test**: Can be fully tested by attempting ATTACH with invalid TLS settings and verifying immediate, clear error messages.

**Acceptance Scenarios**:

1. **Given** a SQL Server configured to require TLS, **When** the user ATTACHes without enabling encryption, **Then** the system returns a clear error indicating TLS is required by the server.
2. **Given** a SQL Server with TLS configured, **When** the user ATTACHes with `use_encrypt=true`, **Then** the system validates TLS connectivity by executing a simple query before completing ATTACH.
3. **Given** invalid TLS certificate chain or untrusted certificate, **When** the user ATTACHes with encryption enabled, **Then** the system returns a clear error describing the certificate validation failure.

---

### User Story 2 - Query Timeout Configuration (Priority: P1)

As a user running complex analytical queries, I want to configure query timeout so that I can control how long queries run before being cancelled, rather than being limited to the default 30-second timeout.

**Why this priority**: Directly impacts usability for analytical workloads that may legitimately require longer execution times.

**Independent Test**: Can be fully tested by setting a custom query timeout, running a long query, and verifying timeout behavior.

**Acceptance Scenarios**:

1. **Given** the default configuration, **When** I execute a query that takes longer than 30 seconds, **Then** the query times out with a clear timeout error message.
2. **Given** `mssql_query_timeout` is set to 120 seconds, **When** I execute a query that takes 60 seconds, **Then** the query completes successfully.
3. **Given** `mssql_query_timeout` is set to 10 seconds, **When** I execute a query that takes 60 seconds, **Then** the query times out after 10 seconds with a clear timeout error message.

---

### User Story 3 - Catalog-Free Mode for Serverless Databases (Priority: P2)

As a user connecting to serverless or restricted databases, I want to disable catalog integration so that I can use `mssql_exec` and `mssql_scan` functions without requiring permissions to view or create database objects.

**Why this priority**: Enables use of the extension in environments with limited permissions (Azure SQL Serverless, restricted access databases), expanding the user base significantly.

**Independent Test**: Can be fully tested by attaching with catalog disabled and executing queries via `mssql_scan` without catalog access.

**Acceptance Scenarios**:

1. **Given** a secret with `catalog=false`, **When** I ATTACH the database, **Then** the catalog is not queried and schema discovery is skipped.
2. **Given** a catalog-disabled attachment, **When** I use `mssql_scan(context, 'SELECT 1 as val')`, **Then** the query executes successfully and returns results.
3. **Given** a catalog-disabled attachment, **When** I use `mssql_exec(context, 'INSERT INTO t VALUES (1)')`, **Then** the statement executes successfully and returns affected row count.
4. **Given** a catalog-disabled attachment, **When** I try to access `context.schema.table` via DuckDB SQL, **Then** the system returns an error indicating catalog is disabled.

---

### User Story 4 - Extended Pool Statistics (Priority: P2)

As a system administrator monitoring connection usage, I want to see active and pinned connection counts in pool statistics so that I can diagnose connection leaks and understand transaction behavior.

**Why this priority**: Important for production monitoring and debugging, but not blocking for basic functionality.

**Independent Test**: Can be fully tested by creating connections, running transactions, and verifying pool stats show correct active/pinned counts.

**Acceptance Scenarios**:

1. **Given** 3 idle connections and 2 active connections in the pool, **When** I call `mssql_pool_stats()`, **Then** the result includes `active_connections=2` and `idle_connections=3`.
2. **Given** a transaction in progress with a pinned connection, **When** I call `mssql_pool_stats()`, **Then** the result includes `pinned_connections=1`.
3. **Given** no active transactions, **When** I call `mssql_pool_stats()`, **Then** `pinned_connections=0`.

---

### User Story 5 - TLS by Default (Priority: P3)

As a security-conscious user, I want TLS enabled by default when the server requires it so that connections are secure without requiring explicit configuration.

**Why this priority**: Important security improvement, but ranked lower because existing users with `use_encrypt=false` should not have their workflows broken unexpectedly.

**Independent Test**: Can be fully tested by attempting ATTACH to a TLS-requiring server without specifying encryption settings.

**Acceptance Scenarios**:

1. **Given** a SQL Server that requires TLS and no encryption setting specified, **When** I ATTACH, **Then** the system automatically uses TLS and connects successfully.
2. **Given** a SQL Server that supports both TLS and non-TLS and no encryption setting specified, **When** I ATTACH, **Then** the system uses TLS (no fallback to non-TLS for security).
3. **Given** a user explicitly sets `use_encrypt=false`, **When** connecting to a TLS-required server, **Then** the system returns a clear error that the server requires TLS.
4. **Given** a user explicitly sets `use_encrypt=false`, **When** connecting to a server that supports non-TLS, **Then** the connection succeeds without TLS.

---

### Edge Cases

- What happens when TLS negotiation starts but certificate validation fails mid-handshake? System should return clear certificate error.
- What happens when query timeout is set to 0? System should treat 0 as "no timeout" (infinite wait).
- What happens when the user changes `mssql_query_timeout` mid-session? The new timeout applies to subsequent queries only.
- What happens when catalog-free mode is enabled but user tries DML operations via INSERT INTO? System should return error that catalog is required for DML.
- What happens when pool stats are requested for a non-existent context? System returns empty/null result (existing behavior).

## Requirements *(mandatory)*

### Functional Requirements

**TLS Validation:**
- **FR-001**: System MUST execute a simple validation query (e.g., `SELECT 1`) after TLS handshake during ATTACH to verify TLS is fully functional.
- **FR-002**: System MUST return a clear, descriptive error message when TLS validation fails, including the root cause (certificate error, timeout, server rejection).
- **FR-003**: System MUST detect when server requires TLS but client didn't enable it and provide actionable error message.

**Query Timeout:**
- **FR-004**: System MUST provide a `mssql_query_timeout` setting that users can configure (default: 30 seconds).
- **FR-005**: System MUST pass the configured query timeout to the TDS layer for all query executions.
- **FR-006**: System MUST support timeout value of 0 to indicate no timeout (infinite wait); no upper bound limit is imposed.
- **FR-007**: Query timeout setting MUST be changeable at runtime and apply to subsequent queries.

**Catalog-Free Mode:**
- **FR-008**: System MUST support a `catalog` parameter in secrets with values `true` (default) or `false`.
- **FR-009**: System MUST support a `Catalog` parameter in connection strings with values `yes`/`true` or `no`/`false`.
- **FR-010**: When catalog is disabled, system MUST NOT query SQL Server for schema/table metadata during ATTACH.
- **FR-011**: When catalog is disabled, `mssql_scan` and `mssql_exec` functions MUST work normally.
- **FR-012**: When catalog is disabled, attempts to use DuckDB catalog integration (e.g., `SELECT * FROM context.schema.table`) MUST return a clear error.

**Pool Statistics Extension:**
- **FR-013**: `mssql_pool_stats()` MUST include `active_connections` column showing currently borrowed/in-use connections.
- **FR-014**: `mssql_pool_stats()` MUST include `pinned_connections` column showing connections pinned to transactions.
- **FR-015**: Pool statistics MUST accurately reflect real-time connection states.

**TLS Default Behavior:**
- **FR-016**: When no encryption setting is specified, system MUST use TLS by default (no fallback to non-TLS for security).
- **FR-017**: When user explicitly sets `use_encrypt=false` and server requires TLS, system MUST return clear error rather than silent failure.
- **FR-018**: System MUST NOT change behavior for existing users who explicitly configured encryption settings.
- **FR-019**: Users MUST explicitly set `use_encrypt=false` to connect without TLS to servers that support non-TLS connections.

### Key Entities

- **PoolStatistics**: Extended with `active_connections` (size_t) and `pinned_connections` (size_t) fields.
- **MSSQLPoolConfig**: Extended with `catalog_enabled` (bool, default: true).
- **MSSQLConnectionInfo**: Extended with `catalog_enabled` field parsed from secret/connection string.
- **mssql_query_timeout setting**: New DuckDB setting for query timeout configuration.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users receive TLS-related error messages within 5 seconds of ATTACH attempt when TLS is misconfigured.
- **SC-002**: Users can run queries exceeding 30 seconds by configuring longer query timeouts.
- **SC-003**: Users with limited database permissions can successfully use `mssql_scan` and `mssql_exec` in catalog-free mode.
- **SC-004**: System administrators can monitor active and pinned connection counts via pool stats without additional tooling.
- **SC-005**: New users connecting to TLS-required servers succeed without manually enabling encryption.
- **SC-006**: Zero breaking changes for existing users with explicit encryption settings.

## Clarifications

### Session 2026-01-28

- Q: When server supports both TLS and non-TLS and no encryption setting is specified, should the system fall back to non-TLS if TLS fails? → A: No fallback - TLS-only when unspecified; users must explicitly set `use_encrypt=false` for non-TLS servers.
- Q: Should there be a maximum allowed value for query timeout to prevent misconfiguration? → A: No maximum limit; users have full control.

## Assumptions

- The TDS layer already supports passing query timeout values (DEFAULT_QUERY_TIMEOUT exists in tds_types.hpp).
- The PRELOGIN response already includes server encryption requirements (EncryptionOption enum exists).
- Connection pinning for transactions is already tracked internally (transaction manager uses pinned connections).
- The `mssql_exec` and `mssql_scan` functions already work independently of the catalog system.

# Feature Specification: Azure Token-Only Secret & Documentation Updates

**Feature Branch**: `036-azure-token-docs`
**Created**: 2026-02-13
**Status**: Draft
**Input**: User description: "Add ACCESS_TOKEN support in Azure secret without requiring HOST/DATABASE/PORT (issue #57). Add missing documentation notice for primary key update limitation (issue #53)."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Token-Only MSSQL Secret (Priority: P1)

A user managing multiple Azure SQL databases wants to create a single MSSQL secret containing only an ACCESS_TOKEN, then reuse it across multiple ATTACH statements with different servers and databases. Currently, the secret requires HOST, DATABASE, and PORT even when using ACCESS_TOKEN, preventing token reuse across connections.

**Why this priority**: This is the core feature request (issue #57). An access token represents a user identity, not a specific server/database. Requiring server details in the secret forces users to create separate secrets per database, which is cumbersome and defeats the purpose of centralized token management.

**Independent Test**: Can be fully tested by creating a secret with only ACCESS_TOKEN (no HOST/DATABASE/PORT), then attaching to multiple servers using that single secret with connection details in the ATTACH statement.

**Acceptance Scenarios**:

1. **Given** a user has a valid Azure AD access token, **When** they create an MSSQL secret with only `ACCESS_TOKEN` (no HOST, DATABASE, or PORT), **Then** the secret is created successfully without validation errors.

2. **Given** a token-only MSSQL secret exists, **When** the user attaches to different servers using the same secret with connection details in the ATTACH connection string, **Then** each attachment succeeds and queries return data.

3. **Given** a token-only MSSQL secret exists, **When** the user attaches using the secret and provides HOST/DATABASE via ATTACH options, **Then** the connection uses the ATTACH-provided values.

4. **Given** a token-only MSSQL secret exists, **When** the user attempts to attach without providing server details in either the secret or the ATTACH statement, **Then** a clear error message indicates that server/database information is required.

5. **Given** a user creates a secret with both ACCESS_TOKEN and HOST/DATABASE, **When** they attach using that secret, **Then** the behavior remains unchanged from current functionality (backward compatible).

---

### User Story 2 - Documentation for ACCESS_TOKEN in Azure Secret (Priority: P2)

A user reading the AZURE.md documentation wants to understand how to create a token-only secret for reuse across multiple connections.

**Why this priority**: Documentation ensures users can discover and correctly use the new token-only secret feature.

**Independent Test**: Can be verified by reading AZURE.md and finding clear examples of token-only secrets with multi-connection reuse.

**Acceptance Scenarios**:

1. **Given** a user reads AZURE.md, **When** they look at the "Manual Access Token" or "Using MSSQL Secrets" section, **Then** they find an example of creating a secret with only ACCESS_TOKEN.

2. **Given** a user reads AZURE.md, **When** they look at the token-only secret example, **Then** they find a multi-connection reuse example showing the same secret used for different servers.

---

### User Story 3 - Primary Key Update Limitation Notice (Priority: P3)

A user attempting to UPDATE primary key columns on an attached MSSQL table receives an error message but cannot find documentation explaining this limitation. The README should clearly document that updating primary key columns is not supported (issue #53).

**Why this priority**: Documentation-only change that helps users understand an existing limitation without code changes.

**Independent Test**: Can be verified by reading the README Limitations section and finding a clear statement about primary key column updates.

**Acceptance Scenarios**:

1. **Given** a user reads the README Limitations section, **When** they look for UPDATE limitations, **Then** they find a clear notice that updating primary key columns is not supported.

---

### Edge Cases

- What happens when a token-only secret is used with ATTACH but no connection string and no ATTACH options for server? The system should provide a helpful error.
- What happens when a secret has ACCESS_TOKEN plus HOST/DATABASE and the ATTACH also provides HOST/DATABASE? ATTACH values should take precedence (existing behavior).
- What happens when a token-only secret is created with an empty ACCESS_TOKEN string? Validation should reject it.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST allow creating an MSSQL secret with only `ACCESS_TOKEN` parameter (no HOST, DATABASE, or PORT required).
- **FR-002**: System MUST allow a token-only secret to be reused across multiple ATTACH statements where connection details are provided in the ATTACH connection string or options.
- **FR-003**: System MUST validate that when using a token-only secret, server/database information is provided at ATTACH time (either in connection string or ATTACH options).
- **FR-004**: System MUST remain backward compatible — existing secrets with ACCESS_TOKEN plus HOST/DATABASE/PORT continue to work unchanged.
- **FR-005**: System MUST reject token-only secrets with an empty ACCESS_TOKEN string.
- **FR-006**: AZURE.md MUST include documentation for token-only secret creation and multi-connection reuse patterns.
- **FR-007**: README.md Limitations section MUST document that updating primary key columns is not supported.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can create a token-only MSSQL secret and attach to 3+ different servers using the same secret, each returning query results.
- **SC-002**: Existing secrets with ACCESS_TOKEN plus HOST/DATABASE continue to work with zero changes required from users.
- **SC-003**: AZURE.md contains a documented example of token-only secret with multi-connection reuse.
- **SC-004**: README Limitations section includes primary key update limitation notice.

## Assumptions

- Access tokens are not server-specific — a single Azure AD token can authenticate to multiple Azure SQL databases the user has access to.
- The ATTACH connection string or ATTACH options will always provide the necessary server/database information when using a token-only secret.
- The existing precedence rules (ATTACH options > secret values) remain unchanged.
- The error message "Updating primary key columns is not supported" is the existing behavior and only needs documentation, not code changes.

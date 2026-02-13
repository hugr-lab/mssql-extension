# Feature Specification: DDL Schema Support (CREATE IF NOT EXISTS + Cache Invalidation)

**Feature Branch**: `035-ddl-schema-support`
**Created**: 2026-02-13
**Status**: Draft
**Input**: User description: "add support ddl create schema if not exists - should check if schema exists and skip. Check the cache invalidation after DDL DROP SCHEMA. (issue 54)"
**Issue**: [#54](https://github.com/hugr-lab/mssql-extension/issues/54)

## User Scenarios & Testing

### User Story 1 - Create Schema Idempotently (Priority: P1)

A user working with multiple schemas (e.g., staging environments) needs to run `CREATE SCHEMA IF NOT EXISTS` without errors when the schema already exists. Currently, the extension sends a raw `CREATE SCHEMA` statement to SQL Server, which fails if the schema already exists, producing a confusing error ("CREATE SCHEMA failed due to previous errors").

**Why this priority**: This is the core bug reported in issue #54. Users cannot safely create schemas in scripts or notebooks where the schema may already exist.

**Independent Test**: Can be fully tested by running `CREATE SCHEMA IF NOT EXISTS` twice in succession — first creates the schema, second silently succeeds.

**Acceptance Scenarios**:

1. **Given** a connected database with no schema named "staging", **When** the user runs `CREATE SCHEMA IF NOT EXISTS staging`, **Then** the schema is created successfully on SQL Server.
2. **Given** a connected database where schema "staging" already exists, **When** the user runs `CREATE SCHEMA IF NOT EXISTS staging`, **Then** the statement completes silently without error.
3. **Given** a connected database, **When** the user runs `CREATE SCHEMA staging` (without IF NOT EXISTS) and the schema already exists, **Then** an appropriate error is returned (existing behavior preserved).
4. **Given** a connected database in read-only mode, **When** the user runs `CREATE SCHEMA IF NOT EXISTS staging`, **Then** a clear "read-only mode" error is returned.

---

### User Story 2 - Cache Consistency After Schema DDL (Priority: P2)

After creating or dropping a schema, the extension's internal metadata cache must reflect the change immediately. Users should see new schemas in catalog queries and not see dropped schemas, without needing to manually refresh the cache.

**Why this priority**: Cache staleness after DDL leads to confusing behavior where schemas appear to not exist after creation or still appear after deletion.

**Independent Test**: Can be tested by creating a schema and immediately querying the catalog to verify the schema is visible; then dropping the schema and verifying it disappears.

**Acceptance Scenarios**:

1. **Given** a user creates a new schema, **When** they immediately query the catalog (e.g., list schemas), **Then** the newly created schema appears in the results.
2. **Given** a user drops an existing schema, **When** they immediately query the catalog, **Then** the dropped schema no longer appears.
3. **Given** a user drops a schema that contains no tables, **When** they run `DROP SCHEMA`, **Then** the schema is removed and the cache is updated without errors.
4. **Given** a user drops a schema and then re-creates it with `CREATE SCHEMA IF NOT EXISTS`, **Then** the schema is created successfully and visible in catalog queries.

---

### Edge Cases

- What happens when the user attempts to create a schema with special characters in the name (e.g., brackets, spaces)?
- What happens when `DROP SCHEMA` is called on a schema that does not exist (with and without IF EXISTS)?
- What happens when `DROP SCHEMA` is called on a non-empty schema (contains tables)?
- What happens when multiple concurrent sessions create the same schema with IF NOT EXISTS?

## Requirements

### Functional Requirements

- **FR-001**: The system MUST support `CREATE SCHEMA IF NOT EXISTS <name>` by checking whether the schema exists before attempting creation, and silently succeeding if it already exists.
- **FR-002**: The system MUST preserve existing behavior for `CREATE SCHEMA <name>` (without IF NOT EXISTS) — an error is returned if the schema already exists.
- **FR-003**: After a successful `CREATE SCHEMA` operation, the metadata cache MUST be invalidated so the new schema is immediately visible in catalog queries.
- **FR-004**: After a successful `DROP SCHEMA` operation, the metadata cache MUST be invalidated and the schema entry removed so the dropped schema is no longer visible in catalog queries.
- **FR-005**: Schema existence checks for IF NOT EXISTS MUST accurately determine whether the schema already exists on the remote server.
- **FR-006**: All schema DDL operations MUST respect the read-only mode access check.
- **FR-007**: Schema names with special characters MUST be properly quoted using the appropriate identifier quoting mechanism.

## Success Criteria

### Measurable Outcomes

- **SC-001**: `CREATE SCHEMA IF NOT EXISTS` succeeds without error when the schema already exists, 100% of the time.
- **SC-002**: After any schema DDL operation (CREATE or DROP), the catalog reflects the change immediately without manual cache refresh.
- **SC-003**: All existing DDL tests continue to pass without modification.
- **SC-004**: The fix resolves the error reported in issue #54 (`CREATE SCHEMA failed due to previous errors`).

## Assumptions

- The remote database server does not natively support `IF NOT EXISTS` syntax for `CREATE SCHEMA`. The extension must handle this logic by checking schema existence before executing the DDL.
- The existing `DROP SCHEMA` already calls cache invalidation and removes the schema entry — this spec requires verification that this works correctly end-to-end.
- Schema existence can be checked via the metadata cache if it's loaded, or via a direct query otherwise.
- `DROP SCHEMA IF EXISTS` handling follows the same pattern — check existence before executing.

## Out of Scope

- `ALTER SCHEMA` operations
- Schema ownership or authorization management
- Handling of non-empty schemas during DROP (the remote server already returns an error for this)
- Schema-level permissions

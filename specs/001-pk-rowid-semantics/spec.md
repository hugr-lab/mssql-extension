# Feature Specification: MSSQL rowid Semantics (PK-based Row Identity)

**Feature Branch**: `001-pk-rowid-semantics`
**Created**: 2026-01-25
**Status**: Draft
**Input**: User description: "Spec 05.04a - Expose a stable rowid for MSSQL tables in DuckDB by mapping DuckDB's row identity semantics to SQL Server primary keys."

## Overview

DuckDB relies on a stable row identifier (`rowid`) to support UPDATE/DELETE planning. For MSSQL tables accessed through this extension, the extension must provide a deterministic, stable mapping from SQL Server primary keys to DuckDB's `rowid` pseudo-column.

This specification covers:
- Primary key discovery and metadata extraction
- Caching PK metadata in table entries
- Exposing `rowid` as a pseudo-column for scans/projections
- Decoding `rowid` values into DuckDB vectors

**Scope boundaries**: UPDATE/DELETE execution is covered in a separate specification (05.04b).

## Clarifications

### Session 2026-01-25

- Q: Should PK discovery operations be logged for debugging/troubleshooting? → A: Debug-level logging only (via existing MSSQL_DEBUG)
- Q: How should PK cache invalidation be triggered? → A: Use existing invalidation mechanism (manual, TTL, DDL operations, mssql_exec)
- Q: Should unique constraints be used as fallback for rowid if no PK exists? → A: No, primary key only - no fallback to unique constraints
- Q: Which PK data types should rowid support? → A: All types valid for SQL Server PKs that are already supported by the extension's type mapping
- Q: How should views handle rowid requests? → A: Views get a distinct error message ("MSSQL: rowid not supported for views")

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Query rowid with Scalar Primary Key (Priority: P1)

A DuckDB user wants to retrieve the row identifier alongside data from an MSSQL table that has a single-column primary key. This enables them to reference specific rows for subsequent operations or debugging.

**Why this priority**: This is the most common use case - most tables have single-column primary keys. Without this, DuckDB cannot plan UPDATE/DELETE operations on MSSQL tables.

**Independent Test**: Can be fully tested by running `SELECT rowid, * FROM attached_mssql_table LIMIT 10` and verifying the rowid values match the primary key column values.

**Acceptance Scenarios**:

1. **Given** an MSSQL table with an INT primary key column `id`, **When** user runs `SELECT rowid, id, name FROM mssql.dbo.customers LIMIT 100`, **Then** the `rowid` column contains the same values as the `id` column with the correct integer type.

2. **Given** an MSSQL table with a BIGINT primary key column, **When** user selects rowid, **Then** the rowid type is BIGINT matching the PK type.

3. **Given** an MSSQL table with a VARCHAR primary key column, **When** user selects rowid, **Then** the rowid type is VARCHAR matching the PK type and values.

---

### User Story 2 - Query rowid with Composite Primary Key (Priority: P1)

A DuckDB user wants to retrieve the row identifier from an MSSQL table that has a multi-column primary key. The rowid must be a STRUCT containing all PK columns in the correct order.

**Why this priority**: Composite primary keys are common in multi-tenant applications and junction tables. Supporting this is essential for complete rowid semantics.

**Independent Test**: Can be fully tested by running `SELECT rowid, * FROM composite_pk_table LIMIT 10` and verifying the rowid is a STRUCT with fields matching PK column names and values.

**Acceptance Scenarios**:

1. **Given** an MSSQL table with composite PK `(tenant_id INT, id BIGINT)`, **When** user runs `SELECT rowid FROM mssql.dbo.orders LIMIT 10`, **Then** rowid is a `STRUCT(tenant_id INTEGER, id BIGINT)` with correct field values.

2. **Given** a table with 3-column composite PK in index order `(region, year, seq)`, **When** user selects rowid, **Then** the STRUCT field order matches the SQL Server PK ordinal order exactly.

---

### User Story 3 - Query Without rowid (No Overhead) (Priority: P2)

A DuckDB user queries an MSSQL table without requesting the rowid column. The system should not add overhead by fetching unnecessary PK columns.

**Why this priority**: Performance optimization - most queries don't need rowid, and adding hidden columns would increase network and processing overhead.

**Independent Test**: Can be verified by checking that `SELECT name, value FROM mssql_table` does not include PK columns in the generated SQL when PK columns aren't explicitly requested.

**Acceptance Scenarios**:

1. **Given** an MSSQL table with PK column `id` not in the select list, **When** user runs `SELECT name, value FROM mssql.dbo.products`, **Then** the remote query does not include `id` unless it's already in the user's select list.

2. **Given** a table where user explicitly selects the PK column, **When** user runs `SELECT id, name FROM table`, **Then** id is fetched once (not duplicated for rowid purposes).

---

### User Story 4 - Handle Tables Without Primary Key (Priority: P2)

A DuckDB user attempts to query rowid from an MSSQL table that has no primary key defined. The system must provide a clear error while still allowing regular SELECT queries.

**Why this priority**: Graceful degradation - users should be able to read data from PK-less tables but receive clear feedback when rowid is unavailable.

**Independent Test**: Can be tested by creating a table without PK and verifying `SELECT * FROM table` works but `SELECT rowid FROM table` produces a clear error.

**Acceptance Scenarios**:

1. **Given** an MSSQL table without a primary key, **When** user runs `SELECT rowid, * FROM mssql.dbo.logs`, **Then** the system returns error "MSSQL: rowid requires a primary key".

2. **Given** an MSSQL table without a primary key, **When** user runs `SELECT * FROM mssql.dbo.logs`, **Then** the query succeeds normally without error.

3. **Given** an MSSQL table without a primary key, **When** user runs `SELECT COUNT(col) FROM mssql.dbo.logs`, **Then** the query succeeds normally.

---

### Edge Cases

- What happens when a PK column value is NULL? System must throw "MSSQL: invalid NULL primary key value in rowid mapping" (PK columns should never be NULL per SQL Server constraints).
- How does the system handle very wide composite keys (e.g., 5+ columns)? The STRUCT must still be built correctly with all fields.
- What if PK discovery query fails due to permissions? System should throw a clear error about metadata access.
- How are unicode/special characters in PK column names handled? Field names in STRUCT must preserve the original column names.
- What about tables with unique constraints but no PK? Rowid requires an explicit primary key; unique constraints are not used as a fallback.
- What about SQL Server views? Views receive a distinct error "MSSQL: rowid not supported for views" rather than the generic "no PK" error.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST discover primary key metadata for each MSSQL table including: column list, ordinal order, data types, and collation (for string columns).

- **FR-002**: System MUST cache PK metadata in `MSSQLTableEntry` including: `pk_exists` (bool), `pk_columns` (ordered list), `pk_column_types` (DuckDB LogicalType per column), `pk_column_collations` (for string columns), and `rowid_type` (scalar or STRUCT).

- **FR-003**: System MUST support lazy loading of PK cache - populated on first rowid access or DML planning.

- **FR-004**: System MUST map scalar primary keys (single column) directly to rowid with matching DuckDB type. All SQL Server PK-eligible types that are supported by the extension's existing type mapping MUST be supported for rowid.

- **FR-005**: System MUST map composite primary keys (multiple columns) to a DuckDB STRUCT type with field names matching PK column names and field order matching SQL Server PK ordinal order.

- **FR-006**: System MUST only include PK columns in the remote SELECT when rowid is explicitly requested in the scan projection.

- **FR-007**: System MUST populate the rowid vector in the DuckDB DataChunk when rowid is requested, correctly constructing scalar values or STRUCT values.

- **FR-008**: System MUST throw error "MSSQL: rowid requires a primary key" when rowid is requested from a table without a defined primary key.

- **FR-009**: System MUST throw error "MSSQL: invalid NULL primary key value in rowid mapping" if a PK column value is unexpectedly NULL during rowid construction.

- **FR-012**: System MUST throw error "MSSQL: rowid not supported for views" when rowid is requested from a SQL Server view (distinct from the "no PK" error for tables).

- **FR-010**: System MUST NOT use physical row addressing mechanisms (%%physloc%%, RID-based addressing, or any unstable locator). Only PK-based logical identity is allowed.

- **FR-011**: System MUST invalidate the PK cache using the existing catalog invalidation mechanism (manual refresh, TTL expiration, DDL operations via catalog, mssql_exec).

### Non-Functional Requirements

- **NFR-001**: PK discovery operations MUST emit debug-level log entries when MSSQL_DEBUG is enabled, for troubleshooting purposes. No logging overhead in normal operation.

### Key Entities

- **PrimaryKeyInfo**: Represents cached PK metadata for an MSSQL table - contains existence flag, column list with types and collations, and computed rowid type.

- **MSSQLTableEntry**: Extended to include PrimaryKeyInfo cache with lazy loading support.

- **RowidVector**: The DuckDB vector populated during scan when rowid is projected - either scalar type or STRUCT type depending on PK composition.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Queries requesting rowid from tables with scalar PK return correct values matching the PK column with 100% accuracy.

- **SC-002**: Queries requesting rowid from tables with composite PK return correctly structured STRUCT values with all fields in proper order.

- **SC-003**: Queries not requesting rowid do not incur measurable overhead from PK column fetching (verified by inspecting generated SQL).

- **SC-004**: Tables without primary keys remain fully queryable for all non-rowid operations (SELECT, aggregations, filters).

- **SC-005**: Error messages for rowid-related failures are clear and actionable, identifying the specific issue (no PK, NULL value, etc.).

- **SC-006**: PK metadata discovery completes within the same latency bounds as existing table metadata queries.

## Assumptions

- SQL Server system catalog views (`sys.key_constraints`, `sys.index_columns`, `sys.columns`, `sys.indexes`) are accessible to the connected user.
- Primary key constraints in SQL Server always define NOT NULL columns (standard SQL behavior).
- DuckDB's STRUCT type can accommodate any reasonable number of PK columns (typical limit is well above practical composite key sizes).
- Collation metadata affects DML predicate generation (Spec 05.04b) but not rowid value decoding itself.

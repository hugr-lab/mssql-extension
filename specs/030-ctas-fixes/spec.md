# Feature Specification: CTAS Fixes - IF NOT EXISTS and Auto-TABLOCK

**Feature Branch**: `030-ctas-fixes`
**Created**: 2026-02-05
**Status**: Draft
**Input**: User description: "Fix CREATE TABLE IF NOT EXISTS and add TABLOCK for new tables (issues #44 and #45)"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Idempotent Table Creation (Priority: P1)

As a data engineer running ETL pipelines, I want to use `CREATE TABLE IF NOT EXISTS` to safely create tables without errors when the table already exists, so that my pipeline scripts are idempotent and can be re-run safely.

**Why this priority**: This is a bug fix (issue #44) that breaks a standard SQL pattern users expect to work. Without this fix, users cannot write idempotent scripts that safely handle existing tables.

**Independent Test**: Can be fully tested by running `CREATE TABLE IF NOT EXISTS` twice and verifying no error occurs on the second run.

**Acceptance Scenarios**:

1. **Given** a table does not exist, **When** user executes `CREATE TABLE IF NOT EXISTS db.schema.table AS SELECT ...`, **Then** the table is created and populated with data.
2. **Given** a table already exists, **When** user executes `CREATE TABLE IF NOT EXISTS db.schema.table AS SELECT ...`, **Then** the operation completes successfully without error and the existing table remains unchanged.
3. **Given** a table already exists, **When** user executes `CREATE TABLE IF NOT EXISTS db.schema.table (col1 INT)` (without AS SELECT), **Then** the operation completes successfully without error and the existing table remains unchanged.

---

### User Story 2 - Automatic TABLOCK for New Tables (Priority: P2)

As a data engineer performing bulk data loads, I want CTAS and COPY operations to automatically use TABLOCK when creating a new table, so that I get optimal performance without manually configuring settings.

**Why this priority**: This is a performance enhancement (issue #45) that provides 15-30% faster bulk loads with no downside when creating new tables (no concurrent readers exist yet).

**Independent Test**: Can be verified by creating a large table via CTAS and observing improved performance compared to the current default behavior.

**Acceptance Scenarios**:

1. **Given** the user executes `CREATE TABLE db.schema.new_table AS SELECT ...` where `new_table` does not exist, **When** the BCP protocol is used for data transfer, **Then** TABLOCK hint is automatically applied for optimal performance.
2. **Given** the user executes `COPY TO 'mssql://...' (CREATE_TABLE true)` where the target table does not exist, **When** the BCP protocol is used, **Then** TABLOCK hint is automatically applied.
3. **Given** the user has manually set `mssql_copy_tablock = false`, **When** creating a new table via CTAS, **Then** the user's explicit setting is respected and TABLOCK is not used.
4. **Given** the user executes `CREATE OR REPLACE TABLE ...` where the table already exists, **When** the table is dropped and recreated, **Then** TABLOCK is automatically applied since the new table has no concurrent readers.

---

### Edge Cases

- What happens when `CREATE TABLE IF NOT EXISTS` targets a view or other non-table object with the same name? The behavior follows SQL Server's semantics; the operation should succeed without modification (treating the existing object as "exists").
- What happens when `CREATE TABLE IF NOT EXISTS` fails for reasons other than table existence (e.g., permission denied, schema doesn't exist)? The error is propagated to the user normally.
- What happens when a user explicitly sets `mssql_copy_tablock = true` and creates a table that already exists (overwrite scenario)? The explicit setting takes precedence, and TABLOCK is used.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST handle `OnCreateConflict::IGNORE_ON_CONFLICT` (IF NOT EXISTS) by silently succeeding when the target table already exists, without modifying the existing table.
- **FR-002**: System MUST create the table and populate it with data when using `IF NOT EXISTS` and the table does not exist.
- **FR-003**: System MUST return 0 rows affected when `IF NOT EXISTS` is used and the table already exists (no data inserted).
- **FR-004**: System MUST automatically enable TABLOCK hint for BCP operations when creating a brand-new table (table did not exist before the operation).
- **FR-005**: System MUST respect the user's explicit `mssql_copy_tablock` setting when it is explicitly set (user preference overrides automatic behavior).
- **FR-006**: System MUST apply automatic TABLOCK to both CTAS (`CREATE TABLE AS SELECT`) and COPY TO (`COPY TO 'mssql://...'`) operations when creating new tables.
- **FR-007**: System MUST apply automatic TABLOCK when using `CREATE OR REPLACE TABLE` since the table is dropped and recreated as new.
- **FR-008**: System MUST NOT apply automatic TABLOCK when inserting into an existing table (e.g., `COPY TO` without `CREATE_TABLE` option).

### Key Entities

- **CTASTarget**: Extended to track whether `IF NOT EXISTS` behavior is requested via `if_not_exists` boolean flag.
- **BCPConfig/CTASConfig**: Extended to support automatic TABLOCK detection based on whether a new table is being created.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: `CREATE TABLE IF NOT EXISTS` operations complete successfully without error when the table already exists, matching standard SQL semantics.
- **SC-002**: Bulk load operations on new tables achieve 15-30% performance improvement compared to the baseline without TABLOCK.
- **SC-003**: All existing CTAS and COPY tests continue to pass (no regression).
- **SC-004**: Users can run idempotent scripts using `CREATE TABLE IF NOT EXISTS` without modification from standard DuckDB syntax.

## Assumptions

- The `OnCreateConflict::IGNORE_ON_CONFLICT` enum value is available from DuckDB's API and represents the `IF NOT EXISTS` clause.
- TABLOCK provides performance benefits only for BCP protocol; INSERT-based CTAS does not benefit from table-level locking.
- The determination of "new table" can be made at the DDL phase before data transfer begins.
- SQL Server's `OBJECT_ID()` function can reliably detect table existence for the IF NOT EXISTS check.

## Dependencies

- DuckDB's `OnCreateConflict` enum must include `IGNORE_ON_CONFLICT` for IF NOT EXISTS handling.
- Existing BCP protocol implementation (Spec 024, 027) provides the TABLOCK hint infrastructure.

## Related Issues

- GitHub Issue #44: CREATE TABLE IF NOT EXISTS is broken
- GitHub Issue #45: Use TABLOCK in the CTAS and COPY when a new table is created

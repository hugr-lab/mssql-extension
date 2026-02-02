# Feature Specification: CTAS BCP Integration

**Feature Branch**: `027-ctas-bcp-integration`
**Created**: 2026-02-02
**Status**: Draft
**Input**: User description: "CTAS: Add settings flag to use BCP in CTAS (true by default), implement BCP flow in CTAS, update documentation, set TABLOCK default to false, update testing docs"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - CTAS with BCP Mode (Priority: P1)

A DuckDB user executes `CREATE TABLE mssql_db.schema.table AS SELECT ...` to materialize DuckDB query results into a new SQL Server table. With `mssql_ctas_use_bcp = true` (the new default), the extension creates the table via DDL then uses the TDS BulkLoadBCP protocol (packet type 0x07) for data transfer instead of batched INSERT statements, achieving higher throughput.

**Why this priority**: This is the core improvement - BCP provides significantly better performance (2-10x faster) than batched INSERT for bulk data loads. This is the primary value proposition of the feature.

**Independent Test**: Execute `CREATE TABLE mssql.dbo.test_bcp AS SELECT * FROM range(100000)` with `mssql_ctas_use_bcp = true` (default) and verify the table contains exactly 100000 rows, transferred via BCP protocol.

**Acceptance Scenarios**:

1. **Given** `mssql_ctas_use_bcp = true` (default) and an attached MSSQL database, **When** user executes `CREATE TABLE mssql.dbo.ctas_bcp AS SELECT i AS id, 'Item ' || i AS name FROM range(1, 1001)`, **Then** table `dbo.ctas_bcp` is created with 1000 rows using BCP protocol.
2. **Given** `mssql_ctas_use_bcp = true`, **When** CTAS completes, **Then** debug logs (MSSQL_DEBUG=1) show BCP batch progress instead of INSERT batch progress.
3. **Given** `mssql_ctas_use_bcp = true` and a source query producing zero rows, **When** CTAS executes, **Then** the table is created with correct schema and no data (BCP with zero rows is valid).

---

### User Story 2 - CTAS with Legacy INSERT Mode (Priority: P2)

A user who needs the legacy batched INSERT behavior (for compatibility, debugging, or specific error handling requirements) can disable BCP by setting `mssql_ctas_use_bcp = false`, reverting to the original multi-VALUES INSERT approach.

**Why this priority**: Provides backward compatibility and a fallback mechanism. Not the primary use case but important for users who encounter edge cases with BCP.

**Independent Test**: Execute CTAS with `SET mssql_ctas_use_bcp = false` and verify that debug logs show INSERT batches instead of BCP batches.

**Acceptance Scenarios**:

1. **Given** `mssql_ctas_use_bcp = false`, **When** user executes CTAS, **Then** data is inserted via batched INSERT statements (legacy behavior).
2. **Given** `mssql_ctas_use_bcp = false`, **When** CTAS fails during data transfer, **Then** error messages reference INSERT failures (not BCP failures).

---

### User Story 3 - TABLOCK Default Change (Priority: P1)

The `mssql_copy_tablock` setting default changes from `true` to `false` to ensure safer default behavior. TABLOCK provides 15-30% better performance but acquires exclusive table locks that can cause blocking in multi-user environments. Users who want maximum performance can explicitly enable TABLOCK.

**Why this priority**: Changing a default setting affects all users. The safer default prevents unexpected blocking issues in production environments.

**Independent Test**: Run COPY TO without explicitly setting TABLOCK and verify that the operation completes without acquiring TABLOCK (observable via SQL Server's `sys.dm_tran_locks` or by concurrent read test).

**Acceptance Scenarios**:

1. **Given** `mssql_copy_tablock` is not explicitly set, **When** user executes COPY TO, **Then** the BCP operation does NOT use TABLOCK hint (default is now `false`).
2. **Given** `mssql_copy_tablock = true` explicitly set, **When** user executes COPY TO, **Then** the BCP operation uses TABLOCK hint for better performance.
3. **Given** `mssql_ctas_use_bcp = true` (default), **When** CTAS executes the BCP phase, **Then** it respects the `mssql_copy_tablock` setting.

---

### User Story 4 - Documentation Updates (Priority: P2)

Architecture documentation, README, and CLAUDE.md are updated to reflect the new BCP-based CTAS behavior, the TABLOCK default change, and all features added since v0.1.14.

**Why this priority**: Documentation is essential for users to understand and correctly use the new features, but it doesn't affect runtime behavior.

**Independent Test**: Review updated documentation files and verify they accurately describe the new behavior, settings, and defaults.

**Acceptance Scenarios**:

1. **Given** the updated README, **When** a user reads the CTAS section, **Then** they understand that BCP is used by default and can be disabled.
2. **Given** the updated README, **When** a user reads the COPY TO section, **Then** they see that TABLOCK is now `false` by default.
3. **Given** the updated docs/architecture.md, **When** a developer reads the CTAS internals section, **Then** they understand the BCP integration points.

---

### User Story 5 - Testing Documentation Updates (Priority: P3)

The `docs/testing.md` document is updated to include guidance for testing features added since v0.1.14, including CTAS tests, VARCHAR encoding tests, and any new test patterns.

**Why this priority**: Testing documentation is important for contributors but doesn't affect end-user functionality.

**Independent Test**: Review docs/testing.md and verify it includes sections for all test directories and test patterns added since v0.1.14.

**Acceptance Scenarios**:

1. **Given** the updated testing.md, **When** a developer looks up CTAS testing, **Then** they find guidance on writing CTAS tests with examples.
2. **Given** the updated testing.md, **When** a developer checks the test directory structure, **Then** the list includes `test/sql/ctas/` and `test/sql/catalog/varchar_encoding.test`.

---

### Edge Cases

- What happens when BCP encounters a type not supported by the BCP encoder? The operation fails with a clear error naming the unsupported type before any data is transferred.
- What happens when CTAS with BCP is executed in a transaction? BCP requires its own transaction context; the operation may fail or auto-commit depending on transaction state.
- What happens when the connection is lost during BCP transfer? The partial BCP batch is rolled back; the table may contain partial data from previous batches.
- What happens when `mssql_ctas_drop_on_failure = true` and BCP fails? The cleanup DROP TABLE is attempted using the same logic as INSERT failures.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The extension MUST support a new setting `mssql_ctas_use_bcp` (BOOLEAN, default: `true`) to control whether CTAS uses BCP protocol for data transfer.
- **FR-002**: When `mssql_ctas_use_bcp = true`, CTAS MUST use TDS BulkLoadBCP (packet type 0x07) for data transfer after successful CREATE TABLE.
- **FR-003**: When `mssql_ctas_use_bcp = false`, CTAS MUST use the existing batched INSERT approach (legacy behavior).
- **FR-004**: The `mssql_copy_tablock` setting default MUST change from `true` to `false`.
- **FR-005**: CTAS with BCP MUST respect the `mssql_copy_tablock` setting for controlling TABLOCK hint usage.
- **FR-006**: CTAS with BCP MUST use the existing BCP type mapping and encoding infrastructure from the COPY TO implementation.
- **FR-007**: CTAS with BCP MUST maintain bounded memory usage by streaming rows in batches, respecting `mssql_copy_flush_rows` setting.
- **FR-008**: CTAS with BCP MUST emit debug-level logging (MSSQL_DEBUG, MSSQL_DML_DEBUG) showing BCP batch progress.
- **FR-009**: CTAS with BCP failure handling MUST follow the same cleanup semantics as INSERT mode (`mssql_ctas_drop_on_failure`).
- **FR-010**: README MUST document the `mssql_ctas_use_bcp` setting with its default value and behavior.
- **FR-011**: README MUST document the `mssql_copy_tablock` default change from `true` to `false`.
- **FR-012**: docs/architecture.md MUST document the CTAS BCP integration architecture.
- **FR-013**: docs/testing.md MUST include guidance for CTAS tests and any new test patterns since v0.1.14.
- **FR-014**: CLAUDE.md MUST document the `mssql_ctas_use_bcp` setting.

### Settings

| Setting              | Type    | Old Default | New Default | Description                             |
|----------------------|---------|-------------|-------------|-----------------------------------------|
| `mssql_ctas_use_bcp` | BOOLEAN | N/A         | `true`      | Use BCP protocol for CTAS data transfer |
| `mssql_copy_tablock` | BOOLEAN | `true`      | `false`     | Use TABLOCK hint for BCP operations     |

### Key Entities

- **CTASExecutionState**: Extended to support both INSERT and BCP execution modes, selected based on `mssql_ctas_use_bcp` setting.
- **BCPExecutor**: Existing BCP infrastructure from COPY TO, reused for CTAS data transfer.
- **CTASConfig**: Extended to include `use_bcp` flag derived from the setting.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: CTAS with BCP (default) achieves at least 2x throughput compared to INSERT mode for 100K+ row transfers.
- **SC-002**: CTAS with `mssql_ctas_use_bcp = false` produces identical results to the current INSERT-based implementation.
- **SC-003**: COPY TO operations with default settings (TABLOCK=false) complete successfully without blocking concurrent reads.
- **SC-004**: All existing CTAS tests pass with both BCP mode (default) and INSERT mode (legacy).
- **SC-005**: Documentation accurately reflects all setting changes and new features.
- **SC-006**: Testing documentation covers all test directories and patterns added since v0.1.14.

## Assumptions

- The existing BCP encoder from COPY TO (src/copy/bcp_copy_function.cpp, src/tds/encoding/bcp_row_encoder.cpp) is reusable for CTAS.
- BCP type mapping matches the CTAS type mapping (both derived from DuckDB types).
- The connection pool and pinned connection infrastructure supports BCP operations initiated from CTAS.
- Zero-row BCP transfers (CTAS from empty result set) are valid and create an empty table.

## Scope Boundaries

### In Scope

- New `mssql_ctas_use_bcp` setting with default `true`
- CTAS BCP integration using existing BCP infrastructure
- `mssql_copy_tablock` default change from `true` to `false`
- README documentation updates for new settings and behavior
- Architecture documentation updates for CTAS BCP integration
- Testing documentation updates for features since v0.1.14
- CLAUDE.md updates for new settings

### Out of Scope

- Changes to COPY TO functionality (only default change)
- New BCP features beyond what exists for COPY TO
- Transaction semantics changes for CTAS
- OR REPLACE behavior changes
- Performance benchmarking infrastructure
- Backward-incompatible API changes

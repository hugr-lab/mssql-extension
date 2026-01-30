# Feature Specification: BCP Improvements

**Feature Branch**: `025-bcp-improvements`
**Created**: 2026-01-30
**Status**: Draft
**Input**: User description: "Some small improvements to BCP: 1. In copy target should be allow to use empty schema name for the temporary tables: 'mssql://db//#tmp' or 'db..#temp'. The current schema should work to ('mssql://db/#temp', 'db.#tmp'). 2. connection leaks and column type mismatch to pass in the existing table. 3. Can we also support ordinary insert?"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Empty Schema Syntax for Temp Tables (Priority: P1)

Users want flexibility when specifying temp table targets in COPY TO commands. Currently temp tables work with `mssql://db/#temp` or `db.#temp`, but users also want to use explicit empty schema notation like `mssql://db//#temp` or `db..#temp` for consistency with SQL Server conventions.

**Why this priority**: This is a usability enhancement that aligns with SQL Server naming conventions where temp tables have no schema. Users coming from SQL Server expect both syntaxes to work.

**Independent Test**: Can be fully tested by running COPY TO commands with different temp table URL/catalog syntaxes and verifying all formats work correctly.

**Acceptance Scenarios**:

1. **Given** a local DuckDB table with data, **When** user runs `COPY data TO 'mssql://db//#temp' (FORMAT 'bcp')`, **Then** the temp table is created successfully with data copied.
2. **Given** a local DuckDB table with data, **When** user runs `COPY data TO 'db..#temp' (FORMAT 'bcp')`, **Then** the temp table is created successfully with data copied.
3. **Given** existing syntax `mssql://db/#temp` and `db.#temp`, **When** user uses these formats, **Then** they continue to work as before (backward compatibility).

---

### User Story 2 - Connection Leak Prevention (Priority: P1)

Users need reliable connection management during COPY operations. When errors occur (such as column type mismatches or network issues), connections should be properly released back to the pool to prevent connection exhaustion.

**Why this priority**: Connection leaks can cause production issues where the application runs out of connections and fails to execute any queries. This is a critical reliability fix.

**Independent Test**: Can be tested by intentionally causing errors during COPY and verifying connections are properly returned to the pool via `mssql_pool_stats()`.

**Acceptance Scenarios**:

1. **Given** a COPY operation that fails due to column type mismatch, **When** the error occurs, **Then** the connection is released back to the pool (not leaked).
2. **Given** a COPY operation that fails at any stage (bind, sink, or finalize phase), **When** the error occurs, **Then** the connection is properly cleaned up.
3. **Given** multiple failed COPY operations, **When** checking pool stats with `mssql_pool_stats()`, **Then** the connection count remains stable (no accumulating leaked connections).

---

### User Story 3 - Column Type Mismatch Handling for Existing Tables (Priority: P2)

When copying data to an existing table, users need clear feedback when the source data types don't match the target table's column types. Instead of cryptic errors or silent failures, the system should provide actionable error messages.

**Why this priority**: Type mismatches are a common error scenario. Clear error messages reduce debugging time and improve user experience.

**Independent Test**: Can be tested by attempting COPY to existing tables with incompatible types and verifying clear error messages are returned.

**Acceptance Scenarios**:

1. **Given** an existing SQL Server table with INT column, **When** user copies data with VARCHAR values to that column, **Then** a clear error message indicates which column has a type mismatch.
2. **Given** an existing SQL Server table with specific column types, **When** source data has compatible but different types (e.g., INT to BIGINT), **Then** implicit conversion occurs where SQL Server allows it.
3. **Given** a type mismatch error, **When** the error message is displayed, **Then** it includes the column name, expected type, and actual type.

---

### Edge Cases

- What happens when using `mssql://db///table` (multiple empty path segments)? Should reject with clear error.
- How does the system handle a COPY operation interrupted mid-stream (network disconnect)? Connection should be cleaned up.
- What happens when INSERT method encounters a row that violates a constraint? Should report which row failed.
- How does INSERT method handle very large datasets? Should batch appropriately and warn about performance.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST accept empty schema syntax `mssql://db//#table` for temp tables, treating empty schema as valid for temp table targets.
- **FR-002**: System MUST accept empty schema syntax `db..#table` (catalog dot-dot table) for temp tables.
- **FR-003**: System MUST maintain backward compatibility with existing `mssql://db/#table` and `db.#table` syntax.
- **FR-004**: System MUST release connections back to the pool on any COPY error (bind, sink, or finalize phase).
- **FR-005**: System MUST provide clear error messages for column type mismatches when copying to existing tables, including column name and type information.
- **FR-006**: System MUST support an optional INSERT method via a COPY option (e.g., `METHOD 'insert'`) as an alternative to BCP.
- **FR-007**: INSERT method MUST batch rows using the existing `mssql_insert_batch_size` setting.
- **FR-008**: INSERT method MUST support RETURNING clause for identity columns when used with compatible options.
- **FR-009**: System MUST reject invalid URL formats (e.g., triple slashes for non-temp tables) with clear error messages.

### Key Entities

- **BCPCopyTarget**: Represents the target table for COPY operations. Extended to handle empty schema notation.
- **Connection Pool**: Manages database connections. Must properly track and release connections on errors.
- **Copy Method**: New concept distinguishing between BCP protocol and INSERT statement approaches.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: All four temp table syntaxes (`mssql://db/#t`, `mssql://db//#t`, `db.#t`, `db..#t`) successfully create and populate temp tables.
- **SC-002**: After 100 intentionally failed COPY operations, connection pool shows zero leaked connections.
- **SC-003**: Type mismatch errors include column name and both expected and actual types in the error message.
- **SC-005**: Existing COPY tests continue to pass (backward compatibility maintained).

## Assumptions

- The empty schema syntax is only valid for temp tables (starting with `#` or `##`). Regular tables still require a valid schema name.
- INSERT method will use the existing INSERT infrastructure already implemented in the extension.
- Connection cleanup follows the existing transaction-aware patterns (pinned connections in transactions stay pinned).
- INSERT method performance is expected to be slower than BCP; this is acceptable for the use cases it serves.

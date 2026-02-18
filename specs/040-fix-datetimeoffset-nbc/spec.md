# Feature Specification: Fix DATETIMEOFFSET in NBC Row Reader

**Feature Branch**: `040-fix-datetimeoffset-nbc`
**Created**: 2026-02-18
**Status**: Draft
**Input**: User description: "Fix issue #78 — DATETIMEOFFSET columns cause error when SQL Server sends NBCROW tokens; verify all datetime types at all scales in both ROW and NBCROW paths"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Read tables containing DATETIMEOFFSET columns via NBCROW (Priority: P1)

A user ATTACHes a SQL Server database and queries a table that contains one or more DATETIMEOFFSET columns. The query returns correct UTC timestamps regardless of how SQL Server encodes the rows (standard ROW or compressed NBCROW format).

**Why this priority**: This is the core bug. Users cannot read any table with DATETIMEOFFSET columns when SQL Server chooses the NBCROW encoding (which happens automatically when rows have nullable columns). This blocks real-world usage since most production tables have nullable columns.

**Independent Test**: Can be tested by creating a SQL Server table with many nullable columns (to force NBCROW encoding) including DATETIMEOFFSET columns with both NULL and non-NULL values, then querying it through DuckDB and verifying correct results.

**Acceptance Scenarios**:

1. **Given** a SQL Server table with many nullable columns (triggering NBCROW) and a DATETIMEOFFSET(7) column containing non-NULL values, **When** the user runs `SELECT * FROM mssql_db.dbo.table_name`, **Then** all DATETIMEOFFSET values are returned as TIMESTAMP WITH TIME ZONE in DuckDB, converted to UTC.
2. **Given** a SQL Server table with a DATETIMEOFFSET column where some rows are NULL, **When** the user runs a SELECT via NBCROW encoding, **Then** NULL values are returned as NULL and non-NULL values are correctly converted to UTC timestamps.
3. **Given** a SQL Server table with a DATETIMEOFFSET column, **When** SQL Server sends results using NBCROW format, **Then** the extension reads values without error (no "Unsupported type in NBC RowReader: DATETIMEOFFSET" exception).

---

### User Story 2 - All datetime types at all scales in both ROW and NBCROW (Priority: P2)

A user queries tables containing scale-dependent datetime types (TIME, DATETIME2, DATETIMEOFFSET) with various fractional-second scales (0 through 7). The wire encoding byte length varies by scale (3-5 bytes for the time component), so all scales must work correctly in both standard ROW and NBCROW encoding paths.

**Why this priority**: The scale determines the wire byte count. Scale 0 (3 bytes), scale 3 (4 bytes), and scale 7 (5 bytes) hit all three byte-length buckets. Existing tests cover DATETIME2 at scales 0/3/7 and DATETIMEOFFSET at scales 3/7, but DATETIMEOFFSET scale 0 and TIME at varying scales are untested. Comprehensive scale coverage prevents subtle byte-length bugs.

**Independent Test**: Can be tested by creating tables with TIME(0), TIME(3), TIME(7), DATETIME2(0), DATETIME2(3), DATETIME2(7), DATETIMEOFFSET(0), DATETIMEOFFSET(3), DATETIMEOFFSET(7) and verifying each returns the expected value.

**Acceptance Scenarios**:

1. **Given** columns with DATETIMEOFFSET(0), DATETIMEOFFSET(3), DATETIMEOFFSET(7), **When** queried through DuckDB (both ROW and NBCROW), **Then** each returns correct UTC timestamps at the appropriate precision.
2. **Given** columns with TIME(0), TIME(3), TIME(7), **When** queried through DuckDB (both ROW and NBCROW), **Then** each returns correct time values at the appropriate precision.
3. **Given** columns with DATETIME2(0), DATETIME2(3), DATETIME2(7), **When** queried through DuckDB (both ROW and NBCROW), **Then** each returns correct timestamps at the appropriate precision (existing scale tests pass, plus new NBCROW coverage).

---

### User Story 3 - Verify all supported types read correctly in NBCROW (Priority: P2)

All data types supported by the extension must be readable in both standard ROW and NBCROW formats. A code audit confirms this is already the case for all types except DATETIMEOFFSET, but the fix must be validated alongside existing types to ensure no regressions.

**Why this priority**: NBCROW is not a rare edge case — SQL Server uses it automatically when it estimates bandwidth savings. Any type gap in the NBC reader silently blocks users. A comprehensive NBC test ensures future type additions don't miss the NBC path.

**Independent Test**: Can be tested by creating a table with columns of all supported types (including many nullable columns to trigger NBCROW) and verifying a SELECT * returns correct values for every column.

**Acceptance Scenarios**:

1. **Given** a table with columns of every supported data type (integers, floats, strings, decimals, date, time at multiple scales, datetime, datetime2 at multiple scales, datetimeoffset at multiple scales, uniqueidentifier, binary), **When** queried via NBCROW encoding, **Then** all values are returned correctly with proper type mapping.
2. **Given** a row where all nullable columns are NULL, **When** queried via NBCROW, **Then** all NULL columns return NULL and non-nullable columns return correct values.

---

### Edge Cases

- What happens when a DATETIMEOFFSET column contains only NULL values in all rows? The extension must handle the NBCROW null bitmap correctly without attempting to read value bytes.
- What happens when DATETIMEOFFSET is used with scale 0 (smallest wire encoding: 3 time bytes + 3 date + 2 offset = 8 data bytes)? The 1-byte length prefix must be read correctly.
- What happens when DATETIMEOFFSET is used with scale 7 (largest wire encoding: 5 time bytes + 3 date + 2 offset = 10 data bytes)? The 1-byte length prefix must be read correctly.
- What happens when TIME is used with scale 0 (3 bytes) vs scale 7 (5 bytes)? Both ROW and NBCROW must handle the varying byte length.
- What happens when DATETIMEOFFSET columns with different scales (e.g., scale 0 and scale 7) appear in the same table? Each column must use its own scale for byte length calculation.
- What happens when a table has many nullable columns alongside datetime types, triggering NBCROW encoding? The null bitmap parsing must correctly identify which columns are null vs non-null.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The extension MUST read DATETIMEOFFSET values from NBCROW-encoded result rows without error.
- **FR-002**: The extension MUST return DATETIMEOFFSET values as DuckDB TIMESTAMP WITH TIME ZONE type, converted to UTC.
- **FR-003**: The extension MUST support all DATETIMEOFFSET scales (0 through 7) in both ROW and NBCROW rows, with correct byte length handling for each scale.
- **FR-004**: The extension MUST return NULL for DATETIMEOFFSET columns when the NBCROW null bitmap indicates the value is null.
- **FR-005**: The extension MUST handle DATETIMEOFFSET in NBCROW rows identically to how it handles DATETIMEOFFSET in standard ROW tokens.
- **FR-006**: The extension MUST correctly read TIME at all scales (0 through 7) in both ROW and NBCROW rows.
- **FR-007**: The extension MUST correctly read DATETIME2 at all scales (0 through 7) in both ROW and NBCROW rows.
- **FR-008**: All other data types already supported in NBCROW MUST continue to work correctly (no regression).

### Key Entities

- **DATETIMEOFFSET**: A SQL Server date/time type that stores a UTC-adjusted datetime plus a timezone offset. Wire format: time bytes (3-5 depending on scale) + date bytes (3) + offset bytes (2). SQL Server stores the time component already in UTC; the offset is metadata for display.
- **DATETIME2**: A SQL Server date/time type with configurable fractional-second precision. Wire format: time bytes (3-5 depending on scale) + date bytes (3).
- **TIME**: A SQL Server time-only type with configurable fractional-second precision. Wire format: time bytes (3-5 depending on scale).
- **Scale-to-byte-length mapping**: Scale 0-2 = 3 time bytes, scale 3-4 = 4 time bytes, scale 5-7 = 5 time bytes. This applies to TIME, DATETIME2, and DATETIMEOFFSET identically.
- **NBCROW (Null Bitmap Compressed Row)**: An alternative TDS row format (token 0xD2) where a bitmap at the start of the row indicates which columns are NULL, avoiding per-column null markers. SQL Server automatically uses this format when it estimates it will reduce wire size, typically for tables with several nullable columns.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Queries against tables with DATETIMEOFFSET columns succeed without error regardless of whether SQL Server uses ROW or NBCROW encoding.
- **SC-002**: All existing integration tests continue to pass (no regression in any data type).
- **SC-003**: DATETIMEOFFSET values are returned with correct UTC conversion for all scale values (0, 3, 7 at minimum) and timezone offsets, in both ROW and NBCROW formats.
- **SC-004**: TIME values are returned correctly at scales 0, 3, and 7 in both ROW and NBCROW formats.
- **SC-005**: DATETIME2 values are returned correctly at scales 0, 3, and 7 in both ROW and NBCROW formats.
- **SC-006**: A dedicated NBCROW integration test covers all datetime types at multiple scales alongside other data types in the same table.

## Assumptions

- SQL Server's NBCROW format for DATETIMEOFFSET uses the same wire encoding as standard ROW tokens (1-byte length prefix + data bytes), just with the null bitmap optimization. This matches the existing `SkipValueNBC()` implementation that already handles DATETIMEOFFSET.
- The standard ROW reader already handles DATETIMEOFFSET correctly (the bug is isolated to the NBCROW code path).
- DuckDB's TIMESTAMP WITH TIME ZONE has microsecond precision, so DATETIMEOFFSET(7) and DATETIME2(7) values (100-nanosecond precision) will be truncated to microseconds. This is existing behavior and not a regression.
- Code audit confirmed DATETIMEOFFSET is the **only** type missing from the NBC read path. All other supported types are already handled in both ReadValueNBC and SkipValueNBC.

## Code Audit Results

### NBC Reader type coverage

All supported types verified across the three row-reading functions:

| Type Category              | ReadValue (ROW) | ReadValueNBC | SkipValueNBC |
| -------------------------- | :-------------: | :----------: | :----------: |
| Fixed-length integers      | yes             | yes          | yes          |
| Fixed-length floats        | yes             | yes          | yes          |
| Fixed-length money         | yes             | yes          | yes          |
| Fixed-length datetime      | yes             | yes          | yes          |
| Nullable fixed (INTN etc.) | yes             | yes          | yes          |
| Variable-length strings    | yes             | yes          | yes          |
| Variable-length binary     | yes             | yes          | yes          |
| PLP (MAX types)            | yes             | yes          | yes          |
| DECIMAL / NUMERIC          | yes             | yes          | yes          |
| DATE                       | yes             | yes          | yes          |
| TIME                       | yes             | yes          | yes          |
| DATETIME2                  | yes             | yes          | yes          |
| **DATETIMEOFFSET**         | **yes**         | **MISSING**  | **yes**      |
| UNIQUEIDENTIFIER           | yes             | yes          | yes          |

### Existing scale test coverage

| Type            | Scale 0 | Scale 3 | Scale 7 | NBC path |
| --------------- | :-----: | :-----: | :-----: | :------: |
| TIME            | no      | no      | no      | no       |
| DATETIME2       | yes     | yes     | yes     | no       |
| DATETIMEOFFSET  | no      | yes     | yes     | no       |

Scale 0 is the most critical boundary: it produces the smallest wire encoding (3 time bytes) and was the source of a previous DATETIME2 bug (issue #73).

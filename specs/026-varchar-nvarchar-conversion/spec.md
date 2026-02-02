# Feature Specification: VARCHAR to NVARCHAR Conversion for Non-UTF8 Collations

**Feature Branch**: `026-varchar-nvarchar-conversion`
**Created**: 2026-02-02
**Status**: Draft
**Input**: User description: "Spec 026: VARCHAR to NVARCHAR Conversion for Non-UTF8 Collations"

## Clarifications

### Session 2026-02-02

- Q: When truncation occurs (VARCHAR >4000 converting to NVARCHAR(4000)), should the system warn users about data loss? → A: Silent truncation (documented in README only)
- Q: What is the acceptable performance threshold for CAST overhead? → A: No specific target; trust SQL Server CAST efficiency, validate via existing test suite

## Problem Statement

When querying SQL Server tables with VARCHAR/CHAR columns that use non-UTF-8 collations (e.g., `SQL_Latin1_General_CP1`, `Latin1_General_CI_AS`), the extension returns raw bytes that may contain invalid UTF-8 sequences. DuckDB validates UTF-8 during statistics updates and throws errors like:

```
Invalid Input Error: Failed to append: Invalid unicode (byte sequence mismatch) detected in segment statistics update
```

### Root Cause

1. SQL Server VARCHAR/CHAR columns use collation-dependent code pages (e.g., Windows-1252, Latin1)
2. The TDS protocol sends these as raw single-byte encoded data
3. The extension copies raw bytes without encoding conversion
4. DuckDB expects all VARCHAR data to be valid UTF-8

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Query Tables with Extended ASCII Characters (Priority: P1)

As a user querying SQL Server tables containing European language characters (accented letters, special symbols), I need the extension to correctly retrieve and display these characters without UTF-8 validation errors.

**Why this priority**: This is the core bug fix. Without it, users cannot query any tables with non-ASCII VARCHAR data, which is extremely common in production databases.

**Independent Test**: Can be fully tested by creating a table with VARCHAR columns containing characters like e, n, u, EUR and verifying the query returns correct data.

**Acceptance Scenarios**:

1. **Given** a SQL Server table with VARCHAR column containing "Cafe resume", **When** I query the table via DuckDB, **Then** I see "Cafe resume" without any UTF-8 errors
2. **Given** a SQL Server table with CHAR column containing "Naive senor", **When** I query the table, **Then** I see "Naive senor" correctly decoded
3. **Given** a SQL Server table with VARCHAR column containing NULL values, **When** I query the table, **Then** NULL values are preserved correctly

---

### User Story 2 - Mixed Column Types in Same Table (Priority: P2)

As a user querying tables with both VARCHAR and NVARCHAR columns, I need the extension to handle each column type appropriately without affecting already-Unicode NVARCHAR columns.

**Why this priority**: Many databases have mixed column types. The conversion should only apply where needed.

**Independent Test**: Create a table with INT, VARCHAR, and NVARCHAR columns, query it, and verify all column types return correct data.

**Acceptance Scenarios**:

1. **Given** a table with VARCHAR (non-UTF8 collation) and NVARCHAR columns, **When** I query all columns, **Then** VARCHAR columns are converted and NVARCHAR columns pass through unchanged
2. **Given** a table with INT, VARCHAR, and NVARCHAR columns, **When** I query specific columns, **Then** only the selected columns are processed appropriately

---

### User Story 3 - VARCHAR(MAX) Column Handling (Priority: P2)

As a user with tables containing VARCHAR(MAX) columns (large text fields), I need these to be converted correctly, handling the size difference between VARCHAR and NVARCHAR limits.

**Why this priority**: Large text columns are common for descriptions, comments, and content fields.

**Independent Test**: Create a VARCHAR(MAX) column with extended ASCII content and verify it returns correctly.

**Acceptance Scenarios**:

1. **Given** a VARCHAR(MAX) column with extended ASCII text, **When** I query the table, **Then** the text is correctly decoded
2. **Given** a VARCHAR(8000) column (max non-MAX size), **When** I query the table, **Then** conversion uses NVARCHAR(4000) and truncates if necessary

---

### User Story 4 - Length Boundary Handling (Priority: P3)

As a user with VARCHAR columns at or near the NVARCHAR length limit (4000 characters), I need the extension to handle the conversion correctly, truncating only when absolutely necessary.

**Why this priority**: Edge case handling for data integrity near size boundaries.

**Independent Test**: Create VARCHAR columns with lengths around the 4000-character boundary and verify behavior.

**Acceptance Scenarios**:

1. **Given** a VARCHAR(4000) column with 4000 single-byte characters, **When** I query the table, **Then** conversion uses NVARCHAR(4000) without truncation
2. **Given** a VARCHAR(8000) column with text exceeding 4000 characters, **When** I query the table, **Then** text is truncated to 4000 characters with trailing characters removed
3. **Given** a VARCHAR(5000) column definition, **When** the extension builds the query, **Then** it uses NVARCHAR(4000) (the maximum allowed)

---

### Edge Cases

- What happens when VARCHAR column contains only ASCII characters (0-127)? Conversion still applies transparently.
- How does the system handle empty strings? Correctly returns empty string.
- What happens with very long VARCHAR(MAX) content (>4000 chars)? Converted to NVARCHAR(MAX).
- How are binary-like bytes (non-text data stored in VARCHAR) handled? Converted based on collation code page; may produce unexpected Unicode characters.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST wrap VARCHAR/CHAR columns with non-UTF8 collations in `CAST(column AS NVARCHAR(n))` when generating table scan queries
- **FR-002**: System MUST preserve the original column length when converting, using `NVARCHAR(n)` where n equals the VARCHAR length (up to 4000)
- **FR-003**: System MUST use `NVARCHAR(MAX)` for VARCHAR(MAX) columns
- **FR-004**: System MUST NOT convert columns that already use UTF-8 collations (columns ending in `_UTF8`)
- **FR-005**: System MUST NOT convert NCHAR/NVARCHAR columns (already Unicode)
- **FR-006**: System MUST preserve NULL values through the CAST operation
- **FR-007**: System MUST silently truncate trailing characters when VARCHAR length exceeds 4000 (NVARCHAR max non-MAX length); no runtime warning or error
- **FR-008**: System MUST apply conversion only to table scans, NOT to `mssql_scan()` raw SQL queries (user-controlled)
- **FR-009**: System MUST apply conversion only to SELECT queries, NOT to INSERT/UPDATE/DELETE operations
- **FR-010**: System MUST NOT apply conversion to COPY TO operations (uses BCP protocol)

### Key Entities

- **Column Metadata**: Type ID, collation flags, max_length, name - used to determine if conversion is needed
- **Query Builder**: Component that generates SELECT statements for table scans - applies CAST wrapping
- **Collation Info**: 5-byte collation data from TDS protocol - 5th byte contains UTF-8 flag

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can query tables with extended ASCII characters without encountering UTF-8 validation errors
- **SC-002**: All existing tests continue to pass (no regression)
- **SC-003**: Mixed VARCHAR/NVARCHAR tables return correct data for both column types
- **SC-004**: VARCHAR(MAX) columns with >4000 characters are fully retrieved via NVARCHAR(MAX)
- **SC-005**: VARCHAR columns with length >4000 are truncated to 4000 characters (documented limitation)
- **SC-006**: No measurable performance regression in existing test suite; rely on SQL Server's native CAST efficiency

## Scope & Boundaries

### In Scope

- Table scan queries (`SELECT * FROM table`)
- VARCHAR and CHAR columns with non-UTF8 collations
- All standard VARCHAR lengths including VARCHAR(MAX)
- Truncation behavior for lengths >4000

### Out of Scope

- `mssql_scan()` with user-provided raw SQL (user responsibility)
- COPY TO operations (BCP protocol, separate code path)
- INSERT/UPDATE/DELETE operations (writing, not reading)
- Client-side code page conversion (rejected approach)
- Filter pushdown expressions (comparisons work regardless of encoding)

## Assumptions

- SQL Server correctly converts VARCHAR to NVARCHAR via CAST based on the column's collation code page
- The extension's existing UTF-16LE decoding for NVARCHAR is correct and complete
- Conservative approach: assume all VARCHAR columns are non-UTF8 unless proven otherwise (UTF-8 detection is optional optimization)
- Truncation at 4000 characters is acceptable for very long VARCHAR columns (documented limitation)

## Limitations (Document in README)

- **VARCHAR to NVARCHAR Truncation**: VARCHAR columns with defined length >4000 characters will be truncated to 4000 characters when read. VARCHAR(MAX) columns are unaffected and converted to NVARCHAR(MAX).
- **Raw SQL Queries**: Users of `mssql_scan(query)` with raw SQL must handle encoding conversion themselves using `CAST(column AS NVARCHAR(...))` in their queries.

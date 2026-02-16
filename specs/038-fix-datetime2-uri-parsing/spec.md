# Feature Specification: Fix datetime2(0) Truncation and URI Password Parsing

**Feature Branch**: `038-fix-datetime2-uri-parsing`
**Created**: 2026-02-16
**Status**: Draft
**Input**: User description: "issue #73 and #71. Fix please"
**References**: [Issue #73](https://github.com/hugr-lab/mssql-extension/issues/73), [Issue #71](https://github.com/hugr-lab/mssql-extension/issues/71)

## User Scenarios & Testing *(mandatory)*

### User Story 1 - datetime2 Values with Low Precision Are Returned Correctly (Priority: P1)

A user queries a SQL Server table containing `datetime2(0)` (second-precision) columns via DuckDB. The returned values must preserve the correct date and time components. Currently, the time portion is effectively lost — e.g., `2020-04-04 12:12:48` in SQL Server appears as `2020-04-04 00:00:00.004396` in DuckDB.

**Why this priority**: This is a data correctness bug. Users receive silently wrong datetime values, which can lead to incorrect analytics, broken joins, and data integrity issues. It affects all datetime2 columns with scale 0–6.

**Independent Test**: Can be fully tested by querying a SQL Server table with datetime2(0) through datetime2(6) columns and verifying the returned values match the original data.

**Acceptance Scenarios**:

1. **Given** a SQL Server table with a `datetime2(0)` column containing `2020-04-04 12:12:48`, **When** the user selects that column via DuckDB, **Then** the value returned is `2020-04-04 12:12:48`.
2. **Given** a SQL Server table with a `datetime2(3)` column containing `2024-01-15 09:30:45.123`, **When** the user selects that column via DuckDB, **Then** the value returned is `2024-01-15 09:30:45.123`.
3. **Given** a SQL Server table with a `datetime2(7)` column (maximum precision), **When** the user selects that column via DuckDB, **Then** the value is returned correctly (existing behavior, must not regress).
4. **Given** a SQL Server table with a `datetime2(0)` column containing a NULL value, **When** the user selects that column via DuckDB, **Then** the value returned is NULL.

---

### User Story 2 - ATTACH with Special Characters in Password (Priority: P1)

A user connects to SQL Server using the `mssql://` URI format where the password contains the `@` character. Currently, the URI parser splits on the first `@`, incorrectly treating part of the password as the hostname.

**Why this priority**: This is a connectivity bug that prevents users from connecting at all when their password contains `@`. The `@` character is common in generated passwords and passphrases.

**Independent Test**: Can be fully tested by attempting ATTACH with a URI containing `@` in the password and verifying the connection succeeds.

**Acceptance Scenarios**:

1. **Given** a SQL Server with user `sa` and password `MyPass@Word`, **When** the user runs `ATTACH 'mssql://sa:MyPass%40Word@127.0.0.1:1433/master' AS db (TYPE mssql)`, **Then** the connection succeeds (URL-encoded `@`).
2. **Given** a SQL Server with user `sa` and password `MyPass@Word`, **When** the user runs `ATTACH 'mssql://sa:MyPass@Word@127.0.0.1:1433/master' AS db (TYPE mssql)`, **Then** the connection succeeds (unencoded `@` — parser uses last `@` as delimiter).
3. **Given** a password containing multiple `@` symbols like `p@ss@word`, **When** the user connects via URI with the password URL-encoded or unencoded, **Then** the connection succeeds with the correct password extracted.
4. **Given** a password with other special characters (`:`, `/`, `?`, `#`), **When** the user URL-encodes them in the URI, **Then** they are correctly decoded and the connection succeeds.

---

### Edge Cases

- What happens when datetime2(0) stores midnight (`00:00:00`)? The time ticks value is 0, which must remain `00:00:00`, not be misinterpreted.
- What happens when datetime2(0) stores the maximum time value (`23:59:59`)? Must decode correctly.
- What happens with negative timestamps (dates before 1970) with datetime2(0)? Must preserve the time portion.
- What happens when the `TIME` type uses scale 0–6? The same time decoding logic is shared and must also be fixed.
- What happens when the URI has no credentials at all (no `@`)? Must still parse correctly.
- What happens when the username itself contains a colon (`:`)? The first colon separates user from password; additional colons are part of the password.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The datetime2 decoder MUST correctly convert time ticks to microseconds based on the column's scale parameter (0–7), not assume scale 7.
- **FR-002**: For all datetime2 scales (0 through 7), the decoded timestamp MUST match the original SQL Server value within the precision of the scale.
- **FR-003**: The `TIME` type decoder MUST also account for scale when converting time ticks to microseconds, as it shares the same encoding.
- **FR-004**: The URI parser MUST use the last `@` character as the credentials/host delimiter, not the first, to support passwords containing `@`.
- **FR-005**: The URI parser MUST correctly URL-decode all components (user, password, database, query parameters) after splitting.
- **FR-006**: Existing ADO.NET connection string parsing (semicolon-delimited) MUST NOT be affected, as it does not use `@` as a delimiter.
- **FR-007**: The BCP encoder for datetime2 (write path) MUST NOT be affected — it already handles scale correctly.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: All datetime2 scales (0–7) return values matching their SQL Server source, with no silent data corruption.
- **SC-002**: Users can connect via `mssql://` URI format with passwords containing `@`, `:`, and other special characters.
- **SC-003**: Existing datetime2(7) behavior does not regress — all current tests continue to pass.
- **SC-004**: Existing connection string parsing (ADO.NET format, secrets) continues to work unchanged.

## Assumptions

- The SQL Server TDS protocol stores datetime2 time values in units of 10^(-scale) seconds, as documented in the MS-TDS specification.
- URL-encoding (`%40` for `@`) is the recommended approach for special characters in URIs, but the parser should also handle the common case of unencoded `@` in passwords by using the rightmost `@` as the delimiter.
- The `TIME` type in SQL Server uses the same time encoding as datetime2 and is also affected by the scale bug.

# Feature Specification: ORDER BY Pushdown with TOP N

**Feature Branch**: `039-order-pushdown`
**Created**: 2026-02-16
**Status**: Draft
**Input**: ORDER BY pushdown with TOP N for MSSQL extension

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Simple ORDER BY Pushdown (Priority: P1)

A user queries an MSSQL-attached table with `ORDER BY` on simple columns. Instead of fetching all rows unsorted and sorting locally in DuckDB, the sort is delegated to SQL Server, which can leverage its indexes for efficient pre-sorted output.

**Why this priority**: This is the core value of the feature. Users with large tables sorted by indexed columns will see the most significant performance improvement.

**Independent Test**: Can be tested by running `SELECT * FROM mssql_db.dbo.MyTable ORDER BY col1 ASC` with pushdown enabled and verifying that the generated SQL sent to SQL Server includes an `ORDER BY` clause.

**Acceptance Scenarios**:

1. **Given** an MSSQL-attached database with pushdown enabled, **When** a user runs `SELECT * FROM db.dbo.T ORDER BY col1 ASC`, **Then** the ORDER BY is delegated to SQL Server and results are returned pre-sorted.
2. **Given** an MSSQL-attached database with pushdown enabled, **When** a user runs `SELECT * FROM db.dbo.T ORDER BY col1 ASC, col2 DESC`, **Then** multi-column ORDER BY is delegated to SQL Server.
3. **Given** an MSSQL-attached database with pushdown **disabled** (default), **When** a user runs the same query, **Then** sorting is performed locally by DuckDB as before.

---

### User Story 2 - Partial ORDER BY Pushdown (Priority: P2)

A user queries with ORDER BY on multiple columns where some can be pushed down and others cannot (e.g., complex expressions). The pushable subset is sent to SQL Server for pre-sorting, while DuckDB still performs the final sort. Pre-sorted input improves DuckDB's sort performance.

**Why this priority**: Ensures graceful degradation. Users get performance benefits even when not all ORDER BY columns can be pushed down.

**Independent Test**: Can be tested by running `ORDER BY col1 ASC, complex_expr DESC` and verifying SQL Server receives `ORDER BY col1 ASC` while DuckDB still performs the full sort on pre-sorted data.

**Acceptance Scenarios**:

1. **Given** an ORDER BY with a mix of simple columns and unsupported expressions, **When** the query executes, **Then** the pushable columns are sent to SQL Server and DuckDB performs the final sort.
2. **Given** a partial pushdown, **When** results are returned, **Then** final result order is correct (identical to non-pushdown behavior).

---

### User Story 3 - TOP N Pushdown (ORDER BY + LIMIT) (Priority: P2)

A user queries with both ORDER BY and LIMIT. When all ORDER BY columns are fully pushable, both operations are combined into a `SELECT TOP N ... ORDER BY ...` query to SQL Server, minimizing data transfer and server-side work.

**Why this priority**: Common pattern for "get the top/bottom N rows" queries. Combines sort and row limiting for maximum efficiency.

**Independent Test**: Can be tested by running `SELECT * FROM db.dbo.T ORDER BY col1 LIMIT 10` with pushdown enabled and verifying SQL Server receives `SELECT TOP 10 ... ORDER BY [col1] ASC`.

**Acceptance Scenarios**:

1. **Given** a fully pushable ORDER BY with LIMIT, **When** the query executes, **Then** SQL Server receives a `SELECT TOP N ... ORDER BY ...` query and only N rows are transferred.
2. **Given** a partially pushable ORDER BY with LIMIT, **When** the query executes, **Then** the pushable ORDER BY subset is sent to SQL Server but LIMIT is handled by DuckDB.
3. **Given** ORDER BY + LIMIT with pushdown disabled, **When** the query executes, **Then** both operations are handled locally by DuckDB.

---

### User Story 4 - ORDER BY with Function Expressions (Priority: P3)

A user queries with ORDER BY on simple function expressions like `YEAR(date_col)` or `MONTH(date_col)`. These are translated to their T-SQL equivalents and pushed to SQL Server.

**Why this priority**: Extends pushdown beyond plain column references. Covers common date-based sorting patterns.

**Independent Test**: Can be tested by running `SELECT * FROM db.dbo.T ORDER BY YEAR(date_col)` and verifying SQL Server receives `ORDER BY YEAR([date_col])`.

**Acceptance Scenarios**:

1. **Given** an ORDER BY with a supported function (e.g., `YEAR`, `MONTH`), **When** the query executes, **Then** the function is translated to T-SQL and included in the pushed ORDER BY clause.
2. **Given** an ORDER BY with an unsupported or complex function, **When** the query executes, **Then** that column is excluded from pushdown (treated as partial pushdown).

---

### User Story 5 - Configuration Control (Priority: P1)

A user controls whether ORDER BY pushdown is active via a global setting and/or a per-database ATTACH option. The feature is disabled by default to ensure no behavior change for existing users.

**Why this priority**: Users must be able to opt in explicitly. Disabled by default ensures backward compatibility and safety.

**Independent Test**: Can be tested by toggling the setting and ATTACH option and verifying pushdown is active or inactive accordingly.

**Acceptance Scenarios**:

1. **Given** default configuration (no setting change), **When** a user runs an ORDER BY query, **Then** no pushdown occurs (DuckDB sorts locally).
2. **Given** the global setting is enabled, **When** a user runs an ORDER BY query on any attached MSSQL database, **Then** pushdown is attempted.
3. **Given** a per-database ATTACH option set to disabled while the global setting is enabled, **When** a user runs an ORDER BY query on that database, **Then** pushdown is disabled for that database.
4. **Given** a per-database ATTACH option set to enabled while the global setting is disabled, **When** a user runs an ORDER BY query on that database, **Then** pushdown is enabled for that database.

---

### Edge Cases

- What happens when ORDER BY references a column not in the scan output? Pushdown is skipped for that column.
- What happens when NULL ordering doesn't match SQL Server defaults (e.g., ASC with NULLS LAST on a nullable column)? That column is excluded from pushdown.
- What happens when ORDER BY references a NOT NULL column with default null ordering? Pushdown proceeds since NULL ordering differences are irrelevant.
- What happens when the query involves a non-MSSQL table? The optimizer ignores it completely.
- What happens when ORDER BY includes a mix of pushable and non-pushable columns? Partial pushdown: pushable subset sent to SQL Server, DuckDB performs final sort.
- What happens when LIMIT is present but ORDER BY is only partially pushable? Only ORDER BY subset is pushed; LIMIT stays in DuckDB.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST provide a global setting to enable/disable ORDER BY pushdown, defaulting to disabled.
- **FR-002**: The system MUST support a per-database ATTACH option that overrides the global setting for that specific database.
- **FR-003**: The system MUST push ORDER BY clauses with simple column references to SQL Server when pushdown is enabled.
- **FR-004**: The system MUST push ORDER BY clauses with supported simple function expressions (e.g., YEAR, MONTH) to SQL Server.
- **FR-005**: The system MUST validate NULL ordering compatibility with SQL Server before pushing down each ORDER BY column (SQL Server: NULLs first in ASC, NULLs last in DESC).
- **FR-006**: The system MUST skip pushdown for ORDER BY columns with NULL ordering that doesn't match SQL Server behavior.
- **FR-007**: The system MUST check column nullability metadata -- NOT NULL columns can always be pushed regardless of NULL ordering.
- **FR-008**: The system MUST support full pushdown (all columns pushed, local sort removed) and partial pushdown (subset pushed for pre-sort benefit, local sort kept).
- **FR-009**: The system MUST combine ORDER BY and LIMIT into a TOP N query when all ORDER BY columns are fully pushable.
- **FR-010**: The system MUST NOT push down complex expressions, nested functions, or casts.
- **FR-011**: The system MUST only detect simple plan patterns (ORDER BY directly above scan) -- no recursive plan tree walking.
- **FR-012**: The system MUST NOT affect queries on non-MSSQL tables.
- **FR-013**: The system MUST produce identical result ordering whether pushdown is enabled or disabled.

### Key Entities

- **Order Column Info**: Represents a single ORDER BY column that can be pushed down -- includes column name or function expression, sort direction, and NULL ordering.
- **Pushdown Configuration**: The effective pushdown state for a given database -- derived from the global setting and per-database ATTACH option.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Queries with ORDER BY on indexed columns complete faster with pushdown enabled versus disabled on tables with 100K+ rows.
- **SC-002**: TOP N queries (ORDER BY + LIMIT) transfer only N rows from SQL Server when fully pushed down.
- **SC-003**: Result ordering is identical with pushdown enabled and disabled for all supported query patterns.
- **SC-004**: Queries on non-MSSQL tables are unaffected (no performance regression or behavior change).
- **SC-005**: Feature is disabled by default -- existing users experience no behavior change without explicit opt-in.

## Assumptions

- SQL Server indexes exist on commonly sorted columns (pushdown benefits depend on server-side index availability).
- The existing function mapping infrastructure covers the small set of simple functions needed for ORDER BY pushdown.
- The existing metadata cache provides column nullability information needed for NULL order validation.
- Standalone LIMIT (without ORDER BY) is not in scope -- the existing ATTENTION packet mechanism already handles early query cancellation when DuckDB stops pulling rows.

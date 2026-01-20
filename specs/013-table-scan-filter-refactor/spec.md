# Feature Specification: Table Scan and Filter Pushdown Refactoring

**Feature Branch**: `013-table-scan-filter-refactor`
**Created**: 2026-01-20
**Status**: Draft
**Input**: User description: "Refactoring mssql_table_scan and filter pushdown with code reorganization, enhanced filter support (LIKE patterns, math operations), and improved filtering strategy"

## Clarifications

### Session 2026-01-20

- Q: What logging strategy should be used for filter pushdown debugging? → A: Use existing MSSQL_DEBUG env var; log filter expression, generated T-SQL, and re-filter flag

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Query with LIKE Pattern Filtering (Priority: P1)

A DuckDB user queries an MSSQL table with LIKE pattern conditions. The system pushes down the LIKE-related filter expressions (prefix, suffix, contains) to SQL Server, reducing data transfer.

**Why this priority**: LIKE patterns are extremely common in real-world queries. Pushing them down significantly reduces network traffic and improves query performance for text searches.

**Independent Test**: Can be tested by executing a query with `WHERE column LIKE '%pattern%'` and verifying the generated T-SQL includes the appropriate LIKE clause.

**Acceptance Scenarios**:

1. **Given** a table with a VARCHAR column, **When** user executes `SELECT * FROM mssql.schema.table WHERE name LIKE 'John%'`, **Then** the generated T-SQL includes `WHERE [name] LIKE N'John%'` and only matching rows are transferred.

2. **Given** a table with text data, **When** user executes a query with `WHERE column LIKE '%test%'`, **Then** the contains pattern is pushed down as `WHERE [column] LIKE N'%test%'`.

3. **Given** a LIKE pattern with special SQL Server characters (%, _, [), **When** user executes the query, **Then** special characters in the pattern are properly escaped.

4. **Given** a case-insensitive search, **When** user executes `SELECT * FROM mssql.schema.table WHERE name ILIKE 'john%'`, **Then** the generated T-SQL includes `WHERE LOWER([name]) LIKE LOWER(N'john%')` applying LOWER() to both sides.

---

### User Story 2 - Query with Function Expressions and CASE (Priority: P1)

A DuckDB user queries an MSSQL table with filter conditions involving SQL-compatible functions (like LOWER, LENGTH) and CASE expressions. The system pushes down these expressions to SQL Server.

**Why this priority**: Function expressions are common in real-world queries for data transformation and conditional logic. Supporting them significantly expands filter pushdown coverage.

**Independent Test**: Can be tested by executing `SELECT * FROM table WHERE LOWER(name) = 'test'` and verifying the LOWER function appears in the generated T-SQL.

**Acceptance Scenarios**:

1. **Given** a table with text columns, **When** user executes `SELECT * FROM mssql.schema.table WHERE LOWER(name) = 'john'`, **Then** the generated T-SQL includes `WHERE LOWER([name]) = N'john'`.

2. **Given** a filter with reversed comparison, **When** user executes `SELECT * FROM table WHERE 12 = quantity`, **Then** the generated T-SQL includes `WHERE 12 = [quantity]` (constant on left side preserved).

3. **Given** a filter with nested functions, **When** user executes `SELECT * FROM table WHERE LOWER(TRIM(name)) = 'test'`, **Then** the nested function call is pushed down as `WHERE LOWER(LTRIM(RTRIM([name]))) = N'test'`.

4. **Given** a filter with CASE expression, **When** user executes `SELECT * FROM table WHERE CASE WHEN status = 1 THEN 'active' ELSE 'inactive' END = 'active'`, **Then** the CASE expression is pushed down to SQL Server.

5. **Given** a complex function comparison, **When** user executes `SELECT * FROM table WHERE '334' = LOWER(code)`, **Then** the generated T-SQL includes `WHERE N'334' = LOWER([code])` with constant on left side.

---

### User Story 3 - Query with Date/Time Functions in Filters (Priority: P2)

A DuckDB user queries an MSSQL table with filter conditions involving date/time functions. The system pushes down these expressions to SQL Server.

**Why this priority**: Date/time filtering is common in real-world queries for time-based data analysis. Supporting these functions extends filter pushdown to temporal queries.

**Independent Test**: Can be tested by executing `SELECT * FROM table WHERE created_date > CURRENT_DATE - INTERVAL 7 DAY` and verifying the date arithmetic appears in the generated T-SQL.

**Acceptance Scenarios**:

1. **Given** a table with a DATE column, **When** user executes `SELECT * FROM mssql.schema.events WHERE event_date > DATE '2024-01-01'`, **Then** the generated T-SQL includes the date comparison.

2. **Given** a table with DATETIME column, **When** user executes `SELECT * FROM table WHERE YEAR(created_at) = 2024`, **Then** the YEAR function is pushed down as `WHERE YEAR([created_at]) = 2024`.

3. **Given** a filter with date arithmetic, **When** user executes `SELECT * FROM table WHERE order_date >= CURRENT_DATE - INTERVAL 30 DAY`, **Then** the date arithmetic is pushed down using DATEADD.

4. **Given** a filter with DATEDIFF, **When** user executes `SELECT * FROM table WHERE date_diff('day', start_date, end_date) > 7`, **Then** the generated T-SQL includes `WHERE DATEDIFF(day, [start_date], [end_date]) > 7`.

---

### User Story 4 - Query with Arithmetic Expressions in Filters (Priority: P2)

A DuckDB user queries an MSSQL table with filter conditions involving simple arithmetic operations (addition, subtraction, multiplication, division). The system pushes down these expressions to SQL Server.

**Why this priority**: Arithmetic expressions in WHERE clauses are common for range queries and computed comparisons. Supporting them extends filter pushdown coverage significantly.

**Independent Test**: Can be tested by executing `SELECT * FROM table WHERE price * quantity > 1000` and verifying the arithmetic expression appears in the generated T-SQL.

**Acceptance Scenarios**:

1. **Given** a table with numeric columns, **When** user executes `SELECT * FROM mssql.schema.orders WHERE price * quantity > 100`, **Then** the generated T-SQL includes `WHERE ([price] * [quantity]) > 100`.

2. **Given** a filter with column arithmetic, **When** user executes `SELECT * FROM table WHERE (a + b) = 10`, **Then** the addition expression is pushed down correctly.

3. **Given** a filter combining arithmetic with comparison, **When** user executes `SELECT * FROM table WHERE amount - discount >= 50`, **Then** the subtraction expression is included in the T-SQL WHERE clause.

---

### User Story 5 - Complex OR Conditions with Unsupported Expressions (Priority: P2)

A DuckDB user queries an MSSQL table with complex boolean expressions containing OR conditions where some branches contain unsupported filter expressions. The system correctly handles partial pushdown.

**Why this priority**: Correct handling of OR semantics is critical for query correctness. Incorrectly pushing down partial OR conditions would return wrong results.

**Independent Test**: Can be tested by executing queries with mixed supported/unsupported OR branches and verifying correct filtering behavior.

**Acceptance Scenarios**:

1. **Given** a filter `(a = 1 AND b = 34) OR unsupported_func(a) = 55`, **When** user executes the query, **Then** no filter is pushed down (entire OR is unsupported) and DuckDB applies all filtering.

2. **Given** a filter `a = 1 AND (b = 34 OR unsupported_func(a) = 55) AND c = 35`, **When** user executes the query, **Then** only `a = 1 AND c = 35` is pushed down to SQL Server, and DuckDB re-applies all original filters.

3. **Given** a fully supported OR condition `(a = 1 OR b = 2)`, **When** user executes the query, **Then** the entire OR expression is pushed down to SQL Server.

---

### User Story 6 - Code Organization and Maintainability (Priority: P3)

A developer working on the mssql-extension needs to understand and modify the table scan functionality. The code is organized in dedicated files with clear separation of concerns.

**Why this priority**: Better code organization improves maintainability and makes future enhancements easier. This is foundational but doesn't directly affect end-user functionality.

**Independent Test**: Can be verified by inspecting the file structure and ensuring table scan code is in `src/table_scan/` with clear module boundaries.

**Acceptance Scenarios**:

1. **Given** the codebase, **When** a developer looks for table scan implementation, **Then** they find it in `src/table_scan/` directory with separate files for bind, init, execute, and filter encoding.

2. **Given** the codebase, **When** a developer checks `src/pushdown/` directory, **Then** it no longer exists (removed as unused).

3. **Given** the table scan module, **When** a developer needs to add a new filter type, **Then** they can do so by modifying only the filter encoder without touching other table scan logic.

---

### User Story 7 - Consistent Filter Application Strategy (Priority: P1)

A DuckDB user executes queries against MSSQL tables and receives correct results regardless of which filters are pushed down. The system applies a safe filtering strategy that never returns incorrect data.

**Why this priority**: Data correctness is paramount. The filtering strategy must guarantee correct results even when partial pushdown occurs.

**Independent Test**: Can be tested by comparing query results with and without filter pushdown enabled, verifying identical results.

**Acceptance Scenarios**:

1. **Given** any query with filters, **When** some filters cannot be pushed down, **Then** all original filters are re-applied by DuckDB to ensure correctness.

2. **Given** a query where all filters are pushed down, **When** the query executes, **Then** DuckDB still validates that no additional filtering is needed (or skips re-filtering as optimization).

3. **Given** the filter encoder, **When** it processes filters, **Then** it returns both the T-SQL WHERE clause string and a boolean indicating whether DuckDB must re-apply filters.

---

### Edge Cases

- What happens when a LIKE pattern contains only wildcard characters (`%` or `_`)?
- How does the system handle NULL values in arithmetic expressions?
- What happens when division by zero could occur in a pushed-down expression?
- How are very long LIKE patterns (exceeding SQL Server limits) handled?
- What happens with nested arithmetic expressions like `((a + b) * c) / d`?
- How does the system behave when column names conflict with T-SQL reserved words?
- What happens with deeply nested function calls (e.g., `LOWER(UPPER(TRIM(LOWER(col))))`)?
- How are CASE expressions with NULL in WHEN/THEN/ELSE branches handled?
- What happens when a DuckDB function has no direct SQL Server equivalent?
- How does ILIKE handle non-ASCII characters and collation differences?

## Requirements *(mandatory)*

### Functional Requirements

#### Code Organization

- **FR-001**: System MUST move all table scan related code from `mssql_functions.cpp` to a new `src/table_scan/` directory
- **FR-002**: System MUST organize table scan code into separate files: bind data structures, global/local state, scan function implementation, and filter encoding
- **FR-003**: System MUST remove the `src/pushdown/` directory and its placeholder files as they are unused
- **FR-004**: System MUST maintain backward compatibility with existing DuckDB catalog integration (`mssql_table_entry.cpp`)

#### Filter Encoder

- **FR-005**: System MUST implement a filter encoder component that accepts a DuckDB expression tree and produces a T-SQL boolean expression string
- **FR-006**: Filter encoder MUST return a result containing: (a) the T-SQL WHERE clause fragment, and (b) a boolean flag indicating whether DuckDB must re-apply all filters
- **FR-007**: Filter encoder MUST handle the existing supported filter types: CONSTANT_COMPARISON, IS_NULL, IS_NOT_NULL, IN_FILTER, CONJUNCTION_AND, CONJUNCTION_OR

#### LIKE Pattern Support

- **FR-008**: System MUST support pushdown of `prefix` function expressions (translating to `LIKE 'pattern%'`)
- **FR-009**: System MUST support pushdown of `suffix` function expressions (translating to `LIKE '%pattern'`)
- **FR-010**: System MUST support pushdown of `contains` function expressions (translating to `LIKE '%pattern%'`)
- **FR-011**: System MUST properly escape SQL Server LIKE special characters (%, _, [) in pattern literals
- **FR-012**: System MUST support ILIKE (case-insensitive LIKE) by applying LOWER() to both the column and the pattern value in T-SQL

#### Function Expression Support

- **FR-013**: System MUST support pushdown of LOWER() function on columns and expressions
- **FR-014**: System MUST support pushdown of UPPER() function on columns and expressions
- **FR-015**: System MUST support pushdown of LENGTH()/LEN() function (mapping DuckDB LENGTH to SQL Server LEN)
- **FR-016**: System MUST support pushdown of TRIM() function (mapping to LTRIM(RTRIM()) in SQL Server)
- **FR-017**: System MUST support nested function expressions (e.g., `LOWER(TRIM(column))`)
- **FR-018**: System MUST map DuckDB function names to their SQL Server equivalents where they differ
- **FR-019**: When a DuckDB function has no SQL Server equivalent, that expression MUST NOT be pushed down

#### CASE Expression Support

- **FR-020**: System MUST support pushdown of simple CASE expressions (`CASE column WHEN value THEN result ... END`)
- **FR-021**: System MUST support pushdown of searched CASE expressions (`CASE WHEN condition THEN result ... END`)
- **FR-022**: System MUST support CASE expressions with ELSE clause
- **FR-023**: System MUST support nested expressions within CASE WHEN conditions, THEN results, and ELSE results

#### Date/Time Function Support

- **FR-024**: System MUST support pushdown of YEAR() function (mapping to SQL Server YEAR)
- **FR-025**: System MUST support pushdown of MONTH() function (mapping to SQL Server MONTH)
- **FR-026**: System MUST support pushdown of DAY() function (mapping to SQL Server DAY)
- **FR-027**: System MUST support pushdown of date_diff() function (mapping to SQL Server DATEDIFF with appropriate interval)
- **FR-028**: System MUST support pushdown of date_add() function (mapping to SQL Server DATEADD)
- **FR-029**: System MUST support pushdown of date_part() function (mapping to SQL Server DATEPART)
- **FR-030**: System MUST support pushdown of current_date (mapping to SQL Server CAST(GETDATE() AS DATE))
- **FR-031**: System MUST support pushdown of current_timestamp (mapping to SQL Server GETDATE() or SYSDATETIME())

#### Comparison Expression Support

- **FR-032**: System MUST support reversed comparison expressions where constant is on the left side (e.g., `12 = column`)
- **FR-033**: System MUST support comparisons where both sides contain expressions (e.g., `LOWER(a) = LOWER(b)`)
- **FR-034**: System MUST preserve the original comparison order in the generated T-SQL (not normalize to column-on-left)

#### Arithmetic Expression Support

- **FR-035**: System MUST support pushdown of addition (+) operations between columns and/or constants
- **FR-036**: System MUST support pushdown of subtraction (-) operations between columns and/or constants
- **FR-037**: System MUST support pushdown of multiplication (*) operations between columns and/or constants
- **FR-038**: System MUST support pushdown of division (/) operations between columns and/or constants
- **FR-039**: System MUST properly parenthesize arithmetic expressions to preserve operator precedence

#### OR Condition Handling

- **FR-040**: When an OR condition contains any unsupported child expression, the entire OR branch MUST NOT be pushed down
- **FR-041**: When an AND condition contains unsupported children, only the supported children MUST be pushed down (partial pushdown allowed)
- **FR-042**: System MUST set the "re-apply filters" flag to true whenever any filter expression is not fully pushed down

#### Filtering Strategy

- **FR-043**: System MUST apply the following filtering strategy: push down all supported filters to SQL Server, then re-apply all original filters in DuckDB if any filter was not fully pushed down
- **FR-044**: When all filters are fully pushed down, system MAY skip re-applying filters in DuckDB as an optimization
- **FR-045**: System MUST never return incorrect results due to partial filter pushdown

#### Observability

- **FR-046**: When MSSQL_DEBUG environment variable is set, system MUST log filter pushdown decisions including: (a) original DuckDB filter expression, (b) generated T-SQL WHERE clause, (c) whether DuckDB re-filtering is required
- **FR-047**: System MUST use the existing MSSQL_DEBUG logging mechanism (no new debug flags)

### Key Entities

- **FilterEncoder**: Component that traverses DuckDB expression trees and produces T-SQL WHERE clause fragments. Tracks whether full pushdown was achieved.
- **FilterEncoderResult**: Result structure containing the T-SQL string and the "needs DuckDB filtering" boolean flag.
- **TableScanBindData**: Bind-time data structure containing table metadata, column information, and schema details.
- **TableScanGlobalState**: Execution-time state containing the result stream, timing information, and filter pushdown status.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Queries with LIKE patterns on text columns show the pattern in the generated T-SQL WHERE clause (verifiable via debug logging)
- **SC-002**: Queries with simple arithmetic expressions (+, -, *, /) in filters include those expressions in the generated T-SQL
- **SC-003**: Queries mixing supported and unsupported filter expressions return identical results to queries executed entirely in DuckDB
- **SC-004**: Table scan code is located in `src/table_scan/` directory with at least 4 separate source files
- **SC-005**: The `src/pushdown/` directory no longer exists in the codebase
- **SC-006**: All existing integration tests continue to pass after refactoring
- **SC-007**: Filter encoder produces valid T-SQL syntax for all supported expression types (no SQL syntax errors from SQL Server)
- **SC-008**: Queries with ILIKE patterns generate T-SQL with LOWER() applied to both column and value
- **SC-009**: Queries with SQL-compatible functions (LOWER, UPPER, LEN, TRIM) show the function calls in the generated T-SQL
- **SC-010**: Queries with CASE expressions are pushed down to SQL Server with correct CASE syntax
- **SC-011**: Queries with reversed comparisons (constant on left) preserve the comparison order in generated T-SQL
- **SC-012**: Queries with nested function expressions (e.g., `LOWER(TRIM(col))`) are correctly pushed down
- **SC-013**: Queries with date/time functions (YEAR, MONTH, DAY, DATEDIFF, DATEADD) show the functions in generated T-SQL

## Assumptions

- DuckDB's optimizer decomposes LIKE expressions into function calls (prefix, suffix, contains) before passing them to the table function's filter pushdown
- The existing `ValueToSQLLiteral()` function correctly handles all data types needed for filter values
- SQL Server's LIKE operator uses the same wildcard semantics as expected (% for any characters, _ for single character)
- Integer division in T-SQL matches DuckDB's semantics for the use cases targeted
- The extension targets SQL Server 2016 or later, which supports all required T-SQL syntax
- DuckDB function names can be mapped to SQL Server equivalents: LENGTH→LEN, TRIM→LTRIM(RTRIM()), LOWER→LOWER, UPPER→UPPER
- SQL Server's LOWER() function behaves equivalently to DuckDB's LOWER() for ASCII characters (collation may affect non-ASCII)
- CASE expressions from DuckDB can be directly translated to T-SQL CASE syntax without semantic differences

## Out of Scope

- Pushdown of aggregate functions (SUM, COUNT, AVG, etc.)
- Pushdown of ORDER BY clauses
- Pushdown of LIMIT/OFFSET
- Pushdown of window functions
- Pushdown of complex string functions beyond basic ones (SUBSTRING, REPLACE, CONCAT, etc.)
- Parameterized queries (sp_executesql) - this remains future work
- Statistics feedback to DuckDB for cardinality estimation
- Timezone conversion functions (AT TIME ZONE, etc.)

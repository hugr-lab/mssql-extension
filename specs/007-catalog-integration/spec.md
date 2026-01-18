# Feature Specification: Catalog Integration & Read-Only SELECT with Pushdown

**Feature Branch**: `007-catalog-integration`
**Created**: 2026-01-18
**Status**: Draft
**Input**: Expose SQL Server schemas, tables, and views through DuckDB catalog. Enable read-only queries via `catalog.schema.table` with projection and filter pushdown. Implement prepared/parameterized execution via `sp_executesql` for safe predicate handling. Support metadata caching with TTL and explicit refresh.

## Clarifications

### Session 2026-01-18

- Q: Should the extension use `sp_prepare`/`sp_execute` or `sp_executesql` for parameterized queries? → A: `sp_executesql` is recommended as the primary approach. It provides parameterized execution without requiring handle management, and SQL Server internally caches execution plans for identical statement text + parameter signatures.
- Q: What should be the default metadata cache TTL? → A: TTL should be an extension parameter with default 0 (disabled). When 0, no automatic refresh occurs; user must manually refresh via `mssql_refresh_catalog()` function.
- Q: What escape character should be used for LIKE pushdown? → A: Backslash (`\`) - matches DuckDB default behavior.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Attach SQL Server and Browse Schemas (Priority: P1)

As a DuckDB user, I want to attach a SQL Server database and browse its schemas so that I can discover available data structures.

**Why this priority**: This is the foundational capability for catalog integration. Users must be able to discover schemas before they can query tables.

**Independent Test**: Can be fully tested by attaching a database and executing `SHOW SCHEMAS FROM catalog` to verify schemas are listed.

**Acceptance Scenarios**:

1. **Given** valid SQL Server credentials in a secret, **When** I execute `ATTACH '' AS sales (TYPE mssql, SECRET my_mssql)`, **Then** the database attaches successfully and appears as a catalog.
2. **Given** an attached catalog with multiple schemas (dbo, hr, finance), **When** I execute `SHOW SCHEMAS FROM sales`, **Then** all accessible schemas are listed.
3. **Given** a schema with restricted permissions, **When** I list schemas, **Then** only schemas accessible to the current user are shown.

---

### User Story 2 - List Tables and Views (Priority: P1)

As a DuckDB user, I want to list tables and views in a schema so that I can find the data I need to query.

**Why this priority**: Table/view discovery is essential for users to understand what data is available before writing queries.

**Independent Test**: Can be fully tested by executing `SHOW TABLES FROM sales.dbo` and verifying tables and views are listed with correct types.

**Acceptance Scenarios**:

1. **Given** a schema with tables and views, **When** I execute `SHOW TABLES FROM sales.dbo`, **Then** both tables and views are listed with their object types.
2. **Given** a view in the schema, **When** I list tables, **Then** the view is included and marked as type `VIEW`.
3. **Given** temporary tables in the schema, **When** I list tables, **Then** only permanent tables and views are shown (temporary tables excluded).

---

### User Story 3 - Describe Table Columns with Collation (Priority: P1)

As a DuckDB user, I want to describe a table's columns including their collation information so that I understand the data types and text handling.

**Why this priority**: Column metadata including collation is required for correct filter pushdown and parameter binding decisions.

**Independent Test**: Can be fully tested by executing `DESCRIBE sales.dbo.customers` and verifying columns show correct types, nullability, and collation.

**Acceptance Scenarios**:

1. **Given** a table with various column types, **When** I execute `DESCRIBE sales.dbo.customers`, **Then** columns are listed with correct DuckDB type mappings and nullability.
2. **Given** a varchar column with explicit collation, **When** I describe the table, **Then** the collation_name is available in metadata (may require extended describe).
3. **Given** a column with NULL collation, **When** I describe the table, **Then** the database default collation is available as fallback.

---

### User Story 4 - Query Table via Catalog Namespace (Priority: P1)

As a DuckDB user, I want to query tables using `catalog.schema.table` syntax so that I can access SQL Server data naturally.

**Why this priority**: This is the core query capability. Users expect to query remote tables using standard SQL syntax.

**Independent Test**: Can be fully tested by executing `SELECT * FROM sales.dbo.orders LIMIT 10` and verifying results are returned.

**Acceptance Scenarios**:

1. **Given** an attached catalog with a table, **When** I execute `SELECT * FROM sales.dbo.orders`, **Then** results are streamed from SQL Server.
2. **Given** a query selecting specific columns, **When** I execute `SELECT order_id, total FROM sales.dbo.orders`, **Then** only requested columns are fetched (projection pushdown).
3. **Given** a view in the catalog, **When** I query it via `SELECT * FROM sales.dbo.order_summary`, **Then** the view is queried successfully.

---

### User Story 5 - Filter Pushdown with Parameters (Priority: P1)

As a DuckDB user, I want my WHERE clauses pushed down to SQL Server with bound parameters so that queries are efficient and secure.

**Why this priority**: Filter pushdown dramatically improves query performance by reducing data transfer. Parameters ensure correctness and prevent SQL injection.

**Independent Test**: Can be fully tested by executing a filtered query and verifying via SQL Server trace/profiler that filters are applied remotely with parameters.

**Acceptance Scenarios**:

1. **Given** a query with equality filter, **When** I execute `SELECT * FROM sales.dbo.orders WHERE status = 'shipped'`, **Then** the filter is pushed down using `sp_executesql` with a bound NVARCHAR parameter.
2. **Given** a query with multiple filters, **When** I execute `SELECT * FROM sales.dbo.orders WHERE status = 'shipped' AND amount > 100`, **Then** both predicates are pushed down with AND logic.
3. **Given** a query with LIKE pattern, **When** I execute `SELECT * FROM sales.dbo.products WHERE name LIKE 'Widget%'`, **Then** LIKE is pushed down with proper escaping.
4. **Given** a query with IN list, **When** I execute `SELECT * FROM sales.dbo.orders WHERE status IN ('new', 'pending', 'shipped')`, **Then** the IN predicate is pushed down with multiple parameters.

---

### User Story 6 - VARCHAR Column Collation Handling (Priority: P1)

As a DuckDB user, I want queries against varchar columns to handle collation correctly so that comparisons work as expected without breaking indexes.

**Why this priority**: Incorrect collation handling can cause wrong results or prevent index usage. This is critical for correctness and performance.

**Independent Test**: Can be fully tested by querying varchar columns with different collations and verifying correct results and sargable query form.

**Acceptance Scenarios**:

1. **Given** a varchar column with SQL_Latin1_General_CP1_CI_AS collation, **When** I filter with `WHERE name = 'test'`, **Then** the query uses `WHERE [name] = CONVERT(varchar(max), @p1) COLLATE SQL_Latin1_General_CP1_CI_AS`.
2. **Given** a varchar column, **When** the query is executed, **Then** the column expression is NOT wrapped in COLLATE or functions (sargable form preserved).
3. **Given** an ILIKE filter against a case-insensitive column, **When** the filter is pushed down, **Then** it is converted to LIKE (CI collation makes ILIKE equivalent to LIKE).
4. **Given** an ILIKE filter against a case-sensitive column, **When** evaluating pushdown, **Then** the filter remains local (not pushed) to avoid index-hostile rewrites.

---

### User Story 7 - Metadata Caching and Refresh (Priority: P2)

As a DuckDB user, I want schema metadata to be cached for performance but refreshable when needed so that I see current schema state.

**Why this priority**: Caching improves performance for repeated metadata access. Refresh capability ensures users can get fresh metadata when schemas change.

**Independent Test**: Can be fully tested by querying metadata, modifying schema externally, and verifying refresh updates the cached metadata.

**Acceptance Scenarios**:

1. **Given** an attached catalog, **When** I execute `SHOW TABLES` twice in quick succession, **Then** the second call uses cached metadata (no remote query).
2. **Given** a schema change on SQL Server (new table added), **When** I execute `CALL mssql_refresh_catalog('sales')`, **Then** subsequent `SHOW TABLES` includes the new table.
3. **Given** default TTL setting (0), **When** schema changes on SQL Server, **Then** cache is NOT automatically refreshed; user must call `mssql_refresh_catalog()` explicitly.
4. **Given** TTL set to non-zero value via `SET mssql_catalog_cache_ttl = 300`, **When** cached metadata is older than TTL, **Then** cache is automatically refreshed on next metadata query.

---

### User Story 8 - Unsupported Filter Fallback (Priority: P2)

As a DuckDB user, I want unsupported filters to work correctly via local filtering so that queries never fail due to pushdown limitations.

**Why this priority**: Graceful fallback ensures all valid SQL queries work, even if some filters cannot be pushed down.

**Independent Test**: Can be fully tested by executing queries with complex/unsupported filters and verifying correct results via local filtering.

**Acceptance Scenarios**:

1. **Given** a query with a function not supported for pushdown, **When** I execute the query, **Then** results are correct (function evaluated locally).
2. **Given** a query mixing supported and unsupported filters, **When** executed, **Then** supported filters are pushed, unsupported remain local, results are correct.
3. **Given** a filter that would produce incorrect semantics if pushed (collation conflict), **When** evaluating pushdown, **Then** the filter remains local with no error.

---

### User Story 9 - Write Operations Blocked (Priority: P1)

As a DuckDB user, I want clear errors when attempting write operations so that I understand this spec is read-only.

**Why this priority**: Users need immediate feedback that writes are not supported to avoid confusion about data modification capabilities.

**Independent Test**: Can be fully tested by attempting INSERT/UPDATE/DELETE and verifying clear error messages.

**Acceptance Scenarios**:

1. **Given** an attached catalog, **When** I execute `INSERT INTO sales.dbo.orders VALUES (...)`, **Then** the operation fails with "Write operations not supported" error.
2. **Given** an attached catalog, **When** I execute `UPDATE sales.dbo.orders SET status = 'x'`, **Then** the operation fails with clear error.
3. **Given** an attached catalog, **When** I execute `DELETE FROM sales.dbo.orders`, **Then** the operation fails with clear error.

---

### Edge Cases

- What happens when SQL Server schema changes between cache refresh? → Queries may fail with "object not found"; user should refresh catalog.
- How does the system handle very long identifier names? → SQL Server limit is 128 characters; properly quoted in generated SQL.
- What happens when a pushed filter returns different results due to collation? → This should not happen if collation handling is correct; if detected, fall back to local.
- How does the system handle queries against tables with computed columns? → Computed columns are treated as regular columns for SELECT; their definitions are not exposed.
- What happens when IN list exceeds configured limit? → Predicate remains local; query still works but filters locally.
- How does the system handle nullable columns in filter pushdown? → IS NULL / IS NOT NULL are pushed; nullable semantics preserved in comparisons.

## Requirements *(mandatory)*

### Functional Requirements

#### Catalog Object Model

- **FR-001**: System MUST expose attached MSSQL database as a DuckDB catalog namespace.
- **FR-002**: System MUST support `catalog.schema.table` and `catalog.schema.view` addressing.
- **FR-003**: System MUST implement correct SQL Server identifier quoting using `[identifier]` with `]` escaped as `]]`.
- **FR-004**: System MUST support schemas, tables, and views as catalog objects.

#### Metadata Discovery

- **FR-005**: System MUST query SQL Server `sys.*` catalogs for metadata (sys.schemas, sys.tables, sys.views, sys.columns, sys.types).
- **FR-006**: System MUST list schemas accessible to the current user via `SHOW SCHEMAS FROM catalog`.
- **FR-007**: System MUST list tables and views via `SHOW TABLES FROM catalog.schema`.
- **FR-008**: System MUST expose column metadata via `DESCRIBE catalog.schema.table` including name, type, nullability.
- **FR-009**: System MUST capture and store `collation_name` for each column from `sys.columns`.
- **FR-010**: System MUST capture database default collation via `DATABASEPROPERTYEX(DB_NAME(), 'Collation')` as fallback.

#### Read-Only SELECT

- **FR-011**: System MUST support `SELECT` queries via catalog namespace (`SELECT * FROM catalog.schema.table`).
- **FR-012**: System MUST reject write operations (INSERT, UPDATE, DELETE) with clear error message.
- **FR-013**: System MUST stream results using existing `MSSQLResultStream` infrastructure.

#### Projection Pushdown

- **FR-014**: System MUST push down projection (SELECT list) to remote query.
- **FR-015**: System MUST NOT fetch `SELECT *` unless explicitly requested by user.
- **FR-016**: System MUST generate column list using properly quoted identifiers.

#### Filter Pushdown

- **FR-017**: System MUST push down supported predicate operators: `=`, `<>`, `<`, `<=`, `>`, `>=`.
- **FR-018**: System MUST push down null checks: `IS NULL`, `IS NOT NULL`.
- **FR-019**: System MUST push down boolean logic: `AND`, `OR`, `NOT` with correct parentheses.
- **FR-020**: System MUST push down `BETWEEN a AND b`.
- **FR-021**: System MUST push down `IN (...)` for constant lists up to configurable limit (default 100).
- **FR-022**: System MUST push down `LIKE` with proper escaping using backslash (`\`) as escape character and explicit `ESCAPE '\'` clause in generated SQL.
- **FR-023**: System SHOULD push down `ILIKE` as `LIKE` only when column collation is case-insensitive (CI).
- **FR-024**: System SHOULD push down scalar functions: `LOWER`, `UPPER`, `LENGTH`→`LEN`, `SUBSTR`→`SUBSTRING`.
- **FR-025**: System MUST evaluate unsupported predicates locally (fetch superset, filter in DuckDB).
- **FR-026**: System MUST NOT push predicates that would produce incorrect semantics (e.g., collation conflicts).

#### Parameterized Execution

- **FR-027**: System MUST execute pushed predicates using `sp_executesql` with bound parameters.
- **FR-028**: System MUST bind all pushed constants as parameters (no literal formatting).
- **FR-029**: System MUST bind text parameters as `NVARCHAR` (UTF-16LE on wire).
- **FR-030**: System MUST derive SQL Server parameter types from DuckDB types and column metadata.

#### Collation-Aware Parameter Handling

- **FR-031**: System MUST generate sargable filter forms that do NOT apply COLLATE on the column side.
- **FR-032**: System MUST normalize parameters to column collation: `WHERE [col] = CONVERT(varchar(max), @p1) COLLATE <col_collation>`.
- **FR-033**: System MUST use database default collation when column collation is NULL.
- **FR-034**: System MUST keep predicate local if collation cannot be determined and would affect semantics.

#### Prepared Statement Model

- **FR-035**: System MUST implement two-phase execution: prepare/bind phase and execute phase.
- **FR-036**: System MUST build parameterized statement text and parameter signature during bind.
- **FR-037**: System MUST cache prepared statement structure per query shape.
- **FR-038**: System MUST invalidate cached statements when connection is closed or reset.
- **FR-039**: System MUST support streaming results from prepared execution.
- **FR-040**: System MUST support cancellation during prepared statement execution.
- **FR-041**: System MUST handle early termination (LIMIT) without corrupting connection state.

#### Metadata Caching

- **FR-042**: System MUST cache schemas, tables, views, and columns per attached catalog.
- **FR-043**: System MUST implement TTL as an extension parameter `mssql_catalog_cache_ttl` with default value 0 (disabled).
- **FR-044**: System MUST implement `mssql_refresh_catalog(catalog_name)` function for explicit manual refresh.
- **FR-045**: System MUST refresh cache automatically on TTL expiration only when TTL > 0; when TTL = 0, only manual refresh via `mssql_refresh_catalog()` triggers cache update.

#### Error Handling

- **FR-046**: System MUST fall back to local filtering when pushdown translation fails (where safe).
- **FR-047**: System MUST error explicitly when fallback would produce incorrect semantics.
- **FR-048**: System MUST surface SQL Server errors with message and error code.

### Key Entities

- **MSSQLSchemaEntry**: DuckDB schema catalog entry representing a SQL Server schema. Manages tables and views within the schema.
- **MSSQLTableEntry**: DuckDB table catalog entry representing a SQL Server table or view. Contains column metadata, collation info, and supports table function generation.
- **MSSQLColumnInfo**: Column metadata including name, SQL Server type, DuckDB type, nullability, precision, scale, and collation_name.
- **MSSQLFilterPushdown**: Translates DuckDB filter expressions to SQL Server WHERE clauses with parameter binding.
- **MSSQLPreparedStatement**: Represents a parameterized query with statement text, parameter signature, and type mappings.
- **MSSQLBindData**: Bind-time data structure holding prepared statement, column projections, and filter plans.
- **MSSQLMetadataCache**: Per-catalog cache of schemas, tables, and column metadata with TTL management.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: `ATTACH` + `SHOW SCHEMAS` + `SHOW TABLES` + `DESCRIBE` all work correctly for SQL Server databases.
- **SC-002**: `SELECT * FROM catalog.schema.table` returns correct results for all supported column types.
- **SC-003**: Projection pushdown verified: query fetching 3 columns from 10-column table only transfers 3 columns (verifiable via network trace or SQL Server profiler).
- **SC-004**: Filter pushdown verified: query with `WHERE id = 1` on 1M row table returns in <100ms (full scan would be >1s).
- **SC-005**: All pushed predicates use `sp_executesql` with bound parameters (verifiable via SQL Server profiler).
- **SC-006**: VARCHAR filter uses sargable form: `WHERE [col] = CONVERT(varchar(max), @p1) COLLATE <collation>`.
- **SC-007**: ILIKE on CI column pushed as LIKE; ILIKE on CS column filtered locally.
- **SC-008**: Cache refresh updates metadata within 1 second of refresh call.
- **SC-009**: Write operations (INSERT/UPDATE/DELETE) fail with clear error message.
- **SC-010**: Unsupported filters return correct results via local filtering.

## Scope Boundaries

### In Scope

- Schema, table, view discovery via sys.* catalogs
- Column metadata including types, nullability, and collation
- Read-only SELECT via catalog namespace
- Projection pushdown (SELECT list)
- Filter pushdown for comparison, null, boolean, BETWEEN, IN, LIKE operators
- ILIKE pushdown for CI collations
- Basic scalar function pushdown (LOWER, UPPER, LENGTH, SUBSTR)
- Parameterized execution via sp_executesql
- Collation-aware parameter normalization
- Metadata caching with TTL and explicit refresh
- Error handling with local fallback

### Out of Scope (Non-Goals)

- DDL operations (CREATE, ALTER, DROP) - Spec 05.02
- INSERT operations - Spec 05.03
- UPDATE/DELETE operations - Spec 05.04
- Foreign key discovery and relationships
- Index metadata discovery
- Constraint discovery (beyond basic nullability)
- Stored procedure discovery or execution
- User-defined type support
- XML type support
- Query plan optimization hints
- Parallel query execution across partitions
- WASM/browser support

## Assumptions

- Specs 001-006 are implemented: extension bootstrap, DuckDB API, connection pooling, streaming SELECT, TLS, split build.
- SQL Server 2019+ with TDS 7.4 protocol.
- `MSSQLResultStream` from spec 004 is available for result streaming.
- `ConnectionPool` from spec 003 is available for connection management.
- DuckDB catalog API supports custom schema and table entries.
- SQL Server credentials have SELECT permission on sys.* views.
- Database default collation can be determined via `DATABASEPROPERTYEX`.

## Technical Notes

### sp_executesql Usage Pattern

```sql
EXEC sp_executesql
    N'SELECT [col1], [col2] FROM [schema].[table] WHERE [col1] = CONVERT(varchar(max), @p1) COLLATE SQL_Latin1_General_CP1_CI_AS AND [col2] > @p2',
    N'@p1 NVARCHAR(MAX), @p2 INT',
    @p1 = N'value',
    @p2 = 100
```

### Collation Detection Query

```sql
SELECT
    c.name AS column_name,
    c.collation_name,
    DATABASEPROPERTYEX(DB_NAME(), 'Collation') AS database_default
FROM sys.columns c
WHERE c.object_id = OBJECT_ID('[schema].[table]')
```

### Case-Insensitive Collation Detection

Collations ending with `_CI_` or `_CI` (before AS/AI suffix) are case-insensitive:
- `SQL_Latin1_General_CP1_CI_AS` - CI (case-insensitive)
- `SQL_Latin1_General_CP1_CS_AS` - CS (case-sensitive)

## References

- DuckDB Postgres Extension: https://github.com/duckdb/duckdb-postgres
- DuckDB SQLite Extension: https://github.com/duckdb/duckdb-sqlite
- Airport Extension (multi-source): https://github.com/Query-farm/airport
- SQL Server sp_executesql: https://docs.microsoft.com/en-us/sql/relational-databases/system-stored-procedures/sp-executesql-transact-sql
- SQL Server Collation Reference: https://docs.microsoft.com/en-us/sql/relational-databases/collations/collation-and-unicode-support

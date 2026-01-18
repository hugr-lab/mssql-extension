# Feature Specification: Catalog-Driven DDL and Statistics

**Feature Branch**: `008-catalog-ddl-statistics`
**Created**: 2026-01-18
**Status**: Draft
**Input**: Implement catalog-driven DDL for DuckDB MSSQL extension by translating DuckDB catalog DDL operations into remote T-SQL, plus SQL Server-backed statistics for optimizer support.

## Clarifications

### Session 2026-01-18
- Q: Should `mssql_exec` in READ_ONLY mode block ALL statements, or attempt to allow DML while blocking DDL? → A: Block ALL `mssql_exec` calls when READ_ONLY (simplest, safest)

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Execute Remote T-SQL via mssql_exec (Priority: P1)

As a DuckDB user, I want to execute arbitrary T-SQL statements on SQL Server so that I can perform DDL, DML, and administrative operations remotely from DuckDB.

**Why this priority**: This is the foundational capability that all other DDL operations depend on. Without `mssql_exec`, no remote schema modifications are possible.

**Independent Test**: Can be fully tested by executing `SELECT mssql_exec('secret', 'CREATE TABLE test(id INT)');` and verifying the table exists on SQL Server, then dropping it.

**Acceptance Scenarios**:

1. **Given** a valid MSSQL secret and connection, **When** user executes `SELECT mssql_exec('secret', 'CREATE TABLE dbo.TestTable (id INT)');`, **Then** the table is created on SQL Server and the function returns affected row count.
2. **Given** a valid connection, **When** user executes invalid SQL via mssql_exec, **Then** SQL Server error is surfaced as a DuckDB exception with error code, state, and message.
3. **Given** an identifier with special characters (e.g., `]`), **When** user creates a table with that name, **Then** the identifier is properly escaped using `]]` and the operation succeeds.

---

### User Story 2 - Create and Drop Tables via DuckDB DDL (Priority: P1)

As a DuckDB user, I want to create and drop tables on SQL Server using standard DuckDB DDL syntax so that I can manage remote schema without writing T-SQL directly.

**Why this priority**: CREATE/DROP TABLE are the most fundamental DDL operations and enable users to manage remote data structures through DuckDB's familiar interface.

**Independent Test**: Can be fully tested by running `CREATE TABLE mssql.dbo.MyTable (id INTEGER, name VARCHAR(100));` in DuckDB and verifying the table exists in SQL Server, then dropping it with `DROP TABLE mssql.dbo.MyTable;`.

**Acceptance Scenarios**:

1. **Given** an attached MSSQL catalog, **When** user executes `CREATE TABLE mssql.dbo.NewTable (id INT NOT NULL, name VARCHAR(50));`, **Then** the table is created on SQL Server with correct column types and constraints.
2. **Given** an existing table in MSSQL, **When** user executes `DROP TABLE mssql.dbo.ExistingTable;`, **Then** the table is removed from SQL Server.
3. **Given** a CREATE TABLE operation succeeds, **When** querying the DuckDB catalog, **Then** the new table appears in metadata without requiring manual refresh.

---

### User Story 3 - Create and Drop Schemas (Priority: P2)

As a DuckDB user, I want to create and drop schemas on SQL Server using DuckDB DDL so that I can organize my remote database objects.

**Why this priority**: Schema management is important for organization but less frequently used than table operations.

**Independent Test**: Can be fully tested by running `CREATE SCHEMA mssql.test_schema;` and verifying it exists, then dropping it.

**Acceptance Scenarios**:

1. **Given** an attached MSSQL catalog, **When** user executes `CREATE SCHEMA mssql.new_schema;`, **Then** the schema is created on SQL Server.
2. **Given** an empty schema, **When** user executes `DROP SCHEMA mssql.empty_schema;`, **Then** the schema is removed from SQL Server.
3. **Given** a non-empty schema, **When** user attempts `DROP SCHEMA mssql.non_empty;`, **Then** SQL Server error is surfaced indicating schema is not empty.

---

### User Story 4 - Alter Table Columns (Priority: P2)

As a DuckDB user, I want to add, rename, drop, and modify columns on SQL Server tables using DuckDB ALTER TABLE syntax so that I can evolve my schema over time.

**Why this priority**: Column modifications are common schema evolution operations that users need after initial table creation.

**Independent Test**: Can be fully tested by adding a column to an existing table, verifying it exists, then renaming and dropping it.

**Acceptance Scenarios**:

1. **Given** an existing table, **When** user executes `ALTER TABLE mssql.dbo.T ADD COLUMN new_col VARCHAR(100);`, **Then** the column is added to the SQL Server table.
2. **Given** an existing column, **When** user executes `ALTER TABLE mssql.dbo.T RENAME COLUMN old_name TO new_name;`, **Then** the column is renamed on SQL Server.
3. **Given** an existing column, **When** user executes `ALTER TABLE mssql.dbo.T DROP COLUMN col;`, **Then** the column is removed from SQL Server.
4. **Given** an existing column, **When** user changes its nullability, **Then** the SQL Server column constraint is updated accordingly.

---

### User Story 5 - Rename Tables (Priority: P3)

As a DuckDB user, I want to rename tables on SQL Server using DuckDB DDL so that I can refactor my schema naming.

**Why this priority**: Table renaming is less frequent than other operations but still needed for schema maintenance.

**Independent Test**: Can be fully tested by renaming an existing table and verifying the new name is accessible.

**Acceptance Scenarios**:

1. **Given** an existing table `dbo.OldName`, **When** user executes `ALTER TABLE mssql.dbo.OldName RENAME TO NewName;`, **Then** the table is renamed to `dbo.NewName` on SQL Server.
2. **Given** a rename operation, **When** querying metadata, **Then** both old name (removed) and new name (present) are correctly reflected.

---

### User Story 6 - Query Optimizer Uses Table Row Count (Priority: P2)

As a DuckDB user running queries against SQL Server tables, I want DuckDB's optimizer to have access to table row counts so that it can make better join ordering and execution plan decisions.

**Why this priority**: Row count statistics significantly impact query optimization and are cheap to obtain from SQL Server system DMVs.

**Independent Test**: Can be fully tested by querying a known table with N rows and verifying that DuckDB's EXPLAIN output shows approximately N rows for table scans.

**Acceptance Scenarios**:

1. **Given** an MSSQL table with 1000 rows, **When** DuckDB optimizes a query against it, **Then** the optimizer receives cardinality estimate close to 1000.
2. **Given** statistics caching is enabled, **When** row count is requested multiple times within TTL, **Then** SQL Server is queried only once.
3. **Given** a DDL operation modifies a table, **When** statistics are next requested, **Then** cached statistics are invalidated and fresh values are fetched.

---

### User Story 7 - Distinguish Tables from Views in Catalog (Priority: P2)

As a DuckDB user, I want SQL Server views to appear as views (not tables) in DuckDB's catalog so that I have accurate metadata and appropriate behavior for each object type.

**Why this priority**: Correct object type representation is essential for proper DDL routing and user experience with commands like SHOW TABLES.

**Independent Test**: Can be fully tested by creating a view in SQL Server, attaching the database in DuckDB, and verifying the view appears as a view type (not table).

**Acceptance Scenarios**:

1. **Given** SQL Server has both tables and views, **When** user runs `SHOW TABLES` in DuckDB, **Then** only tables are listed.
2. **Given** a SQL Server view, **When** user queries it via DuckDB, **Then** SELECT works normally.
3. **Given** a SQL Server view, **When** user attempts INSERT/UPDATE/DELETE, **Then** an error is raised indicating the view is read-only.

---

### User Story 8 - Optional Column Statistics (Priority: P3)

As a DuckDB power user, I want the option to fetch column-level statistics (min/max, NDV) from SQL Server so that the optimizer can make even better decisions for filtered queries.

**Why this priority**: Column statistics provide additional optimizer value but require DBCC permissions and are more expensive to fetch.

**Independent Test**: Can be fully tested by enabling the statistics level setting, running a filtered query, and verifying EXPLAIN shows refined cardinality estimates.

**Acceptance Scenarios**:

1. **Given** `mssql_statistics_level = 1` and statistics exist on a column, **When** optimizer requests column stats, **Then** min/max values are returned for numeric/temporal columns.
2. **Given** `mssql_statistics_use_dbcc = false` (default), **When** statistics are requested, **Then** DBCC commands are not executed.
3. **Given** `mssql_statistics_level = 2`, **When** optimizer requests NDV, **Then** approximate distinct value count is derived from density vector.

---

### User Story 9 - Read-Only Catalog Mode (Priority: P1)

As a DuckDB user, I want to attach a SQL Server database in read-only mode so that I can safely query data without risk of accidental schema modifications.

**Why this priority**: Read-only mode is essential for production safety, analytics workloads, and environments where users should not modify remote schemas. This is a safety feature that prevents accidental data loss.

**Independent Test**: Can be fully tested by attaching with `ATTACH ... (TYPE mssql, READ_ONLY);` and verifying that all DDL operations are rejected while SELECT queries work normally.

**Acceptance Scenarios**:

1. **Given** a database attached with `READ_ONLY` parameter, **When** user attempts any DDL operation (CREATE/DROP/ALTER), **Then** an error is raised indicating the catalog is read-only.
2. **Given** a database attached with `READ_ONLY` parameter, **When** user executes SELECT queries, **Then** queries execute normally.
3. **Given** a database attached without `READ_ONLY` parameter, **When** user attempts DDL operations, **Then** DDL operations proceed normally (existing behavior).
4. **Given** a database attached with `READ_ONLY` parameter, **When** user attempts any `mssql_exec` call, **Then** an error is raised indicating the catalog is read-only (all `mssql_exec` blocked, not just DDL).

---

### Edge Cases

- What happens when DDL targets a non-existent schema or table? SQL Server error is surfaced with clear message.
- What happens when ALTER COLUMN changes type to incompatible type? SQL Server rejects and error is surfaced.
- What happens when adding NOT NULL column to non-empty table without default? SQL Server error is surfaced.
- What happens when dropping a column that has dependencies (indexes, constraints)? SQL Server error is surfaced.
- What happens when statistics DMV query fails due to permissions? Graceful fallback to no statistics (return nullptr).
- What happens when DBCC SHOW_STATISTICS is attempted without permissions? Operation skipped, warning logged, fallback to level 0.
- What happens when DDL is attempted on a READ_ONLY attached catalog? Clear error message indicating catalog is read-only, operation rejected before any remote call.

## Requirements *(mandatory)*

### Functional Requirements

#### mssql_exec Function
- **FR-001**: System MUST provide `mssql_exec(secret_name VARCHAR, sql VARCHAR) -> BIGINT` function to execute arbitrary T-SQL on SQL Server.
- **FR-002**: `mssql_exec` MUST return affected row count where meaningful.
- **FR-003**: `mssql_exec` MUST surface SQL Server errors with error code, state, class, and message as DuckDB exceptions.

#### DDL via Catalog Hooks
- **FR-004**: System MUST translate DuckDB DDL operations to T-SQL via catalog hooks (not by parsing SQL strings).
- **FR-005**: All DDL operations MUST route through `mssql_exec` for remote execution.
- **FR-006**: All identifiers MUST be quoted using T-SQL square-bracket syntax with `]` escaped as `]]`.

#### Schema Operations
- **FR-007**: System MUST support `CREATE SCHEMA` translated to `CREATE SCHEMA [schema];`.
- **FR-008**: System MUST support `DROP SCHEMA` translated to `DROP SCHEMA [schema];`.

#### Table Operations
- **FR-009**: System MUST support `CREATE TABLE` with columns emitted in ordinal order and appropriate type mappings.
- **FR-010**: System MUST support `DROP TABLE` translated to `DROP TABLE [schema].[table];`.
- **FR-011**: System MUST support `RENAME TABLE` using `sp_rename` (within same schema only).

#### Column Operations
- **FR-012**: System MUST support `ADD COLUMN` translated to `ALTER TABLE ... ADD [column] <type> [NULL|NOT NULL];`.
- **FR-013**: System MUST support `RENAME COLUMN` using `sp_rename` with COLUMN parameter.
- **FR-014**: System MUST support `DROP COLUMN` translated to `ALTER TABLE ... DROP COLUMN [column];`.
- **FR-015**: System MUST support `ALTER COLUMN SET TYPE` preserving nullability explicitly.
- **FR-016**: System MUST support `ALTER COLUMN SET NULL/NOT NULL` with type explicitly specified.

#### Type Mapping
- **FR-017**: DuckDB types MUST be mapped to SQL Server types using consistent mapping rules.
- **FR-018**: `DECIMAL(p,s)` MUST be clamped to SQL Server supported ranges.
- **FR-019**: `TIMESTAMP_TZ` MUST map to `DATETIMEOFFSET(7)`.
- **FR-020**: String types SHOULD prefer `NVARCHAR` for Unicode sources, `VARCHAR` otherwise.

#### Cache Invalidation
- **FR-021**: Metadata cache MUST be invalidated after successful DDL operations.
- **FR-022**: Cache MUST NOT be invalidated if remote DDL fails.
- **FR-023**: Rename operations MUST invalidate cache for both old and new names.

#### Statistics - Row Count (MUST)
- **FR-024**: System MUST fetch table row count from `sys.dm_db_partition_stats` DMV.
- **FR-025**: Row count MUST be cached with configurable TTL (default 300 seconds).
- **FR-026**: Statistics cache MUST be invalidated after DDL operations affecting the table.

#### Statistics - Configuration
- **FR-027**: System MUST provide `mssql_enable_statistics` setting (default: true).
- **FR-028**: System MUST provide `mssql_statistics_level` setting (default: 0 = rowcount only).
- **FR-029**: System MUST provide `mssql_statistics_use_dbcc` setting (default: false).
- **FR-030**: System MUST provide `mssql_statistics_cache_ttl_seconds` setting (default: 300).

#### Statistics - Column Stats (SHOULD)
- **FR-031**: When `mssql_statistics_level >= 1` and `mssql_statistics_use_dbcc = true`, system SHOULD fetch min/max from histogram for numeric/temporal columns.
- **FR-032**: When `mssql_statistics_level >= 2`, system SHOULD derive NDV approximation from density vector.

#### Table vs View Distinction
- **FR-033**: Catalog MUST differentiate SQL Server tables (`sys.objects.type = 'U'`) from views (`type = 'V'`).
- **FR-034**: Tables MUST be exposed as DuckDB TABLE entries.
- **FR-035**: Views MUST be exposed as DuckDB VIEW entries (read-only).
- **FR-036**: SELECT on views MUST work via mssql_scan.
- **FR-037**: INSERT/UPDATE/DELETE on views MUST raise "MSSQL view is read-only" error.
- **FR-038**: View and table metadata MUST be cached separately.

#### Read-Only Mode
- **FR-042**: System MUST support `READ_ONLY` parameter on ATTACH statement.
- **FR-043**: When `READ_ONLY` is set, all DDL operations (CREATE/DROP/ALTER SCHEMA, TABLE, COLUMN) MUST be rejected with clear error.
- **FR-044**: When `READ_ONLY` is set, ALL `mssql_exec` calls MUST be rejected (no SQL parsing to distinguish DDL from DML).
- **FR-045**: When `READ_ONLY` is set, SELECT queries MUST continue to work normally.
- **FR-046**: Read-only check MUST occur before any remote SQL Server call is made.
- **FR-047**: Error message for read-only rejection MUST clearly indicate that the catalog was attached in read-only mode.

#### Error Handling
- **FR-039**: Error messages MUST include operation type (e.g., `ALTER_TABLE_ADD_COLUMN`).
- **FR-040**: Error messages SHOULD include MSSQL error code/state/class when available.
- **FR-041**: Error messages MUST include schema/table name being modified.

### Key Entities

- **DDL Operation**: Represents a catalog modification request with operation type, target schema/table/column, and parameters.
- **T-SQL Statement**: The translated SQL Server statement generated from a DDL operation.
- **Table Statistics**: Cached statistics for a table including row count, optional column min/max, and NDV.
- **Metadata Cache Entry**: Cached schema/table/column metadata with TTL and invalidation key.
- **Catalog Object Kind**: Discriminator between TABLE and VIEW for proper DuckDB catalog representation.
- **Catalog Access Mode**: Discriminator between read-write (default) and read-only modes, set at ATTACH time.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can create, alter, and drop tables on SQL Server using standard DuckDB DDL syntax without writing T-SQL.
- **SC-002**: DDL operations complete with SQL Server error details surfaced within 5 seconds for typical operations.
- **SC-003**: After DDL operations, DuckDB catalog reflects changes without requiring manual refresh.
- **SC-004**: Query optimizer receives accurate row count estimates (within 10% of actual) for table scans.
- **SC-005**: Statistics queries add less than 100ms latency when cached (cache hit).
- **SC-006**: Views are correctly identified and queries against views return data successfully.
- **SC-007**: Attempts to modify views raise clear read-only errors within 1 second.
- **SC-008**: All DDL tests pass in CI/CD pipeline with docker-compose SQL Server environment.
- **SC-009**: Read-only catalogs reject DDL operations immediately with clear error messages.
- **SC-010**: Read-only catalogs allow all SELECT queries without degradation.

## Scope Boundaries

### In Scope
- CREATE/DROP SCHEMA
- CREATE/DROP/RENAME TABLE
- ADD/RENAME/DROP/ALTER COLUMN
- Primary key support (if already modeled in catalog)
- Row count statistics (MUST)
- Optional column min/max and NDV (SHOULD/NICE-TO-HAVE)
- Table vs View distinction in catalog
- mssql_exec function implementation
- READ_ONLY attach parameter support

### Out of Scope (Non-Goals)
- Index creation/modification
- Foreign key constraints
- Constraints beyond primary key
- Triggers
- View DDL (CREATE VIEW, ALTER VIEW)
- INSERT/UPDATE/DELETE through views
- Data migration workflows for incompatible type changes
- Stored procedure management
- User/permission management

## Assumptions

- SQL Server connection is already established via DuckDB's secret and ATTACH mechanism (spec 007).
- DuckDB catalog API supports hooks for DDL operations.
- User has appropriate SQL Server permissions for DDL operations they attempt.
- Statistics DMV queries (`sys.dm_db_partition_stats`) are available without special permissions.
- DBCC commands may require elevated permissions and are opt-in.

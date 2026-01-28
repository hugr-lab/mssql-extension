# Feature Specification: CTAS for MSSQL (CREATE TABLE AS SELECT)

**Feature Branch**: `022-mssql-ctas`
**Created**: 2026-01-28
**Status**: Draft
**Input**: User description: "Spec 05.06 — CTAS for MSSQL (DuckDB-driven: CREATE then INSERT, no RETURNING)"

## Clarifications

### Session 2026-01-28

- Q: Should CTAS emit structured observability signals (DDL size, row counts, phase timing, failure phase)? → A: Yes, add observability FR: emit debug-level logs for DDL size, row counts, phase timing, failure phase (via existing debug env vars)
- Q: Should CTAS support OR REPLACE or IF NOT EXISTS modifiers? → A: Add OR REPLACE support now (drop + recreate if table exists)

## User Scenarios & Testing

### User Story 1 - Basic CTAS from DuckDB Query (Priority: P1)

A DuckDB user with an attached MSSQL database executes `CREATE TABLE mssql_db.schema.table AS SELECT ...` to materialize DuckDB query results into a new SQL Server table. The extension translates the DuckDB output schema into SQL Server column definitions, creates the table remotely, then bulk-inserts the result set.

**Why this priority**: This is the core capability. Without basic CTAS, no other scenarios are possible.

**Independent Test**: Execute `CREATE TABLE mssql.dbo.test1 AS SELECT 1 AS id, 'hello' AS name` and verify the table exists in SQL Server with correct schema and data.

**Acceptance Scenarios**:

1. **Given** an attached MSSQL database, **When** user executes `CREATE TABLE mssql.dbo.ctas1 AS SELECT 1 AS id, 'x' AS name`, **Then** table `dbo.ctas1` is created in SQL Server with columns `id (int, NOT NULL)` and `name (nvarchar(max), NULL)`, containing one row.
2. **Given** an attached MSSQL database, **When** user executes CTAS with a query producing multiple rows, **Then** all rows are inserted into the newly created table.
3. **Given** an attached MSSQL database, **When** user executes CTAS referencing a non-existent schema, **Then** the operation fails with a clear error indicating the schema does not exist.

---

### User Story 2 - CTAS with Large Result Sets (Priority: P1)

A user materializes a large DuckDB query result (e.g., 1M+ rows) into SQL Server. The insert phase uses batched bulk insert to maintain stable memory usage and acceptable throughput.

**Why this priority**: Large data loads are a primary use case for CTAS. Correctness at scale is essential.

**Independent Test**: Execute CTAS from `generate_series(1, 1000000)` and verify row count matches and memory stays bounded.

**Acceptance Scenarios**:

1. **Given** an attached MSSQL database, **When** user executes CTAS producing 1,000,000 rows, **Then** the target table contains exactly 1,000,000 rows.
2. **Given** a CTAS operation with large output, **When** the insert phase runs, **Then** memory usage remains stable (no unbounded growth).

---

### User Story 3 - CTAS Type Mapping (Priority: P1)

A user creates a table whose columns span all supported DuckDB types. The extension maps each DuckDB type to the correct SQL Server type. Unsupported types produce a clear error before any DDL is executed.

**Why this priority**: Correct type mapping is fundamental to data integrity. Incorrect mappings cause silent data corruption.

**Independent Test**: Execute CTAS with columns of each supported DuckDB type and verify SQL Server schema matches expected types.

**Acceptance Scenarios**:

1. **Given** a CTAS query producing columns of types BOOLEAN, INTEGER, BIGINT, FLOAT, DOUBLE, VARCHAR, DATE, TIMESTAMP, UUID, BLOB, **When** the table is created, **Then** SQL Server columns use the correct mapped types (bit, int, bigint, real, float, nvarchar(max), date, datetime2(7), uniqueidentifier, varbinary(max)).
2. **Given** a CTAS query producing a column with an unsupported DuckDB type, **When** the user executes CTAS, **Then** the operation fails with an error naming the unsupported column and type before any table is created.
3. **Given** CTAS with DECIMAL(p,s) columns, **When** precision/scale exceed SQL Server maximums, **Then** they are clamped to SQL Server limits and the table is created successfully.

---

### User Story 4 - Failure Handling and Cleanup (Priority: P2)

A user's CTAS operation fails during the insert phase (after the table was already created). By default the table remains for debugging. With `mssql_ctas_drop_on_failure` enabled, the extension attempts to drop the partially-loaded table.

**Why this priority**: Deterministic failure behavior is important for production use, but secondary to basic correctness.

**Independent Test**: Induce an insert failure and verify table remains by default; then enable the cleanup setting and verify the table is dropped.

**Acceptance Scenarios**:

1. **Given** a CTAS where the DDL phase succeeds but the insert phase fails, **When** `mssql_ctas_drop_on_failure` is `false` (default), **Then** the operation reports an error and the empty/partial table remains in SQL Server.
2. **Given** a CTAS where the DDL phase succeeds but the insert phase fails, **When** `mssql_ctas_drop_on_failure` is `true`, **Then** the extension attempts `DROP TABLE` and reports the original insert error.
3. **Given** a CTAS where cleanup is enabled and DROP TABLE also fails, **When** the insert phase fails, **Then** both the original error and the drop error are surfaced to the user.

---

### User Story 5 - Text Type Policy (Priority: P2)

A user controls whether text columns are created as `nvarchar(max)` (Unicode-safe default) or `varchar(max)` (advanced, collation-dependent) via the `mssql_ctas_text_type` setting.

**Why this priority**: Unicode correctness is the safe default. The setting provides flexibility for advanced users.

**Independent Test**: Execute CTAS with text columns under each setting value and verify the SQL Server column type.

**Acceptance Scenarios**:

1. **Given** `mssql_ctas_text_type` set to `NVARCHAR` (default), **When** CTAS creates a table with VARCHAR columns from DuckDB, **Then** SQL Server columns are `nvarchar(max)`.
2. **Given** `mssql_ctas_text_type` set to `VARCHAR`, **When** CTAS creates a table with VARCHAR columns from DuckDB, **Then** SQL Server columns are `varchar(max)`.

---

### User Story 6 - Transactional CTAS (Priority: P3)

A user executes CTAS inside a DuckDB transaction. The DDL (CREATE TABLE) may auto-commit on SQL Server, but the data inserted during the load phase respects transaction boundaries.

**Why this priority**: Transaction semantics add correctness guarantees but are secondary to basic functionality.

**Independent Test**: Execute CTAS inside a DuckDB transaction, then rollback. Verify that the table exists (DDL auto-committed) but contains no data (DML rolled back).

**Acceptance Scenarios**:

1. **Given** a CTAS executed inside a DuckDB transaction that is committed, **When** the transaction completes, **Then** the table exists with all data.
2. **Given** a CTAS executed inside a DuckDB transaction that is rolled back, **When** the rollback completes, **Then** the table may exist (DDL auto-commit) but contains no data from the rolled-back insert.

---

### User Story 7 - OR REPLACE Behavior (Priority: P2)

A user executes `CREATE OR REPLACE TABLE mssql_db.schema.table AS SELECT ...` to recreate an existing table with new schema and data. The extension drops the existing table first, then proceeds with standard CTAS.

**Why this priority**: OR REPLACE simplifies iterative development workflows where users re-run CTAS statements without manual cleanup.

**Independent Test**: Create a table, then execute `CREATE OR REPLACE TABLE` targeting the same name. Verify the old table is dropped and replaced with new schema/data.

**Acceptance Scenarios**:

1. **Given** an existing table `dbo.target`, **When** user executes `CREATE OR REPLACE TABLE mssql.dbo.target AS SELECT ...`, **Then** the old table is dropped and replaced with the new schema and data.
2. **Given** no existing table `dbo.newtable`, **When** user executes `CREATE OR REPLACE TABLE mssql.dbo.newtable AS SELECT ...`, **Then** the table is created normally (OR REPLACE is a no-op for non-existent tables).
3. **Given** OR REPLACE where the DROP succeeds but subsequent CREATE fails, **When** the error occurs, **Then** the operation fails and the original table is gone (non-atomic behavior documented).

---

### Edge Cases

- What happens when the target table already exists? CTAS must fail with a clear "table already exists" error.
- What happens when the source query returns zero rows? The table should be created with the correct schema but contain no rows.
- What happens when column names contain special characters or reserved words? Column names must be bracket-escaped in the DDL.
- What happens when the DuckDB query itself fails? No DDL should be executed; the error propagates to the user.
- What happens when the connection is lost during the insert phase? The table remains (per default behavior); partial data may exist.
- What happens when OR REPLACE DROP succeeds but CREATE fails? The original table is lost; this is documented non-atomic behavior.

## Requirements

### Functional Requirements

- **FR-001**: The extension MUST support `CREATE TABLE <mssql_db>.<schema>.<table> AS SELECT ...` syntax for attached MSSQL databases.
- **FR-002**: CTAS MUST execute in two deterministic phases: (1) CREATE TABLE via remote DDL, (2) INSERT via batched bulk insert without RETURNING/OUTPUT.
- **FR-003**: The DDL phase MUST translate the DuckDB planned output schema to SQL Server column definitions using the type mapping defined in this spec.
- **FR-004**: After successful CREATE TABLE, the extension MUST invalidate the local catalog cache for the target table.
- **FR-005**: The insert phase MUST use the SQL Batch bulk insert path (Mode A) with configurable batch size and SQL size limits.
- **FR-006**: If CREATE TABLE fails, CTAS MUST fail with no side effects (no table created).
- **FR-007**: If INSERT fails after table creation, CTAS MUST fail. The table remains by default. When `mssql_ctas_drop_on_failure` is `true`, the extension MUST attempt `DROP TABLE` (best-effort).
- **FR-008**: If cleanup DROP TABLE also fails, the extension MUST surface both the original insert error and the drop error.
- **FR-009**: Target table name MUST be schema-qualified. CTAS MUST fail if the target schema does not exist.
- **FR-010**: Column names MUST be bracket-escaped in generated DDL.
- **FR-011**: DuckDB NOT NULL columns MUST produce `NOT NULL` in SQL Server; all other columns MUST be `NULL`.
- **FR-012**: CTAS MUST fail with an actionable error listing the column name and type if an unsupported DuckDB type is encountered.
- **FR-013**: Text columns MUST default to `nvarchar(max)`. The `mssql_ctas_text_type` setting MUST allow switching to `varchar(max)`.
- **FR-014**: CTAS (without OR REPLACE) MUST fail if the target table already exists.
- **FR-015**: CTAS with a source query returning zero rows MUST create the table with correct schema and no data.
- **FR-016**: The generated DDL MUST NOT include primary keys, indexes, or constraints beyond nullability.
- **FR-017**: DECIMAL(p,s) columns MUST clamp precision and scale to SQL Server maximums when exceeded.
- **FR-018**: The extension MUST support `CREATE OR REPLACE TABLE ... AS SELECT` syntax. When OR REPLACE is specified and the target table exists, the extension MUST drop it before creating the new table.
- **FR-019**: OR REPLACE on a non-existent table MUST behave identically to standard CTAS (no error).
- **FR-020**: OR REPLACE is non-atomic: if DROP succeeds but CREATE fails, the original table is lost. This behavior MUST be documented.
- **FR-021**: CTAS MUST emit debug-level observability logs (when `MSSQL_DEBUG` or `MSSQL_DML_DEBUG` is set) including: DDL statement size (bytes), rows produced by source query, rows inserted, time spent in CREATE vs INSERT phases, and failure phase (DDL vs DML) on error.

### Type Mapping

| DuckDB Type  | SQL Server Type   | Notes                                   |
| ------------ | ----------------- | --------------------------------------- |
| BOOLEAN      | bit               |                                         |
| TINYINT      | tinyint           |                                         |
| SMALLINT     | smallint          |                                         |
| INTEGER      | int               |                                         |
| BIGINT       | bigint            |                                         |
| FLOAT        | real              |                                         |
| DOUBLE       | float             |                                         |
| DECIMAL(p,s) | decimal(p,s)      | Clamp to SQL Server max (38,38)         |
| VARCHAR      | nvarchar(max)     | Configurable via `mssql_ctas_text_type` |
| UUID         | uniqueidentifier  |                                         |
| BLOB         | varbinary(max)    |                                         |
| DATE         | date              |                                         |
| TIME         | time(7)           |                                         |
| TIMESTAMP    | datetime2(7)      |                                         |
| TIMESTAMP_TZ | datetimeoffset(7) |                                         |

### Settings

| Setting                      | Type   | Default    | Description                               |
| ---------------------------- | ------ | ---------- | ----------------------------------------- |
| `mssql_ctas_drop_on_failure` | BOOL   | `false`    | Drop table if insert phase fails          |
| `mssql_ctas_text_type`       | STRING | `NVARCHAR` | Text column type: `NVARCHAR` or `VARCHAR` |

### Key Entities

- **Target Table**: The SQL Server table created by CTAS. Defined by schema-qualified name, column definitions (name, type, nullability), and no additional constraints.
- **Source Query**: The DuckDB SELECT query whose planned output schema drives column definitions and whose result set provides the data.
- **Type Mapping**: The deterministic mapping from DuckDB logical types to SQL Server column types.

## Success Criteria

### Measurable Outcomes

- **SC-001**: CTAS with a single-row query completes successfully and produces a table with correct schema and data.
- **SC-002**: CTAS with 1,000,000 rows completes with exact row count match and stable memory usage.
- **SC-003**: All supported DuckDB types map correctly to the specified SQL Server types.
- **SC-004**: Unsupported types produce a clear, actionable error naming the problematic column before any DDL is executed.
- **SC-005**: Insert-phase failure with `mssql_ctas_drop_on_failure=false` leaves the table in SQL Server.
- **SC-006**: Insert-phase failure with `mssql_ctas_drop_on_failure=true` removes the table from SQL Server (best-effort).
- **SC-007**: Text columns default to `nvarchar(max)` and can be switched to `varchar(max)` via setting.
- **SC-008**: CTAS against a non-existent schema fails with a clear error.
- **SC-009**: CTAS against an already-existing table fails with a clear error.
- **SC-010**: Zero-row CTAS creates the table with correct schema and no data.
- **SC-011**: OR REPLACE on an existing table drops and recreates it with new schema/data.
- **SC-012**: OR REPLACE on a non-existent table creates it normally without error.
- **SC-013**: With debug env vars enabled, CTAS emits logs showing DDL size, row counts, phase timing, and failure phase.

## Assumptions

- The DDL execution path from Spec 05.02 (Catalog-Driven DDL) is available and functional.
- The SQL Batch MVP insert path (Spec 05.03, Mode A) is available and functional.
- SQL Server CREATE TABLE is auto-committed (implicit transaction) even when executed within a DuckDB transaction context. Only the insert phase participates in transaction semantics.
- The existing catalog cache invalidation mechanism from Spec 05.02 is reusable.
- HUGEINT and other exotic DuckDB types are considered unsupported for MVP.

## Scope Boundaries

### In Scope

- `CREATE TABLE mssql_db.schema.table AS SELECT ...` syntax
- `CREATE OR REPLACE TABLE mssql_db.schema.table AS SELECT ...` syntax
- DuckDB-to-SQL Server type mapping for all types listed in the type mapping table
- Two-phase execution (DDL then DML)
- Failure handling with optional cleanup
- Text type policy setting
- Catalog cache invalidation after DDL
- Debug-level observability logging (DDL size, row counts, phase timing, failure phase)

### Out of Scope (MVP)

- Primary keys, indexes, or constraints beyond nullability
- Identity column inference
- CTAS into views
- TEMPORARY tables
- WITH (...) storage options and filegroups
- Distributed or partitioned tables
- Automatic schema creation when target schema does not exist
- Statistics creation beyond cache invalidation
- MERGE/UPSERT operations (covered in a separate spec)

# Feature Specification: Fix Partitioned Table Catalog

**Feature Branch**: `049-fix-partitioned-table-catalog`
**Created**: 2026-05-19
**Status**: Draft
**Input**: User description: "Fix issue #85 — querying a SQL Server table with a partitioned clustered index fails with `Catalog Error: Column with name <col> already exists!`. Root cause: the three catalog metadata queries in `src/catalog/mssql_metadata_cache.cpp` join `sys.partitions` without aggregating, so for an N-partition table every column row is multiplied N times. Fix the queries, add a regression test, and confirm tables with single partitions still report the same row counts."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Query a table with a partitioned clustered index (Priority: P1)

A user ATTACHes a SQL Server database that contains one or more tables with a partitioned clustered index. They run `SELECT * FROM attached_db.dbo.<table>` (or any other read against that table). The query succeeds and returns the expected rows.

**Why this priority**: This is the reported bug. Without the fix any table whose clustered index sits on a partition scheme — a common pattern in time-series / append-only OLTP designs (logs, events, telemetry, audit trails) — is unreadable. The failure mode is at catalog binding time, so even `LIMIT 1`, `SELECT 1`, and `DESCRIBE` fail with the same error. Users cannot work around the bug from DuckDB; the only escape is `mssql_scan()` (raw T-SQL), which bypasses the catalog entirely and loses filter/projection pushdown.

**Independent Test**: Create a partitioned-clustered-index table in SQL Server (3 boundary values → 4 partitions is sufficient), `ATTACH`, then run `SELECT * FROM attached.dbo.<table>`. The query returns rows without the "Column with name X already exists!" exception.

**Acceptance Scenarios**:

1. **Given** a SQL Server table with a partitioned clustered index across N≥2 partitions, **When** the user runs `SELECT * FROM attached_db.dbo.<table>` for the first time after `ATTACH`, **Then** the query succeeds and column metadata is built with each column appearing exactly once.
2. **Given** the same table, **When** the user runs `DESCRIBE attached_db.dbo.<table>`, **Then** each column appears exactly once with its correct type, nullability, and collation.
3. **Given** the same table, **When** the user runs `SELECT col_a, col_b FROM attached_db.dbo.<table>` with projection pushdown enabled, **Then** the generated T-SQL projects each column once (no duplicate-column SQL error from SQL Server).
4. **Given** a partitioned heap (no clustered index — partitioned table without an explicit clustered index), **When** queried, **Then** behaviour is the same as US1.1 — columns are not duplicated.

---

### User Story 2 - Correct row-count estimate for partitioned tables (Priority: P2)

A user runs `SHOW ALL TABLES` (or `duckdb_tables()`, or any query that reads the table's cardinality estimate) against a partitioned table. The row count returned is the **sum of rows across all partitions**, not the row count of an arbitrary single partition.

**Why this priority**: The current code accidentally reports the row count of whichever partition `sys.partitions` happens to emit first (it's not even consistently the first partition — SQL Server is free to order rows). For a log table with 100M rows spread across 12 monthly partitions, DuckDB may report ~8M rows and pick wildly wrong join orders. The bug is invisible (no error) but corrupts every cardinality-driven optimization. Fixing US1 without fixing US2 leaves a silent correctness bug behind.

**Independent Test**: Create a partitioned table, insert known counts into multiple partitions (e.g., 100 rows into partition 1, 200 into partition 2, 300 into partition 3), `ATTACH`, then read the cardinality via `SELECT estimated_size FROM duckdb_tables() WHERE table_name = '<name>'`. Expected: 600. Without the fix: 100 (or 200, or 300 — whichever partition SQL Server emits first).

**Acceptance Scenarios**:

1. **Given** a partitioned table with known row counts per partition, **When** the user reads `duckdb_tables().estimated_size`, **Then** the value equals `SUM(p.rows)` from `sys.partitions WHERE index_id IN (0, 1)` for that object.
2. **Given** a non-partitioned table (single partition), **When** the user reads `duckdb_tables().estimated_size`, **Then** the value equals the table's row count and is bit-for-bit identical to the pre-fix value (no regression on the 99% case).
3. **Given** a partitioned table, **When** the planner picks a join order, **Then** the chosen order reflects the full table size, not a single partition's size.

---

### User Story 3 - Bulk catalog preload handles partitioned tables (Priority: P2)

A user calls `mssql_preload_catalog('ctx')` against a database that contains one or more partitioned tables. The preload completes without error, each table has columns loaded exactly once, and row counts are aggregated across partitions.

**Why this priority**: `mssql_preload_catalog` runs the `BULK_METADATA_SCHEMA_SQL_TEMPLATE` query — the same query path as US1, just batched. If US1 is fixed but the bulk path isn't, calling preload on a database that contains any partitioned table will crash with the same "Column with name X already exists!" error and abort the entire schema load. All three queries must be fixed together.

**Independent Test**: Attach to a database containing at least one partitioned table, call `SELECT mssql_preload_catalog('ctx')`, then read any non-partitioned and any partitioned table. Both succeed; preload reports the expected total column count (no inflated count from partition multiplication).

**Acceptance Scenarios**:

1. **Given** a database containing at least one partitioned table, **When** the user calls `SELECT mssql_preload_catalog('ctx')`, **Then** the call succeeds and the returned `column_count` equals `SUM(column count per table)` with each column counted once.
2. **Given** the same setup, **When** the user runs `SHOW ALL TABLES` after preload, **Then** all tables (partitioned and not) are listed with correct row counts.
3. **Given** `LoadAllTableMetadata` is called for a schema that contains partitioned tables, **When** the cache is populated, **Then** each table in the cache has its columns vector free of duplicates.

---

### User Story 4 - Cross-cutting paths against a partitioned table (Priority: P2)

The catalog fix unblocks partitioned-table reads, but a user typically does more than `SELECT *`. They push filters on the partition key, INSERT new rows, UPDATE/DELETE (when a PK is present), and use `COPY TO` for bulk loads. Each of these paths consumes catalog metadata before doing its own work — a fresh-eyes audit must confirm they all still work end-to-end against a partitioned table, not just the catalog read.

**Why this priority**: Without explicit coverage, the fix could silently regress in any of these paths. The metadata cache is the foundation; every DML/COPY/CTAS/filter operation walks it first. Adding even smoke-level coverage on each cross-cutting path is cheap insurance and turns latent regressions into clear test failures.

**Independent Test**: Against the same partitioned `dbo.PartitionedLog` fixture used by US1-3, run one assertion per cross-cutting path and verify the expected behaviour. Each path is independently testable.

**Acceptance Scenarios**:

1. **Given** a partitioned table whose clustered key is its partition key (`id`), **When** the user runs `SELECT * FROM testdb_part.dbo.PartitionedLog WHERE id < 100`, **Then** the query returns exactly the rows in partition 1 (10 rows in the fixture) and the pushed-down T-SQL contains the predicate (verified by `EXPLAIN`).
2. **Given** a partitioned table, **When** the user runs `INSERT INTO testdb_part.dbo.PartitionedLog (id, message) VALUES (250, 'inserted')`, **Then** the INSERT succeeds and `SELECT COUNT(*) WHERE id = 250` returns 1, and the cardinality estimate (after `mssql_refresh_cache`) goes up by 1.
3. **Given** a partitioned table **with** a primary key (`dbo.PartitionedWithPK`, added to the fixture), **When** the user runs an `UPDATE` and a `DELETE` filtered by PK, **Then** both succeed and the rowid path executes correctly — the spec 001 rowid pseudo-column is exposed by `GetVirtualColumns()` because the PK exists.
4. **Given** an empty partitioned target table, **When** the user runs `COPY testdb_part.dbo.PartitionedLog FROM '<file.csv>'` (or `COPY (SELECT ...) TO 'mssql:testdb_part.dbo.PartitionedLog' (FORMAT BCP)`), **Then** the BCP path succeeds — `src/copy/target_resolver.cpp` queries `sys.columns` directly (no `sys.partitions` join), so it should remain unaffected, but the catalog binding that precedes target resolution depends on the fix.

---

### User Story 5 - Tighten SQL injection surface in metadata cache (Priority: P3, follow-up scope)

The three SQL templates in `src/catalog/mssql_metadata_cache.cpp` substitute `schema_name` and `table_name` via `StringUtil::Format` without escaping single quotes — unlike `src/catalog/mssql_statistics.cpp:155-169` which does escape them. This is **pre-existing** behaviour, not introduced by this fix, but the audit performed for spec 049 surfaced it.

**Why this priority** (P3, not P1): DuckDB's parser rejects single quotes in unquoted identifiers, and `ATTACH` options also reject them — so realistic user input never reaches this code path with a quote. The exploit window requires a SQL Server-side table created with `CREATE TABLE [name'with'quote] (...)` (legal in T-SQL) that DuckDB then enumerates. Possible but exotic. Hardening is worth doing, but it's a separate hygiene improvement, not part of the issue #85 crash fix.

**Scope decision**: US5 is documented here so the finding isn't lost, but it is **out of scope for this spec's PR**. A follow-up spec or PR should add the same `'` → `''` escape used by `mssql_statistics.cpp::FetchRowCount` to `MSSQLMetadataCache::GetTableMetadata`, `LoadAllTableMetadata`, `BulkLoadAll`, and `EnsureTablesLoaded`.

**Acceptance Scenarios** (deferred):

1. **Given** a SQL Server table named `[evil';DROP TABLE x;--]`, **When** DuckDB enumerates the schema containing it, **Then** the generated metadata SQL escapes the quote to `''` and SQL Server treats the string as a literal — no extra statements execute.
2. **Given** the same setup, **When** the user runs `SELECT * FROM attached.dbo.[evil';DROP TABLE x;--]`, **Then** the query either succeeds (if the name is otherwise valid) or fails cleanly with "object not found", but never executes the injected statement.

---

### Edge Cases

- **Heap with partitions** (partitioned table with no clustered index — `index_id = 0` rows in `sys.partitions`). The fix must still aggregate row counts across partitions for heaps, since the existing predicate `index_id IN (0, 1)` covers both heaps (0) and clustered indexes (1).
- **Table with no partitions and no rows** (newly created, never inserted). `sys.partitions` still has one row with `rows = 0`. The `LEFT JOIN` should yield `approx_rows = 0`, not NULL — `ISNULL(p.rows, 0)` must remain in the outer projection.
- **System / hidden tables** with no entry in `sys.partitions`. The `LEFT JOIN` (not `INNER`) preserves the row; `ISNULL(p.rows, 0)` yields 0. Same behaviour as today.
- **Multiple non-clustered indexes** on a partitioned table. `sys.partitions` has rows with `index_id` ≥ 2 for those, but the `WHERE index_id IN (0, 1)` filter excludes them. The fix must keep this filter (or aggregation will inflate row counts by index count).
- **Partitioned columnstore index** (clustered or non-clustered). Clustered columnstore uses `index_id = 1`, so it's covered. Non-clustered columnstore uses `index_id ≥ 2`, so it's excluded by the filter — correct behaviour (we don't want to count rows twice).
- **Partition with 0 rows in the middle** (e.g., empty Q1 partition between full Q4 prev year and Q2 current year). `SUM(rows)` correctly handles this — zero contributes zero.
- **Very large partitioned tables** (1B+ rows across hundreds of partitions). `SUM(rows)` returns BIGINT; SQL Server's `rows` column is already BIGINT, so no overflow risk.
- **Concurrent partition split** mid-query. The aggregation runs in a single SELECT under SQL Server's read-committed semantics; we get a consistent snapshot for that query.
- **View on a partitioned table**. Views don't have entries in `sys.partitions`. `o.type IN ('U', 'V')` includes views; the `LEFT JOIN` yields no match for views and `approx_rows = 0` — unchanged behaviour.

## Requirements *(mandatory)*

### Functional Requirements

**Column duplication fix (bug)**:

- **FR-001**: `SINGLE_TABLE_METADATA_SQL_TEMPLATE` MUST return exactly one row per `(object_id, column_id)` pair, regardless of how many partitions the table has.
- **FR-002**: `BULK_METADATA_SCHEMA_SQL_TEMPLATE` MUST return exactly one row per `(schema, object_id, column_id)` triple, regardless of how many partitions any table in the schema has.
- **FR-003**: `MakeTableInfo()` (in `src/catalog/mssql_table_entry.cpp`) MUST NOT throw "Column with name X already exists!" for any partitioned table after the fix.

**Row-count accuracy**:

- **FR-004**: `TABLE_DISCOVERY_SQL_TEMPLATE` MUST report `approx_rows` equal to `SUM(p.rows)` across all heap (`index_id = 0`) and clustered-index (`index_id = 1`) partitions of the object. For non-partitioned tables this equals the single partition's row count (no regression).
- **FR-005**: The `SINGLE_TABLE_METADATA_SQL_TEMPLATE` and `BULK_METADATA_SCHEMA_SQL_TEMPLATE` queries MUST report the same aggregated `approx_rows` value as FR-004 for the same object.

**Backward compatibility**:

- **FR-006**: For tables with exactly one heap-or-clustered partition (the 99% case — i.e., every non-partitioned user table), the reported `approx_rows` value MUST be bit-for-bit identical to the pre-fix value. No existing integration test may begin to fail because of a different cardinality estimate.
- **FR-007**: The fix MUST work on SQL Server 2016 and later. `sys.partitions` and `SUM()` in a derived table have been available since SQL Server 2000; the chosen approach must not depend on any 2019+ feature (since the extension supports older versions for ad-hoc connections).
- **FR-008**: The fix MUST work on SQL Server 2017 and later, on Azure SQL Database, on Azure SQL Managed Instance, and on SQL Server on Linux. No platform-specific SQL is introduced.
- **FR-009**: The `index_id IN (0, 1)` filter MUST be preserved. Including additional `index_id` values would count rows once per non-clustered index per partition and inflate row counts.

**Regression coverage**:

- **FR-010**: A new integration test MUST create a SQL Server table with a partitioned clustered index (≥3 partitions, ≥2 columns), insert rows into multiple partitions, then verify: (a) `SELECT *` succeeds, (b) `DESCRIBE` returns each column exactly once, (c) `duckdb_tables().estimated_size` equals the aggregated row count.
- **FR-011**: The docker init SQL MUST set up the regression-test partitioned table so the test runs against `make integration-test` without per-test setup. Setup may live in a dedicated init file (loaded after `init.sql`) so the partition-function dependency on `tempdb`/`PRIMARY` filegroup is isolated.
- **FR-012**: A C++ unit test SHOULD assert that the generated SQL for the three templates contains a `GROUP BY object_id` (or equivalent aggregation marker), so a future refactor that drops aggregation breaks at unit-test time rather than at integration-test time.

**Cross-cutting coverage (US4)**:

- **FR-013**: The integration test suite MUST include a filter-pushdown smoke test against the partitioned-table fixture verifying `WHERE id < <boundary>` returns only rows in the matching partition.
- **FR-014**: The integration test suite MUST include a basic INSERT smoke test against the partitioned table that confirms a new row lands and is visible on a subsequent SELECT.
- **FR-015**: The docker init fixture MUST also provide a partitioned table **with** a primary key (`dbo.PartitionedWithPK`) so UPDATE/DELETE/rowid coverage can be added without changing the no-PK `dbo.PartitionedLog` table (which exists to mirror issue #85's exact shape).
- **FR-016**: The integration test suite MUST include UPDATE and DELETE smoke tests against `dbo.PartitionedWithPK` to confirm the rowid path (spec 001) still binds correctly when the table is partitioned.
- **FR-017**: The integration test suite MUST include a `COPY TO` smoke test against the partitioned table to confirm `src/copy/target_resolver.cpp`'s catalog-binding step works for partitioned targets.

### Key Entities

- **`sys.partitions`**: SQL Server DMV. One row per `(object_id, index_id, partition_number)`. A non-partitioned heap or clustered-index table has one row per index. A partitioned table has N rows per index (one per partition). The `rows` column is the per-partition row count (BIGINT, updated by statistics maintenance — approximate, not exact).
- **`SINGLE_TABLE_METADATA_SQL_TEMPLATE`** (`src/catalog/mssql_metadata_cache.cpp:57-75`): Loads object type, row count, and all columns for one table in a single round trip. Driver path for `MSSQLTableSet::LoadSingleEntry` (every `SELECT ... FROM <fresh table>`).
- **`BULK_METADATA_SCHEMA_SQL_TEMPLATE`** (`src/catalog/mssql_metadata_cache.cpp:79-105`): Loads all tables + columns for one schema in one query. Driver path for `MSSQLTableSet::Scan` and `mssql_preload_catalog`.
- **`TABLE_DISCOVERY_SQL_TEMPLATE`** (`src/catalog/mssql_metadata_cache.cpp:44-53`): Lists table names + row counts (no columns). Driver path for `MSSQLMetadataCache::EnsureTablesLoaded`. Does not crash today (`std::map::emplace` dedups by name) but silently reports a single-partition row count.
- **`MakeTableInfo`** (`src/catalog/mssql_table_entry.cpp:40-51`): Builds a `CreateTableInfo` from `MSSQLTableMetadata.columns`. Calls `info.columns.AddColumn(...)`, which throws if a column name repeats. This is where the user-visible error surfaces.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The query `SELECT * FROM attached_db.dbo.<partitioned_table>` succeeds against a SQL Server table with 4+ partitions. Before the fix: throws `Catalog Error: Column with name <col> already exists!`. After the fix: returns rows.
- **SC-002**: `duckdb_tables().estimated_size` for a partitioned table equals `SELECT SUM(rows) FROM sys.partitions WHERE object_id = OBJECT_ID('<table>') AND index_id IN (0, 1)`. Before the fix: equals the rows column of the first emitted partition row (non-deterministic).
- **SC-003**: `SELECT mssql_preload_catalog('ctx')` succeeds against a database containing partitioned tables; reported `column_count` equals the actual unique column count (no inflation).
- **SC-004**: All existing integration tests in `test/sql/catalog/`, `test/sql/integration/`, and the rest of `test/sql/` pass unchanged. No row-count regressions on any non-partitioned table.
- **SC-005**: The new regression test (`test/sql/catalog/partitioned_table.test`) passes against the dockerized SQL Server.
- **SC-006**: The fix is contained to `src/catalog/mssql_metadata_cache.cpp` (SQL template strings only). No header changes, no API changes, no new dependencies. (US5 — SQL injection hardening — is explicitly out of scope and tracked as a follow-up.)
- **SC-007**: Cross-cutting smoke tests added in US4 pass: filter pushdown on partition key, INSERT, UPDATE/DELETE on `PartitionedWithPK`, and `COPY TO` against a partitioned target. Each test demonstrates that the catalog fix did not break any consumer that walks catalog metadata.

## Security Notes

- **No new SQL injection surface introduced by this fix.** The three modified templates substitute the same `schema_name`/`table_name` values via `StringUtil::Format` as before. The fix adds *literal* SQL (no new `%s` substitutions, no new identifiers concatenated into queries).
- **Pre-existing finding (deferred to US5/follow-up)**: `mssql_metadata_cache.cpp` does not escape `'` in `schema_name`/`table_name` before substitution, in contrast to `mssql_statistics.cpp::FetchRowCount` which does. This is reachable only through SQL Server-side identifiers that contain literal quotes — unusual but legal. The crash fix in issue #85 must not be blocked on the hardening work; both can ship independently.
- **No expanded permission requirement.** `sys.partitions` is readable by any principal with `VIEW DEFINITION` (or its membership in `db_owner`, `db_datareader`, etc.) — identical to today's requirement.
- **No information disclosure expansion.** The fix changes only the *aggregation* shape of metadata results, not which rows are visible. Same objects, same columns.

## Assumptions

- The extension's existing approach of treating `approx_row_count` as an estimate (not an exact count) remains acceptable. `sys.partitions.rows` is updated by statistics maintenance and may lag the true count; this is unchanged behaviour and accepted by all upstream callers.
- SQL Server's query optimizer handles the derived-table aggregation efficiently. For each `sys.objects` row the optimizer can use a streaming aggregate over `sys.partitions` keyed on `object_id`; the cost is `O(total_partitions)` per query, which is the same as the current query's cost (the current query also scans every partition row, just without aggregating).
- The fix does not need a new `sys.dm_db_partition_stats` path. That DMV has the same per-partition granularity and would require the same aggregation; using `sys.partitions` keeps the change minimal and avoids introducing a new dependency on dynamic management views (which have stricter permission requirements in some hardened environments).
- Users running `mssql_refresh_cache()` after the upgrade will pick up the corrected row counts on the next access. No migration path is required since the cache is in-memory only.

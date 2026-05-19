# Implementation Plan: Fix Partitioned Table Catalog

**Branch**: `049-fix-partitioned-table-catalog` | **Date**: 2026-05-19 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/049-fix-partitioned-table-catalog/spec.md`

## Summary

Replace the per-partition `LEFT JOIN sys.partitions` clause in the three metadata-cache SQL templates with a pre-aggregated derived table. Add a docker init fixture for a partitioned table and a SQLLogicTest regression test (`test/sql/catalog/partitioned_table.test`). Optional C++ assertion test on the templates to guard the aggregation marker.

Single hot point of change: `src/catalog/mssql_metadata_cache.cpp` — three `static const char *` SQL templates at lines 44, 57, 79.

## Technical Context

**Language/Version**: C++17 (C++11-compatible for ODR on Linux)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (vcpkg) — none added by this spec
**Storage**: In-memory metadata cache (`MSSQLMetadataCache`). No persistence.
**Testing**: SQLLogicTest integration tests (require SQL Server via `make docker-up`), C++ unit tests (Catch2, no SQL Server needed)
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW)
**Project Type**: DuckDB extension (single shared library)
**Performance Goals**: Metadata-query wall-clock unchanged (the optimizer's plan changes from scan-then-flatten to scan-then-aggregate, both O(partitions)). No regression expected on the 99% non-partitioned case.
**Constraints**: Must use `GEN=ninja` for builds; no new vcpkg deps; SQL must work on SQL Server 2016+ including Azure SQL DB / MI / SQL Server on Linux; ODR-safe (no C++17-only headers)
**Scale/Scope**: 3 SQL template edits, 1 docker init file, 1 SQLLogicTest file, 1 optional unit test. Net diff ≈ 60 lines incl. tests.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
| --------- | :----: | ----- |
| I. Native and Open | PASS | Pure SQL template fix; no new libraries; uses standard `sys.partitions` documented since SQL Server 2005 |
| II. Streaming First | PASS | No buffering changes. The aggregation runs server-side; result-row streaming through `MSSQLSimpleQuery::ExecuteWithCallback` is unchanged. |
| III. Correctness over Convenience | PASS | Fixes a hard crash for partitioned tables AND a silent cardinality bug. No correctness trade-off introduced. |
| IV. Explicit State Machines | PASS | No state-machine changes. Metadata cache state transitions (`NOT_LOADED → LOADING → LOADED`) untouched. |
| V. DuckDB-Native UX | PASS | Restores ability to use DuckDB's standard catalog operators (`SELECT *`, `DESCRIBE`, `duckdb_tables`) against partitioned SQL Server tables. |
| VI. Incremental Delivery | PASS | Three queries change independently; the spec ships as a single PR but each query change is independently testable. |

No violations. No complexity tracking needed.

## Project Structure

### Documentation (this feature)

```text
specs/049-fix-partitioned-table-catalog/
├── spec.md                         # Feature specification
├── plan.md                         # This file
├── research.md                     # Phase 0 — why aggregate vs. DISTINCT vs. DMV alternatives
├── data-model.md                   # SQL shape: before vs. after, sys.partitions semantics
├── quickstart.md                   # Drop-in SQL diff + repro steps
├── checklists/
│   └── requirements.md             # Spec quality checklist
├── contracts/
│   └── metadata-query-contract.md  # Output-row shape contract for the three templates
└── tasks.md                        # Phase 2 output
```

### Source Code (files to modify)

```text
src/catalog/mssql_metadata_cache.cpp
    # Edit 3 static SQL templates (TABLE_DISCOVERY, SINGLE_TABLE_METADATA, BULK_METADATA_SCHEMA)
    # Replace `LEFT JOIN sys.partitions p ON ... AND index_id IN (0,1)`
    # with `LEFT JOIN (SELECT object_id, SUM(rows) AS rows FROM sys.partitions WHERE index_id IN (0,1) GROUP BY object_id) p ON p.object_id = o.object_id`

docker/init/init-partitioned-tables.sql       # New: partition function/scheme + Logs table + seed rows
docker/docker-compose.yml                      # Mount the new init file (if not auto-loaded by /init/)
test/sql/catalog/partitioned_table.test        # New: SQLLogicTest regression coverage
test/cpp/test_metadata_cache_queries.cpp       # Optional: unit test asserting GROUP BY appears in each template
```

No header changes. No CMakeLists changes unless the C++ unit test is added (in which case append the new source to the unittest target).

**Structure Decision**: Pure data-fix scoped to three SQL string literals in one file. No new classes, no API surface change, no migration. The optional C++ unit test exists only to make future refactors fail loudly if the aggregation is removed.

### Review Findings Incorporated

Three findings from the spec/security/cross-cutting review were folded into the spec before this plan was finalised:

1. **Statistics provider already correct** — `src/catalog/mssql_statistics.cpp:14-22` uses `ISNULL(SUM(p.rows), 0)` on `sys.dm_db_partition_stats`. The fix brings the metadata cache into alignment with code that already exists; risk of follow-on inconsistency is low.
2. **Pre-existing SQL-injection finding (deferred)** — `mssql_metadata_cache.cpp` does not escape `'` in substituted identifiers, unlike `mssql_statistics.cpp::FetchRowCount`. Documented as US5 in spec.md with explicit "out of scope for this PR" note. A separate hardening PR should add the same `'`→`''` escape across `MSSQLMetadataCache` callers.
3. **Cross-cutting test gaps** — issue #85 reports a read failure but the fix path is the foundation of every DML/COPY/CTAS/filter operation. US4 added with smoke tests for filter pushdown, INSERT, UPDATE/DELETE (with a `PartitionedWithPK` fixture), and COPY. These are new tasks T009a-d.

## Phase 0 — Research (see research.md)

Three candidate fix shapes evaluated:

1. **Derived-table aggregation** (chosen) — `LEFT JOIN (SELECT object_id, SUM(rows) AS rows FROM sys.partitions WHERE index_id IN (0,1) GROUP BY object_id) p`. Minimal change, correct, portable. Selected.
2. **`OUTER APPLY` with scalar aggregate** — works, but adds a correlated-subquery shape that some older SQL Server versions don't optimize as cleanly. Rejected.
3. **`sys.dm_db_partition_stats` instead of `sys.partitions`** — has the same per-partition granularity; switching DMV doesn't fix the problem and introduces stricter permission requirements (`VIEW DATABASE STATE` is sometimes denied to read-only ETL accounts). Rejected.

Open questions resolved during spec authoring (no NEEDS CLARIFICATION items remain in spec.md).

## Phase 1 — Design (see contracts/ and data-model.md)

**Query-output contract** (`contracts/metadata-query-contract.md`): all three templates emit exactly one row per `(object, column)` or `(object)` tuple; `approx_rows` equals `COALESCE(SUM(p.rows), 0)` across partitions where `index_id IN (0,1)`.

**Data model** (`data-model.md`): documents the before/after SQL shape, the `sys.partitions` row multiplication rule, and worked examples for a 4-partition log table.

**Test fixture** (`docker/init/init-partitioned-tables.sql`): creates a partition function on `DATETIME2(7)` with 3 boundary values (→ 4 partitions), a partition scheme aliasing all partitions to `[PRIMARY]`, a `dbo.PartitionedLog` table with a non-PK clustered partitioned index, and seed rows distributed across the partitions (with known per-partition counts so the test can assert `SUM = expected_total`).

**Integration test** (`test/sql/catalog/partitioned_table.test`):

```sql
require mssql
require-env MSSQL_TESTDB_DSN
statement ok
ATTACH '${MSSQL_TESTDB_DSN}' AS testdb_part (TYPE mssql);

# US1: column metadata does not duplicate
query II
SELECT column_name, data_type FROM (DESCRIBE testdb_part.dbo.PartitionedLog) ORDER BY column_name;
----
... expected one row per column ...

# US1: scan succeeds
query I
SELECT COUNT(*) FROM testdb_part.dbo.PartitionedLog;
----
<expected total>

# US2: cardinality estimate equals sum across partitions
query I
SELECT estimated_size FROM duckdb_tables() WHERE database_name = 'testdb_part' AND table_name = 'PartitionedLog';
----
<expected total>

# US3: preload handles partitioned tables
statement ok
SELECT mssql_preload_catalog('testdb_part');

statement ok
DETACH testdb_part;
```

## Constitution Re-Check (Post Phase 1 Design)

| Principle | Status | Notes |
| --------- | :----: | ----- |
| I. Native and Open | PASS | Still SQL-only, no deps added |
| II. Streaming First | PASS | Server aggregates; client streaming pattern unchanged |
| III. Correctness over Convenience | PASS | Restores correctness for two distinct bug classes (crash + silent miscount) |
| IV. Explicit State Machines | PASS | No state-machine changes |
| V. DuckDB-Native UX | PASS | Restores standard catalog UX |
| VI. Incremental Delivery | PASS | Three SQL edits + test, all in one PR; each query individually testable |

All gates pass post-design.

## Implementation Tasks Overview

### Task T001: Fix `TABLE_DISCOVERY_SQL_TEMPLATE` (P2, ~2 min)

Replace the `LEFT JOIN sys.partitions p ON o.object_id = p.object_id AND p.index_id IN (0, 1)` at line ~50 with the pre-aggregated derived table form. See `contracts/metadata-query-contract.md` for the exact SQL.

### Task T002: Fix `SINGLE_TABLE_METADATA_SQL_TEMPLATE` (P1, ~2 min)

Same replacement at line ~72. This is the MVP fix — it unblocks `SELECT * FROM <partitioned_table>`.

### Task T003: Fix `BULK_METADATA_SCHEMA_SQL_TEMPLATE` (P1, ~2 min)

Same replacement at line ~97. Required for `mssql_preload_catalog()` and `MSSQLTableSet::Scan` to handle partitioned tables in any schema.

### Task T004: Add docker init fixture (P1, ~10 min)

Create `docker/init/init-partitioned-tables.sql` with a partition function, partition scheme, `dbo.PartitionedLog` table, and seed rows distributed across ≥3 partitions. Wire it into the compose init order (after `init.sql`).

### Task T005: Add integration test (P1, ~15 min)

Create `test/sql/catalog/partitioned_table.test` per the design above. Cover US1 (scan succeeds, columns not duplicated), US2 (cardinality is the sum), and US3 (`mssql_preload_catalog` succeeds).

### Task T006: Add C++ unit-test guard (P3, ~10 min)

Optional but recommended. New `test/cpp/test_metadata_cache_queries.cpp` asserts that each of the three SQL template strings contains `GROUP BY` and `SUM(rows)` substrings. This catches future refactors that drop the aggregation without needing to spin up SQL Server.

### Task T007: Build & full test suite (P1, ~5 min)

- `GEN=ninja make` — verify clean compile.
- `./build/release/test/unittest` — verify C++ tests pass (including T006 if added).
- `make docker-down && make docker-up` — rebuild container so T004 fixture is loaded.
- `make integration-test` — verify all existing integration tests still pass + new test passes.

### Task T008: Update CLAUDE.md / docs (P3, ~5 min)

Add a line to `CLAUDE.md` under "Recent Changes" noting that spec 049 fixes partitioned-table catalog support. No user-facing documentation change required — the bug fix restores expected behaviour rather than adding a feature.

## Risk & Rollback

- **Rollback**: Reverting the commit restores prior behaviour exactly. The SQL templates are pure data; no schema migration, no on-disk format change.
- **Performance risk**: For schemas with many partitioned tables and many partitions per table, server-side aggregation cost is `O(total_partitions_in_schema)`. The pre-fix query scans the same rows; the aggregation adds a hash/stream-aggregate node but the working set is identical. Expected delta: well under 5% on typical schemas.
- **Permission risk**: `sys.partitions` is readable by any user with `VIEW DEFINITION` on the schema, same as today. The fix changes the SQL shape but not the underlying objects queried — no new permission requirement.
- **Edge-case risk**: A table with no rows in `sys.partitions` (e.g., a freshly-`CREATE`d table before any insert + before statistics maintenance) still benefits from the `ISNULL(p.rows, 0)` wrapper. Verified in spec §Edge Cases.

## Artifacts Generated

| Artifact   | Path                                                       | Status |
| ---------- | ---------------------------------------------------------- | :----: |
| Spec       | specs/049-fix-partitioned-table-catalog/spec.md            | Complete |
| Plan       | specs/049-fix-partitioned-table-catalog/plan.md            | Complete |
| Research   | specs/049-fix-partitioned-table-catalog/research.md        | Complete |
| Data Model | specs/049-fix-partitioned-table-catalog/data-model.md      | Complete |
| Contract   | specs/049-fix-partitioned-table-catalog/contracts/metadata-query-contract.md | Complete |
| Quickstart | specs/049-fix-partitioned-table-catalog/quickstart.md      | Complete |
| Checklist  | specs/049-fix-partitioned-table-catalog/checklists/requirements.md | Complete |
| Tasks      | specs/049-fix-partitioned-table-catalog/tasks.md           | Complete |

# Tasks: Fix Partitioned Table Catalog

**Input**: Design documents from `/specs/049-fix-partitioned-table-catalog/`
**Prerequisites**: plan.md (required), spec.md (required), research.md, data-model.md, contracts/

**Tests**: Integration tests are part of the deliverable — without a regression test the bug can be re-introduced silently. The C++ unit-test guard (T009) is optional but recommended.

**Organization**: Tasks are grouped by user story to enable independent implementation. All three SQL fixes (T002-T004) are independent in the sense that they edit different `static const char *` literals, but they must all ship together (see plan.md Decision 2).

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files or independent template strings, no dependencies between them)
- **[Story]**: Which user story this task belongs to (US1, US2, US3, or shared)

## Path Conventions

- **Source**: `src/catalog/mssql_metadata_cache.cpp` (sole production file changed)
- **Test fixtures**: `docker/init/init-partitioned-tables.sql` (new), `docker/docker-compose.yml` (mount/order)
- **Integration tests**: `test/sql/catalog/partitioned_table.test` (new, SQLLogicTest format, requires SQL Server)
- **Unit tests**: `test/cpp/test_metadata_cache_queries.cpp` (optional, Catch2, no SQL Server needed)

---

## Phase 1: Setup

No setup required. Branch `task/issue85-cc4234` (mapped to spec branch `049-fix-partitioned-table-catalog`) is already checked out as a worktree.

---

## Phase 2: Foundational (Test Fixture)

**Purpose**: Stand up a deterministic partitioned table in the dockerized SQL Server so the integration test has known row counts to assert against. This must be complete before T006-T008 can run.

- [ ] **T001** [Shared] Create `docker/init/init-partitioned-tables.sql`:
   - `USE TestDB` and idempotency guard (`IF NOT EXISTS` for partition function and scheme).
   - `CREATE PARTITION FUNCTION [pf_test_by_id] (INT) AS RANGE RIGHT FOR VALUES (100, 200, 300);` → 4 partitions: (-∞,100), [100,200), [200,300), [300,+∞).
   - `CREATE PARTITION SCHEME [ps_test_by_id] AS PARTITION [pf_test_by_id] ALL TO ([PRIMARY]);`
   - `CREATE TABLE dbo.PartitionedLog (id INT NOT NULL, log_ts DATETIME2(3) NOT NULL DEFAULT SYSUTCDATETIME(), message NVARCHAR(255) NULL);` — no PK, mirrors the issue's `dbo.log` shape.
   - `CREATE CLUSTERED INDEX ix_PartitionedLog_id ON dbo.PartitionedLog (id) ON [ps_test_by_id] (id);`
   - Seed rows with known per-partition counts: 10 rows with id < 100, 20 rows with id ∈ [100,200), 30 with id ∈ [200,300), 40 with id ≥ 300. Total = 100. Plus a non-partitioned control table `dbo.NonPartitionedControl` (a clone schema without partitioning) seeded with the same 100 rows for FR-006 regression coverage.
   - **For US4 cross-cutting coverage**: also create `dbo.PartitionedWithPK` with a primary key on `id`, partitioned identically (`ON [ps_test_by_id] (id)`), seeded with the same 100 rows. This table enables UPDATE/DELETE/rowid testing without disturbing the no-PK `dbo.PartitionedLog` fixture.

- [ ] **T002** [Shared] Wire the new init file into the container. If `docker-compose.yml` already mounts `./docker/init` and SQL Server's init runs all `.sql` files in alphabetical order, simply ensure the new filename sorts *after* `init.sql` (e.g., name it `init-partitioned-tables.sql` so it loads after `init.sql`). Otherwise, add an explicit mount/command sequence to the compose file. Verify with `make docker-down && make docker-up && docker compose -f docker/docker-compose.yml exec mssql /opt/mssql-tools/bin/sqlcmd -S localhost -U sa -P TestPassword1 -d TestDB -Q "SELECT COUNT(*) FROM dbo.PartitionedLog"` reports 100.

**Checkpoint**: `dbo.PartitionedLog` queryable via `sqlcmd` with row count 100; `SELECT $PARTITION.pf_test_by_id(id), COUNT(*) FROM dbo.PartitionedLog GROUP BY $PARTITION.pf_test_by_id(id)` returns 4 groups with counts 10/20/30/40.

---

## Phase 3: User Story 1 — Scan a partitioned table (Priority: P1) MVP

**Goal**: `SELECT * FROM attached.dbo.PartitionedLog` succeeds. The "Column with name X already exists!" error from issue #85 is gone.

**Independent Test**: After T003, running the new `partitioned_table.test` should pass at least the column-uniqueness and scan-succeeds assertions.

### Implementation for User Story 1

- [ ] **T003** [US1] Fix `SINGLE_TABLE_METADATA_SQL_TEMPLATE` in `src/catalog/mssql_metadata_cache.cpp` (line ~57-75). Replace the line `LEFT JOIN sys.partitions p ON o.object_id = p.object_id AND p.index_id IN (0, 1)` with the derived-table form from `contracts/metadata-query-contract.md`. This is the smallest possible fix that unblocks the issue.

- [ ] **T004** [P] [US3] Fix `BULK_METADATA_SCHEMA_SQL_TEMPLATE` in the same file (line ~79-105). Same replacement. Required so `mssql_preload_catalog` and `MSSQLTableSet::Scan` work on partitioned tables.

- [ ] **T005** [P] [US2] Fix `TABLE_DISCOVERY_SQL_TEMPLATE` in the same file (line ~44-53). Same replacement. Required so `approx_rows` reported for partitioned tables equals the true total.

**Note**: T003-T005 edit different template strings in the same file. They are listed as [P] because they're logically independent edits (separate string literals); in practice, do all three in one edit pass since they share the same pattern.

- [ ] **T006** [US1] Build: `GEN=ninja make`. Verify no compile errors or warnings in `mssql_metadata_cache.cpp`.

- [ ] **T007** [US1] Run existing C++ unit tests: `./build/release/test/unittest`. Verify no regressions (none expected — change is SQL-only).

**Checkpoint**: Build green. Existing unit tests pass. Without integration tests we cannot yet prove the fix, so the next phase is mandatory for shipping.

---

## Phase 4: Integration Test (Priority: P1)

**Goal**: Regression test that fails on a future commit which re-introduces the per-partition multiplication.

**Independent Test**: New test file passes against the dockerized SQL Server with the T001-T002 fixture loaded.

- [ ] **T008** [US1, US2, US3] Create `test/sql/catalog/partitioned_table.test`. Follow the pattern in `test/sql/catalog/datetimeoffset.test` for `require mssql` / `require-env MSSQL_TESTDB_DSN` / `ATTACH ... AS testdb_part` / `DETACH` boilerplate. Cover:
   - **US1.1**: `query II SELECT column_name, data_type FROM (DESCRIBE testdb_part.dbo.PartitionedLog) ORDER BY column_name;` — verify 3 columns appear once each.
   - **US1.2**: `query I SELECT COUNT(*) FROM testdb_part.dbo.PartitionedLog;` — expect 100.
   - **US1.3**: `query II SELECT id, message FROM testdb_part.dbo.PartitionedLog WHERE id = 5;` — verify projection pushdown emits each column once (no duplicate-column SQL error).
   - **US2.1**: `query I SELECT estimated_size FROM duckdb_tables() WHERE database_name = 'testdb_part' AND table_name = 'PartitionedLog';` — expect 100 (sum across all partitions, not 10/20/30/40 from any single partition).
   - **US2.2 / FR-006**: same query against `NonPartitionedControl` — expect 100 (regression check that the fix doesn't change non-partitioned cardinalities).
   - **US3.1**: `statement ok SELECT mssql_preload_catalog('testdb_part');` — preload must succeed in presence of partitioned tables.
   - **US3.2**: Re-run `SELECT COUNT(*) FROM testdb_part.dbo.PartitionedLog;` after preload — must still return 100 (no cache corruption).

- [ ] **T009** [US1] Run integration tests: `make docker-up && make integration-test`. Verify the new test passes AND the rest of `test/sql/` still passes (no regressions on non-partitioned tables).

**Checkpoint**: Issue #85 is resolved. All existing integration tests still pass. New regression test is now in CI.

---

## Phase 4b: Cross-cutting smoke tests (Priority: P2, US4)

**Goal**: Verify the catalog fix does not silently regress filter pushdown, DML, or COPY against partitioned tables.

**Independent Test**: Each `[P]` task below adds an isolated assertion block to a new test file. None require code changes beyond the fixtures in T001-T002.

- [ ] **T009a** [P] [US4] Add `test/sql/catalog/partitioned_filter_pushdown.test`:
   - ATTACH the test DB.
   - `query I SELECT COUNT(*) FROM testdb_part.dbo.PartitionedLog WHERE id < 100;` → expect 10 (partition 1 only).
   - `query I SELECT COUNT(*) FROM testdb_part.dbo.PartitionedLog WHERE id >= 100 AND id < 200;` → expect 20 (partition 2 only).
   - `query I SELECT COUNT(*) FROM testdb_part.dbo.PartitionedLog WHERE id >= 300;` → expect 40 (partition 4 only).
   - Optional: `EXPLAIN SELECT ... WHERE id < 100;` → assert the pushed-down T-SQL contains the predicate (via `query II` and a `LIKE '%WHERE%id%<%100%'` check on the explain output).
   - DETACH.

- [ ] **T009b** [P] [US4] Add `test/sql/insert/insert_partitioned.test` (or extend `insert_basic.test`):
   - ATTACH the test DB.
   - `statement ok INSERT INTO testdb_part.dbo.PartitionedLog (id, log_ts, message) VALUES (250, '2026-05-19T12:00:00', 'inserted');`
   - `query I SELECT COUNT(*) FROM testdb_part.dbo.PartitionedLog WHERE id = 250;` → expect 1.
   - `statement ok SELECT mssql_refresh_cache('testdb_part');`
   - `query I SELECT estimated_size FROM duckdb_tables() WHERE database_name = 'testdb_part' AND table_name = 'PartitionedLog';` → expect 101 (post-refresh).
   - Cleanup: `statement ok DELETE FROM testdb_part.dbo.PartitionedLog WHERE id = 250;` and re-refresh.
   - DETACH.

- [ ] **T009c** [P] [US4] Add `test/sql/dml/dml_partitioned.test`:
   - ATTACH the test DB.
   - `statement ok UPDATE testdb_part.dbo.PartitionedWithPK SET message = 'updated' WHERE id = 50;`
   - `query T SELECT message FROM testdb_part.dbo.PartitionedWithPK WHERE id = 50;` → expect 'updated'.
   - `statement ok DELETE FROM testdb_part.dbo.PartitionedWithPK WHERE id = 60;`
   - `query I SELECT COUNT(*) FROM testdb_part.dbo.PartitionedWithPK WHERE id = 60;` → expect 0.
   - Cleanup (restore deleted row + revert message) for test isolation.
   - DETACH.

- [ ] **T009d** [P] [US4] Add `test/sql/copy/copy_partitioned.test`:
   - ATTACH the test DB.
   - `statement ok CREATE TABLE local_src AS SELECT 999 AS id, 'copied' AS message;`
   - `statement ok COPY local_src TO 'mssql:testdb_part.dbo.PartitionedLog' (FORMAT MSSQL, COLUMN_MAPPING 'id,message');` — or equivalent COPY syntax per `test/sql/copy/copy_basic.test` patterns.
   - `query T SELECT message FROM testdb_part.dbo.PartitionedLog WHERE id = 999;` → expect 'copied'.
   - Cleanup: `statement ok DELETE FROM testdb_part.dbo.PartitionedLog WHERE id = 999;`
   - DETACH.

**Checkpoint**: All four cross-cutting smoke tests pass. The catalog fix is verified to not regress filter pushdown, INSERT, UPDATE/DELETE, or COPY against partitioned tables.

---

## Phase 5: Unit-Test Guard (Priority: P3, optional)

**Goal**: Catch a future refactor that drops the `GROUP BY` aggregation without needing to spin up SQL Server.

- [ ] **T010** [P3] Create `test/cpp/test_metadata_cache_queries.cpp`. For each of the three template strings (`TABLE_DISCOVERY_SQL_TEMPLATE`, `SINGLE_TABLE_METADATA_SQL_TEMPLATE`, `BULK_METADATA_SCHEMA_SQL_TEMPLATE`), assert the literal contains the substrings `"GROUP BY object_id"`, `"SUM(rows)"`, and `"index_id IN (0, 1)"`. Use Catch2's `REQUIRE` macros. The templates are `static` (file-local) so the test will need them exposed — either via a small helper accessor in `mssql_metadata_cache.cpp` or by moving the templates into the header file as `inline constexpr const char *` (C++17) — since the project is C++11-compatible, prefer the accessor approach (free function `const char *GetSingleTableMetadataSql();` etc., declared in the existing header).

- [ ] **T011** [P3] Add `test/cpp/test_metadata_cache_queries.cpp` to the unit-test target in `test/cpp/CMakeLists.txt`. Verify with `GEN=ninja make && ./build/release/test/unittest`.

**Checkpoint**: Unit-test guard runs in <1s as part of every CI build. A future refactor that drops aggregation fails at unit-test time.

---

## Phase 6: Polish & Documentation

- [ ] **T012** [Shared] Add a "Recent Changes" entry to `CLAUDE.md`:

   ```text
   - 049-fix-partitioned-table-catalog: Fix issue #85 — querying tables with partitioned clustered indexes
     failed with "Column with name X already exists!" because `LEFT JOIN sys.partitions` in three catalog
     metadata queries multiplied rows by partition count. Pre-aggregate via a derived table. Also fixes a
     silent miscount of cardinality estimates (was: arbitrary single-partition count; now: SUM across all
     partitions). New integration test: test/sql/catalog/partitioned_table.test.
   ```

- [ ] **T013** [Shared] Full integration suite: `make docker-down && make docker-up && make integration-test`. Final clean run end-to-end.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 (Setup)**: N/A.
- **Phase 2 (Foundational)**: T001 → T002 (sequential within phase). Both must complete before T008-T009 in Phase 4.
- **Phase 3 (US1 fix)**: T003-T005 can run in parallel (independent string literals). T006-T007 after all three.
- **Phase 4 (Integration test)**: T008 depends on T001-T005. T009 depends on T008 + T002 (docker fixture loaded).
- **Phase 5 (Unit test, optional)**: T010-T011 depend only on T003-T005 (the templates being changed).
- **Phase 6 (Polish)**: T012-T013 after everything else.

### User Story Dependencies

- **US1 (P1)**: T003 is the core fix. T006-T009 validate it. MVP path is T003 → T006 → T007 → T008 (US1 portions) → T009.
- **US2 (P2)**: T005 fixes the row-count miscount; T008 (US2 portions) validates it.
- **US3 (P2)**: T004 fixes the bulk preload path; T008 (US3 portions) validates it.

### Parallel Opportunities

- T003, T004, T005 — independent string-literal edits in one file (do in one editor pass).
- T010 (unit-test guard) is independent of all integration test work — can be developed in parallel with T008.

---

## Implementation Strategy

### MVP First (User Story 1 only)

1. T001 + T002: Stand up the partitioned-table fixture (the only blocker for proof of fix).
2. T003: One-line SQL fix to `SINGLE_TABLE_METADATA_SQL_TEMPLATE`.
3. T006: Build.
4. T007: Unit tests still pass.
5. T008 (US1 portions only) + T009: Integration test confirms issue #85 resolved.
6. **STOP and VALIDATE**: Issue #85 closed.

### Full Delivery (recommended)

1. **MVP**: T001 → T002 → T003 → T006 → T007 (US1 fix in place).
2. **Cardinality fix**: T005 → re-run T009 with US2 assertions enabled in T008.
3. **Preload fix**: T004 → re-run T009 with US3 assertions enabled in T008.
4. **Guard rail**: T010 → T011 (optional but cheap).
5. **Polish**: T012 → T013.

---

## Notes

- All three SQL template fixes (T003-T005) are mechanical string replacements following the exact pattern in `contracts/metadata-query-contract.md`. Total edited LOC ≈ 12 (3 templates × 1 line replaced with 4 lines).
- T001's `dbo.PartitionedLog` uses `INT` partition key rather than `DATETIME2` for fixture simplicity; the fix is partition-key-type-agnostic so this doesn't reduce coverage of the reported bug.
- T010 requires either exposing the static templates via an accessor or moving them to a header. Prefer the accessor (preserves the templates' internal-linkage benefits).
- T012's CLAUDE.md entry should be added under the existing `## Recent Changes` section, above the spec 044 entry, in reverse-chronological order.
- The fix touches no Windows-specific code and no platform abstraction. No CI matrix changes needed beyond the existing one.

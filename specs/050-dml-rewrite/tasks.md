---

description: "Task list for spec 050 DML Rewrite (UPDATE/DELETE + MERGE guard)"
---

# Tasks: DML Rewrite

**Input**: Design documents from `/specs/050-dml-rewrite/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md (all present)

**Tests**: SQL test files per User Story are required by the spec
(FR-001..FR-070, SC-001..SC-010). They are written alongside their
associated implementation tasks since the correctness contract is a
mix of pre-existing-behavior preservation (INSERT, PK-based DML) and
new behavior (physloc strategy, MERGE guard).

**Organization**: Tasks are grouped by user story to enable
independent implementation and review. US1 is the foundation;
US2–US4 depend on US1 but are otherwise independent of each other;
US5 has only the parser-rename dependency on US2/US3 (RETURNING
consumers).

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: [US1]..[US5]
- File paths are exact and absolute to the repository root

## Path Conventions

- Source under `src/`, headers under `src/include/` mirroring the
  `src/` layout
- Tests under `test/cpp/` (C++) and `test/sql/dml/` (SQLLogicTest)
- Integration benchmark under `test/bench/`

---

## Phase 1: Setup

**Purpose**: Repository preparation; no source code changes.

- [ ] T001 Verify spec 044 and spec 046 are merged on `main` (or, if 046 is not yet merged, record the throughput baseline assumption in `bench_results.md` so SC-004 can be evaluated post-hoc when 046 lands).
- [ ] T001a Verify `duckdb::Catalog::PlanMergeInto` exists as a `virtual` override point in the DuckDB version the extension's CMake fetches. If not, switch the US5 MERGE guard from a `PlanMergeInto` override to a bind-time intercept per research.md §R9b — same observable behavior, different hook site. Record the resolution in `bench_results.md` so the choice is visible to reviewers.
- [ ] T002 Create the new directory structure: `src/dml/delete/`, `src/dml/update/`, `src/dml/merge/`, `src/include/dml/delete/`, `src/include/dml/update/`, `src/include/dml/merge/`. (Existing `src/dml/` and `src/include/dml/` already exist.)
- [ ] T003 Verify pre-implementation build green on the current branch base: `GEN=ninja make`, `GEN=ninja make test`. Both MUST pass before any source-code task starts. Capture the baseline `bench_dml_throughput.sh` timing against v0.1.18 for later SC-004 comparison; store in `/tmp/spec050_baseline.txt`.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Connection-level engine-edition cache + the renamed
`MSSQLDmlReturningParser`. Every user story consumes one or both.

- [ ] T004 [US1, US5] Rename `src/dml/insert/mssql_returning_parser.{hpp,cpp}` → `src/dml/mssql_dml_returning_parser.{hpp,cpp}`. Move the header to `src/include/dml/mssql_dml_returning_parser.hpp`. Update the class name to `MSSQLDmlReturningParser`. Change the constructor to accept `(const vector<TdsColumnMetadata>& output_columns, const vector<idx_t>& returning_column_ids, …)` per data-model.md §E8.
- [ ] T005 [US1, US5] Update `src/dml/insert/mssql_insert.cpp` (and any other INSERT-path consumer) to construct `MSSQLDmlReturningParser` via the shim pattern in data-model.md §E8. INSERT external behavior MUST NOT change. Run the existing INSERT RETURNING SQL tests; all MUST stay green (SC-008).
- [ ] T006 [US1] Add `MSSQLEngineEdition` enum + `GetCachedEngineEdition(conn)` in `src/include/connection/mssql_engine_edition.hpp` and a small `.cpp` next to the existing connection provider. Lazy probe of `SELECT SERVERPROPERTY('EngineEdition')`, cached via `std::once_flag` on the connection state object (data-model.md §E3).
- [ ] T007 [US1] Add the `TableDmlCapabilities` struct in `src/include/dml/mssql_dml_capabilities.hpp` (data-model.md §E2). No implementation yet — just the struct.

**Checkpoint**: Foundation in place. Parser is renamed and routed via shim. Engine-edition probe exists but is unused. `TableDmlCapabilities` is declared. Build green, all pre-existing tests green.

---

## Phase 3: User Story 1 — Catalog enrichment with rowid-strategy detection (Priority: P1) 🎯 MVP

**Goal**: Per-table DML capabilities computed at metadata load and
exposed via `MSSQLTableEntry::GetDmlCapabilities()`. Engine edition
informs the physloc-availability decision.

**Independent Test**: `test/sql/dml/dml_capabilities.test` (7-shape
fixture, data-model.md §E12) plus the new `mssql_table_capabilities`
diagnostic table function.

### Implementation for User Story 1

- [ ] T008 [US1] Implement `ResolveRowIdStrategy(context, table)` in `src/dml/mssql_dml_capabilities.cpp` per research.md §R2. Throw `BinderException` with the documented messages on each error path.
- [ ] T009 [US1] Extend the existing per-table metadata fetch query in `src/catalog/mssql_table_entry.cpp` to add the `is_memory_optimized`, `is_external`, `has_primary_key`, and `has_columnstore` columns per research.md §R3.
- [ ] T010 [US1] Populate `TableDmlCapabilities` from the metadata fetch result. Apply the `physloc_unavailable_reason` priority rules from data-model.md §E2 (engine-level reasons win over table-level). Expose via `MSSQLTableEntry::GetDmlCapabilities()`.
- [ ] T011 [US1] Register the three new session settings (`mssql_rowid_strategy`, `mssql_direct_dml`, `mssql_dml_log_transactions`) per data-model.md §E9. Validation at consumption sites only.
- [ ] T012 [US1] Implement the `mssql_table_capabilities(name)` diagnostic table function (returns one row with `has_primary_key`, `supports_physloc`, `physloc_unavailable_reason`, `pk_column_names`, `engine_edition`).

### Tests for User Story 1

- [ ] T013 [P] [US1] Write `test/sql/dml/dml_capabilities.test` covering the 7-shape fixture (data-model.md §E12). For each shape, assert the expected capability flags via `mssql_table_capabilities()`. Use `mode skipif` for in-memory / external / Synapse shapes that aren't available in the standard Docker SQL Server.
- [ ] T014 [P] [US1] Write `test/cpp/test_resolve_rowid_strategy.cpp` covering all 9 (`auto`/`pk`/`physloc` × PK-only/physloc-only/neither) decision matrix entries from research.md §R2. Assert the exception message text exactly.

**Checkpoint**: User Story 1 complete. Capabilities are visible to users via the diagnostic function and ready to be consumed by US2–US4.

---

## Phase 4: User Story 2 — Direct UPDATE/DELETE pushdown (Priority: P1)

**Goal**: Simple plans (whitelist of `LogicalGet` / `LogicalFilter`
/ `LogicalProjection`) execute as a single round-trip
`UPDATE/DELETE … WHERE <pushdown>` statement, with optional
`OUTPUT INSERTED/DELETED.*` for RETURNING.

**Independent Test**: `test/sql/dml/direct_update_delete.test` —
asserts via TDS-trace capture that exactly one outbound SQL_BATCH
is emitted for the documented plan shapes.

### Implementation for User Story 2

- [ ] T015 [US2] Implement `IsSimpleTablePlan(op, target)` in `src/catalog/mssql_catalog.cpp` per spec.md §R9 + plan.md §3. Whitelist: `LogicalGet`, `LogicalFilter` (every predicate fully pushable per spec 044), `LogicalProjection` (every expression refers only to the target's own columns + constants).
- [ ] T016 [US2] Create `src/dml/delete/mssql_direct_delete.{hpp,cpp}` per data-model.md §E4a. Sink/Source: none. `Finalize` emits the templated `DELETE FROM target WHERE <filter> [OUTPUT DELETED.…]` statement, captures `@@ROWCOUNT`, streams OUTPUT rows via `MSSQLDmlReturningParser`.
- [ ] T017 [US2] Create `src/dml/update/mssql_direct_update.{hpp,cpp}` per data-model.md §E4a. Same pattern as delete but for `UPDATE target SET … WHERE <filter> [OUTPUT INSERTED.…]`. Reject SET expressions that reference other tables or subqueries (force fallback to staging).
- [ ] T018 [US2] Wire `MSSQLCatalog::PlanDelete` and `PlanUpdate` to dispatch to the direct operators when `IsSimpleTablePlan` returns true and `mssql_direct_dml = true`. Otherwise fall through to the staging path (US3).

### Tests for User Story 2

- [ ] T019 [P] [US2] Write `test/sql/dml/direct_update_delete.test`. Assertions: (a) single SQL_BATCH for `DELETE FROM mssql.dbo.t WHERE id = 5`; (b) single SQL_BATCH for the equivalent UPDATE; (c) `mssql_direct_dml = false` forces staging (verified by presence of `#upd_*` in `tempdb.sys.tables` during the operation); (d) UDF in WHERE forces staging; (e) RETURNING `*` works for both operators.
- [ ] T020 [US2] Update existing UPDATE/DELETE tests that previously asserted v0.1.x rowid-batched behavior; tests that asserted "VALUES with > 1000 rows triggers multi-statement" are deleted (no longer applicable).

**Checkpoint**: User Story 2 complete. Simple targeted DML runs at native latency. Plans that don't qualify still work via the legacy path (until US3 lands).

---

## Phase 5: User Story 3 — Staging-path UPDATE/DELETE (Priority: P1)

**Goal**: Non-simple plans go through the staging-table pipeline:
materialize rowids on scan, BCP-upload into `#upd_<uuid>`, execute
a single JOIN statement.

**Independent Test**: `test/sql/dml/staging_update_join.test` —
joins MSSQL target to a DuckDB-local table inside an UPDATE,
asserts correct rowcounts and zero leftover `#upd_*` tables after
success.

### Implementation for User Story 3

- [ ] T021 [US3] Create `src/dml/mssql_materialize_scans.{hpp,cpp}` per data-model.md §E7. Walks the plan tree, sets `emit_rowid`, `rowid_strategy`, `requires_materialization`, `max_threads = 1` on every MSSQL `PhysicalTableScan`.
- [ ] T022 [US3] Extend the MSSQL scan bind data with `emit_rowid` / `rowid_strategy` fields (data-model.md §E7). When `emit_rowid = true`, append the rowid column(s) to the SELECT list emitted by the scan: PK columns (PK strategy) or `%%physloc%% AS __physloc` (physloc strategy). Parse the returned column into the trailing chunk slot.
- [ ] T023 [US3] Create `src/dml/delete/mssql_staging_delete.{hpp,cpp}` per data-model.md §E4b. Sink: accumulate rowid chunks → spec 046 pipelined BCP. Finalize: emit the JOIN-based DELETE statement per data-model.md §E6. Drop the staging table on success. (Transaction wrap comes in US4.)
- [ ] T024 [US3] Create `src/dml/update/mssql_staging_update.{hpp,cpp}` per data-model.md §E4b. Same pattern as staging delete but for UPDATE; staging table additionally carries the updated columns.
- [ ] T025 [US3] Wire `MSSQLCatalog::PlanDelete` and `PlanUpdate` to dispatch to staging operators when the direct path is ineligible. Call `MaterializeMssqlScans(plan, strategy)` to set up the scan side before returning the operator.
- [ ] T026 [US3] Remove the legacy rowid-extractor + delete/update executor/statement/target files listed in data-model.md §E11. Confirm no remaining in-tree references.

### Tests for User Story 3

- [ ] T027 [P] [US3] Write `test/sql/dml/staging_update_join.test` — two variants per (PK / physloc) strategy: (a) UPDATE … FROM duckdb_local_table JOIN … ON …; (b) DELETE … WHERE id IN (SELECT id FROM duckdb_local_table). Assert rowcounts and that `#upd_*` is absent from `tempdb.sys.tables` after the operation.
- [ ] T028 [P] [US3] Write `test/sql/dml/staging_returning.test` — staging UPDATE and DELETE with `RETURNING *`; assert returned rows match the expected post-update / deleted values.

**Checkpoint**: User Story 3 complete. Heaps without PK can now be updated/deleted via physloc; non-simple plans against any table go through the BCP-fast staging path. Spec 050's main value prop is delivered.

---

## Phase 6: User Story 4 — Transaction wrap (Priority: P1)

**Goal**: Staging operators wrap their workflow in either a full
transaction (autocommit case) or a savepoint (user-txn case), per
research.md §R5. DTC and "doomed" transactions throw clear errors.

**Independent Test**: `test/sql/dml/dml_transactions.test` — three
cases: autocommit-failure, user-txn-failure, DTC error. Asserts
zero orphaned transactions afterward.

### Implementation for User Story 4

- [ ] T029 [US4] Add `ProbeTransactionState(connection)` helper that issues `SELECT @@TRANCOUNT, XACT_STATE()` in a single round-trip and returns both values.
- [ ] T030 [US4] At the Sink-phase start of each staging operator (US3's `MSSQLStagingDelete::Sink` and `MSSQLStagingUpdate::Sink`), call `ProbeTransactionState` on first chunk. Branch per research.md §R5: autocommit → `BEGIN TRAN` + `SET XACT_ABORT ON`; user-txn + `XACT_STATE() = 1` → `SAVE TRANSACTION mssql_dml_<uuid>`; `XACT_STATE() = 2` → `NotImplementedException`; `XACT_STATE() = -1` → `NotImplementedException`.
- [ ] T031 [US4] Generate the savepoint UUID per operator instance; truncate the UUID hex to 22 chars to fit the 32-char savepoint name limit (`mssql_dml_<22 hex>` total).
- [ ] T032 [US4] On `Finalize` success: explicit `DROP TABLE #upd_<uuid>` (data-model.md §R7 / FR-035), then `COMMIT TRAN` (autocommit) or savepoint-discard (user-txn — implicit; no statement needed). On any failure: `ROLLBACK TRAN` (autocommit) or `ROLLBACK TRANSACTION mssql_dml_<uuid>` (user-txn), then re-throw.
- [ ] T033 [US4] When `mssql_dml_log_transactions = true`, log each transaction boundary to the extension's debug channel with the operator instance ID + savepoint name + `@@TRANCOUNT` value.

### Tests for User Story 4

- [ ] T034 [P] [US4] Write `test/sql/dml/dml_transactions.test` covering: (a) autocommit-failure (forced via a deliberate type-mismatch in the UPDATE SET expression that fails server-side) → target unchanged, `tempdb.sys.tables` clean; (b) user-txn-failure → outer transaction still active, can be queried after the failure, savepoint name visible in SQL Server trace; (c) DTC error (`BEGIN DISTRIBUTED TRANSACTION` before the UPDATE) → `NotImplementedException` with the documented message.
- [ ] T035 [P] [US4] Write a sub-test that runs two staging UPDATEs inside the same user transaction and asserts via SQL Server trace that two distinct `mssql_dml_*` savepoint names were issued (SC-006).

**Checkpoint**: User Story 4 complete. The staging path is atomic in both autocommit and user-transaction modes. DTC and doomed transactions fail loudly. Spec 050 is now correctness-complete.

---

## Phase 7: User Story 5 — MERGE early-error guard (Priority: P2)

**Goal**: `MERGE INTO mssql.…` throws `NotImplementedException` at
plan time with the documented migration guidance. (RETURNING
generalization already done in Phase 2.)

**Independent Test**: `test/sql/dml/merge_disabled.test` asserts
the exception message contains both `INSERT INTO` and `UPDATE …
FROM` migration patterns.

### Implementation for User Story 5

- [ ] T036 [US5] Create `src/dml/merge/mssql_merge_guard.{hpp,cpp}` per data-model.md §E10. One function: `ThrowMssqlMergeNotImplemented(target_name)`.
- [ ] T037 [US5] Add `MSSQLCatalog::PlanMergeInto` override in `src/catalog/mssql_catalog.cpp`. Calls `ThrowMssqlMergeNotImplemented(table.name)`.

### Tests for User Story 5

- [ ] T038 [P] [US5] Write `test/sql/dml/merge_disabled.test`. Issue `MERGE INTO mssql.dbo.t USING (VALUES (1)) AS s(id) ON t.id = s.id WHEN NOT MATCHED THEN INSERT VALUES (1)` and assert the exception message contains "INSERT INTO", "UPDATE", "FROM", and the table name.
- [ ] T039 [US5] Delete or rewrite any pre-existing MERGE tests under `test/sql/dml/` that relied on the v0.1.x rowid-based MERGE behavior (FR-062, SC-010).

**Checkpoint**: User Story 5 complete. MERGE fails loudly with a clear migration path. Spec 050 is feature-complete.

---

## Phase 8: Stabilization

**Purpose**: Documentation, benchmark recording, final cleanup.

- [ ] T040 Update `README.md` per FR-070: document `mssql_rowid_strategy`, `mssql_direct_dml`, `mssql_dml_log_transactions`; document the `%%physloc%%` concurrency caveat; document the MERGE-temporarily-disabled notice with both migration patterns; document the tempdb sizing rule of thumb.
- [ ] T041 Run `test/bench/bench_dml_throughput.sh` on the spec-050 PR head against the Docker SQL Server. Compare to the v0.1.18 baseline captured in T003. Record both numbers in `specs/050-dml-rewrite/bench_results.md`. Assert SC-004's ≥ 5× speedup.
- [ ] T042 Run `tokei` on the spec-050 PR head vs `main`; assert net LOC change ≤ +800 (SC-009). Record in `bench_results.md`.
- [ ] T043 Final test sweep: `GEN=ninja make test` and `GEN=ninja make integration-test` both green. Re-verify SC-010 (every previously-green non-MERGE DML test stays green or is rewritten with a recorded rationale).
- [ ] T044 PR description: link to `specs/050-dml-rewrite/spec.md`, list every removed file, summarize the `bench_results.md` numbers, and call out the MERGE deferral with the migration patterns.

**Checkpoint**: Spec 050 ready for review and merge.

---

## Parallel-execution map

- Phase 1 (T001–T003): strictly sequential.
- Phase 2 (T004–T007): T004→T005 sequential (parser rename + shim); T006 and T007 independent; all three can finish before Phase 3 starts.
- Phase 3 (T008–T014): T008–T012 share files in `src/catalog/` and `src/dml/`; serialize within one contributor. T013 and T014 can be written in parallel with each other and with T008–T012.
- Phase 4 (T015–T020): same shared-file constraints; tests (T019, T020) parallel with implementation.
- Phase 5 (T021–T028): T021 and T022 are independent (different files); T023 and T024 share infrastructure but different files (delete vs update); T025 and T026 sequential after T021–T024. Tests T027 and T028 parallel.
- Phase 6 (T029–T035): T029 first (helper); T030–T033 share the staging operator files; tests T034/T035 parallel.
- Phase 7 (T036–T039): T036→T037 sequential; T038 and T039 parallel.
- Phase 8 (T040–T044): T040 (README) and T042 (tokei) parallel; T041 (benchmark) needs a stable build so runs after Phase 7 completes; T043/T044 last.

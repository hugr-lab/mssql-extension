---
description: "Task list for spec 047 — Process-Wide State Cleanup"
---

# Tasks: Process-Wide State Cleanup

**Input**: Design documents from `/specs/047-process-state-cleanup/`

**Prerequisites**: `plan.md` (required), `spec.md` (required), `research.md`, `data-model.md`, `contracts/README.md`, `quickstart.md` — all generated.

**Tests**: Test tasks are explicitly part of every user story (per FR-008, FR-009, and SC-001..SC-010). C++ + SQL regression tests are required for merge gate.

**Organization**: Tasks are grouped by user story so each story can be implemented, tested, and merged independently. US1 alone delivers the production-bug fix (issue #96 + Scenarios 1/2/3); the rest are stacked improvements.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies on incomplete tasks)
- **[Story]**: User story label (US1..US4); setup/foundational/polish phases have no story label
- File paths are exact and clickable

## Path Conventions

Single-project C++ extension layout:
- Source: `src/` (mirrored headers in `src/include/`)
- Tests: `test/cpp/` (C++) + `test/sql/` (sqllogictest)
- Specs: `specs/047-process-state-cleanup/`

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Baseline capture and branch sanity. Branch `047-process-state-cleanup` and full spec set already exist from the `/speckit-*` flow.

- [ ] T001 Verify branch and working tree state: `git status` on `047-process-state-cleanup` is clean (no uncommitted changes besides submodule diffs); `git log origin/main..HEAD --oneline` is empty (no commits yet).
- [ ] T002 Capture baseline bench at `main`-at-kickoff: run `GEN=ninja make release && MSSQL_BENCH_ROW_COUNT=1000000 MSSQL_BENCH_DUCKDB_BIN=$(pwd)/build/release/duckdb MSSQL_BENCH_OUTPUT=/tmp/bench_codec_e2e_spec047_baseline.txt bash test/bench/bench_codec_e2e.sh`; save to `specs/047-process-state-cleanup/bench_results_baseline.txt`.
- [ ] T003 Confirm the 3 production-bug repros fail on `main` (per `quickstart.md`): issue #96 Python loop, ATTACH wrong-password silent pass, 100-iteration silent-shutdown socket leak. Record observed failure modes in a scratch note for comparison post-implementation.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Cross-story infrastructure changes that must land before any user story can complete.

- [ ] T004 Add pin-counter to pool: in `src/include/tds/tds_connection_pool.hpp` add private `std::atomic<int64_t> pinned_count_{0}` member and public methods `IncrementPinned()`, `DecrementPinned()`, `GetPinnedCount() const noexcept`. In `src/tds/tds_connection_pool.cpp` implement via `fetch_add` / `fetch_sub` / `load` with `std::memory_order_relaxed`.
- [ ] T005 Extend `PoolStatistics` struct in `src/include/tds/tds_connection_pool.hpp` with `int64_t pinned_count` field. Update `tds::ConnectionPool::GetStats()` in `src/tds/tds_connection_pool.cpp` to populate it from `GetPinnedCount()`.
- [ ] T006 Verify build + existing tests stay green: `GEN=ninja make debug && GEN=ninja make test && GEN=ninja make integration-test`. New members have no callers yet — must not break anything.

---

## Phase 3: User Story 1 — Per-catalog pool ownership (Priority: P1) 🎯 MVP

**Goal**: `MSSQLCatalog` becomes the sole owner of its `tds::ConnectionPool` via `unique_ptr` member. Delete the `MssqlPoolManager` singleton and the `MSSQLContextManager` + `g_context_managers` global container. Pool construction moves inline into `MSSQLCatalog::Initialize`. All consumers route via `catalog.GetConnectionPool()`. Closes issue #96.

**Independent Test**: `test/cpp/test_multi_instance_pool_isolation.cpp` (Scenarios 1/2/3) and `test/cpp/test_issue_96_attach_loop.cpp` (Scenario 4) all pass. `make test && make integration-test` stay green. Verifies SC-001, SC-002, SC-003, SC-005, SC-009.

### Implementation for User Story 1

- [ ] T007 [US1] Change `MSSQLCatalog::connection_pool_` field type in `src/include/catalog/mssql_catalog.hpp:195` from `shared_ptr<tds::ConnectionPool>` to `unique_ptr<tds::ConnectionPool>`. Update `GetConnectionPool()` accessor at line 103 if needed (signature returns reference, should still compile).
- [ ] T008 [US1] Inline SQL-auth pool construction into `MSSQLCatalog::Initialize` in `src/catalog/mssql_catalog.cpp`. Move the body of `MssqlPoolManager::GetOrCreatePool(...)` (`src/connection/mssql_pool_manager.cpp:15-54`) into Initialize, dropping the singleton lookup + map insert + lock. Construct via `make_uniq<tds::ConnectionPool>(...)` and store in `connection_pool_`.
- [ ] T009 [US1] Inline Azure FEDAUTH pool construction into `Initialize` (same file). Move body of `MssqlPoolManager::GetOrCreatePoolWithAzureAuth(...)` (`src/connection/mssql_pool_manager.cpp:56-100`); preserve the token-capturing factory closure.
- [ ] T010 [US1] Inline Integrated-auth (Kerberos / SSPI) pool construction into `Initialize`. Move body of `MssqlPoolManager::GetOrCreatePoolWithIntegratedAuth(...)` (`src/connection/mssql_pool_manager.cpp:102-169`); preserve per-connection `AuthStrategyFactory::Create(info_copy)` invocation in the factory closure (spec 042 invariant — fresh authenticator per pool refill).
- [ ] T011 [US1] Add `auth_method` dispatch in `MSSQLCatalog::Initialize`: branch on `connection_info_->auth_method` between SQL-auth / Azure / Integrated; call the appropriate inlined branch from T008/T009/T010. Replace the existing `MssqlPoolManager::Instance().GetOrCreatePool*(...)` call at `src/catalog/mssql_catalog.cpp:82` with the inline construction.
- [ ] T012 [US1] Drop `MssqlPoolManager::Instance().RemovePool(...)` call in `MSSQLCatalog::OnDetach` at `src/catalog/mssql_catalog.cpp:630`. Pool teardown is now implicit via `~MSSQLCatalog` → `unique_ptr` destruction. Keep the Azure token-cache invalidation that's also in `OnDetach`.
- [ ] T013 [US1] Remove the 3 `MssqlPoolManager::Instance().GetOrCreatePool*(...)` calls in `src/mssql_storage.cpp:1372`, `:1379`, `:1382`. `MSSQLAttach` becomes pure: validate connection info, construct `MSSQLCatalog`, call `Initialize` (which now builds the pool). Also remove the `MssqlPoolManager::Instance().RemovePool(name)` call at `src/mssql_storage.cpp:830` (spec 045 band-aid — no longer reachable once `MSSQLContextManager` is gone in T020).
- [ ] T014 [US1] Migrate pin-counter callers in `src/catalog/mssql_transaction.cpp:139` and `:143` from `MssqlPoolManager::Instance().IncrementPinnedCount(catalog_.GetContextName())` to `catalog_.GetConnectionPool().IncrementPinned()` (and symmetric for `DecrementPinnedCount` → `DecrementPinned()`).
- [ ] T015 [P] [US1] Fix wrong call site in `src/dml/insert/mssql_insert_executor.cpp:74`: replace `MssqlPoolManager::Instance().GetPool(target_.catalog_name)` with `target_.catalog.GetConnectionPool()` (catalog already in `target_`).
- [ ] T016 [P] [US1] Fix wrong call site in `src/dml/update/mssql_update_executor.cpp:73`: same pattern as T015.
- [ ] T017 [P] [US1] Fix wrong call sites in `src/query/mssql_result_stream.cpp:82` and `:89`: route through catalog (use the catalog reference already passed into the stream constructor; if absent, plumb it through).
- [ ] T018 [P] [US1] Fix wrong call site in `src/query/mssql_query_executor.cpp:40`: route through catalog.
- [ ] T019 [US1] Rewrite `mssql_diagnostic.cpp` `MssqlPoolStatsGlobalState` (around `src/connection/mssql_diagnostic.cpp:234`) to enumerate via DuckDB catalog list: `context.db->GetDatabaseManager().GetDatabases(context)`, filter `attached.GetCatalog().GetCatalogType() == "mssql"`, cast to `MSSQLCatalog` and project per-catalog `GetConnectionPool().GetStats()`. Single-name lookup at line 237 uses `Catalog::GetCatalog(context, bind_data.context_name).Cast<MSSQLCatalog>()`. Per-row `pinned_count` sourced from `GetConnectionPool().GetPinnedCount()` (replaces `MssqlPoolManager::Instance().GetPinnedCount(pool_name)` at line 261).
- [ ] T020 [US1] Delete `MSSQLContextManager` class + `g_context_managers` + `g_context_managers_lock` from `src/mssql_storage.cpp` (around `:792-833`). Remove any remaining references to `MSSQLContextManager::Get/RegisterContext/UnregisterContext` from `mssql_storage.cpp`. Retires spec 045 band-aid (`70a4d90`) entirely.
- [ ] T021 [US1] Delete `MssqlPoolManager` class: remove `src/connection/mssql_pool_manager.cpp` and `src/include/connection/mssql_pool_manager.hpp`. Remove these paths from `CMakeLists.txt` source lists. Remove `#include "connection/mssql_pool_manager.hpp"` from any surviving consumers (should be zero after T011-T019).
- [ ] T022 [US1] Build + unit + integration test gate: `GEN=ninja make debug && GEN=ninja make test && GEN=ninja make integration-test`. All previously-green tests must stay green. Fix any breakage before proceeding to T023-T025.

### Tests for User Story 1

- [ ] T023 [US1] Write `test/cpp/test_multi_instance_pool_isolation.cpp`. Three Catch2 cases mirroring Scenarios 1/2/3 from `spec.md`:
  - `scenario_1_routing_correctness` — two `DuckDB` instances each `ATTACH AS db (TYPE mssql)` to different servers; both queries route correctly (assertion on `@@SERVERNAME`).
  - `scenario_2_detach_isolation` — instance A DETACHes; instance B's next query against its own `db` still succeeds.
  - `scenario_3_silent_shutdown` — 100 iterations of construct-attach-query-destruct without explicit DETACH; after loop, `sys.dm_exec_connections` from a verifier connection reports zero residual connections from this process. Fixture skips if `MSSQL_TEST_DSN` env is not set. Add to `CMakeLists.txt` test sources.
- [ ] T024 [US1] Write `test/cpp/test_issue_96_attach_loop.cpp`. Single Catch2 case mirroring Scenario 4 (verbatim Python loop from issue #96, in C++): 100 iterations of `DuckDB db(":memory:"); Connection conn(db); conn.Query("INSTALL mssql FROM ...; LOAD mssql; ATTACH '...' AS TO_MSSQL ..."); conn.Query("SELECT 1 ...");` — every iteration must succeed; no `"Context 'TO_MSSQL' already exists"` errors. Add to `CMakeLists.txt`.
- [ ] T025 [US1] Run both new C++ tests: `./build/debug/test/unittest "test_multi_instance_pool_isolation*"` + `./build/debug/test/unittest "test_issue_96_attach_loop*"`. All cases must pass. Capture before/after pass-fail evidence for the PR description.

**Checkpoint**: US1 alone delivers the production-bug fix. Mergeable as MVP at this point if the rest of the work needs deferral. SC-001, SC-002, SC-003, SC-005, SC-009 all green.

---

## Phase 4: User Story 2 — ATTACH credential validation (Priority: P1)

**Goal**: ATTACH synchronously validates credentials (TCP + LOGIN7 + FEDAUTH / Kerberos round trip) before returning. Bad password fails ATTACH up-front instead of producing confusing error on first query. Opt-out via `lazy_validation=true` ATTACH option preserves today's behavior.

**Independent Test**: `test/sql/attach/attach_validates_credentials.test` passes (3 cases: bad password, unreachable host, lazy-validation opt-out). Verifies SC-010.

**Depends on**: US1 (pool must be constructed inside `MSSQLCatalog::Initialize` so that the validation acquire can happen there).

### Implementation for User Story 2

- [ ] T026 [US2] Add new pool setting `mssql_attach_validation_timeout` (INTEGER seconds, default = value of `mssql_connection_timeout`) to the extension settings registration in `src/mssql_storage.cpp` (search for existing `mssql_connection_timeout` registration as template).
- [ ] T027 [US2] Parse `lazy_validation` ATTACH option in `MSSQLAttach` (`src/mssql_storage.cpp`): accept BOOL value, default `false`; also accept ADO.NET alias `LazyValidation` from connection-string parse path. Plumb the parsed flag into `MSSQLCatalog` (add `bool lazy_validation_` member or pass through `connection_info_`).
- [ ] T028 [US2] Add eager validation step at the end of `MSSQLCatalog::Initialize` in `src/catalog/mssql_catalog.cpp`. After successful pool construction (post-T011), if `lazy_validation_ == false`: call `pool.Acquire()` with `mssql_attach_validation_timeout`; on any exception (TCP refused, LOGIN7 rejected, FEDAUTH token failure, Kerberos failure), propagate out of `Initialize` so `MSSQLAttach` surfaces it to the user with verbatim TDS error attached. On success, immediately release the connection back to the pool (RAII via `tds::ConnectionPool::ConnectionHandle` or equivalent — keep it warm for the first real query, don't leak).
- [ ] T029 [US2] Build + existing test sweep: `GEN=ninja make debug && GEN=ninja make test && GEN=ninja make integration-test`. ATTACH-using tests must stay green (their credentials are good, so validation passes invisibly).

### Tests for User Story 2

- [ ] T030 [US2] Write `test/sql/attach/attach_validates_credentials.test`. Three SQL test cases (per `spec.md` SC-010):
  - `statement error ATTACH 'Server=...;User Id=sa;Password=WRONG_PASSWORD' AS bad (TYPE mssql)` — expects login error containing "Login failed for user 'sa'".
  - `statement error ATTACH 'Server=unreachable.invalid,1433;Database=db;User Id=u;Password=p' AS unreachable (TYPE mssql)` — expects connection error (DNS / TCP) within configured timeout.
  - `statement ok ATTACH 'Server=unreachable.invalid,1433;Database=db;User Id=u;Password=p' AS lazy (TYPE mssql, lazy_validation=true)` — expects ATTACH succeeds (opt-out); subsequent query against `lazy.dbo.x` then fails (asserted by `statement error`).
- [ ] T031 [US2] Run the new SQL test: `./build/debug/test/unittest "test/sql/attach/attach_validates_credentials.test"`. All cases pass; existing ATTACH tests stay green.

**Checkpoint**: US2 ships alongside US1. After this point, ATTACH UX matches expectation (bad creds fail fast).

---

## Phase 5: User Story 3 — Result stream registry → catalog (Priority: P2)

**Goal**: Dissolve `MSSQLResultStreamRegistry` singleton; move its state (mutex + `unordered_map<string, unique_ptr<MSSQLResultStream>>`) into `MSSQLCatalog` as private members. `mssql_scan` `Bind` / `InitGlobal` call `catalog.RegisterStream(...)` / `catalog.RetrieveStream(...)`. Orphan streams auto-cleaned at `~MSSQLCatalog`.

**Independent Test**: `test/cpp/test_result_stream_registry_isolation.cpp` — two concurrent instances, each running `mssql_scan` against different catalogs, UUIDs from one catalog are not retrievable from another. Verifies SC-006.

**Depends on**: US1 (catalog is the natural owner; no per-instance container abstraction needed). Orthogonal to US2.

### Implementation for User Story 3

- [ ] T032 [US3] Add private members to `MSSQLCatalog` in `src/include/catalog/mssql_catalog.hpp`: `mutable std::mutex streams_mutex_;` and `std::unordered_map<std::string, std::unique_ptr<MSSQLResultStream>> active_streams_;`. Include the appropriate forward declarations or headers (look for existing `MSSQLResultStream` usage in `mssql_functions.hpp`).
- [ ] T033 [US3] Add public methods to `MSSQLCatalog` in the same header: `std::string RegisterStream(std::unique_ptr<MSSQLResultStream> stream)` and `std::unique_ptr<MSSQLResultStream> RetrieveStream(const std::string &uuid)`.
- [ ] T034 [US3] Implement `MSSQLCatalog::RegisterStream` in `src/catalog/mssql_catalog.cpp`: generate UUID v4 (use DuckDB's `UUID::ToString(UUID::GenerateRandomUUID())` or std-based equivalent — check what spec 045 used), lock `streams_mutex_`, `active_streams_.emplace(uuid, std::move(stream))`, return UUID.
- [ ] T035 [US3] Implement `MSSQLCatalog::RetrieveStream` in the same file: lock `streams_mutex_`, `find` UUID; if found, `std::move` the unique_ptr out, `erase` the entry, return moved ptr; otherwise return `nullptr`. Atomic find+erase under one lock acquisition.
- [ ] T036 [US3] Update `mssql_scan` Bind in `src/mssql_functions.cpp:153` to call `catalog.RegisterStream(std::move(result_stream))` instead of `MSSQLResultStreamRegistry::Instance().Register(std::move(result_stream))`. Get the catalog from the same context lookup that happens at line 121 (`manager.HasContext(...)`). Store the returned UUID string in `bind_data->result_stream_id` (the field type may need to change from `uint64_t` to `string` — check existing definition).
- [ ] T037 [US3] Update `mssql_scan` InitGlobal in `src/mssql_functions.cpp:177` to call `catalog.RetrieveStream(bind_data.result_stream_id)` instead of `MSSQLResultStreamRegistry::Instance().Retrieve(bind_data.result_stream_id)`. Look up the catalog from `bind_data.context_name` first.
- [ ] T038 [US3] Delete `MSSQLResultStreamRegistry` class definition in `src/include/mssql_functions.hpp:115-130` and its implementation in `src/mssql_functions.cpp:41-64`. Remove any `#include` lines that referenced it as a standalone class.
- [ ] T039 [US3] Build + existing test sweep: `GEN=ninja make debug && GEN=ninja make test && GEN=ninja make integration-test`. `mssql_scan` tests (anywhere in `test/sql/`) must stay green; this is the canary for the Bind→InitGlobal bridge still working.

### Tests for User Story 3

- [ ] T040 [US3] Write `test/cpp/test_result_stream_registry_isolation.cpp`. Single Catch2 case: construct two `DuckDB` instances (`db_a`, `db_b`), each ATTACH to a different MSSQL catalog under names `a` and `b` respectively. From instance A, run `mssql_scan('a', 'SELECT TOP 5 * FROM dbo.TestSimplePK')` (capture mid-scan via a small chunk read). Verify via instrumentation (or test helper accessing catalog state) that the UUID generated by A's Bind is not visible in B's catalog. Add to `CMakeLists.txt`.
- [ ] T041 [US3] Run the new C++ test: `./build/debug/test/unittest "test_result_stream_registry_isolation*"`. Passes.

**Checkpoint**: US3 lands. `MSSQLResultStreamRegistry` is gone; stream registry per-catalog by construction.

---

## Phase 6: User Story 4 — Deprecate diagnostic functions (Priority: P3)

**Goal**: Mark `mssql_open` / `mssql_close` / `mssql_ping` as `[DEPRECATED]` in their function descriptions, in CLAUDE.md's Extension Functions table, and in CHANGELOG.md. The singleton `MSSQLConnectionHandleManager` stays as legitimate (per FR-010 + clarification addendum) — only the labeling changes.

**Independent Test**: `SELECT description FROM duckdb_functions() WHERE function_name IN ('mssql_open','mssql_close','mssql_ping')` returns rows whose descriptions begin with `[DEPRECATED]`.

**Depends on**: nothing (purely cosmetic / documentation; can ship parallel to US3 or even before US1).

### Implementation for User Story 4

- [ ] T042 [P] [US4] In `src/connection/mssql_diagnostic.cpp`, modify the three `ScalarFunctionSet` registrations (around lines 290, 295, 300) to set a `DESCRIPTION` field starting with `[DEPRECATED]`. Example pattern (verify exact API):
  ```cpp
  open_func.SetDescription("[DEPRECATED] Diagnostic helper. Prefer ATTACH + mssql_scan/mssql_exec for catalog-based access; this API is kept for backward compatibility and will be removed in a future major release.");
  ```
  Apply equivalent for `close_func` and `ping_func`.
- [ ] T043 [P] [US4] Update `CLAUDE.md` "Extension Functions" table (around line 220): prefix the `mssql_open` / `mssql_close` / `mssql_ping` Description column with `[DEPRECATED]` and add a one-line note pointing at the catalog-based alternatives.
- [ ] T044 [P] [US4] Add deprecation note to `CHANGELOG.md` under the spec 047 entry: "`mssql_open`/`mssql_close`/`mssql_ping` are deprecated — they remain functional but are kept only for backward compatibility; their handle-manager singleton is the last extension-internal process-wide state and will be removed alongside the functions in a future major release."
- [ ] T045 [P] [US4] Add inline comment block at the top of the `MSSQLConnectionHandleManager` class definition in `src/include/connection/mssql_diagnostic.hpp` explaining the design (singleton retained because diagnostic functions take no catalog discriminator; planned for removal with the functions).

### Tests for User Story 4

- [ ] T046 [US4] Smoke check: build, then run `./build/debug/duckdb -unsigned -c "INSTALL mssql FROM local_build_debug; LOAD mssql; SELECT function_name, description FROM duckdb_functions() WHERE function_name IN ('mssql_open','mssql_close','mssql_ping') ORDER BY function_name;"`. All 3 descriptions start with `[DEPRECATED]`. Capture output for the PR description.

**Checkpoint**: US4 lands. Diagnostic functions are visibly deprecated; users have clear guidance to use catalog-based alternatives.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Audit gates, docs, bench, final test pass, PR prep.

- [ ] T047 [P] SC-004 grep audit: `grep -rn 'MssqlPoolManager\|MSSQLContextManager\|MSSQLResultStreamRegistry' src/ src/include/`. Expect zero matches (excluding comment-only mentions tagged with "removed in spec 047"). Note: `MSSQLConnectionHandleManager` is NOT in this grep — it stays.
- [ ] T048 [P] Write `specs/047-process-state-cleanup/state_inventory.md` (FR-007 deliverable). Final classification table of every process-wide static after implementation: 0 "migrate" entries; legitimate set = Winsock + OpenSSL + thread-local scratch (×2) + Azure TokenCache + MSSQLConnectionHandleManager (with deprecation note linking to FR-010).
- [ ] T049 [P] Add inline comment in `src/azure/azure_token.{hpp,cpp}` explaining that the singleton TokenCache is process-wide intentionally (deduplicates token acquisition for same `secret_name` across DuckDB instances). Point at clarification Q3.
- [ ] T050 [P] Update `CLAUDE.md` "Key Architecture Concepts" → "Connection pool" bullet to reflect per-catalog ownership. Current text mentions "per attached database" — make explicit that pool lifetime is bounded by catalog lifetime (RAII via `unique_ptr`); deleted singletons; no shared cross-instance pool state.
- [ ] T051 [P] Update `CLAUDE.md` "Recent Changes" with spec 047 entry summarizing: 3 singletons removed (pool manager, context managers, result stream registry); handle manager stays + deprecated functions; ATTACH credential validation; closes issue #96; bench-neutral; no public API regression.
- [ ] T052 Run bench parity gate: `MSSQL_BENCH_ROW_COUNT=1000000 MSSQL_BENCH_DUCKDB_BIN=$(pwd)/build/release/duckdb MSSQL_BENCH_OUTPUT=/tmp/bench_codec_e2e_spec047_run1.txt bash test/bench/bench_codec_e2e.sh` × 3 runs, min-of-3 per step. Compare to baseline captured in T002. Save to `specs/047-process-state-cleanup/bench_results.md`. Every step within ±5% (SC-007 gate).
- [ ] T053 clang-format-14 sweep over all touched files: `/opt/homebrew/opt/llvm@14/bin/clang-format -i src/catalog/mssql_catalog.{cpp,hpp} src/catalog/mssql_transaction.cpp src/include/tds/tds_connection_pool.hpp src/tds/tds_connection_pool.cpp src/mssql_storage.cpp src/mssql_functions.{cpp} src/include/mssql_functions.hpp src/connection/mssql_diagnostic.cpp src/dml/insert/mssql_insert_executor.cpp src/dml/update/mssql_update_executor.cpp src/query/mssql_result_stream.cpp src/query/mssql_query_executor.cpp` plus headers under `src/include/`.
- [ ] T054 Final full-suite test gate: `GEN=ninja make test && GEN=ninja make integration-test`. All previously-green tests still green; all new tests (T023, T024, T030, T040) green; SC-006-related stream isolation green; SC-010 ATTACH validation green.
- [ ] T055 Write `specs/047-process-state-cleanup/pr_description.md`: Summary; the 3 production bug classes fixed (Scenarios 1/2/3); issue #96 closure (Scenario 4 + SC-009); ATTACH validation (FR-011 + SC-010); 3 singletons deleted vs 1 kept-deprecated; bench parity; test plan; follow-ups (potential removal of `mssql_open/close/ping` in a future major release closes the handle manager singleton too).
- [ ] T056 Push branch + open PR: `git push -u origin 047-process-state-cleanup`; `gh pr create --title "feat(047): process-wide state cleanup — per-catalog pool ownership; closes #96" --body "$(cat specs/047-process-state-cleanup/pr_description.md)"`. Mark ready for review when CI is green.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 (Setup)** — no dependencies; baseline capture (T002) is reference data only.
- **Phase 2 (Foundational)** — depends on Setup. BLOCKS US1, US2, US3.
- **Phase 3 (US1 — pool ownership)** — depends on Foundational (needs pin counter in T004/T005). MVP-mergeable on its own.
- **Phase 4 (US2 — ATTACH validation)** — depends on US1 (validation acquire lives inside Initialize, which is meaningful only after pool is catalog-owned). Independent of US3 / US4.
- **Phase 5 (US3 — result stream registry)** — depends on US1 (catalog needs to be the natural owner before adding stream methods there). Independent of US2 / US4.
- **Phase 6 (US4 — deprecate diagnostic)** — no real dependencies on other stories; can be implemented in parallel with US1-3 if helpful. Order in the doc reflects priority not blocking.
- **Phase 7 (Polish)** — depends on US1 (must be done) + US3 (singleton-deletion gate); US2 and US4 must be done if they're shipping in this PR. Bench (T052) must run on release build with full implementation applied.

### Story-Level Parallel Execution

**Within US1**: T015, T016, T017, T018 are `[P]` — different files, no interdependency. After T007-T014 land, run them concurrently.

**Across stories**: After US1 lands (T007-T022), US2 (Phase 4), US3 (Phase 5), and US4 (Phase 6) can proceed in parallel — each touches different files / responsibilities. T030 (US2 test) is independent of T040 (US3 test) and T046 (US4 test).

**Within Polish**: T047, T048, T049, T050, T051 are all `[P]` — different files, different concerns.

### Critical Path (longest single-thread chain)

T001 → T002 → T003 → T004 → T005 → T006 → T007 → T008 → T009 → T010 → T011 → T012 → T013 → T014 → T020 → T021 → T022 → T023 → T024 → T025 → T026 → T027 → T028 → T029 → T032 → ... → T054 → T055 → T056.

Tasks T015-T018 (parallel within US1), T030-T046 (cross-story parallelism after US1), and T047-T051 (polish parallelism) cut wall-clock substantially below the critical-path length.

---

## Implementation Strategy

### MVP First (just US1)

If pressure exists to ship the issue #96 fix ASAP:

1. Run Phase 1 + Phase 2 + Phase 3 (T001-T025) only.
2. Phase 7 polish (T047, T050, T051, T053, T054, T055, T056) — minimal subset.
3. Skip Phase 4 (US2), Phase 5 (US3), Phase 6 (US4) — defer to follow-up PRs.

Result: issue #96 closed; production bug fixed; ATTACH still lazy-validates (today's behavior preserved); stream registry still singleton; diagnostic functions not yet labeled deprecated.

### Incremental Delivery (recommended)

Ship the full spec in one PR following the natural order: Phase 1 → 2 → 3 → 4 → 5 → 6 → 7. ATTACH validation (US2) and stream registry move (US3) are small enough to land alongside US1 without bloating the PR. US4 is essentially documentation — trivial to include.

### Parallel-Team Execution

If multiple implementers available:
- **Implementer A** (lead): Phase 1 + 2, then US1 critical path (T007-T014, T019-T022).
- **Implementer B**: After US1 T022 lands, US2 (T026-T031) in parallel with US3.
- **Implementer C**: US3 (T032-T041) in parallel with US2; US4 (T042-T046) can start any time (no blocking).
- **Implementer A or any**: Phase 7 polish after US1+2+3 land.

### Risk Mitigation

- **Build break early in US1**: After T007 (type change), the build will be broken until T011 (Initialize built the pool) lands. Sequence T007-T011 as a single tight loop; don't commit T007 alone.
- **Stale singleton references**: T020 + T021 (deletions) must come AFTER all callers are migrated (T013, T014, T015-T018). Otherwise build breaks.
- **Test fixture missing `MSSQL_TEST_DSN`**: new C++ tests (T023, T024, T040) MUST gate on env-var presence to skip cleanly in CI environments without SQL Server (same pattern as existing integration tests).
- **Bench regression > 5%**: investigate; if `pool.Acquire() + Release()` on ATTACH is the culprit (FR-011), it's expected and should be in noise (one extra round-trip per ATTACH, not per row). Otherwise root-cause before merge.

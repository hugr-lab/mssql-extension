---
description: "Task list for spec 047 — Process-Wide State Cleanup"
---

# Tasks: Process-Wide State Cleanup

**Input**: Design documents from `/specs/047-process-state-cleanup/`

**Prerequisites**: `plan.md` (required), `spec.md` (required), `research.md`, `data-model.md`, `contracts/README.md`, `quickstart.md` — all generated.

**Tests**: Test tasks are explicitly part of every user story (per FR-008, FR-009, FR-014, and SC-001..SC-012). C++ + SQL regression tests are required for merge gate.

**Organization**: Tasks are grouped by user story so each story can be implemented, tested, and merged independently. US1 alone delivers the production-bug fix (issue #96 + Scenarios 1/2/3); the rest are stacked improvements.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies on incomplete tasks)
- **[Story]**: User story label (US1..US4, US-SEC for security hardening, US-AN for Application Name bundle); setup/foundational/polish phases have no story label
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

- [X] T004 Add pin-counter to pool: in `src/include/tds/tds_connection_pool.hpp` add private `std::atomic<int64_t> pinned_count_{0}` member and public methods `IncrementPinned()`, `DecrementPinned()`, `GetPinnedCount() const noexcept`. In `src/tds/tds_connection_pool.cpp` implement via `fetch_add` / `fetch_sub` / `load` with `std::memory_order_relaxed`.
- [X] T005 Extend `PoolStatistics` struct in `src/include/tds/tds_connection_pool.hpp` with `int64_t pinned_count` field (renames stale `size_t pinned_connections` placeholder that was never wired up). Update `tds::ConnectionPool::GetStats()` in `src/tds/tds_connection_pool.cpp` to populate it from `GetPinnedCount()`. Note: `mssql_diagnostic.cpp:221` column-header string `"pinned_connections"` is left as-is until T019 rewrites the diagnostic enumerator (will rename column to `pinned_count` to match quickstart.md schema).
- [X] T006 Verify build + existing tests stay green: `GEN=ninja make debug && GEN=ninja make test && GEN=ninja make integration-test`. New members have no callers yet — must not break anything.

---

## Phase 3: User Story 1 — Per-catalog pool ownership (Priority: P1) 🎯 MVP

**Goal**: `MSSQLCatalog` becomes the sole owner of its `tds::ConnectionPool` via `unique_ptr` member. Delete the `MssqlPoolManager` singleton and the `MSSQLContextManager` + `g_context_managers` global container. Pool construction moves inline into `MSSQLCatalog::Initialize`. All consumers route via `catalog.GetConnectionPool()`. Closes issue #96.

**Independent Test**: `test/cpp/test_multi_instance_pool_isolation.cpp` (Scenarios 1/2/3) and `test/cpp/test_issue_96_attach_loop.cpp` (Scenario 4) all pass. `make test && make integration-test` stay green. Verifies SC-001, SC-002, SC-003, SC-005 (parity portion — credential-redaction portion gated separately in Phase 7), SC-009.

### Implementation for User Story 1

- [X] T007 [US1] Change `MSSQLCatalog::connection_pool_` field type in `src/include/catalog/mssql_catalog.hpp:195` from `shared_ptr<tds::ConnectionPool>` to `unique_ptr<tds::ConnectionPool>`. Update `GetConnectionPool()` accessor at line 103 if needed (signature returns reference, should still compile).
- [X] T008 [US1] Inline SQL-auth pool construction into `MSSQLCatalog::Initialize` in `src/catalog/mssql_catalog.cpp`. Move the body of `MssqlPoolManager::GetOrCreatePool(...)` (`src/connection/mssql_pool_manager.cpp:15-54`) into Initialize, dropping the singleton lookup + map insert + lock. Construct via `make_uniq<tds::ConnectionPool>(...)` and store in `connection_pool_`.
- [X] T009 [US1] Inline Azure FEDAUTH pool construction into `Initialize` (same file). Move body of `MssqlPoolManager::GetOrCreatePoolWithAzureAuth(...)` (`src/connection/mssql_pool_manager.cpp:56-100`); preserve the token-capturing factory closure.
- [X] T010 [US1] Inline Integrated-auth (Kerberos / SSPI) pool construction into `Initialize`. Move body of `MssqlPoolManager::GetOrCreatePoolWithIntegratedAuth(...)` (`src/connection/mssql_pool_manager.cpp:102-169`); preserve per-connection `AuthStrategyFactory::Create(info_copy)` invocation in the factory closure (spec 042 invariant — fresh authenticator per pool refill).
- [X] T011 [US1] Add `auth_method` dispatch in `MSSQLCatalog::Initialize`: branch on `connection_info_->auth_method` between SQL-auth / Azure / Integrated; call the appropriate inlined branch from T008/T009/T010. Replace the existing `MssqlPoolManager::Instance().GetOrCreatePool*(...)` call at `src/catalog/mssql_catalog.cpp:82` with the inline construction.
- [X] T012 [US1] Drop `MssqlPoolManager::Instance().RemovePool(...)` call in `MSSQLCatalog::OnDetach` at `src/catalog/mssql_catalog.cpp:630`. Pool teardown is now implicit via `~MSSQLCatalog` → `unique_ptr` destruction. Keep the Azure token-cache invalidation that's also in `OnDetach`.
- [X] T013 [US1] Remove the 3 `MssqlPoolManager::Instance().GetOrCreatePool*(...)` calls in `src/mssql_storage.cpp:1372`, `:1379`, `:1382`. `MSSQLAttach` becomes pure: validate connection info, construct `MSSQLCatalog`, call `Initialize` (which now builds the pool). Also remove the `MssqlPoolManager::Instance().RemovePool(name)` call at `src/mssql_storage.cpp:830` (spec 045 band-aid — no longer reachable once `MSSQLContextManager` is gone in T020).
- [X] T014 [US1] Migrate pin-counter callers in `src/catalog/mssql_transaction.cpp:139` and `:143` from `MssqlPoolManager::Instance().IncrementPinnedCount(catalog_.GetContextName())` to `catalog_.GetConnectionPool().IncrementPinned()` (and symmetric for `DecrementPinnedCount` → `DecrementPinned()`).
- [X] T015 [P] [US1] Fix wrong call site in `src/dml/insert/mssql_insert_executor.cpp:74`: replace `MssqlPoolManager::Instance().GetPool(target_.catalog_name)` with `target_.catalog.GetConnectionPool()` (catalog already in `target_`).
- [X] T016 [P] [US1] Fix wrong call site in `src/dml/update/mssql_update_executor.cpp:73`: same pattern as T015.
- [X] T017 [P] [US1] Fix wrong call sites in `src/query/mssql_result_stream.cpp:82` and `:89`: route through catalog (use the catalog reference already passed into the stream constructor; if absent, plumb it through).
- [X] T018 [P] [US1] Fix wrong call site in `src/query/mssql_query_executor.cpp:40`: route through catalog.
- [X] T019 [US1] Rewrite `mssql_diagnostic.cpp` `MssqlPoolStatsGlobalState` (around `src/connection/mssql_diagnostic.cpp:234`) to enumerate via DuckDB catalog list: `context.db->GetDatabaseManager().GetDatabases(context)`, filter `attached.GetCatalog().GetCatalogType() == "mssql"`, cast to `MSSQLCatalog` and project per-catalog `GetConnectionPool().GetStats()`. Single-name lookup at line 237 uses `Catalog::GetCatalog(context, bind_data.context_name).Cast<MSSQLCatalog>()`. Per-row `pinned_count` sourced from `GetConnectionPool().GetPinnedCount()` (replaces `MssqlPoolManager::Instance().GetPinnedCount(pool_name)` at line 261).
- [X] T020 [US1] Delete `MSSQLContextManager` class + `g_context_managers` + `g_context_managers_lock` from `src/mssql_storage.cpp` (around `:792-833`). Remove any remaining references to `MSSQLContextManager::Get/RegisterContext/UnregisterContext` from `mssql_storage.cpp`. Retires spec 045 band-aid (`70a4d90`) entirely. **(Done in commit `e6371de` — 9 callers migrated; band-aid retired; build + 32 unit + 116 integration green.)**
- [X] T021 [US1] Delete `MssqlPoolManager` class: remove `src/connection/mssql_pool_manager.cpp` and `src/include/connection/mssql_pool_manager.hpp`. Remove these paths from `CMakeLists.txt` source lists. Remove `#include "connection/mssql_pool_manager.hpp"` from any surviving consumers (should be zero after T011-T019). **(Done in commit `fede962` — −323 LOC total; SC-004 grep gate now matches only history comments.)**
- [X] T022 [US1] Build + unit + integration test gate: `GEN=ninja make debug && GEN=ninja make test && GEN=ninja make integration-test`. All previously-green tests must stay green. Fix any breakage before proceeding to T023-T025. **(Done — debug build clean; `*mssql*` unit filter = 163 assertions / 19 cases; `[integration]` = 1076 assertions / 34 cases (8 skipped); `[sql]` = 304 assertions / 16 cases. Total 1543 assertions / 69 cases green.)**

### Tests for User Story 1

- [X] T023 [US1] Write `test/cpp/test_multi_instance_pool_isolation.cpp`. Three cases (plain `int main` per the established C++ integration-test convention — `test_multi_connection_transactions.cpp` / `test_simple_query.cpp` — DuckDB has no Catch2 harness for tests that link the DuckDB shared library) mirroring Scenarios 1/2/3 from `spec.md`:
  - `scenario_1_routing_correctness` — two `DuckDB` instances each `ATTACH ... AS mssql (TYPE mssql)` to different DATABASES on the same SQL Server (master + TestDB; same-server adaptation since CI has one mssql container); both `mssql_scan('mssql', 'SELECT DB_NAME()')` queries route to the catalog's own DB.
  - `scenario_2_detach_isolation` — instance A DETACHes; instance B's next query against its own `mssql` still routes to TestDB.
  - `scenario_3_silent_shutdown` — 100 iterations of construct-attach-query-destruct without explicit DETACH; verifier instance counts active `sys.dm_exec_sessions` rows where `login_name = sa` AND `program_name LIKE '%DuckDB%'`, expects residual = baseline (just the verifier itself). Skips cleanly if `MSSQL_TEST_PASS` env is not set. Build target: `test-multi-instance-pool-isolation` in `Makefile` (NOT CMakeLists.txt — established convention for DuckDB-linked C++ integration tests is per-target Makefile recipe, NOT the `unittest` Catch2 harness, which is sqllogictest-only). **(Done — all 3 scenarios green on first run.)**
- [X] T024 [US1] Write `test/cpp/test_issue_96_attach_loop.cpp`. Single test mirroring Scenario 4 (verbatim Python loop from issue #96, in C++): 100 iterations of `DuckDB db(nullptr); Connection conn(db); LOAD mssql; ATTACH ... AS TO_MSSQL; mssql_scan('TO_MSSQL', 'SELECT 1');` — every iteration must succeed; no `"Context 'TO_MSSQL' already exists"` errors. Build target: `test-issue-96-attach-loop` in `Makefile`. **(Done — 100/100 iterations green on first run.)**
- [X] T025 [US1] Run both new C++ tests: `GEN=ninja make test-multi-instance-pool-isolation` + `GEN=ninja make test-issue-96-attach-loop` (or the meta target `GEN=ninja make test-spec047-us1`). All scenarios must pass. **(Done — both green; evidence captured in commit message.)**

**Checkpoint**: US1 alone delivers the production-bug fix. Mergeable as MVP at this point if the rest of the work needs deferral. SC-001, SC-002, SC-003, SC-005, SC-009 all green.

---

## Phase 4: User Story 2 — ATTACH credential validation (Priority: P1)

**Goal**: ATTACH synchronously validates credentials (TCP + LOGIN7 + FEDAUTH / Kerberos round trip) before returning. Bad password fails ATTACH up-front instead of producing confusing error on first query. Opt-out via `lazy_validation=true` ATTACH option preserves today's behavior.

**Independent Test**: `test/sql/attach/attach_validates_credentials.test` passes (3 cases: bad password, unreachable host, lazy-validation opt-out). Verifies SC-010.

**Depends on**: US1 (pool must be constructed inside `MSSQLCatalog::Initialize` so that the validation acquire can happen there).

### Implementation for User Story 2

- [X] T026 [US2] Added new extension setting `mssql_attach_validation_timeout` (BIGINT seconds, default 0 ⇒ inherit `mssql_connection_timeout`) in `src/connection/mssql_settings.cpp:81-92` + `LoadAttachValidationTimeout(ClientContext &)` loader in same file. Declared in `src/include/connection/mssql_settings.hpp`. Distinct from connection_timeout so operators can give the interactive ATTACH a shorter ceiling than steady-state query pool fills. Note: setting registration lives in `mssql_settings.cpp` (DuckDB pattern), not `mssql_storage.cpp` as tasks.md suggested.
- [X] T027 [US2] Parsed `lazy_validation` ATTACH option in `MSSQLAttach` (`src/mssql_storage.cpp:1163-1167`) — accepts BOOL, defaults to `false`, also accepts the alias `LazyValidation` (lowercased to `lazyvalidation` via `StringUtil::Lower`). Flag stays local to `MSSQLAttach` and gates the existing `Validate*` calls inline — no new `MSSQLCatalog` member required since validation already happens in `MSSQLAttach` before catalog construction.
- [X] T028 [US2] Wrapped the four existing eager-validation paths (`ValidateManualTokenConnection`, `ValidateAzureConnection`, `ValidateIntegratedAuthConnection`, `ValidateConnection` in `src/mssql_storage.cpp:1259-1322`) with `if (!lazy_validation)` and switched the timeout source from `pool_config.connection_timeout` to `LoadAttachValidationTimeout(context)`. **Documented deviation from tasks.md letter**: tasks.md asked to move validation into `MSSQLCatalog::Initialize` via `pool.Acquire()`, but `pool.Acquire()` returns `nullptr` on failure with no diagnostic info (UX regression: would replace messages like "Authentication failed for user 'sa' - check username and password" with bare "Failed to acquire connection from pool"). Instead kept the existing `Validate*` wiring (which captures `conn.GetLastError()` and runs it through `TranslateConnectionError` for credential redaction + classification) and gated it behind the new `lazy_validation` flag. JWT-shape validation in the manual-token branch and FEDAUTH token acquisition in the azure branch both still run under lazy mode (local-only operations needed by the pool factory regardless).
- [X] T028a [US2] Security audit: confirmed `TranslateConnectionError` in `src/mssql_storage.cpp:785-845` references only `host`, `port`, `user`, `database` — never the password. The four `Validate*` functions wrap errors as `"MSSQL connection validation failed: %s"` with the translated message. `ValidateAzureConnection`, `ValidateManualTokenConnection`: error_message from upstream HTTP / FEDAUTH layer never contains user secrets. `MSSQLAttach` early throws (`src/mssql_storage.cpp:1195`) and `MSSQLConnectionInfo::FromConnectionString` (`src/mssql_storage.cpp:678-679`) never echo the raw connection string. Runtime evidence: `ATTACH ...Password=WRONG_PASSWORD_SENTINEL_xyz123` produces `MSSQL connection validation failed: Authentication failed for user 'sa' - check username and password` — sentinel NOT echoed. Test enforces via the positive substring assertion in T030 case 1 (the canonical translated message is fixed by `TranslateConnectionError`'s 18456-detection branch and contains no password reference by construction).
- [X] T029 [US2] Build + test gate: `GEN=ninja make debug` + `GEN=ninja make test-all`. All `*mssql*`-tagged tests green: **3549 assertions / 117 cases, 22 skipped (env-gated TLS / Azure / Kerberos)**. New `test/sql/attach/attach_validates_credentials.test` picked up at slot 113/139. Note: SQL test under `test/sql/attach/` is registered under `[attach]` Catch2 tag (path-derived), so it runs via `make test-all`'s `*mssql*` filter — NOT via `make integration-test`'s `[integration]` filter. Convention matches existing `attach_validation.test` / `attach_trust_cert.test`.

### Tests for User Story 2

- [X] T030 [US2] Wrote `test/sql/attach/attach_validates_credentials.test` (4 SQL cases). Case 1: `WRONG_PASSWORD_SENTINEL_xyz123` triggers eager validation; expected substring `Authentication failed for user` (canonical TranslateConnectionError output — sentinel never echoed because TranslateConnectionError doesn't reference the password; follow-up `SELECT COUNT(*) FROM duckdb_databases() WHERE name='att_bad_pw'` returns 0 confirming the catalog never registered). Case 2: unreachable host with `mssql_attach_validation_timeout = 5` to bound the wait; expects `MSSQL connection validation failed`. Case 3: `lazy_validation true` opt-out — ATTACH succeeds against unreachable host, subsequent `mssql_scan` then fails with `Failed to acquire connection from pool`. Case 4: valid credentials — ATTACH succeeds, query returns 1 (no observable change vs pre-US2 valid-cred path). **(Test option syntax is space-separated `lazy_validation true`, not `=true`; matches DuckDB ATTACH option convention.)**
- [X] T031 [US2] Ran `GEN=ninja make test-all` (which exercises the new test); 13 assertions in this test alone, all green; full sweep 3549/117 green.

**Checkpoint**: US2 ships alongside US1. After this point, ATTACH UX matches expectation (bad creds fail fast).

---

## Phase 5: User Story 3 — Result stream registry → catalog (Priority: P2)

**Goal**: Dissolve `MSSQLResultStreamRegistry` singleton; move its state (mutex + `unordered_map<string, unique_ptr<MSSQLResultStream>>`) into `MSSQLCatalog` as private members. `mssql_scan` `Bind` / `InitGlobal` call `catalog.RegisterStream(...)` / `catalog.RetrieveStream(...)`. Orphan streams auto-cleaned at `~MSSQLCatalog`.

**Independent Test**: `test/cpp/test_result_stream_registry_isolation.cpp` — two concurrent instances, each running `mssql_scan` against different catalogs, UUIDs from one catalog are not retrievable from another. Verifies SC-006.

**Depends on**: US1 (catalog is the natural owner; no per-instance container abstraction needed). Orthogonal to US2.

### Implementation for User Story 3

- [X] T032 [US3] Added `mutable std::mutex streams_mutex_;` + `std::unordered_map<std::string, std::unique_ptr<MSSQLResultStream>> active_streams_;` to `MSSQLCatalog` (`src/include/catalog/mssql_catalog.hpp:236-237`). Added `#include "query/mssql_result_stream.hpp"` + `<mutex>` / `<string>` headers.
- [X] T033 [US3] Added public methods `RegisterStream(std::unique_ptr<MSSQLResultStream>) -> std::string` and `RetrieveStream(const std::string &uuid) -> std::unique_ptr<MSSQLResultStream>` (`src/include/catalog/mssql_catalog.hpp:142-148`).
- [X] T034 [US3] `MSSQLCatalog::RegisterStream` in `src/catalog/mssql_catalog.cpp` uses `UUID::ToString(UUID::GenerateRandomUUID())`, locks `streams_mutex_`, emplaces, returns UUID string.
- [X] T035 [US3] `MSSQLCatalog::RetrieveStream` does atomic find+erase under one lock; returns moved unique_ptr or nullptr on miss.
- [X] T036 [US3] `mssql_scan` Bind (`src/mssql_functions.cpp:130`) now looks up the catalog via `Catalog::GetCatalog(context, bind_data->context_name).Cast<MSSQLCatalog>()` (the same lookup already happens at line 120 for validation) and calls `mssql_catalog.RegisterStream(...)`. **Field type changed**: `MSSQLScanBindData::result_stream_id` is now `string` (was `uint64_t`); only the active call site needed updating because `MSSQLCatalogScanBindData::result_stream_id` and `mssql::TableScanBindData::result_stream_id` are vestigial (declared + copied but never registered/retrieved — separate cleanup, not US3 scope).
- [X] T037 [US3] `mssql_scan` InitGlobal (`src/mssql_functions.cpp:147`) uses the same catalog lookup pattern; `mssql_catalog.RetrieveStream(...)`. Empty-string check (`result_stream_id.empty()`) replaces the previous `!= 0`.
- [X] T038 [US3] Deleted `MSSQLResultStreamRegistry` class definition in `src/include/mssql_functions.hpp:113-131` and implementation in `src/mssql_functions.cpp:36-64`. Confirmed via grep: only history comments remain (in `src/include/catalog/mssql_catalog.hpp` and `test/cpp/test_result_stream_registry_isolation.cpp`).
- [X] T039 [US3] `GEN=ninja make test-all` green: **3549 assertions / 117 cases** (22 env-gated skips). Identical baseline to US2.

### Tests for User Story 3

- [X] T040 [US3] Wrote `test/cpp/test_result_stream_registry_isolation.cpp` — plain `int main()` style matching the US1.E tests (DuckDB has no Catch2 harness for tests linking the DuckDB shared library). 50 iterations of interleaved `mssql_scan` calls across two DuckDB instances, each with its own MSSQL catalog ATTACHed as `mssql`. Each iteration verifies row count = TOP N from both instances. Direct catalog-state instrumentation is unavailable (extension loaded via dlopen; symbols not addressable from test main), so verification is behavioral. Added Makefile targets `test-result-stream-registry-isolation` + `test-spec047-us3`.
- [X] T041 [US3] `make test-result-stream-registry-isolation` green on first run. 50 pairs of scans, every result = 5 rows.

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

## Phase 7: Security Hardening (post PR #118 review)

**Purpose**: Address security-design items surfaced during PR #118 external review (see `spec.md` Clarifications Session 2026-05-18). Adds FR-012 (TokenCache key namespacing), FR-013 (`mssql_close_all()`), credential-redaction grep gate for diagnostic output (SC-005 extension), and noexcept audit + RAII contract for the teardown chain.

**Independent Tests**: `test/cpp/test_token_cache_isolation.cpp` (SC-011), `test/sql/diagnostic/pool_stats_no_credentials.test` (SC-005 redaction), `test/sql/diagnostic/close_all.test` (FR-013 smoke).

**Depends on**: US1 (catalog ownership), US4 (deprecation labelling baseline). Can ship in same PR.

### Implementation: FR-012 TokenCache key namespacing

- [X] T046a [US-SEC] Change cache key in `src/azure/azure_token.{hpp,cpp}`: replace `std::unordered_map<std::string, CachedToken>` (keyed by `secret_name`) with `std::unordered_map<std::pair<uintptr_t, std::string>, CachedToken, PairHash>` keyed by `(reinterpret_cast<uintptr_t>(&database_instance), secret_name)`. Add `PairHash` specialization or use `boost::hash_combine` equivalent (DuckDB has a similar pattern in `common/types.hpp` — check). All `TokenCache::Get` / `TokenCache::Put` / `TokenCache::Invalidate` call sites take an additional `DatabaseInstance &` (or `ClientContext &` from which we extract `context.db.get()`). Plumb through callers in `src/azure/azure_secret_reader.cpp`, `src/azure/azure_token.cpp`, `src/connection/mssql_connection_provider.cpp` (FEDAUTH path), and any other token-acquisition site. **(Done in commit `8996866`. Inline `PairHash` in header — boost::hash_combine-equivalent, libstdc++ has no std::hash<pair>. FedAuthStrategy gained `DatabaseInstance &db_` member so its connection-refill InvalidateToken/IsTokenExpired calls hit the right namespace. Callers found: azure_token.cpp (AcquireToken), auth_strategy_factory.cpp (BuildTokenAcquirer lambda + CreateFedAuth), fedauth_strategy.cpp (InvalidateToken + IsTokenExpired), mssql_catalog.cpp:729 (OnDetach). `azure_secret_reader.cpp` / `mssql_connection_provider.cpp` did NOT need plumbing — they don't touch TokenCache directly. Build + unit + integration green.)**
- [X] T046b [US-SEC] Update `OnDetach` Azure-cache-invalidation path in `src/catalog/mssql_catalog.cpp` (~line 622) to invalidate only entries matching `(this->db_instance_id, secret_name)`, not all entries with `secret_name`. Prevents inadvertent cross-instance cache invalidation. **(Done in commit `8996866` — actual line 729 in current file; passes `*context.db` to scope the invalidation.)**
- [X] T046c [US-SEC] Inline comment in `src/azure/azure_token.hpp` documenting the namespaced key, the `~DatabaseInstance` non-reaping decision (entries become unreachable on instance death, evicted by TTL), and the FR-012 / clarification reference. **(Done in commit `8996866` — header block above `class TokenCache` explains the namespacing and TTL-eviction rationale.)**

### Implementation: FR-013 mssql_close_all() bulk reset

- [ ] T046d [US-SEC] Add `int64_t MSSQLConnectionHandleManager::CloseAll()` method in `src/include/connection/mssql_diagnostic.hpp` + `src/connection/mssql_diagnostic.cpp`: atomic under existing manager mutex, walks the handle map, closes each `TdsConnection`, returns the count of closed handles. Idempotent.
- [ ] T046e [US-SEC] Register `mssql_close_all()` scalar function in `RegisterMSSQLDiagnosticFunctions` (`src/connection/mssql_diagnostic.cpp`). Returns `INTEGER`. Description starts with `[DEPRECATED]` (per FR-010 group) and points at FR-013 rationale.
- [ ] T046f [US-SEC] Add row for `mssql_close_all` in `CLAUDE.md` "Extension Functions" table with `[DEPRECATED]` marker; update CHANGELOG note in T044 to mention `mssql_close_all` as the recommended shutdown hook.

### Tests for security hardening

- [X] T046g [US-SEC] Write `test/cpp/test_token_cache_isolation.cpp` (SC-011). Single Catch2 case: construct two `DuckDB` instances, in each register a secret named `mssql_secret` with different `client_secret` values via a test-only fake auth-server endpoint (httplib stub). Trigger token acquisition in A first, then B; assert B receives B's secret value, not A's cached token. Fails on `main`-at-kickoff (cache aliasing); passes after T046a. Add to `CMakeLists.txt`. **(Done — plain `int main()` style matching US1.E / US3 convention; DuckDB has no Catch2 harness for tests linking the DuckDB shared library. DEVIATION from task letter: direct-cache test (not httplib stub). Verifies the FR-012 invariant at the cache-contract boundary — same scope as the SC-006 stream registry test (user explicitly accepted behavioral testing in US3 close-out). 5 sub-assertions: cross-instance reads return per-instance tokens; HasValidToken honors the namespace including the C-never-set negative case; Invalidate(A) leaves B intact; A re-set under the same key does not touch B; tenant-suffixed keys (`secret[:tenant]`, the FedAuthStrategy pattern) also independently namespaced. Compiles `src/azure/azure_token.cpp` together with the driver; stubs HttpPost / ReadAzureSecret / AcquireInteractiveToken so the test stays free of httplib / OpenSSL / Secret-API deps. Makefile target `test-token-cache-isolation` + meta `test-spec047-us-sec`. First-run green.)**
- [ ] T046h [US-SEC] Write `test/sql/diagnostic/pool_stats_no_credentials.test` (SC-005 credential-redaction gate). For each auth method available in CI (minimum SQL auth; FEDAUTH and Kerberos only if their env vars are set), `ATTACH` with a known sentinel substring in the credential (e.g., `Password=PWSENTINEL_xyz` for SQL auth; sentinel token / keytab-path for the others); then `SELECT * FROM mssql_pool_stats();` and assert via `query II` + `string_contains` that no column value contains the sentinel substring.
- [ ] T046i [US-SEC] Write `test/sql/diagnostic/close_all.test` (FR-013 smoke). Open N handles via `mssql_open`; call `mssql_close_all()` and assert returned count equals N; second call returns 0; subsequent `mssql_ping(any_handle)` errors (handle gone).
- [ ] T046j [US-SEC] Run the security suite: `./build/debug/test/unittest "test_token_cache_isolation*"` + `./build/debug/test/unittest "test/sql/diagnostic/pool_stats_no_credentials.test"` + `./build/debug/test/unittest "test/sql/diagnostic/close_all.test"`. All pass.

### Teardown-contract hardening (post-PR-118 review item #4)

- [ ] T046k [US-SEC] noexcept audit of the teardown chain. Mark explicitly `noexcept` (or verify implicit noexcept via C++17 rules): `~MSSQLCatalog`, `~ConnectionPool`, `~TdsConnection`, `~TdsSocket`, and (if it owns OpenSSL state) `~TlsContext`. Touched files: `src/include/catalog/mssql_catalog.hpp`, `src/include/tds/tds_connection_pool.hpp`, `src/include/tds/tds_connection.hpp`, `src/include/tds/tds_socket.hpp`, `src/include/tds/tls/tds_tls.hpp`. Any user-defined destructor without explicit `noexcept`: add it. Any destructor body that calls a potentially-throwing function: wrap in `try { ... } catch (...) { /* log and swallow */ }`. A throw from these destructors invokes `std::terminate` during `~AttachedDatabase` unwind.
- [ ] T046l [US-SEC] Add debug-only assert in `~ConnectionPool` (`src/tds/tds_connection_pool.cpp`): `D_ASSERT(checked_out_count_.load() == 0)` (using DuckDB's `D_ASSERT`). Pool destruction with checked-out connections violates the DuckDB quiescence contract — surface it as an invariant failure in debug builds instead of UB. Release build: forcibly close underlying sockets without blocking; do NOT join threads holding checked-out connections (they'll see EBADF on next read).
- [ ] T046m [US-SEC] Add doc-comment at the top of `src/include/tds/tds_connection_pool.hpp` `class ConnectionPool`: explicit contract for destruction — "Sockets close immediately on pool destruction. In-flight TDS requests on other threads observe connection-reset on next read. Server-side rollback of open transactions happens via TCP FIN within seconds. No graceful TDS ATTENTION cancel is sent (would require cross-thread write to the connection's socket — unsafe). See spec 047 Constraints / non-goals; cooperative cancellation is tracked as a follow-up spec."

**Checkpoint**: Phase 7 lands. Spec 047 closes all 5 in-scope items from the PR #118 security review. The 2 out-of-scope items (credential zeroization → issue #119; cooperative cancellation → future spec) are documented in Constraints / non-goals.

---

## Phase 8: User Story US-AN — Custom Application Name (Priority: P3, issue #82)

**Story goal**: User-supplied `Application Name=foo` in connection string (and in MSSQL secrets) reaches SQL Server's `program_name` so ops can attribute sessions in `sys.dm_exec_sessions` and Profiler.

**Independent test**: ATTACH with `Application Name=MyApp` → `SELECT APP_NAME() FROM mssql_scan('s', 'SELECT 1')` returns `MyApp`. SC-012 covers 5 sub-cases.

**Dependencies**: None on US1-4 / US-SEC. Touches only `MSSQLConnectionInfo` parsing and the four auth strategies. Can land any time after Phase 1 — no foundational blocker.

### Implementation for User Story US-AN

- [ ] T060 [US-AN] Add `string application_name;` (default empty) to `MSSQLConnectionInfo` in `src/include/mssql_storage.hpp`. Place alongside `database` / `user` / `password` near line 47.
- [ ] T061 [US-AN] In `MSSQLConnectionInfo::FromConnectionString` (ADO.NET branch, `src/mssql_storage.cpp`): recognize `Application Name`, `ApplicationName`, `App Name` (all case-insensitive). Use `StringUtil::CIEquals` (already in use elsewhere in the parser). Lookup populates `info.application_name`.
- [ ] T062 [US-AN] In the URI branch of the same function: recognize `applicationname` query parameter (no spaces in URI keys per URL convention). Populate `info.application_name`.
- [ ] T063 [US-AN] In `MSSQLConnectionInfo::FromSecret` (`src/mssql_storage.cpp`): read both `application_name` (underscore form, matches existing convention) and `applicationname` secret fields; the first non-empty wins.
- [ ] T064 [US-AN] Plumb `application_name` into all four authentication strategies via an extended constructor parameter (verified plumbing: today none of the strategies see `MSSQLConnectionInfo` — they receive individual fields from `AuthStrategyFactory::Create`). For each strategy: add `const std::string &app_name` constructor parameter (default `""`); store as private member `app_name_`; in `GetLogin7Options()` set `options.app_name = app_name_.empty() ? "DuckDB MSSQL Extension" : app_name_;` (replaces today's hardcoded `"DuckDB"` or `"DuckDB MSSQL Extension"`).
  - `src/include/tds/auth/sql_auth_strategy.hpp:23` constructor + `src/tds/auth/sql_auth_strategy.cpp:15` impl + `:32` `options.app_name = "DuckDB"` site.
  - `src/include/tds/auth/manual_token_strategy.hpp` constructor + `src/tds/auth/manual_token_strategy.cpp:17` impl + `:50` `options.app_name = "DuckDB"` site.
  - `src/include/tds/auth/fedauth_strategy.hpp` constructor + `src/tds/auth/fedauth_strategy.cpp:19` impl + `:36` `options.app_name = "DuckDB"` site.
  - `src/include/tds/auth/integrated_auth_strategy.hpp:27` constructor (header-only class) + `:52` `opts.app_name = "DuckDB MSSQL Extension"` site (already correct default, but parameterize).
  Then `src/tds/auth/auth_strategy_factory.cpp` — 4 construction sites at lines 94, 107, 135 (`CreateSqlAuth`), 141 (`CreateFedAuth`), 152 (`CreateManualToken`): each gets `conn_info.application_name` appended as the new last argument. `CreateSqlAuth`/`CreateFedAuth`/`CreateManualToken` helper signatures also gain the new param.
- [ ] T065 [US-AN] Add 128-char clamp + DEBUG log line in one place (recommended: a tiny helper `ResolveAppName(const MSSQLConnectionInfo &info)` inline in `src/include/mssql_storage.hpp` or near the auth-strategy fan-out). SQL Server clamps `program_name` to 128; truncating client-side keeps the value the user sees in `APP_NAME()` consistent with what we sent.

### Tests for User Story US-AN

- [ ] T066 [US-AN] Write `test/sql/attach/application_name.test` (SC-012). Five test groups:
  1. ATTACH with `Application Name=MyHugrApp` → `APP_NAME()` returns `MyHugrApp`.
  2. Same with `App Name=` and `ApplicationName=` variants.
  3. ATTACH without the key → `APP_NAME()` returns `DuckDB MSSQL Extension` (unified default).
  4. `CREATE SECRET` carrying `application_name = 'SecretApp'` + ATTACH via secret → `APP_NAME()` returns `SecretApp`.
  5. 200-char value → `APP_NAME()` returns the first 128 chars (`LEN(APP_NAME()) = 128`).
  Gate on `MSSQL_TEST_DSN`; skip cleanly when unset.
- [ ] T067 [US-AN] Update `CLAUDE.md` "ATTACH Options & Secret Parameters (Catalog Filters)" table: add a row for `application_name` describing the supported key variants and the LOGIN7 propagation; cross-link to issue #82.

**Checkpoint**: Phase 8 lands. Issue #82 closed. No interaction with the ownership refactor.

---

## Phase 9: Polish & Cross-Cutting Concerns

**Purpose**: Audit gates, docs, bench, final test pass, PR prep.

- [ ] T047 [P] SC-004 grep audit: `grep -rn 'MssqlPoolManager\|MSSQLContextManager\|MSSQLResultStreamRegistry' src/ src/include/`. Expect zero matches (excluding comment-only mentions tagged with "removed in spec 047"). Note: `MSSQLConnectionHandleManager` is NOT in this grep — it stays.
- [ ] T048 [P] Write `specs/047-process-state-cleanup/state_inventory.md` (FR-007 deliverable). Final classification table of every process-wide static after implementation: 0 "migrate" entries; legitimate set = Winsock + OpenSSL + thread-local scratch (×2) + Azure TokenCache + MSSQLConnectionHandleManager (with deprecation note linking to FR-010).
- [ ] T050 [P] Update `CLAUDE.md` "Key Architecture Concepts" → "Connection pool" bullet to reflect per-catalog ownership. Current text mentions "per attached database" — make explicit that pool lifetime is bounded by catalog lifetime (RAII via `unique_ptr`); deleted singletons; no shared cross-instance pool state.
- [ ] T051 [P] Update `CLAUDE.md` "Recent Changes" with spec 047 entry summarizing: 3 singletons removed (pool manager, context managers, result stream registry); handle manager stays + deprecated functions; ATTACH credential validation; closes issue #96; bench-neutral; no public API regression.
- [ ] T052 Run bench parity gate: `MSSQL_BENCH_ROW_COUNT=1000000 MSSQL_BENCH_DUCKDB_BIN=$(pwd)/build/release/duckdb MSSQL_BENCH_OUTPUT=/tmp/bench_codec_e2e_spec047_run1.txt bash test/bench/bench_codec_e2e.sh` × 3 runs, min-of-3 per step. Compare to baseline captured in T002. Save to `specs/047-process-state-cleanup/bench_results.md`. Every step within ±5% (SC-007 gate).
- [ ] T053 clang-format-14 sweep over all touched files: `/opt/homebrew/opt/llvm@14/bin/clang-format -i src/catalog/mssql_catalog.{cpp,hpp} src/catalog/mssql_transaction.cpp src/include/tds/tds_connection_pool.hpp src/tds/tds_connection_pool.cpp src/include/tds/tds_connection.hpp src/include/tds/tds_socket.hpp src/mssql_storage.cpp src/include/mssql_storage.hpp src/mssql_functions.{cpp} src/include/mssql_functions.hpp src/connection/mssql_diagnostic.cpp src/include/connection/mssql_diagnostic.hpp src/azure/azure_token.{cpp,hpp} src/azure/azure_secret_reader.cpp src/dml/insert/mssql_insert_executor.cpp src/dml/update/mssql_update_executor.cpp src/query/mssql_result_stream.cpp src/query/mssql_query_executor.cpp src/tds/auth/sql_auth_strategy.cpp src/tds/auth/manual_token_strategy.cpp src/tds/auth/fedauth_strategy.cpp src/include/tds/auth/integrated_auth_strategy.hpp` plus headers under `src/include/`.
- [ ] T054 Final full-suite test gate: `GEN=ninja make test && GEN=ninja make integration-test`. All previously-green tests still green; all new tests (T023, T024, T030, T040, T046g, T046h, T046i, T066) green; SC-006-related stream isolation green; SC-010 ATTACH validation green; SC-011 TokenCache isolation green; SC-005 credential-redaction green; SC-012 Application Name green.
- [ ] T055 Write `specs/047-process-state-cleanup/pr_description.md`: Summary; the 3 production bug classes fixed (Scenarios 1/2/3); issue #96 closure (Scenario 4 + SC-009); ATTACH validation (FR-011 + SC-010); 3 singletons deleted vs 1 kept-deprecated; **security hardening**: FR-012 TokenCache namespacing + SC-011 isolation test, FR-013 `mssql_close_all()`, SC-005 credential redaction in `mssql_pool_stats`, noexcept teardown chain audit; bench parity; test plan; follow-ups (issue #119 / spec 049 lazy credential materialization; future spec for cooperative TDS cancellation; eventual removal of `mssql_open/close/ping/close_all` closes the handle manager singleton too).
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
- **Phase 7 (Security hardening)** — depends on US1 (the noexcept audit + ~ConnectionPool contract target the new ownership chain) + US4 (FR-013 deprecation label coordinates with FR-010 group). FR-012 TokenCache work is orthogonal — can land independently. SC-005 redaction grep depends on US1's `mssql_pool_stats` rewrite.
- **Phase 8 (US-AN — Application Name)** — fully independent of US1-4 / US-SEC. Touches only `MSSQLConnectionInfo` parsing and the four auth strategies (no overlap with the singleton-cleanup files). Can land any time after Phase 1.
- **Phase 9 (Polish)** — depends on US1 + US3 (singleton-deletion gate) + Phase 7 (security hardening must be in for SC-004 grep + SC-008 inventory) + Phase 8 (US-AN must be done for SC-012 in the final test gate, if shipping in this PR). US2 and US4 must be done if they're shipping in this PR. Bench (T052) must run on release build with full implementation applied.

### Story-Level Parallel Execution

**Within US1**: T015, T016, T017, T018 are `[P]` — different files, no interdependency. After T007-T014 land, run them concurrently.

**Across stories**: After US1 lands (T007-T022), US2 (Phase 4), US3 (Phase 5), US4 (Phase 6), and most of US-SEC (Phase 7) can proceed in parallel — each touches different files / responsibilities. T030 (US2 test) is independent of T040 (US3 test) and T046 (US4 test). FR-012 / TokenCache work (T046a-c, T046g) is orthogonal to all of US1/2/3/4 (different subsystem); can start any time after T022. T046k-m (noexcept audit + RAII contract) wait for US1 ownership-chain stabilization (post-T022). **US-AN (T060-T067)** is fully orthogonal — different files entirely (`MSSQLConnectionInfo` + 4 auth strategies, no overlap with singleton-cleanup files) — can start any time after Phase 1; recommended start: in parallel with US2/US3/US4 or even before, by a separate implementer.

**Within Polish**: T047, T048, T050, T051 are all `[P]` — different files, different concerns.

### Critical Path (longest single-thread chain)

T001 → T002 → T003 → T004 → T005 → T006 → T007 → T008 → T009 → T010 → T011 → T012 → T013 → T014 → T020 → T021 → T022 → T023 → T024 → T025 → T026 → T027 → T028 → T028a → T029 → T032 → ... → T046k → T046l → T052 → T054 → T055 → T056.

Tasks T015-T018 (parallel within US1), T030-T046 (cross-story parallelism after US1), T046a-c + T046g (FR-012 TokenCache work in parallel with US2/3/4), and T047-T051 (polish parallelism) cut wall-clock substantially below the critical-path length.

---

## Implementation Strategy

### MVP First (just US1)

If pressure exists to ship the issue #96 fix ASAP:

1. Run Phase 1 + Phase 2 + Phase 3 (T001-T025) only.
2. Phase 8 polish (T047, T050, T051, T053, T054, T055, T056) — minimal subset.
3. Skip Phase 4 (US2), Phase 5 (US3), Phase 6 (US4), Phase 7 (US-SEC) — defer to follow-up PRs.

Result: issue #96 closed; production bug fixed; ATTACH still lazy-validates (today's behavior preserved); stream registry still singleton; diagnostic functions not yet labeled deprecated; security-review items not yet addressed (TokenCache aliasing remains; no `mssql_close_all`; no noexcept audit). MVP path leaves a debt — only viable if the security items are tracked separately and don't block the production fix.

### Incremental Delivery (recommended)

Ship the full spec in one PR following the natural order: Phase 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 9. ATTACH validation (US2), stream registry move (US3), the security-hardening pack (Phase 7), and US-AN (Phase 8 — issue #82 Application Name) are small enough to land alongside US1 without bloating the PR. US4 is essentially documentation — trivial to include. US-AN is fully orthogonal — different files entirely; can land first, last, or in parallel with anything.

### Parallel-Team Execution

If multiple implementers available:
- **Implementer A** (lead): Phase 1 + 2, then US1 critical path (T007-T014, T019-T022).
- **Implementer B**: After US1 T022 lands, US2 (T026-T031) in parallel with US3.
- **Implementer C**: US3 (T032-T041) in parallel with US2; US4 (T042-T046) can start any time (no blocking); Phase 7 FR-012 work (T046a-c, T046g) is fully orthogonal — can start immediately after T022.
- **Any implementer**: Phase 7 noexcept + RAII contract (T046k-m) after US1 ownership-chain stabilizes (post-T022); SC-005 redaction grep (T046h) after US1 pool_stats rewrite (post-T019).
- **Implementer D (or any)**: US-AN (T060-T067) any time after Phase 1 — touches no file owned by Phases 3-7.
- **Implementer A or any**: Phase 9 polish after US1+2+3+Phase 7+Phase 8 land.

### Risk Mitigation

- **Build break early in US1**: After T007 (type change), the build will be broken until T011 (Initialize built the pool) lands. Sequence T007-T011 as a single tight loop; don't commit T007 alone.
- **Stale singleton references**: T020 + T021 (deletions) must come AFTER all callers are migrated (T013, T014, T015-T018). Otherwise build breaks.
- **Test fixture missing `MSSQL_TEST_DSN`**: new C++ tests (T023, T024, T040) MUST gate on env-var presence to skip cleanly in CI environments without SQL Server (same pattern as existing integration tests).
- **Bench regression > 5%**: investigate; if `pool.Acquire() + Release()` on ATTACH is the culprit (FR-011), it's expected and should be in noise (one extra round-trip per ATTACH, not per row). Otherwise root-cause before merge.

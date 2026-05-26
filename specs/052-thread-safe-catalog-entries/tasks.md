# Tasks: Thread-Safe Catalog Entry Lifetime

**Input**: Design documents from `/specs/052-thread-safe-catalog-entries/`

**Prerequisites**: [plan.md](./plan.md), [spec.md](./spec.md), [research.md](./research.md), [data-model.md](./data-model.md), [contracts/ownership.md](./contracts/ownership.md), [quickstart.md](./quickstart.md)

**Tests**: Included — FR-009 requires the C++ repro test, and SC-001..SC-005 require sanitizer-passing scenarios. Test tasks are first-class deliverables, not optional.

**Organization**: Tasks are grouped by user story so each story can be implemented and verified independently against issue #126.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- C++ source: `src/`, headers `src/include/` (mirror layout)
- Tests: `test/cpp/`, SQL `test/sql/`, bench `test/bench/`
- Per CLAUDE.md: ALWAYS build with `GEN=ninja make …`

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Capture baseline measurements before any code changes so SC-006 (perf budget ±10%/±20%) can be evaluated objectively.

- [ ] T001 Record v0.2.0 perf baseline: run `make docker-up && GEN=ninja make integration-test 2>&1 | tail -3` on a clean `main` checkout, capture wallclock; write to `specs/052-thread-safe-catalog-entries/bench_results.md` under a "Baseline (main @ da0204c)" section. No code changes.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Type-level changes that all three user stories build on. Both entry classes must support `shared_from_this()` and the catalog must own the graveyard before any caller can switch from `unique_ptr` to `shared_ptr`.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [X] T002 [P] Add `std::enable_shared_from_this<MSSQLTableEntry>` inheritance to `MSSQLTableEntry` (alongside existing `TableCatalogEntry`) in `src/include/catalog/mssql_table_entry.hpp`. Add `#include <memory>` if not already present. Verify build (no usage yet — just enables `shared_from_this()` for later phases).
- [X] T003 [P] Add `std::enable_shared_from_this<MSSQLSchemaEntry>` inheritance to `MSSQLSchemaEntry` (alongside existing `SchemaCatalogEntry`) in `src/include/catalog/mssql_schema_entry.hpp`. Add `#include <memory>` if not already present.
- [X] T004 Add per-catalog graveyards + helpers to `MSSQLCatalog` — depends on T002 and T003 (graveyards hold `shared_ptr` of these types). In `src/include/catalog/mssql_catalog.hpp`: add `private` fields `vector<shared_ptr<MSSQLTableEntry>> table_graveyard_`, `vector<shared_ptr<MSSQLSchemaEntry>> schema_graveyard_`, `std::mutex graveyard_mutex_`; and public methods `void AppendToTableGraveyard(vector<shared_ptr<MSSQLTableEntry>> retired)`, `void AppendToSchemaGraveyard(shared_ptr<MSSQLSchemaEntry> retired)`, `size_t GetTableGraveyardSize() const`, `size_t GetSchemaGraveyardSize() const`. In `src/catalog/mssql_catalog.cpp`: implement all four methods using `std::lock_guard<std::mutex>` on `graveyard_mutex_`. `Append*` MUST be `noexcept` (per spec-047 teardown invariant). Verify build.

**Checkpoint**: Foundation ready — entries are `shared_ptr`-friendly and the catalog has a graveyard. US1/US2/US3 can now proceed.

---

## Phase 3: User Story 1 — Eliminate the concurrent-first-load UAF (Priority: P1) 🎯 MVP

**Goal**: Two threads racing to first-load the same table never destroy each other's entry. `LoadSingleEntry` (table) and `LookupSchema` (schema) become emplace-only; `entries_` and `schema_entries_` switch to `shared_ptr` ownership.

**Independent Test**: `test/cpp/test_concurrent_reads.cpp` scenario 4 (4 threads × 50 catalog-bound reads) passes under UBSan with zero `invalid vptr` reports across 10 consecutive runs (SC-001). TSan run also clean (SC-002).

### Implementation for User Story 1

- [X] T005 [P] [US1] Change `MSSQLTableSet::entries_` value type from `unique_ptr<MSSQLTableEntry>` to `shared_ptr<MSSQLTableEntry>` in `src/include/catalog/mssql_table_set.hpp`. Change `CreateTableEntry` return type from `unique_ptr<MSSQLTableEntry>` to `shared_ptr<MSSQLTableEntry>`. Add forward declaration / include for `shared_ptr` if needed. (Depends on T002 — entry must inherit `enable_shared_from_this` before being put into shared_ptr.)
- [X] T006 [US1] In `src/catalog/mssql_table_set.cpp`: update `CreateTableEntry` to use `make_shared<MSSQLTableEntry>(...)` (was `make_uniq`); update `LoadSingleEntry` to call `entries_.emplace(name, std::move(entry))` instead of `entries_[entry->name] = std::move(entry)` (line 190); on `emplace` collision return result, do `return entries_.find(name)->second.get()` so concurrent first-load callers all return the winner's entry. (Depends on T005.)
- [X] T007 [US1] In `src/catalog/mssql_table_set.cpp::Scan` (the bulk-loaded path, line 138-143): replace `entries_[entry->name] = std::move(entry)` with `auto [it, inserted] = entries_.emplace(entry->name, std::move(entry)); callback(*it->second);` — winner-wins on Scan-vs-LoadSingleEntry races too. (Depends on T005.)
- [X] T008 [P] [US1] Change `MSSQLCatalog::schema_entries_` value type from `unique_ptr<MSSQLSchemaEntry>` to `shared_ptr<MSSQLSchemaEntry>` in `src/include/catalog/mssql_catalog.hpp` (line 231). (Depends on T003.)
- [X] T009 [US1] In `src/catalog/mssql_catalog.cpp::LookupSchema` (line 380-388): replace `schema_entries_[schema_name] = std::move(entry)` with `auto [it, inserted] = schema_entries_.emplace(schema_name, std::move(entry)); return it->second.get();`; switch `make_uniq<MSSQLSchemaEntry>` (line 386) to `make_shared<MSSQLSchemaEntry>`. (Depends on T008.)
- [X] T010 [US1] Build with `GEN=ninja make debug` then run `./build/debug/test/unittest "test/cpp/test_concurrent_reads.cpp"` 10× in a loop under UBSan (existing scenario 4 — no test changes needed). Confirm 10/10 clean (was failing pre-fix). Document the before/after in `bench_results.md`. (Depends on T006, T007, T009.)

**Checkpoint**: US1 done — concurrent first-load UAF gone, scenario 4 passes 10/10 under UBSan. SC-001 + SC-002 met for this scenario.

---

## Phase 4: User Story 2 — Survive concurrent invalidation (Priority: P1) 🎯 MVP

**Goal**: `Invalidate()` / `RefreshCache()` / DDL hooks / TTL expiry don't destroy entries that in-flight binders are using. Retired entries move to the per-catalog graveyard; the binder's bind-data anchor keeps them alive through query execute.

**Independent Test**: New scenario 5 in `test/cpp/test_concurrent_reads.cpp` (4 readers + 1 `mssql_refresh_cache` invalidator at 50ms cadence) runs for 30 s with zero crashes / sanitizer reports (SC-003).

### Implementation for User Story 2

- [X] T011 [US2] Rewrite `MSSQLTableSet::Invalidate()` in `src/catalog/mssql_table_set.cpp` (line 202-215): instead of `entries_.clear()`, drain `entries_` into a local `vector<shared_ptr<MSSQLTableEntry>> retired; retired.reserve(entries_.size()); for (auto &kv : entries_) retired.push_back(std::move(kv.second)); entries_.clear();` and pass `std::move(retired)` to `schema_.GetMSSQLCatalog().AppendToTableGraveyard(...)`. `attempted_tables_.clear()` and the names path stay as-is. (Depends on T004, T005.)
- [X] T012 [US2] In `src/catalog/mssql_catalog.cpp::OnDetach` (line ~445): replace `schema_entries_.erase(info.name)` with `auto it = schema_entries_.find(info.name); if (it != schema_entries_.end()) { AppendToSchemaGraveyard(std::move(it->second)); schema_entries_.erase(it); }` so DETACH-while-bind-in-flight on the schema entry survives until the bind completes. (Depends on T004, T008.)
- [X] T013 [US2] In `src/catalog/mssql_catalog.cpp::InvalidateMetadataCache` (line 838-840), `InvalidateSchemaTableSet` (line 851-854), `RefreshCache` (line 914-917): each one walks `schema_entries_` and calls `.GetTableSet().Invalidate()` — no change needed at these call sites since the TableSet now self-routes to the graveyard via T011. Verify the call chain by reading code; add a brief comment on each call site referencing "`Invalidate()` routes to catalog graveyard, in-flight binders retain validity". (Depends on T011.)
- [X] T014 [US2] Add `shared_ptr<MSSQLTableEntry> table_entry_anchor_` field to `MSSQLCatalogScanBindData` in `src/include/mssql_functions.hpp` (next to the existing `optional_ptr<TableCatalogEntry> table_entry` at line 87). Update `MSSQLCatalogScanBindData::Copy()` (around line 108) to copy the anchor (`result->table_entry_anchor_ = table_entry_anchor_;` — a shared_ptr copy = atomic refcount inc, exactly the spec-mandated cost). `Equals()` does NOT compare anchors (table_name + schema_name + context_name already define equality; anchor is purely a lifetime device).
- [X] T015 [US2] In `src/catalog/mssql_table_entry.cpp::GetScanFunction` (line 88): immediately after `catalog_bind_data->table_entry = this;` add `catalog_bind_data->table_entry_anchor_ = shared_from_this();`. This is the single line that anchors the entry for the entire execute phase. (Depends on T002, T014.)
- [X] T016 [US2] Add **scenario 5** to `test/cpp/test_concurrent_reads.cpp`: spawn 4 reader threads each running a tight loop of `SELECT * FROM mssql.dbo.t LIMIT 10` for 30 seconds; spawn 1 invalidator thread calling `mssql_refresh_cache('mssql')` every 50ms (use `std::this_thread::sleep_for(50ms)`). Test passes if no crash, no UBSan report, no TSan report, and total reader iterations are within ±20% of a no-invalidator baseline run (measure with `std::atomic<size_t>` counters). (Depends on T011-T015.)

**Checkpoint**: US2 done — concurrent invalidation safe, scenario 5 clean 30s soak. SC-003 met.

---

## Phase 5: User Story 3 — Audit and harden sibling caches (Priority: P2)

**Goal**: The other catalog-cache layers (`MSSQLMetadataCache`, `MSSQLStatisticsProvider`) do not regress under the same race patterns. Per [research.md § Decision 5]: `MSSQLMetadataCache::GetTableMetadata` returns a raw pointer but all current callers copy immediately — we **document and assert** the contract rather than refactor. `MSSQLStatisticsProvider::GetRowCount` is already by-value — confirm and pin via comment. `MSSQLCatalog::schema_entries_` (the schema set) is already handled in US1/US2 (T008/T009/T012).

**Independent Test**: New scenario 6 (scenario 5 + 1 thread running `duckdb_schemas()` / `duckdb_tables()` in a loop) runs 30 s clean under UBSan + TSan (SC-004).

### Implementation for User Story 3

- [X] T017 [P] [US3] Document the `MSSQLMetadataCache::GetTableMetadata` raw-pointer-handout contract in `src/include/catalog/mssql_metadata_cache.hpp` (lines 133-136): add a `// LIFETIME CONTRACT (spec 052): …` comment block stating the returned pointer is valid only until the next `Invalidate*` / bulk reload call on this cache; callers MUST copy fields out (as `MSSQLTableSet::LoadSingleEntry` and `Scan` already do) and MUST NOT store the raw pointer beyond the immediate call site.
- [X] T018 [P] [US3] In `src/catalog/mssql_metadata_cache.cpp::GetTableMetadata`: add a debug-only `D_ASSERT(mutex_.try_lock() == false /* held by caller-side serialisation */); mutex_.unlock();` — no, simpler: add a `// Caller MUST NOT outlive the schemas_ mutex` comment + a single-line static guard via `#ifdef DEBUG` that records the current call's expected scope. Actually simplest: just a doc comment + a `D_ASSERT(table_meta == nullptr || schemas_.count(schema_name))` post-condition reminding maintainers. Pick the lowest-overhead form that's reviewable.
- [X] T019 [P] [US3] Pin the `MSSQLStatisticsProvider::GetRowCount` contract: in `src/include/catalog/mssql_statistics.hpp` (around line 48) add a comment `// LIFETIME CONTRACT (spec 052): returns by value (idx_t); SAFE to call concurrently; no pointer-handout pattern present in this surface.` No code changes; this is a maintenance pin so a future refactor doesn't silently regress.
- [X] T020 [US3] Add **scenario 6** to `test/cpp/test_concurrent_reads.cpp`: scenario 5 + a 6th thread running `SELECT * FROM duckdb_schemas() WHERE database_name = 'mssql'` and `SELECT * FROM duckdb_tables() WHERE database_name = 'mssql'` alternately in a 30s loop. Test passes if no crash / sanitizer report. (Depends on T016.)

**Checkpoint**: US3 done — sibling caches confirmed safe (one by fix, two by audit). All three user stories functional and independently tested. SC-004 met.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Test infrastructure, perf gates, documentation, PR readiness.

- [ ] T021 [P] Create `test/cpp/test_catalog_graveyard.cpp` per `quickstart.md` § 8: insert N entries, hold raw pointers, call `Invalidate()`, verify `entries_` empty + graveyard size = N + raw pointers still valid; then drop raw-pointer-holder + destruct catalog + verify entries freed (use a destructor-counter member on a test subclass of `MSSQLTableEntry`, or instrument with a static atomic counter under `#ifdef MSSQL_GRAVEYARD_TEST`). Add to `test/CMakeLists.txt` source list.
- [ ] T022 [P] Add `Makefile` target `test-concurrent-sanitizer`: builds `debug` with `-fsanitize=undefined -fsanitize=thread` (run UBSan and TSan as **separate** builds since they're mutually exclusive; macro: `make test-concurrent-ubsan` + `make test-concurrent-tsan`); runs `test_concurrent_reads.cpp` scenarios 4/5/6 against the docker container. Document in `Makefile` help block.
- [ ] T023 [P] Add `test/bench/bench_catalog_concurrent.cpp` per [contracts/ownership.md § I7]: 4 threads × 10000 binds against a hot single-table cache (`SELECT 1 FROM mssql.dbo.t LIMIT 1`); measure wallclock for total iterations. Add Makefile target `bench-catalog-concurrent` invoking the binary with `MSSQL_TESTDB_DSN` from env.
- [ ] T024 After T010+T016+T020+T021+T023: append a "Post-fix (052)" section to `specs/052-thread-safe-catalog-entries/bench_results.md` recording integration-test wallclock, scenario 4/5/6 iteration counts, and `bench-catalog-concurrent` numbers. Verify SC-005 (no regression) and SC-006 (±10%/±20%) gates.
- [ ] T025 [P] Update `CHANGELOG.md` `[Unreleased]` section with a spec-052 entry: "Fix dbt segfault under `threads >= 2` (issue #126). Catalog entries now use shared_ptr ownership; concurrent first-load is emplace-only; Invalidate routes retired entries to a per-catalog graveyard kept alive for the catalog's lifetime so in-flight binders cannot UAF."
- [ ] T026 [P] Verify [contracts/ownership.md PR checklist] line by line: run each grep listed there, confirm zero hits in mutation paths; verify every `MSSQLCatalogScanBindData` construction site sets `table_entry_anchor_`; record grep output snippets in `specs/052-thread-safe-catalog-entries/pr_description.md` as the validation receipt.
- [ ] T027 Run the full pre-merge gate: `GEN=ninja make test` + `make docker-up` + `GEN=ninja make integration-test` + `GEN=ninja make test-spec047-us1` + `GEN=ninja make test-spec047-us3` + `GEN=ninja make test-spec047-us-sec` + `GEN=ninja make test-token-cache-isolation`. Document any failures. (SC-005 gate.) (Depends on all prior tasks.)
- [ ] T028 Walk `specs/052-thread-safe-catalog-entries/quickstart.md` steps 1-9 manually one final time on a clean checkout of the branch; correct any drift. (No code edits unless drift is found.)
- [ ] T029 Draft PR description at `specs/052-thread-safe-catalog-entries/pr_description.md` per SC-007: include before/after UBSan trace, before/after TSan run, 30s scenario 5 soak result, perf-budget table, and links to issue #126 + this spec dir. Close issue #126 via `Closes #126` footer.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — must run before any code change so baseline is from clean `main`.
- **Foundational (Phase 2)**: Depends on Setup. **BLOCKS all user stories** — T005/T008 require T002/T003 (enable_shared_from_this); T011/T012 require T004 (graveyards).
- **User Stories (Phases 3-5)**: Depend on Foundational. Each story is independently testable (its own scenario in `test_concurrent_reads.cpp`) but US2 logically extends US1's ownership change (anchor in bind data only matters once entries are shared_ptr). Order: US1 → US2 → US3 in priority, though within-US tasks can parallelise on different files (see [P] markers).
- **Polish (Phase 6)**: Depends on all US phases complete + scenario tests green.

### User Story Dependencies

- **US1**: Standalone — emplace + shared_ptr ownership. Fully testable once T010 passes scenario 4 under UBSan.
- **US2**: Builds on US1's `shared_ptr` ownership (graveyard holds `shared_ptr<MSSQLTableEntry>` — only meaningful once T005 lands). Fully testable once T016 passes scenario 5.
- **US3**: Independent of US1/US2 implementation (it's an audit + comments + one new scenario), but scenario 6 piggy-backs on scenario 5 infra. Schedule alongside or after US2.

### Within Each User Story

- Header changes (entries_ value type, anchor field) MUST land before `.cpp` changes that use them.
- `LoadSingleEntry` `entries_[…] = …` → `entries_.emplace(…)` is one cohesive edit per file — sequential within US1's T005-T007.
- Tests are added AFTER the implementation in their phase (per repo's existing convention; this is a fix, not TDD-spec'd).

### Parallel Opportunities

- **Phase 2** (Foundational): T002 + T003 in parallel (different headers, different classes); T004 follows.
- **Phase 3** (US1): T005 + T008 in parallel (different headers); T006/T007 sequential after T005; T009 after T008; T010 after T006/T007/T009.
- **Phase 4** (US2): T011 (table set) and T012 (catalog OnDetach) in parallel after T004; T014 (header field add) parallel with T011/T012; T015 (anchor wire-up) needs T014.
- **Phase 5** (US3): T017/T018/T019 all in parallel (three different files, doc-only); T020 sequential after T016 (extends test file).
- **Phase 6**: T021/T022/T023/T025/T026 all in parallel (different files). T024 depends on the test/bench runs. T027/T028/T029 sequential at the end.

---

## Parallel Example: Phase 2 (Foundational)

```bash
# Two header edits truly independent — fire in parallel:
# Task: "Add enable_shared_from_this to MSSQLTableEntry in src/include/catalog/mssql_table_entry.hpp"
# Task: "Add enable_shared_from_this to MSSQLSchemaEntry in src/include/catalog/mssql_schema_entry.hpp"
# Then T004 once both land:
# Task: "Add graveyards + AppendTo* helpers + graveyard_mutex_ to MSSQLCatalog (.hpp + .cpp)"
```

## Parallel Example: User Story 1

```bash
# Header-level changes parallel:
# Task: "Change MSSQLTableSet::entries_ to shared_ptr in src/include/catalog/mssql_table_set.hpp"
# Task: "Change MSSQLCatalog::schema_entries_ to shared_ptr in src/include/catalog/mssql_catalog.hpp"
# Then per-file .cpp updates (sequential within each file, parallel across files):
# Task: "Update src/catalog/mssql_table_set.cpp LoadSingleEntry + Scan to use emplace"
# Task: "Update src/catalog/mssql_catalog.cpp LookupSchema to use emplace + make_shared"
```

---

## Implementation Strategy

### MVP First (US1 only)

1. Setup (T001 — capture baseline).
2. Foundational (T002-T004).
3. US1 (T005-T010).
4. **STOP and VALIDATE**: 10× UBSan run of scenario 4. If green — concurrent-first-load UAF gone. **This is sufficient to close issue #126's dbt repro** — dbt's segfault is the concurrent first-load race, not the invalidation race. US2 closes a related but independent UAF that dbt with default config does not trigger.

### Incremental Delivery (recommended for this spec)

1. Setup + Foundational → infrastructure ready.
2. US1 → scenario 4 clean → partial fix on its own branch HEAD.
3. US2 → scenario 5 clean → issue #126's user-visible symptom (dbt segfault) is now fixed.
4. US3 → scenario 6 clean → audit complete; all sibling caches confirmed safe.
5. Polish (Phase 6) → CHANGELOG / perf-record / PR description / final regression sweep.
6. Open PR with three sequential commits (US1 / US2 / US3) — easy review, easy bisect if a regression later surfaces.

### Parallel Team Strategy

For this fix the bulk of the change is in 3 files (`mssql_table_set.{cpp,hpp}`, `mssql_catalog.{cpp,hpp}`, `mssql_table_entry.cpp` + `mssql_functions.hpp`); two engineers can work side-by-side at most. One on US1 (entries_ + emplace + LookupSchema), one on US2 (Invalidate + graveyard + anchor) once Foundational is done. US3 is doc-only and can be done by either at the end.

---

## Notes

- [P] tasks = different files, no dependencies.
- All builds use `GEN=ninja make …` per repo convention.
- Every `.cpp` change should be followed by a local `GEN=ninja make debug` to catch ODR / template instantiation issues early (recall: extension is C++11-ABI-compatible despite C++17 source — `enable_shared_from_this` is C++11, safe).
- Commit per task or per logical group (US1 as one commit, US2 anchor as one commit, etc.) — easier bisect than one mega-commit.
- Do NOT skip Phase 1 baseline — without it SC-006 cannot be evaluated objectively.
- Per [Clarifications Q3 2026-05-26]: do NOT add a `mssql_pool_stats` graveyard column, no debug WARNING. Internal invariant only.
- Per [Clarifications Q4 2026-05-26]: do NOT ship `test/dbt/`. C++ scenarios 4/5/6 cover it.

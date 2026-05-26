# Feature Specification: Thread-Safe Catalog Entry Lifetime

**Feature Branch**: `052-thread-safe-catalog-entries`

**Created**: 2026-05-26

**Status**: Draft

**Input**: User description: Fix the thread-safety / use-after-free bug in `MSSQLTableSet` that causes dbt segfaults with threads ≥ 2 (issue #126). Two distinct races on `entries_` map of `unique_ptr<MSSQLTableEntry>`: (1) `LoadSingleEntry` unconditionally overwrites existing entries — concurrent first-load of the same table destroys one thread's entry under another thread's feet; (2) `Invalidate()` clears the entire map, dangling every outstanding raw pointer the binder is holding. Both happen with the current pointer-handout model where the caller continues to use a raw pointer past the mutex-guarded lookup. Verified with UBSan in `test/cpp/test_concurrent_reads.cpp` scenario 4.

## Clarifications

### Session 2026-05-26

- Q: Should the lifetime mechanism cap the number of entries kept alive past their `Invalidate()` (graveyard size), and how is it observed? → A: No hard cap, no user-visible diagnostic surface. Mechanism is an internal invariant of the fix. Natural-bound: binds are short-lived (10s of ms), invalidation cadence under realistic workloads (dbt with N workers × M tables, TTL expiry, `mssql_refresh_cache()`) yields steady-state graveyard in single digits — self-collapses within milliseconds of binds releasing. Pathological growth (a leaked reference bug in the anchor mechanism) manifests as monotonic RSS growth, caught by standard OS-level / k8s memory monitoring + heap-profiler tools (`heaptrack`, Instruments) — a dedicated `mssql_pool_stats` column adds no actionable signal beyond what RSS already provides. No FR-009, no SC-008.
- Q: Must US3 (sibling-cache audit) land in the same release as US1+US2, or can it be deferred? → A: All three user stories ship in the same PR/release. Rationale: spec-047 showed singleton bugs surface in production long after the latent defect appears; sibling caches share the same raw-pointer-handout pattern and will UAF under analytic-workload growth. Unified fix while ownership model is fresh is cheaper than a repro-driven follow-up. US3's "P2" priority denotes lower repro confidence (no UBSan trace yet), NOT shippability deferral — SC-004 is a hard release gate.
- Q: Should the spec require shipping a real dbt project in `test/dbt/` as a smoke artifact? → A: No. C++ stress tests (scenarios 4/5/6) reproduce the exact race window dbt hits — concurrent readers + invalidator in a tight loop. Shipping a real dbt project adds ~50 MB of Python deps, a cross-platform CI dependency, and a live DB requirement for what is fundamentally the same race that the C++ harness already covers. Drop SC-007. The dbt scenario remains the canonical motivating workload (per Assumptions) but acceptance is gated entirely on the C++ stress suite.

## User Scenarios & Testing *(mandatory)*

### User Story 1 — Eliminate the concurrent-first-load UAF (Priority: P1) 🎯 MVP

A developer running parallel queries (dbt with `threads: 2+`, an analytics service issuing concurrent reads, a CI job warming up multiple DuckDB connections against a freshly-attached catalog) must not crash when two threads independently bind a query referencing the same SQL Server table that is not yet in the in-memory metadata cache.

**Why this priority**: This is the failure mode currently reproduced by `test/cpp/test_concurrent_reads.cpp` scenario 4 (4 threads × 50 catalog-bound reads). It needs **no DDL** and **no external trigger** — just concurrent first-binds against a table neither thread has seen before. Every dbt user with `threads ≥ 2` hits this on the first concurrent run after attach.

**Independent Test**: `make test-concurrent-reads` scenario 4 passes (currently aborts under UBSan with `invalid vptr` on `MSSQLTableEntry` member access). Running the same test under TSan must also report zero data races on the entries map.

**Acceptance Scenarios**:

1. **Given** a freshly attached MSSQL catalog with empty in-memory entry cache, **When** 4 threads simultaneously bind `SELECT * FROM mssql.dbo.t` for the same previously-unloaded table `t`, **Then** every binder receives a stable reference to one consistent `MSSQLTableEntry` whose lifetime survives the bind/execute of every concurrent caller, with no UBSan trap and no TSan race report.
2. **Given** two threads binding different unloaded tables `t1` and `t2` concurrently, **When** both load metadata in parallel, **Then** both binds succeed independently and both entries remain valid for the duration of every subsequent query that references them.
3. **Given** the test runs at 8 threads × 25 iterations against a single small table, **When** the run completes, **Then** zero allocations of `MSSQLTableEntry` are leaked (verified via post-DETACH heap snapshot) and the entries map contains exactly one entry per distinct table seen.

---

### User Story 2 — Survive concurrent invalidation (Priority: P1) 🎯 MVP

A developer running parallel queries while a sibling thread (or a DDL statement on the same DuckDB connection) triggers a metadata invalidation — `mssql_refresh_cache()`, `CREATE TABLE` through DuckDB, `DROP TABLE`, the catalog-cache TTL expiring, the spec-047 `InvalidateSchemaTableSet` hook — must not crash. Binders holding entry references obtained before the invalidation must continue to see those entries (or a stable replacement) for the remainder of their current bind/execute, and only re-resolve on the NEXT bind.

**Why this priority**: A real UAF in its own right, independent of the US1 dbt failure mode. The crash window is `entries_.clear()` inside `Invalidate()` while a binder holds a raw pointer obtained earlier. Triggers: explicit `mssql_refresh_cache()`, TTL boundary when `mssql_catalog_cache_ttl > 0`, and the DDL hooks that fire `InvalidateSchemaTableSet`/`InvalidateMetadataCache` whenever DuckDB issues a `CREATE TABLE`/`DROP TABLE`. dbt does not exercise this path under its default config (per-worker connection serialises DDL, and `mssql_refresh_cache` is not in dbt's hot loop) — but any application that mixes reads with metadata refreshes is exposed today, and the fix is small enough that splitting it out of this spec would be artificial. P1 because the underlying ownership model has to land coherently with US1; you can't have shared_ptr-owned entries that an unrelated code path still `clear()`s out from under binders.

**Independent Test**: New stress scenario 5 in `test_concurrent_reads.cpp`: N reader threads in a tight loop of catalog-bound SELECTs while a separate writer thread calls `mssql_refresh_cache()` every 50ms (synthetic stress — dbt with default config does NOT do this; the scenario isolates the invalidation race so we can prove US2 actually closes it). Currently this is expected to UAF on the first invalidation hit; after this story, it must run for 30 s with zero crashes and zero TSan races.

**Acceptance Scenarios**:

1. **Given** 4 reader threads issuing catalog-bound SELECTs in a tight loop, **When** a 5th thread calls `mssql_refresh_cache('mssql')` mid-flight, **Then** every in-flight reader finishes its current query without crash; subsequent queries pick up the refreshed metadata.
2. **Given** a binder has resolved `MSSQLTableEntry` for table `t` and is mid-`EnsurePKLoaded`, **When** another thread triggers `InvalidateSchemaTableSet("dbo")`, **Then** the binder's reference remains valid until the binder releases it; the entry's memory is reclaimed only when the last reference goes away.
3. **Given** the spec-047 catalog-cache TTL expires while a query is mid-bind, **When** the TTL-triggered invalidation fires, **Then** the in-flight bind completes against the pre-TTL entry; the next bind resolves fresh metadata.

---

### User Story 3 — Audit and harden sibling caches (Priority: P2 — ships in same release as US1+US2)

The same raw-pointer-handout pattern likely exists in adjacent caches that the binder touches: `MSSQLSchemaSet`, `MSSQLMetadataCache`, `MSSQLStatisticsProvider`. A robust fix audits all of them and applies the same lifetime model wherever the binder receives a non-owning reference and the underlying object can be destroyed by concurrent invalidation.

**Why this priority**: After US1+US2 land, `MSSQLTableSet` is safe but the binder still touches sibling caches. Any one of them hitting the same UAF leaves the fix incomplete. "P2" denotes lower repro confidence (no UBSan trace on a sibling cache yet) — NOT shippability deferral. All three user stories ship in the same PR/release; SC-004 is a hard release gate (per Clarifications 2026-05-26).

**Independent Test**: Extended stress scenario 6: 4 reader threads + 1 invalidator + 1 schema-list-querier (`duckdb_schemas()`, `duckdb_tables()` calls in a loop). Must run for 30 s with zero crashes / TSan races. A subset of the fix per cache can be enabled independently to confirm each fix removes its specific class of report.

**Acceptance Scenarios**:

1. **Given** the spec-047 `mssql_pool_stats` enumeration walks every attached MSSQL catalog, **When** another thread `DETACH`es one mid-walk, **Then** the enumeration finishes cleanly (the detached catalog either appears or doesn't, no UAF).
2. **Given** `MSSQLStatisticsProvider::GetStatistics` is called concurrently by 4 binders for the same table, **When** the cache TTL expires mid-call, **Then** every call returns either the pre-TTL statistic or the freshly-loaded one — no UAF, no torn read.
3. **Given** `MSSQLSchemaSet::GetEntry` returns a `MSSQLSchemaEntry*` to one thread while another thread calls `InvalidateMetadataCache`, **Then** the first thread's schema reference remains valid for its bind/execute scope.

---

### Edge Cases

- **Two threads first-load the SAME table, both finish metadata fetch, both attempt to insert**: the winner's entry is preserved; the loser's entry is discarded silently. Both binders return references to the winning entry. Pre-fix: the loser overwrites and destroys the winner's entry → UAF.
- **Two threads first-load DIFFERENT tables in parallel**: both succeed; both entries land in the map; both references stable. No interaction.
- **A binder holds an entry reference and the user calls `mssql_refresh_cache()` before the binder finishes**: the binder's reference remains valid; on its next access, it picks up the new entry. The map shows the new entries to subsequent lookups.
- **A binder holds an entry reference and the catalog is DETACHed mid-flight**: the catalog destructor must not be reachable while bindings against it are outstanding. DuckDB's quiescence contract should prevent this; if violated, the fix should at least fail loudly (debug `D_ASSERT`) rather than silently UAF.
- **The entries-cache TTL fires while a binder is mid-`EnsurePKLoaded`**: the binder's reference must survive until it releases the reference; TTL-driven invalidation is deferred behind the same lifetime gate as explicit `Invalidate()`.
- **The map rehashes on insert under concurrent reader**: irrelevant — `unordered_map` rehash moves the `unique_ptr` slot, not the underlying object. The current bug is `unique_ptr` destruction on overwrite, not rehash relocation. This must remain irrelevant post-fix.
- **Pool exhaustion during `LoadSingleEntry` metadata fetch**: existing behavior — throws `IOException`, no entry inserted, no race introduced. Post-fix this must be preserved.
- **A binder gets a reference and the entry is found to be stale (metadata changed in SQL Server externally)**: out of scope for this fix; the existing "stale read" surface (between TTL refresh windows) is unchanged.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: `MSSQLTableSet::GetEntry` MUST return references to `MSSQLTableEntry` whose lifetime is bounded by the caller's scope, NOT by the next `Invalidate()` / `entries_.clear()` / `entries_[name] = move(...)` call from another thread.
- **FR-002**: `MSSQLTableSet::LoadSingleEntry` MUST NOT destroy an existing `MSSQLTableEntry` when a concurrent thread has already created one for the same table — the existing entry MUST win, the late-arriving entry MUST be discarded, every caller MUST receive a reference to the winning entry.
- **FR-003**: `MSSQLTableSet::Invalidate` MUST NOT destroy `MSSQLTableEntry` objects that are currently referenced by any in-flight caller. Destruction MUST be deferred until the last outstanding reference is released. Subsequent lookups via `GetEntry` MUST see a fresh (empty) map and reload on demand.
- **FR-004**: The same lifetime guarantees in FR-001 / FR-002 / FR-003 MUST apply to `MSSQLSchemaSet` (per US3) and to any other catalog cache that hands a non-owning reference to a binder and exposes invalidation (`MSSQLMetadataCache::Invalidate*`, `MSSQLStatisticsProvider` row-count + histogram caches).
- **FR-005**: The fix MUST work within DuckDB's existing `optional_ptr<CatalogEntry>` API surface — no DuckDB binder changes, no new DuckDB hooks required. If a thread-local "anchor" container is needed to extend reference lifetime through the bind/execute, it MUST live entirely inside the extension and MUST NOT leak into DuckDB-side bind data.
- **FR-006**: The fix MUST preserve the current cache TTL semantics: when TTL > 0 and a TTL-driven invalidation fires, future lookups MUST see fresh metadata; the in-flight binder is allowed (and EXPECTED — see edge case) to finish against the pre-invalidation entry.
- **FR-007**: The fix MUST preserve the lazy-load semantics: first reference to a table MUST trigger metadata fetch; subsequent references MUST hit the cache; the fetch path MUST remain mutex-protected so two threads asking for the same table do not both issue the SQL Server round trip (existing `load_mutex_` invariant).
- **FR-008**: The fix MUST NOT regress single-threaded performance: existing `make test-spec047-us1`, `test-spec047-us3`, integration tests, and full `make test` MUST stay green; runtime within ±10% of pre-fix baseline.
- **FR-009**: A repro test MUST be checked in at `test/cpp/test_concurrent_reads.cpp` (or a successor file) that fails before this fix and passes after. The test MUST cover both US1 (concurrent first-load) and US2 (concurrent invalidation injection). The test MUST run under UBSan and TSan in addition to the regular harness.

### Key Entities

- **`MSSQLTableEntry`**: A single SQL Server table's catalog representation (columns, PK info, statistics handle). Today held by `unique_ptr` in `MSSQLTableSet::entries_`; binders consume via raw pointer returned from `GetEntry`. Lifetime currently bounded by the next overwrite / clear of `entries_`; post-fix lifetime must be bounded by "no outstanding references remain".
- **`MSSQLTableSet::entries_` map**: The in-memory cache of loaded table entries keyed by table name, scoped to one schema. Current type `unordered_map<string, unique_ptr<MSSQLTableEntry>>`; post-fix the value type must support shared ownership OR the map+entry pair must be replaced by an entry-lifetime mechanism that decouples map state from entry destruction.
- **`MSSQLSchemaEntry`** + **`MSSQLSchemaSet::entries_`**: Same shape as `MSSQLTableEntry` / `MSSQLTableSet` at the schema level (US3 audit target).
- **`MSSQLMetadataCache`**: The shared per-catalog metadata cache that backs `LoadSingleEntry` / `LoadAllTableMetadata`. Already has multiple mutexes for concurrent loads; needs a separate audit pass for the raw-pointer-handout pattern in its own GetXxx APIs.
- **`MSSQLStatisticsProvider`**: Per-catalog statistics cache. Already mutex-guarded for the row-count map; needs a per-entry lifetime guarantee for histogram pointers if they are returned by raw pointer to the binder.
- **Outstanding-reference anchor**: A new conceptual object (thread-local, query-scoped, or refcount-on-entry — implementation strategy choice in `plan.md`) that holds the entry alive across the gap between `GetEntry` returning and the binder releasing the reference.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: `make test-concurrent-reads` scenario 4 (4 threads × 50 catalog-bound reads) PASSES under UBSan with **zero** `invalid vptr`/UAF reports across 10 consecutive runs.
- **SC-002**: `make test-concurrent-reads` scenario 4 PASSES under ThreadSanitizer with **zero** data-race reports against `MSSQLTableSet::entries_`, `LoadSingleEntry`, or any of the lifetime-extended members.
- **SC-003**: New stress scenario 5 (4 readers + 1 `mssql_refresh_cache()` invalidator at 50 ms cadence) runs for **30 s** with **zero** crashes / TSan races / UBSan traps; total iterations completed match the expected throughput within ±20% of the no-invalidation baseline.
- **SC-004**: New stress scenario 6 (sibling-cache audit: readers + invalidator + `duckdb_schemas()` / `duckdb_tables()` loop) runs for **30 s** clean. Each individual sibling-cache fix can be toggled and verified independently.
- **SC-005**: Pre-existing acceptance tests stay green: `make test-spec047-us1`, `test-spec047-us3`, `test-spec047-us-sec`, `test-token-cache-isolation`, `make test`, `make integration-test`. **Zero** regressions.
- **SC-006**: `make integration-test` total runtime within **±10%** of the v0.2.0 baseline (`da0204c` on `main`) on the local Docker SQL Server harness. Concurrent-path latency (new microbench: 4 threads × 1000 simple binds against a hot cache) within **±20%** vs v0.2.0.
- **SC-007**: Issue #126 closes as fixed. PR description includes the UBSan trace before/after, the TSan run before/after, and the 30-second concurrent-invalidation soak result. The dbt scenario described in the issue is the canonical motivating workload; it is exercised via the C++ stress harness rather than a checked-in dbt project (per Clarifications 2026-05-26).

## Assumptions

- DuckDB's binder honors its own quiescence contract: a `MSSQLCatalog` is not destroyed while in-flight binds hold references to its `MSSQLTableEntry` objects via the DuckDB-side `optional_ptr<CatalogEntry>`. Spec 047 added a `noexcept` teardown audit and a debug-only `D_ASSERT(active_connections_.empty())`; this spec assumes the same destruction-ordering rules apply to catalog teardown vs. in-flight binders.
- The fix can live entirely inside the extension. No DuckDB upstream change is needed: the extension owns its catalog cache, defines its own ownership semantics, and only hands DuckDB a non-owning `optional_ptr<CatalogEntry>` whose pointed-to object the extension keeps alive on its own schedule.
- dbt-duckdb's threading model is the worst-case workload: N worker threads, one shared `DatabaseInstance`, per-thread `Connection` objects, model materialization (DDL) interleaved with `dbt test` reads. If the fix passes this, it passes the canonical analytic-workload concurrency profile.
- TTL-driven invalidation is acceptable behavior: a binder mid-bind across a TTL boundary uses the pre-TTL entry; the next bind picks up the new one. This is the documented and tested behavior pre-spec-052; this spec preserves it.
- `make test-concurrent-reads` already exists (from the issue #126 investigation) and is the canonical regression test for this fix. New scenarios 5 and 6 land in the same file. CI gets a new `test-concurrent-reads` target invoked from `make test-all` and from a `workflow_dispatch`-only sanitizer job (UBSan + TSan run lazily — they're slow).
- Performance budget: the lifetime mechanism (whether `shared_ptr`, generation counter, or graveyard) adds at most one atomic operation per `GetEntry` call on the cache-hit fast path. Catalog-bound bind is already dominated by SQL Server round-trip cost; an extra atomic is invisible.
- Spec 047 invariants stay intact: per-catalog `unique_ptr<ConnectionPool>`, no process-wide singletons reintroduced, `noexcept` teardown chain preserved. This spec extends ownership rules INSIDE the catalog but does not change the catalog-vs-process boundary.

Closes [issue #126](https://github.com/hugr-lab/mssql-extension/issues/126).

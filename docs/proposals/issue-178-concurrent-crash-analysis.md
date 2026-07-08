# Issue #178 — Concurrent same-table queries crash (Python / DuckDB.NET, Windows): analysis & fix plan

Status: analysis complete, fix plan proposed. Date: 2026-07-08. Main @ `f7ddb6e`.

## TL;DR

1. **Two of the three DuckDB versions the reporter tested ship pre-spec-052 extension
   binaries that contain the already-fixed concurrent-read UAF (#126).** Verified
   empirically via `INSTALL mssql FROM community` + `mssql_version()`:

   | DuckDB | community `mssql` binary | spec 052 fix (#127, PR merged 2026-05-27)? |
   |---|---|---|
   | 1.5.2 | **0.1.18** (2026-04-14) | NO — contains #126 crash |
   | 1.5.3 | **0.2.0** (2026-05-20) | NO — contains #126 crash |
   | 1.5.4 | 0.2.1 (2026-06-05) | yes |

   The reported symptom (two threads reading the **same table** through the attached
   catalog segfault; one thread fine) is exactly the #126 / spec-052 signature
   ("dbt threads>=2 segfault", concurrent `MSSQLTableSet::GetEntry` on one table).

2. **The minimal 2-SELECT repro on 1.5.4 (= 0.2.1) could not be reproduced** on
   macOS (release) or Linux (debug + ASan/UBSan + TSan), including a purpose-built
   8-connection same-table stress over the previously-untested abandoned-stream
   path. The report's evidence is Windows-only (Windows Store Python 3.11,
   DuckDB.NET) and **CI has zero Windows concurrency coverage**.

3. **TSan found two crash-grade mutex-mismatch bugs on main / v0.2.1 (D6, D7)**:
   `MSSQLMetadataCache::Refresh` frees the whole `schemas_` map under `mutex_`
   while all readers synchronize on `schemas_mutex_` (use-after-free), and
   `MSSQLTableSet::Scan` mutates `known_table_names_` under `entry_mutex_` while
   everyone else uses `names_mutex_` (container corruption). Both fire when
   reads run concurrently with `mssql_refresh_cache` / invalidation / catalog
   enumeration — a plausible match for the reporter's real 1.5.4 workload even
   though their minimal repro doesn't hit these paths. Additional non-crash
   defects: D1 (missing `stability = VOLATILE`), D2 (raw pointer into cache),
   D3–D5 (destructor contract, unsynchronized config fields, debug-logger races).

## Reproduction attempts (none crashed, except as noted)

Environment: SQL Server 2022 in Docker (`mssql-dev`), tables: 500k-row PK table,
200k-row heap (no PK) table. `LIMIT 10` on those tables abandons the TDS stream
mid-flight → exercises Cancel/ATTENTION/drain/pool-reuse.

| # | Build | Platform | Scenario | Result |
|---|---|---|---|---|
| 1 | main release (DuckDB v1.5.4) | macOS, python 1.5.4 | issue repro verbatim, 2–8 threads × 30 iters, same table | no crash |
| 2 | main release | macOS | exact-match: secret with `USE_ENCRYPT FALSE, CATALOG TRUE, SCHEMA_FILTER '^(dbo)$'`, heap table, LIMIT 10, 2 threads × 15 | no crash |
| 3 | community **0.1.18** (pre-052!) | macOS, python 1.5.2 | 8 threads × 50, warm + cold binds (cache dropped per round) | no crash (see note) |
| 4 | main debug **ASan+UBSan** | Linux arm64 (ubuntu 22.04) | new `test/sql/integration/issue178_concurrent_same_table.test`: `concurrentloop` 8 connections × same 500k-row table, LIMIT 10 | **PASS, sanitizer-clean** |
| 5 | main debug ASan+UBSan | Linux | spec-052 harness `test_concurrent_reads` scenarios 1–8 (incl. 30s invalidation-race & sibling-cache soaks) | **ALL PASS** |
| 6 | main debug **TSan** | Linux | issue178 test (8 connections, same table) | test PASSES; 5 data races reported — none crash-grade (see D4/D5) |
| 7 | main debug **TSan** | Linux | spec-052 harness scenarios 1–8 | all PASS; **55 race reports → 2 CRASH-GRADE mutex-mismatch bugs (D6, D7)** + repeats of D4/D5 |
| 8 | main (pre-fix) debug **ASan** | Linux | harness scenario 6 (readers + `mssql_refresh_cache` invalidator + `duckdb_tables()` walker), repeated runs | **CRASHES: heap-use-after-free, 2 of 3 runs** — freed `MSSQLTableMetadata` read in `MakeTableInfo` (mssql_table_entry.cpp:46) while `Refresh()` freed the map. This IS the D6 bug producing a hard segfault-class abort on v0.2.1-era code. |
| 9 | `fix/178-concurrency-races` | Linux | issue178 test + harness under TSan/ASan | **0 race reports** (was 5 + 55); functional flag-stomp bug in `Scan`-vs-`Invalidate` found & fixed via invalidation epoch guard |

Note on (3): #126 was only ever trapped by UBSan on Linux; a macOS release binary
not crashing is weak evidence of absence. The pre-052 UAF in 0.1.18/0.2.0 is
documented and fixed history, not a hypothesis.

macOS-local sanitizers were unusable during this work: on Darwin 25.5 the Xcode
clang ASan runtime hangs in `InitializeShadowMemory`/`get_dyld_hdr` and TSan
SIGSEGVs in `__tsan::SlotLock` at startup — hence the Linux container lab
(`docker/Dockerfile.build` derivative, arm64).

## Code audit (main @ f7ddb6e)

Audited for the two-concurrent-same-table-scans scenario: catalog entry lifetime
(`MSSQLTableSet::GetEntry` singleflight + `MSSQLBindAnchors`), `EnsurePKLoaded`,
metadata cache, statistics provider, catalog filter regexes, transaction manager,
`ConnectionPool` (acquire/release/cleanup thread), `ConnectionProvider`,
`MSSQLResultStream` cancel/drain/destructor, connection factory closures,
optimizer hooks, `MaxThreads()`. **No crash-grade race found on this path
post-052** — consistent with the sanitizer-clean Linux runs.

Real defects found (to fix regardless):

### D6. CRASH-GRADE (TSan-confirmed): `MSSQLMetadataCache::Refresh` guards `schemas_` with the WRONG mutex
The cache has two mutexes. Hot-path readers/writers of the `schemas_` map —
`GetTableMetadata` (:281), `EnsureSchemasLoaded` (:926), `ForEachTableInSchema`
(:787), `GetTableNames` (:261), `LoadAllTableMetadata` (:631),
`TryGetCachedSchemaNames` (:412), `InvalidateAll` (:1100) — all take
**`schemas_mutex_`**. But **`Refresh()` (:801) takes `mutex_` and does
`schemas_.clear()` (:808) + full rebuild**, and `HasSchema` (:398) / `HasTable`
(:403) also read `schemas_` under `mutex_`. There is **no mutual exclusion**
between `Refresh` and the entire read side: a refresh frees every
`MSSQLSchemaMetadata` node and `vector<MSSQLColumnInfo>` while a concurrent bind
iterates them → **use-after-free → segfault**. TSan caught the freed-vs-read
pairs live in harness scenarios 5/6 (32 `operator delete` race reports on
`_Hash_node<pair<string, MSSQLSchemaMetadata>>` and `vector<MSSQLColumnInfo>`).

Triggers: `mssql_refresh_cache()` concurrent with any query on the same catalog
(a documented, recommended call after external DDL — very plausible in notebook
/ .NET app workloads), and any future TTL-driven refresh path. NOT triggered by
the minimal 2-SELECT repro (that path never calls `Refresh`).

Fix: ONE mutex for all `schemas_` access (fold `mutex_`-guarded methods onto
`schemas_mutex_`, or vice versa), and audit every `schemas_` touch site.

### D7. CRASH-GRADE (TSan-confirmed): `known_table_names_` written under mismatched mutexes
`MSSQLTableSet::Scan` phase 1 inserts into `known_table_names_` at
mssql_table_set.cpp:152 while holding **`entry_mutex_`**; every other reader /
writer (`GetEntry` :86, `EnsureNamesLoaded` :341/:368, `Invalidate` :297,
`InvalidateEntry` :316) uses **`names_mutex_`**. Concurrent
`SHOW TABLES` / `duckdb_tables()` / any `Scan` + a refresh/invalidate →
unsynchronized `unordered_set` mutation → heap corruption. TSan caught
`Scan`-insert racing `Invalidate`-clear live (read stack:
`PhysicalTableScan::GetGlobalSourceState → MSSQLSchemaEntry::Scan → :152`;
write stack: `RefreshCache → MSSQLTableSet::Invalidate → :297`).

Fix: take `names_mutex_` around the insert in `Scan` phase 1 (lock order
`entry_mutex_` → `names_mutex_` matches the documented order in
`InvalidateEntry`).

### D1. Side-effecting scalar functions are constant-foldable
`src/catalog/mssql_refresh_function.cpp` registers `mssql_refresh_cache` and
`mssql_invalidate_cache` without `stability = FunctionStability::VOLATILE`
(default CONSISTENT). DuckDB constant-folds them:
* debug builds: `D_ASSERT(allow_unfoldable || result.GetVectorType() == CONSTANT_VECTOR)`
  fires in `expression_executor.cpp:136` (reproduced on Linux debug — the function
  writes a FLAT vector);
* release builds: the side effect executes at **plan time** and the result may be
  folded/cached — semantically wrong.

No function in `src/` sets `stability` at all → audit every side-effecting scalar
(`mssql_exec`, `mssql_open/close/ping/close_all`, auth-test functions, preload).

### D2. `MSSQLMetadataCache::GetTableMetadata` returns a raw pointer into the cache
`src/catalog/mssql_metadata_cache.cpp:276-395` returns `&table_it->second` /
`&table_meta`; `schemas_mutex_` is released on return, and the caller
(`MSSQLTableSet::LoadSingleEntry`) dereferences it afterwards
(`CreateTableEntry(*table_meta)`). Safe today only because the table-set
singleflight serialises same-table loads and map node pointers survive sibling
inserts — any future caller without those guarantees is a UAF. Copy out under the
lock (or provide a visit-under-lock API).

### D4. Unsynchronized config fields on `MSSQLMetadataCache` (TSan-confirmed)
`MSSQLCatalog::EnsureCacheLoaded` (mssql_catalog.cpp:933) calls
`SetMetadataTimeout` / `SetTTL` on **every catalog lookup**, writing
`metadata_timeout_ms_` (int) and `ttl_seconds_` (long) with **no lock**, while
concurrent threads read them mid-metadata-query (`ExecuteMetadataQuery`
mssql_metadata_cache.cpp:909, `EnsureSchemasLoaded` fast path :919). The
`EnsureSchemasLoaded` fast path also reads `schemas_load_state_` /
`schemas_last_refresh_` outside `schemas_mutex_` (unsynchronized double-checked
locking). Not crash-grade (same-value scalar rewrites in steady state), but
formal UB and it pollutes every future TSan run. Fix: make the two fields
`std::atomic`, and make the DCL fast path an atomic load (or drop the fast path
— the mutex is cheap here).

### D5. Lazy-init `static int level` debug-logger race (TSan-confirmed, project-wide)
`GetCatalogDebugLevel` / `GetDebugLevel` / `GetExecutorDebugLevel` etc. use
`static int level = -1; if (level == -1) level = atoi(getenv(...))` — concurrent
first calls race on the plain int (TSan reports 3 instances; the pattern exists
in ~10 files). Benign in practice (same value written), formal UB. Fix once:
`static const int level = [] { ... }();` (C++11 magic statics) in a shared
header, or `std::atomic<int>`.

### D3. `~MSSQLResultStream` may run outside the owning query
The destructor releases the connection via `ConnectionProvider::ReleaseConnection`
→ `MetaTransaction::Get(context)`. Host apps can destroy query results after the
query/transaction is gone (.NET reader disposal, Python GC); today a catch-all
downgrades that to "drop connection instead of pooling it". Works by accident —
make the no-active-transaction path explicit.

## Fix plan

### Phase 0 — reporter follow-up (issue comment, no code)
1. Ask for `SELECT mssql_version();` from the **crashing 1.5.4** environment — was
   0.2.1 actually exercised, or was the crash observed on 1.5.2/1.5.3 (= 0.1.18 /
   0.2.0, both pre-fix) and extrapolated?
2. Explain the version mapping above; recommend DuckDB 1.5.4 + `FORCE INSTALL
   mssql FROM community` and re-test.
3. If it still crashes on 0.2.1: request a native stack
   (`python -X faulthandler ...` or WinDbg `!analyze -v`), the table's row count &
   schema (does `LIMIT` abandon a large stream?), and SQL Server version.

### Phase 1 — Windows concurrency CI (highest leverage)
The crash evidence is Windows-only and `concurrency-tests.yml` runs only on
ubuntu. Add a `windows-latest` job: MSVC build (existing `x64-windows-static-release`
config), SQL Server 2022 Express (choco silent install, TCP enabled) or LocalDB
with TCP; run `test_concurrent_reads` scenarios + the new
`issue178_concurrent_same_table.test`. If it reproduces → root-cause there
(prime suspects: Winsock teardown timing in cancel/drain, `WSAPoll` semantics).

### Phase 2 — TSan sweep
`THREADSAN=1` Linux build (in progress in the lab); if clean, add a
`workflow_dispatch` TSan variant to `concurrency-tests.yml` so races that don't
corrupt the heap stay catchable.

### Phase 3 — deterministic fixes (independent of repro), in priority order
1. **D6 (crash-grade)**: unify `schemas_` locking in `MSSQLMetadataCache` on one
   mutex; audit all `schemas_` touch sites (`Refresh`, `HasSchema`, `HasTable`
   are on the wrong mutex today).
2. **D7 (crash-grade)**: `Scan` phase 1 must take `names_mutex_` around the
   `known_table_names_` insert.
3. **D2**: `GetTableMetadata` → copy-out-under-lock (or callback API) — removes
   the raw-pointer-into-cache contract that D6-style bugs feed on.
4. **D1**: set `stability = VOLATILE` on all side-effecting scalars (+ the
   `SetVectorType` contract review). Small, testable, fixes a real release-mode bug.
5. **D3**: explicit transaction-less release path in `~MSSQLResultStream`.
6. **D4**: atomics for `metadata_timeout_ms_` / `ttl_seconds_` + fix the
   `EnsureSchemasLoaded` double-checked fast path.
7. **D5**: replace the lazy-init debug-level pattern with a magic-static in one
   shared helper. After D4–D7 the extension is TSan-clean on these paths — keep
   it that way via the Phase-2 CI job.
8. Keep `test/sql/integration/issue178_concurrent_same_table.test` (added by this
   work); add an abandoned-stream scenario 9 (big table + LIMIT) AND a
   reads-vs-`mssql_refresh_cache` scenario 10 (deterministic D6 repro) to
   `test_concurrent_reads.cpp`; wire everything into the concurrency CI job.

Note on issue #178 attribution: D6 is a **proven hard crash** on v0.2.1-era code —
ASan aborts with heap-use-after-free in 2 of 3 harness scenario-6 runs (reads +
`mssql_refresh_cache` + `duckdb_tables()` walker). Workloads mixing reads with
refresh/invalidation/catalog enumeration (notebook variable explorers, IDE
introspection, dbt) hit exactly this — a very plausible match for the reporter's
real notebook / .NET workloads on 1.5.4, even though their minimal 2-SELECT
repro is sanitizer-clean on that build. Phase 0 confirms or refutes.

Additional fix shipped with D7: `Scan`'s unconditional trailing
`names_loaded_/is_fully_loaded_ = true` stomped a concurrent `Invalidate()`,
leaving TRUE flags over cleared containers — `GetEntry` then reported existing
tables as missing ("Table ... does not exist", harness scenario 6). Fixed with
an invalidation epoch counter: `Invalidate`/`InvalidateEntry` bump it,
`Scan` publishes the flags only if the epoch is unchanged.

### Phase 4 — release & docs
* Ship as v0.2.2 for DuckDB 1.5.4+.
* README/troubleshooting note: concurrency requires extension ≥ 0.2.1; DuckDB
  1.5.2/1.5.3 community channels permanently serve pre-fix binaries (0.1.18 /
  0.2.0) — the only remedy is upgrading DuckDB.
* If Phase 1/2 uncovers a new race: fix + DATAMODEL.md invariant update in the
  same PR (per project policy).

## Artifacts from this investigation
* New regression test: `test/sql/integration/issue178_concurrent_same_table.test`
  (8-connection same-table `concurrentloop`, big-table LIMIT → abandoned-stream path).
* Linux sanitizer lab: ubuntu 22.04 container, repo copy + vcpkg + `GEN=ninja make debug`
  (ASan/UBSan) and `GEN=ninja THREADSAN=1 DUCKDB_PLATFORM=linux_arm64 make debug` (TSan).
* Known-broken: macOS Darwin 25.5 + current Xcode clang sanitizer runtimes
  (ASan hangs at init, TSan SIGSEGV at init) — use the Linux lab.

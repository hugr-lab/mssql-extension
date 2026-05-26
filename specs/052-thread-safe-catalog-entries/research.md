# Research — Thread-Safe Catalog Entry Lifetime

**Spec**: [spec.md](./spec.md) · **Plan**: [plan.md](./plan.md)

## Open questions resolved

All FRs are pinned in the spec; the open design decisions were:

1. **Which ownership model fixes the UAF?** — see [Decision 1](#decision-1-ownership-model-shared_ptr--per-catalog-graveyard).
2. **How does the binder anchor refs across the bind→execute boundary?** — see [Decision 2](#decision-2-extend-the-extension-owned-bind-data-with-a-shared_ptr-anchor).
3. **When does the graveyard drain?** — see [Decision 3](#decision-3-graveyard-drains-on-catalog-destruction-no-runtime-gc).
4. **What does `LoadSingleEntry` do on concurrent first-load?** — see [Decision 4](#decision-4-loadsingleentry--lookupschema-use-emplace-only-semantics).
5. **What sibling caches need the same treatment?** — see [Decision 5](#decision-5-sibling-cache-audit-scope).
6. **What's the perf budget cost?** — see [Decision 6](#decision-6-performance-budget).

---

## Decision 1: Ownership model — `shared_ptr` + per-catalog graveyard

**Decision**: change `MSSQLTableSet::entries_` from `unordered_map<string, unique_ptr<MSSQLTableEntry>>` to `unordered_map<string, shared_ptr<MSSQLTableEntry>>`. Same for `MSSQLCatalog::schema_entries_`. Add a `vector<shared_ptr<MSSQLTableEntry>> graveyard_` (and a sibling `vector<shared_ptr<MSSQLSchemaEntry>>`) to `MSSQLCatalog` as the per-catalog destruction-deferral container.

**Rationale**:

- The UAF is rooted in `unique_ptr`'s single-owner semantics: when the map drops/overwrites, destruction happens **immediately**, even if the binder holds a raw pointer.
- `shared_ptr` co-ownership trivially fixes the race: drop the map's shared_ptr → entry stays alive while any other holder (graveyard, bind data) keeps a reference.
- `optional_ptr<CatalogEntry>` is a DuckDB-side **non-owning** wrapper; we keep returning a raw pointer extracted from `shared_ptr::get()`. No DuckDB API change required.
- The graveyard is the lifetime extension mechanism: `Invalidate()` moves the old entries into the graveyard before clearing the live map. Outstanding raw pointers (held by binders) remain valid because the graveyard's shared_ptr keeps the objects alive.

**Alternatives considered**:

| Alternative | Why rejected |
|---|---|
| **Generation counter + re-resolve on stale** | DuckDB binder has no "check generation before each access" hook. We'd have to wrap every member access with `entry->is_current_generation()` checks — invasive and still racy on the check-then-use window. |
| **Refcount-tracking graveyard with explicit acquire/release** | Same DuckDB cooperation gap — we'd need every binder use-site to call `acquire/release`. DuckDB doesn't give us those hooks. Reduces to (a) but with hand-rolled refcount instead of shared_ptr. |
| **`unique_ptr` + thread-local anchor stack** | Anchor cleanup hook missing — DuckDB doesn't fire callbacks at bind/execute end, so anchors leak per-thread. Also breaks for cross-thread binds (DuckDB worker pool). |
| **Move to DuckDB-upstream `CatalogSet`** | DuckDB's `CatalogSet` has the same fundamental problem internally; it gets away with it because DuckDB's binder is also DuckDB-side and they coordinate. We can't piggyback. |
| **Synchronous quiescence: `Invalidate()` blocks until no binds in flight** | We don't have per-binder ref tracking; can't tell. Even if we did — would block invalidation indefinitely on long-running queries. |
| **Keep `unique_ptr` and forbid concurrent invalidation** | Doesn't fix US1 (concurrent first-load); also breaks DDL hooks and TTL invalidation that fire opportunistically. |

---

## Decision 2: Extend the extension-owned bind data with a `shared_ptr` anchor

**Decision**: `MSSQLBindData` (the `unique_ptr<FunctionData>` returned by `GetScanFunction` and friends) gains a `shared_ptr<MSSQLTableEntry> table_entry_anchor_` field. The anchor holds the entry alive for the entire query execute phase. The `optional_ptr<CatalogEntry>` returned by `LookupEntry` is the **same** object — pointed-to by the shared_ptr in `entries_` AND co-owned via the anchor in bind data.

**Rationale**:

- The bind phase (LookupEntry → catalog_entry->GetScanFunction → bind data construction) is the narrow window where the entry must stay alive. Once the bind data is constructed, the shared_ptr anchor inside it keeps the entry alive through execute.
- The bind data is **already extension-owned** (we implement `GetScanFunction` and friends). Adding a member is zero-API-change.
- DuckDB destroys the bind data when the query plan dies (end of execute, error, cancellation). So the anchor releases at the right moment naturally.
- For the bind-phase gap itself (between LookupEntry returning and GetScanFunction starting): the `entries_` map's shared_ptr is alive throughout (lookup holds the map's reference). Invalidation moves to graveyard but doesn't destroy. So the entry survives. The anchor is grabbed in GetScanFunction from `entries_` or graveyard via the raw `this` pointer — wait, we don't know which container holds it. **Solution**: every `MSSQLTableEntry` has a `weak_ptr<MSSQLTableEntry>` self-reference (or we look up the catalog → table set → graveyard / live map to find the shared_ptr).

**Refinement**: the cleanest way is for `MSSQLTableEntry` itself to support `enable_shared_from_this<MSSQLTableEntry>`. Then `GetScanFunction` calls `shared_from_this()` to construct the anchor. `enable_shared_from_this` is the standard library answer to "I have a raw `this` pointer and need the shared_ptr that owns me."

**Alternatives considered**:

| Alternative | Why rejected |
|---|---|
| **Anchor only at LookupEntry, not in bind data** | LookupEntry's return value lifetime is per-call; can't span execute. |
| **Look up TableSet → entries_ at GetScanFunction time** | Requires the parent set reference; works but is more code. `enable_shared_from_this` is one-line. |
| **Anchor inside MSSQLTableEntry itself (per-entry refcount field)** | Re-implements shared_ptr. Worse. |
| **Drop the bind-phase concern; rely on rapid invalidation being rare** | Spec FR-001 explicitly requires the lifetime guarantee across all invalidation events. |

---

## Decision 3: Graveyard drains on catalog destruction, no runtime GC

**Decision**: `MSSQLCatalog::graveyard_` is a `vector<shared_ptr<...>>` that's only freed in `~MSSQLCatalog()`. No periodic GC, no manual `mssql_gc_catalog()` function in this spec.

**Rationale**:

- Without a binder-unwind hook from DuckDB, we cannot prove that an entry in the graveyard has zero outstanding binder references. The only safe time to free is when the catalog itself dies (DETACH or process exit) — at which point all binds against this catalog must have completed.
- Memory budget: spec says ≤10K entries for a 24h dbt-style server. At ~5KB per `MSSQLTableEntry`, that's ~50MB. Acceptable for an analytical workload; documented as a known limitation in the PR description.
- For long-running production processes that hit the cap meaningfully: a future spec can add a manual GC trigger (`mssql_gc_catalog(catalog)` that's safe to call when the operator knows no binds are in flight) or a generation-bumped automatic GC after N invalidations with a `quiet_period` argument. Out of scope here.

**Alternatives considered**:

| Alternative | Why rejected |
|---|---|
| **Automatic GC: shared_ptr::use_count() ≤ 1 → free** | Race: thread A checks use_count==1, decides to free; thread B calls GetEntry that resurrects a graveyard'd entry via lookup; A frees mid-use. Safer to never auto-free. |
| **Soft cap (e.g., 10K) with FIFO eviction** | Eviction creates new UAFs for in-flight binders holding the evicted shared_ptr. Defeats the fix. |
| **Manual `mssql_gc_catalog()` function** | Adds API surface for a problem that may never materialise in practice. Add later if telemetry warrants. |
| **Generation-counter GC: free entries N generations old** | Requires tracking which generation each binder started in; we don't have the hook. |

---

## Decision 4: `LoadSingleEntry` / `LookupSchema` use emplace-only semantics

**Decision**: replace `entries_[name] = std::move(entry)` and `schema_entries_[name] = std::move(entry)` with `entries_.emplace(name, std::move(entry))` (or `try_emplace`). When two threads race to first-load the same table:

1. Both fetch metadata in parallel (existing behaviour — `load_mutex_` is per-set, not per-table).
2. Both construct a fresh `MSSQLTableEntry` on the stack.
3. First thread to grab `entry_mutex_` calls `emplace` → succeeds, its entry wins.
4. Second thread calls `emplace` → returns existing iterator (no-op); its local `unique_ptr`/`shared_ptr` dies → its entry is destroyed.
5. **Both** threads, after `emplace`, do `return entries_.find(name)->second.get()` — they both return a pointer to the **winner's** entry.

**Rationale**:

- Fixes US1 (concurrent-first-load UAF) at the root: never overwrite, never destroy under a live raw pointer.
- The loser's wasted SQL Server round trip is acceptable (rare; same cost as the winner's). Optionally optimisable later with per-table load mutex.
- Preserves the lazy-load semantics (FR-007): first lookup triggers fetch, subsequent lookups hit cache.

**Alternatives considered**:

| Alternative | Why rejected |
|---|---|
| **Per-table load mutex (one mutex per table name)** | More memory, more complexity. Optimisation, not correctness. Defer. |
| **`load_mutex_` held across the whole fetch** | Serialises all first-loads of different tables. Tanks parallel-bind throughput. |
| **Optimistic insert + retry on collision** | Equivalent to emplace; emplace is cleaner. |

---

## Decision 5: Sibling-cache audit scope

The user spec (US3) calls for auditing **`MSSQLSchemaSet`**, **`MSSQLMetadataCache`**, **`MSSQLStatisticsProvider`**. Findings:

### `MSSQLCatalog::schema_entries_` (the schema set)

Same pattern as `MSSQLTableSet::entries_`:

- `unordered_map<string, unique_ptr<MSSQLSchemaEntry>>` (mssql_catalog.hpp:231).
- `LookupSchema` returns `optional_ptr<SchemaCatalogEntry>` from `it->second.get()` (mssql_catalog.cpp:380-388).
- `OnDetach` calls `schema_entries_.erase(info.name)` (mssql_catalog.cpp:445).
- `InvalidateSchemaTableSet` does NOT erase the schema entry — only invalidates the TableSet inside it. **But** `RefreshCache` and `InvalidateMetadataCache` walk all `schema_entries_` and call `GetTableSet().Invalidate()` (mssql_catalog.cpp:838, 916).

**Fix**: same treatment — `shared_ptr<MSSQLSchemaEntry>` + graveyard append on erase. Concurrent SchemaSet first-load uses emplace-only.

### `MSSQLMetadataCache`

`GetTableMetadata` returns `const MSSQLTableMetadata *` (mssql_metadata_cache.hpp:135) — raw pointer into `schemas_[schema].tables[name]`.

The pointee is destroyed when:
- `InvalidateSchema(schema)` clears `tables` for that schema.
- `InvalidateAll()` clears `schemas_`.
- A bulk reload replaces `MSSQLTableMetadata` in `tables[name]`.

The caller (`MSSQLTableSet::LoadSingleEntry`) **immediately copies** the relevant fields into a new `MSSQLTableEntry` (mssql_table_set.cpp:187: `auto entry = CreateTableEntry(*table_meta);`) and never holds `table_meta` past the function return. So the metadata cache's raw-pointer-handout is OK in current usage — but it's fragile. A new caller could be added that violates this. 

**Fix**: change `GetTableMetadata` to return by value (`MSSQLTableMetadata` is move-only with a `mutex_` member — needs careful refactor) OR document the contract via a comment + add a TSan annotation. **Decision**: document + add a debug assert; full move-by-value refactor is yak-shaving for a non-bug. If future regression appears, escalate.

### `MSSQLStatisticsProvider`

`GetRowCount` returns `idx_t` by value (mssql_statistics.hpp:48) — already safe.

No other pointer-handout patterns found in the header.

**Audit conclusion**: 
- `MSSQLSchemaSet` (= `MSSQLCatalog::schema_entries_`): **same fix as TableSet** (in scope, US3).
- `MSSQLMetadataCache`: **document the contract + debug assert** (in scope, US3).
- `MSSQLStatisticsProvider`: **no fix needed** (already by-value).

---

## Decision 6: Performance budget

**Decision**: target ≤1 atomic op (the shared_ptr refcount increment in `shared_from_this()`) per `GetEntry` cache-hit. No new mutex contention.

**Rationale**:

- Current `GetEntry` cache-hit fast path: 1 mutex acquire (entry_mutex_) + 1 hash lookup + 1 raw ptr return. Post-fix: same + 1 atomic increment (shared_ptr copy when bind data anchors). The atomic is unconditional in bind data construction, not in GetEntry — so the lookup path is unchanged.
- For the rare path (concurrent first-load): `emplace` is a single hash lookup + insert; same as current.
- `Invalidate()`: move map contents into graveyard via `std::move` + `vector::insert(end, make_move_iterator…)` — O(N entries) but rare (only called on explicit DDL / refresh / TTL boundary). Pre-fix is also O(N) for `entries_.clear()`.

**Validation plan**:

- Add `make bench-catalog-concurrent` microbench: 4 threads × 10000 binds against a hot cache (single-table SELECT) before/after. Target ±20% per SC-006.
- Existing `make integration-test` runtime measured before/after; target ±10% per SC-006.

**Alternatives considered**:

| Alternative | Why rejected |
|---|---|
| **Lock-free entries_ map (e.g., tbb::concurrent_hash_map)** | Adds vcpkg dep. Unjustified — single mutex is fine at our scale. |
| **Per-bucket sharding** | Premature optimisation. |

---

## Open follow-ups (out of scope for this spec)

- **Graveyard GC**: if production telemetry shows monotonic graveyard growth being a problem for long-running processes, add a `mssql_gc_catalog([context])` manual trigger or a generation-bumped automatic GC. Track in a future spec / issue.
- **Per-table load mutex**: optimisation to avoid duplicate SQL Server round trips on concurrent first-load. Not a correctness issue.
- **`MSSQLMetadataCache` move-by-value refactor**: only if a real regression appears. Today's call sites all copy into `MSSQLTableEntry` immediately.

---

## References

- DuckDB `optional_ptr<CatalogEntry>` API: `duckdb/common/optional_ptr.hpp`
- DuckDB `enable_shared_from_this` usage in upstream catalog: `duckdb/catalog/catalog_entry.hpp` (already inherits from `enable_shared_from_this`, so the pattern is upstream-blessed)
- Spec 047 ownership model: per-`MSSQLCatalog` `unique_ptr<ConnectionPool>` (precedent for "ownership lives inside the catalog, no process-wide statics")
- Issue #126: https://github.com/hugr-lab/mssql-extension/issues/126

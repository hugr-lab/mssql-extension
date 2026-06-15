# Contract — Catalog Entry Ownership & Lifetime

**Spec**: [spec.md](../spec.md) · **Plan**: [plan.md](../plan.md) · **Data model**: [data-model.md](../data-model.md)

This document defines the internal C++-level ownership contract enforced by spec 052. It is the source-of-truth that PR review checks each affected file against.

## Scope

Internal to the `mssql` extension. No user-visible API. No DuckDB upstream changes.

## Roles

| Role | Object | Lifetime |
|---|---|---|
| **Live cache** | `MSSQLTableSet::entries_`, `MSSQLCatalog::schema_entries_` | From insertion to the next `Invalidate()` / `clear()` |
| **Graveyard** | `MSSQLCatalog::table_graveyard_`, `MSSQLCatalog::schema_graveyard_` | From `Invalidate()` to `~MSSQLCatalog()` |
| **Bind data anchor** | `MSSQLScanBindData::table_entry_anchor_` (and analogous DML bind data) | Bind construction → bind data destruction (= end of query execute) |
| **Transient binder pointer** | `optional_ptr<CatalogEntry>` returned by `LookupSchema` / `LookupEntry` / `GetEntry` | One call site, not stored beyond the immediate bind step |

## Invariants the implementation MUST hold

### I1 — `shared_ptr` is the sole owning type for `MSSQLTableEntry` and `MSSQLSchemaEntry`

- No `unique_ptr<MSSQLTableEntry>` or `unique_ptr<MSSQLSchemaEntry>` anywhere in the codebase post-fix. Verified by grep:
  ```bash
  grep -rn 'unique_ptr<MSSQLTableEntry>\|unique_ptr<MSSQLSchemaEntry>' src/ test/
  # MUST return zero hits
  ```
- Factories return `shared_ptr` (or `make_shared` directly).

### I2 — `enable_shared_from_this` on both entry classes

- `MSSQLTableEntry` and `MSSQLSchemaEntry` both inherit `std::enable_shared_from_this<...>` with the most-derived type.
- Any code that needs to anchor an entry from a raw `this` pointer (= inside `GetScanFunction` and friends) MUST use `shared_from_this()` rather than reconstructing a `shared_ptr` (which would create a second, disjoint refcount group and is undefined behaviour).

### I3 — Live-cache insertion is emplace-only

- `MSSQLTableSet::LoadSingleEntry` MUST call `entries_.emplace(...)` (or `try_emplace`) — never `entries_[name] = ...` or `entries_.insert_or_assign(...)`.
- `MSSQLCatalog::LookupSchema` MUST call `schema_entries_.emplace(...)` for first-load — never overwrite.
- On collision, the late-arriving constructed entry MUST be discarded (its local `shared_ptr` goes out of scope and the entry destroys cleanly because no other holder exists yet).

### I4 — `Invalidate()` retires, does NOT destroy

- `MSSQLTableSet::Invalidate()` MUST move (`std::move`) the contents of `entries_` into the parent catalog's `table_graveyard_` via `MSSQLCatalog::AppendToTableGraveyard(vector<shared_ptr<MSSQLTableEntry>>)`. It MUST NOT call `entries_.clear()` directly on populated entries.
- `MSSQLCatalog::OnDetach` / `~MSSQLCatalog` MUST drain the graveyards (RAII). No leak.
- `MSSQLCatalog::InvalidateSchemaTableSet` / `InvalidateMetadataCache` / `RefreshCache` MUST all follow the move-to-graveyard pattern via the same `AppendTo*Graveyard` helpers.

### I5 — Bind data MUST anchor the entry

- Every extension-implemented `FunctionData` derivative that captures a `MSSQLTableEntry*` MUST also store a `shared_ptr<MSSQLTableEntry>` that's set to `entry.shared_from_this()` at construction.
- This is enforced by code review + a checklist item in PR template.
- Affected types (audit pass during implementation):
  - `MSSQLScanBindData` (table scan)
  - DML bind data (insert / update / delete)
  - `mssql_scan` / `mssql_exec` table-function bind data — these do NOT take a catalog entry (they take a query string), so no anchor needed.

### I6 — Mutex acquisition order

- Catalog `schema_entries_mutex_` BEFORE set `entry_mutex_` (when both needed).
- Set `entry_mutex_` BEFORE catalog `graveyard_mutex_` IS NOT ALLOWED — instead, the set releases `entry_mutex_` after moving entries into a local vector, then calls `catalog.AppendToTableGraveyard(local_vector)` which independently acquires `graveyard_mutex_`. Decoupled to avoid lock inversion.
- Documented in `data-model.md` § "Mutex protocol".

### I7 — Performance budget

- `GetEntry` cache-hit fast path: identical wallclock to pre-fix within measurement noise (±5%). The change is `unique_ptr.get()` → `shared_ptr.get()`, which is a single pointer load — same.
- Bind-data construction: +1 atomic op (the `shared_from_this()` refcount inc). Wallclock change <1µs.
- `Invalidate()`: O(N) for the move into graveyard, plus O(M) for the graveyard mutex section. Same big-O as pre-fix `entries_.clear()`.

### I8 — No graveyard auto-GC in this spec

- `MSSQLCatalog` MUST NOT contain any code that frees entries from the graveyard before `~MSSQLCatalog()`. Specifically: no use_count-based eviction, no timer-driven trim, no soft cap with FIFO eviction.
- Future GC additions are out of scope (tracked separately if needed).

## Validation gates (PR checklist)

PR description / review MUST confirm:

- [ ] `grep -rn 'unique_ptr<MSSQLTableEntry>\|unique_ptr<MSSQLSchemaEntry>' src/ test/` returns zero hits.
- [ ] `grep -rn 'entries_\[' src/catalog/mssql_table_set.cpp` (assignment) returns zero hits in mutation paths (`emplace` only).
- [ ] `grep -rn 'schema_entries_\[' src/catalog/mssql_catalog.cpp` (assignment) returns zero hits in mutation paths.
- [ ] `grep -rn '\.clear()' src/catalog/mssql_table_set.cpp src/catalog/mssql_catalog.cpp` is reviewed for each hit — clears on `entries_` / `schema_entries_` MUST be preceded by `AppendTo*Graveyard(move(...))`.
- [ ] Every `MSSQLScanBindData` / DML-bind-data construction site holds an `anchor_` shared_ptr; verified by greppable field name.
- [ ] UBSan run: `test/cpp/test_concurrent_reads.cpp` scenarios 4/5/6 — clean.
- [ ] TSan run: same — clean.
- [ ] Microbench `make bench-catalog-concurrent` within ±20% of v0.2.0 baseline.

## Anti-patterns to reject in PR review

- ❌ Constructing a `shared_ptr<MSSQLTableEntry>` from a raw `this` pointer without `enable_shared_from_this` — UB.
- ❌ Holding an `optional_ptr<MSSQLTableEntry>` past the bind step without anchoring.
- ❌ Adding a "use_count-based GC" trim in graveyard — re-introduces the race (use_count check is racy with concurrent shared_ptr copies).
- ❌ Adding a graveyard size limit with eviction — defeats the lifetime guarantee.
- ❌ Returning `shared_ptr<MSSQLTableEntry>` from `GetEntry` to DuckDB — DuckDB API is `optional_ptr`; do not extend.

## Out-of-contract (explicitly allowed)

- `MSSQLMetadataCache::GetTableMetadata` continues to return `const MSSQLTableMetadata *`. Per research.md Decision 5: every current caller copies immediately. A documented comment + debug assert is added; full move-by-value refactor is deferred unless a real regression appears.
- `mssql_pool_stats` is NOT extended in this spec (per Clarification 2026-05-26 Q3).
- `test/dbt/` is NOT shipped (per Clarification 2026-05-26 Q4).

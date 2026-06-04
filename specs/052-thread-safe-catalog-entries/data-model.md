# Data Model — Thread-Safe Catalog Entry Lifetime

**Spec**: [spec.md](./spec.md) · **Plan**: [plan.md](./plan.md) · **Research**: [research.md](./research.md)

## Ownership graph (post-fix)

```text
MSSQLCatalog (owned by DuckDB AttachedDatabase, lifetime = ATTACH...DETACH)
├── schema_entries_  : unordered_map<string, shared_ptr<MSSQLSchemaEntry>>     [live]
├── graveyard_       : vector<shared_ptr<MSSQLSchemaEntry>>                    [retired]
│                      vector<shared_ptr<MSSQLTableEntry>>                     [retired]
├── metadata_cache_  : unique_ptr<MSSQLMetadataCache>                          [unchanged]
└── stats_provider_  : unique_ptr<MSSQLStatisticsProvider>                     [unchanged]

MSSQLSchemaEntry  : public SchemaCatalogEntry, public enable_shared_from_this  [NEW]
└── tables_       : MSSQLTableSet
    ├── entries_  : unordered_map<string, shared_ptr<MSSQLTableEntry>>         [live]
    └── (no per-set graveyard — retired entries flow to MSSQLCatalog::graveyard_)

MSSQLTableEntry  : public TableCatalogEntry, public enable_shared_from_this    [NEW]
└── (data unchanged)

Bind data (extension-owned, lifetime = single query):
MSSQLScanBindData : public FunctionData
└── table_entry_anchor_ : shared_ptr<MSSQLTableEntry>                          [NEW]
                          (held for duration of execute; released on bind-data destruction)
```

### Invariants

1. **Single source of truth**: `entries_` holds the "live" shared_ptr for currently-resolvable lookups. `graveyard_` holds shared_ptrs that have been retired by Invalidate but may still be referenced.
2. **No double-owning unique_ptr**: every `MSSQLTableEntry` / `MSSQLSchemaEntry` is owned by `shared_ptr`. Constructors stay private-friend-of-the-set or via factory methods that return `shared_ptr`.
3. **Graveyard append-only at runtime**: entries leave only when `~MSSQLCatalog()` runs.
4. **`enable_shared_from_this` is mandatory**: bind data must reach a shared_ptr from a raw `this` pointer.

---

## State machine: Entry lifecycle

```text
                          ┌───────────────────────────────┐
                          │      LoadSingleEntry called   │
                          └───────────────┬───────────────┘
                                          │  emplace
                                          ▼
                          ┌───────────────────────────────┐
                          │  LIVE in entries_             │
                          │  (cache hit fast path)        │
                          └───────────────┬───────────────┘
                                          │
                          ┌───────────────┴────────────────┐
                          │                                │
              Invalidate fired                    Catalog destroyed
                          │                                │
                          ▼                                ▼
        ┌──────────────────────────────────┐   ┌──────────────────────────┐
        │ RETIRED — in graveyard_          │   │ FREED                    │
        │  - not visible to new lookups    │   │  (refcount → 0)          │
        │  - still alive for any binder    │   └──────────────────────────┘
        │    holding raw ptr / anchor      │
        │  - cannot be looked up by name   │
        └──────────────────┬───────────────┘
                           │ Catalog destroyed (or refcount→0 if no anchors)
                           ▼
              ┌──────────────────────────┐
              │ FREED                    │
              └──────────────────────────┘
```

States are **implicit** (which container holds the shared_ptr). No explicit state machine field on the entry.

**Why no `STALE` flag on the entry itself**: a STALE flag would require every binder to check it before use — re-introduces the check-then-use race. Container-based state (live vs retired) is invisible to the binder, which is correct.

---

## Mutex protocol

| Mutex | Guards | Acquire order |
|---|---|---|
| `MSSQLCatalog::schema_entries_mutex_` (NEW name; today implicit on `mutex_`) | `schema_entries_`, `graveyard_` | First |
| `MSSQLTableSet::entry_mutex_` (existing) | `entries_`, `attempted_tables_` | Second |
| `MSSQLTableSet::load_mutex_` (existing) | `is_fully_loaded_`, `names_loaded_` flags during `Invalidate` | Wraps `entry_mutex_` in `Invalidate` only |
| `MSSQLTableSet::names_mutex_` (existing) | `known_table_names_` | Third |

**Invariant**: nested acquisitions go top-down only (catalog → set → names). No reverse path exists in the call graph (verified by grep).

**`Invalidate()` lock-order**: holds `load_mutex_`, acquires `entry_mutex_` briefly to move entries into the catalog's graveyard. The catalog's `schema_entries_mutex_` is **not** acquired by the set — instead, the set calls `MSSQLCatalog::AppendToGraveyard(vector<shared_ptr<MSSQLTableEntry>>)` which acquires the catalog mutex internally. Decouples lock order.

---

## Data shape changes

### `src/include/catalog/mssql_table_set.hpp`

```cpp
// BEFORE
unordered_map<string, unique_ptr<MSSQLTableEntry>> entries_;
// AFTER
unordered_map<string, shared_ptr<MSSQLTableEntry>> entries_;
```

`GetEntry` continues to return `optional_ptr<CatalogEntry>` via `entries_[name].get()` — only the value type of the map changes. No public API change.

### `src/include/catalog/mssql_catalog.hpp`

```cpp
// BEFORE
unordered_map<string, unique_ptr<MSSQLSchemaEntry>> schema_entries_;
// AFTER
unordered_map<string, shared_ptr<MSSQLSchemaEntry>> schema_entries_;
vector<shared_ptr<MSSQLSchemaEntry>> schema_graveyard_;        // NEW
vector<shared_ptr<MSSQLTableEntry>>  table_graveyard_;         // NEW (filled by MSSQLTableSet via friend / public method)
std::mutex graveyard_mutex_;                                   // NEW (guards both graveyards)

// Public surface (new):
void AppendToTableGraveyard(vector<shared_ptr<MSSQLTableEntry>> retired);
void AppendToSchemaGraveyard(shared_ptr<MSSQLSchemaEntry> retired);
size_t GetGraveyardSize() const;  // debug-only; not surfaced in SQL per Clarification Q3
```

### `src/include/catalog/mssql_table_entry.hpp` + `mssql_schema_entry.hpp`

```cpp
class MSSQLTableEntry : public TableCatalogEntry,
                        public std::enable_shared_from_this<MSSQLTableEntry> {  // NEW
    // ...
};

class MSSQLSchemaEntry : public SchemaCatalogEntry,
                         public std::enable_shared_from_this<MSSQLSchemaEntry> { // NEW
    // ...
};
```

**Caveat**: DuckDB's `CatalogEntry` does NOT inherit from `enable_shared_from_this` (as of main). Multiple inheritance with `enable_shared_from_this` requires the most-derived `make_shared` call to construct via the correct path. Verify at compile/link time that `shared_from_this()` returns the correctly-typed pointer (no slicing).

### `src/include/dml/scan_bind_data.hpp` (or wherever `MSSQLScanBindData` lives)

```cpp
struct MSSQLScanBindData : public FunctionData {
    // ... existing fields
    shared_ptr<MSSQLTableEntry> table_entry_anchor_;  // NEW — keeps entry alive for execute
};
```

Set by `MSSQLTableEntry::GetScanFunction` (or wherever `MSSQLScanBindData` is constructed) via `shared_from_this()`.

---

## Lifecycle scenarios

### Scenario A: concurrent first-load of the same table (US1)

```text
T1: LookupSchema("dbo") → schema_entry (shared_ptr in schema_entries_)
T1: schema_entry.LookupEntry("t")
    → MSSQLTableSet::GetEntry("t")
    → cache miss
    → LoadSingleEntry("t")
        → fetch metadata (~50ms SQL Server round trip, no lock held)
        → constructs shared_ptr<MSSQLTableEntry> entry1
        → acquire entry_mutex_
        → entries_.emplace("t", entry1) ⇒ inserted=true
        → return entries_["t"].get()
T2: (parallel) LookupSchema("dbo") → same schema_entry (cache hit)
T2: schema_entry.LookupEntry("t")
    → MSSQLTableSet::GetEntry("t")
    → cache miss (T1 not done yet)
    → LoadSingleEntry("t")
        → fetch metadata in parallel
        → constructs shared_ptr<MSSQLTableEntry> entry2
        → acquire entry_mutex_ (waits for T1 to release)
        → entries_.emplace("t", entry2) ⇒ inserted=false (T1 won)
        → entry2 goes out of scope, refcount→0, destroyed cleanly (nobody held it)
        → return entries_["t"].get()  ← T1's entry1
T1 and T2 both have a raw pointer to entry1. Both call GetScanFunction → both bind data anchor entry1. Refcount = 1 (entries_) + 2 (anchors) = 3.
T1 finishes execute, anchor destroyed, refcount → 2.
T2 finishes execute, anchor destroyed, refcount → 1 (only entries_).
✓ No UAF. Both binders saw consistent data.
```

### Scenario B: invalidation during in-flight execute (US2)

```text
T1: LookupSchema("dbo") → schema_entry (shared_ptr in schema_entries_, refcount=1)
T1: schema_entry.LookupEntry("t") → entry1 (refcount=1: entries_)
T1: GetScanFunction("t") → bind data anchors entry1 (refcount=2)
T1: starts execute (long analytical query, 30s)
T2: (mid-query) mssql_refresh_cache('mssql')
    → MSSQLCatalog::RefreshCache
    → MSSQLMetadataCache::Invalidate
    → for each schema_entry: schema_entry.GetTableSet().Invalidate()
        → MSSQLTableSet::Invalidate
            → acquire load_mutex_ + entry_mutex_
            → catalog.AppendToTableGraveyard(move(entries_))
                → graveyard_mutex_, push_back(s) for each shared_ptr
                → entries_ is now empty
            → release
        → after Invalidate, entries_ is empty; entry1 lives in graveyard
T1: continues execute, dereferencing raw pointer to entry1 → SAFE, entry1 alive
T1: finishes execute, bind data destroyed → anchor releases, entry1 refcount = 1 (only graveyard)
T3: (later) LookupEntry("t") → cache miss → LoadSingleEntry → fresh entry3 in entries_
✓ No UAF. T1 finished against old metadata (acceptable per FR-006). T3 sees fresh metadata.
```

### Scenario C: DETACH while binds in flight

```text
T1: long-running execute, anchor holds entry1 (refcount=2: entries_ + anchor)
T2: DETACH mssql
    → ~MSSQLCatalog()
    → entries_.clear() (refcount→1)
    → graveyard_.clear() (refcount→1, still held by T1's anchor)
    → ~MSSQLCatalog returns; AttachedDatabase tears down
T1: continues execute holding the only remaining shared_ptr → entry1 stays alive
T1: finishes execute → anchor released → refcount→0 → ~MSSQLTableEntry runs
```

**This is the edge case spec calls out**: DuckDB's quiescence contract SHOULD prevent DETACH while binds are in flight. But if violated, the shared_ptr in bind data keeps the entry alive — no UAF, just delayed destruction. `D_ASSERT` in `~MSSQLCatalog` checks `active_connections_.empty()` per spec 047 — if it fires, we know DuckDB violated quiescence, but the shared_ptr saves us from a UAF.

---

## Key entities (cross-reference to spec.md § Key Entities)

| Entity | Pre-fix | Post-fix | Notes |
|---|---|---|---|
| `MSSQLTableEntry` | held by `unique_ptr` in TableSet | held by `shared_ptr` in TableSet + graveyard + bind data | `enable_shared_from_this` added |
| `MSSQLTableSet::entries_` | `map<string, unique_ptr>` | `map<string, shared_ptr>` | emplace-only insertion |
| `MSSQLSchemaEntry` | held by `unique_ptr` in Catalog | held by `shared_ptr` in Catalog + graveyard | `enable_shared_from_this` added |
| `MSSQLCatalog::schema_entries_` | `map<string, unique_ptr>` | `map<string, shared_ptr>` | emplace-only insertion |
| `MSSQLMetadataCache` | raw-pointer-returning `GetTableMetadata` | unchanged (callers immediately copy; documented contract + debug assert) | Audit only |
| `MSSQLStatisticsProvider` | by-value `GetRowCount` (idx_t) | unchanged | Already safe |
| **Outstanding-reference anchor** (spec-level concept) | did not exist | `shared_ptr` in bind data + transient graveyard membership | The lifetime-extension mechanism |

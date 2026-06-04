# Spec 047 — Data Model (Ownership Graph)

The "entities" in spec 047 are ownership relationships between C++ objects. This document captures the before/after object graph and the lifetime invariants the refactor enforces.

## Today (pre-spec-047) — implicit, singleton-rooted

```text
process
└── MssqlPoolManager  [Meyers singleton, leaked at process exit]
    ├── pools_  unordered_map<name, unique_ptr<ConnectionPool>>   [keyed by alias only — cross-instance collision]
    │   └── ConnectionPool
    │       └── connections_  vector<unique_ptr<TdsConnection>>
    │           └── socket_
    └── pinned_counts_  unordered_map<name, atomic<int>>

process
└── g_context_managers  unordered_map<DatabaseInstance*, unique_ptr<MSSQLContextManager>>   [pointer-reuse hazard]
    └── MSSQLContextManager
        └── contexts_  unordered_map<name, shared_ptr<MSSQLContext>>
            └── MSSQLContext  [connection_info, settings; pool reached via singleton lookup]

process
├── MSSQLResultStreamRegistry  [Meyers singleton]
│   └── streams_  unordered_map<uint64_t, unique_ptr<MSSQLResultStream>>   [mssql_scan bridge]
├── MSSQLConnectionHandleManager  [Meyers singleton — LEGITIMATE, stays + deprecated]
│   └── handles_  unordered_map<int64_t, shared_ptr<TdsConnection>>
└── mssql::azure::TokenCache  [Meyers singleton — LEGITIMATE, stays]
    └── tokens_  unordered_map<secret_name, OAuth2Token>

DatabaseInstance
└── MSSQLCatalog  [owned by DuckDB DatabaseManager]
    └── connection_pool_  shared_ptr<ConnectionPool>   [no-op deleter; ACTUAL owner is the singleton]
```

**Lifetime problems** (recorded in spec.md §"Current state"):
1. Pool destruction is detached from catalog destruction → silent-shutdown leak.
2. Pool is shared across instances via name collision → cross-instance contamination.
3. `DETACH` calls `RemovePool(name)` on the singleton → cascades to other instances.
4. `g_context_managers` keyed by raw pointer → pointer-reuse hazard.
5. ATTACH does not validate credentials — pool factory is lazy; auth happens on first `Acquire()`.

## After spec 047 — explicit, RAII-rooted

```text
process
├── MSSQLConnectionHandleManager  [Meyers singleton — RETAINED, deprecated functions]
│   └── handles_  unordered_map<int64_t, shared_ptr<TdsConnection>>
└── mssql::azure::TokenCache  [Meyers singleton — RETAINED, deliberate]
    └── tokens_  unordered_map<secret_name, OAuth2Token>

DatabaseInstance
└── DatabaseManager
    └── (one entry per ATTACH)
        └── MSSQLCatalog  [owned by DuckDB via shared_ptr]
            ├── connection_pool_  unique_ptr<ConnectionPool>   [SOLE owner; RAII destruction]
            │   ├── connections_  vector<unique_ptr<TdsConnection>>
            │   │   └── socket_
            │   └── pinned_count_  atomic<int64_t>   [moved from singleton]
            └── active_streams_  unordered_map<string uuid, unique_ptr<MSSQLResultStream>>   [moved from MSSQLResultStreamRegistry]
                                  + streams_mutex_  mutex
```

**Lifetime invariants** (enforced by construction):

1. **Pool lifetime ≤ catalog lifetime ≤ instance lifetime.** No catalog → no pool. No instance → no catalog → no pool.
2. **Sockets close at `~MSSQLCatalog`.** `unique_ptr<ConnectionPool>` destruction triggers the full RAII chain down to `socket_.Close()`.
3. **Cross-instance isolation by construction.** Each catalog has its own pool; no shared map; no name collision possible across instances.
4. **DETACH semantics local to instance.** `DETACH` removes the catalog from this instance's DatabaseManager → catalog destroyed → pool destroyed. No effect on other instances.
5. **Stream lifetime ≤ catalog lifetime.** Orphan streams (abandoned bind / cancelled query) are cleaned at `~MSSQLCatalog`.
6. **ATTACH validates credentials.** Successful return from `MSSQLCatalog::Initialize` implies one round-trip auth round-trip succeeded; bad credentials surface as ATTACH failure, not first-query failure (unless `lazy_validation=true`).

## Entity definitions

### `MSSQLCatalog` (modified)

| Field | Type (before) | Type (after) | Notes |
|---|---|---|---|
| `connection_pool_` | `shared_ptr<ConnectionPool>` (no-op deleter into singleton) | `unique_ptr<ConnectionPool>` (RAII) | Owner. |
| `connection_info_` | (unchanged) | (unchanged) | Used by `Initialize` to build the pool. |
| `pool_config_` | (unchanged) | (unchanged) | Same. |
| `streams_mutex_` | (none) | `mutex` | **NEW** — protects `active_streams_`. |
| `active_streams_` | (none) | `unordered_map<string, unique_ptr<MSSQLResultStream>>` | **NEW** — migrated from `MSSQLResultStreamRegistry`. |

**Lifecycle**:
- Constructor: copies info + config from `MSSQLAttach`.
- `Initialize`: inlines the 3-branch pool construction (formerly in `MssqlPoolManager::GetOrCreatePool*`); stores result in `connection_pool_`. Performs eager `pool.Acquire()` + immediate release for credential validation, unless `lazy_validation=true`.
- `OnDetach`: Azure token-cache invalidation only (pool cleanup is implicit via RAII).
- Destructor: default. `unique_ptr<ConnectionPool>` destroys pool. `active_streams_` map destroys orphan streams.

**New methods**:
- `string RegisterStream(unique_ptr<MSSQLResultStream> stream)` — generates UUID, inserts into `active_streams_`, returns UUID.
- `unique_ptr<MSSQLResultStream> RetrieveStream(const string &uuid)` — atomic find + erase + return. Returns `nullptr` if not found.

### `tds::ConnectionPool` (modified)

| Field | Type | Notes |
|---|---|---|
| `connections_` | `vector<unique_ptr<TdsConnection>>` | Unchanged. |
| `pinned_count_` | `atomic<int64_t>` | **NEW** — migrated from `MssqlPoolManager`. |
| `idle_timeout_thread_` | (unchanged) | Unchanged. |

**New methods**: `IncrementPinned()`, `DecrementPinned()`, `GetPinnedCount() const`. `GetStats()` returns a struct that now includes the `pinned_count` field.

### `MSSQLConnectionHandleManager` (RETAINED, deprecated)

No structural changes. Class + singleton stay. The 3 surrounding functions (`mssql_open` / `mssql_close` / `mssql_ping`) get `[DEPRECATED]` markers in their function descriptions, CLAUDE.md table, and CHANGELOG entry (per FR-010). The singleton itself is retained as legitimate per Inventory; slated for removal when the deprecated functions are removed in a future major release.

### `MssqlPoolManager` (DELETED)

Class + header + `.cpp` removed entirely. All call sites migrated:
- `mssql_storage.cpp` `MSSQLAttach` — pool construction moves into `MSSQLCatalog::Initialize`; `RemovePool` call dropped (RAII).
- `mssql_diagnostic.cpp` `MssqlPoolStatsGlobalState` — enumeration via DuckDB catalog list (FR-003).
- `mssql_catalog.cpp:82,630` — direct catalog-internal pool access (no singleton hop).
- `mssql_transaction.cpp:139,143` — `catalog_.GetConnectionPool().IncrementPinned()`.
- `mssql_insert_executor.cpp:74`, `mssql_update_executor.cpp:73`, `mssql_result_stream.cpp:82,89`, `mssql_query_executor.cpp:40` — `catalog.GetConnectionPool()`.

### `MSSQLContextManager` (DELETED)

Class + `g_context_managers` + `g_context_managers_lock` removed entirely. The only consumer was pool-lookup dispatch; with pool owned by catalog, the indirection collapses. Spec 045 band-aid (`70a4d90` `RegisterContext` silent-overwrite + conditional pool sweep) is retired.

### `MSSQLResultStreamRegistry` (DELETED)

Class removed. State (mutex + map) moves into `MSSQLCatalog` (see above). `mssql_scan` `Bind` / `InitGlobal` call sites use `catalog.RegisterStream(...)` / `catalog.RetrieveStream(...)`.

## State transitions

### `ATTACH '...' AS db (TYPE mssql)` (before → after diff)

| Before | After |
|---|---|
| Validate connection info syntax | Same |
| Call `MssqlPoolManager::Instance().GetOrCreatePool*(name, ...)` | (Removed — pool built inside `MSSQLCatalog::Initialize`) |
| Construct `MSSQLCatalog`, store `shared_ptr<ConnectionPool>` (no-op deleter) | Construct `MSSQLCatalog`, `Initialize` builds + stores `unique_ptr<ConnectionPool>` |
| (Nothing — lazy connect, bad creds surface on first query) | **`pool.Acquire()` + immediate release** to validate credentials, unless `lazy_validation=true` |
| Register `MSSQLContext` in `g_context_managers[&db][name]` | (Removed — `MSSQLCatalog` IS the context) |
| Throw if name already exists | (Removed — name collision impossible by construction) |

### `DETACH db`

| Before | After |
|---|---|
| Call `MSSQLCatalog::OnDetach` (Azure cleanup + ...) | Same |
| Call `MssqlPoolManager::Instance().RemovePool(name)` — **bug: cross-instance kill** | (Removed — pool destroyed via catalog destruction) |
| Erase from `g_context_managers[&db]` | (Removed — no such map) |
| `~MSSQLCatalog` runs (default) | `~MSSQLCatalog` runs → `unique_ptr<ConnectionPool>` destroys → sockets close; `active_streams_` releases orphan streams |

### `~DatabaseInstance` (silent shutdown)

| Before | After |
|---|---|
| `DatabaseManager` destroys all attached catalogs | Same |
| `~MSSQLCatalog` (default, no-op) — **pool lives on in singleton, sockets leak** | `~MSSQLCatalog` → `unique_ptr<ConnectionPool>` destroys → sockets close; orphan streams cleaned |
| `g_context_managers[&db]` entry leaks (no destructor hook) | (No such map) |
| `MSSQLResultStreamRegistry` in-flight streams for this instance leak | (No such registry; catalog owned them, catalog destroyed them) |
| `MSSQLConnectionHandleManager` handles persist process-wide (no per-instance scoping) | Same — handle manager is process-wide by design (legitimate, deprecated) |

## Validation rules

- **`MSSQLCatalog::connection_pool_` is never null after `Initialize` returns successfully.** If pool construction or eager validation throws, `Initialize` propagates; catalog never becomes usable.
- **`MSSQLCatalog::RegisterStream` returns a unique UUID** within the catalog (collisions impossible — UUID v4).
- **`MSSQLCatalog::RetrieveStream` is atomic** (find + erase under one lock acquisition); concurrent retrievals of the same UUID get exactly one non-null result and the rest `nullptr`.
- **No production source references the removed singleton names** (SC-004 grep gate: `MssqlPoolManager`, `MSSQLContextManager`, `MSSQLResultStreamRegistry`). `MSSQLConnectionHandleManager` is NOT in the grep set.
- **ATTACH with bad credentials throws synchronously** (SC-010) unless `lazy_validation=true`.

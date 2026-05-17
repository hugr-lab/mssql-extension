# Feature Specification: Process-Wide State Cleanup

**Feature Branch**: `047-process-state-cleanup`

**Created**: 2026-05-17

**Status**: Draft / Future

**Input**: Discussion during spec 045 Phase 7 — pre-existing parallel-isolation
COPY test failures led to investigating the global `MssqlPoolManager` singleton.
User question: "If two DuckDB instances run in the same process, why do they
share an MSSQL connection pool? Why doesn't the pool live inside the catalog
that owns it?" Subsequent question: "And when a user destroys one of those
instances, do its TDS connections leak?" The answer to the second question is
"yes, today they do, in several reproducible scenarios."

## Overview

The extension currently owns several pieces of **process-wide global state**.
Some of that state is legitimate (one-time OS init) and some is the wrong
ownership model — it should be per-`DatabaseInstance` or per-`MSSQLCatalog`
but lives in a Meyers singleton out of historical convenience. The mismatch
causes three classes of bugs:

1. **Cross-instance contamination** — two concurrent `DuckDB` instances in
   the same process that both `ATTACH … AS db (TYPE mssql)` silently share a
   single TDS connection pool. The pool is built with the **first** attach's
   credentials; the second instance's queries flow over those credentials.
2. **Cross-instance cascade failure** — if instance A issues `DETACH db`,
   `MSSQLCatalog::OnDetach` calls `MssqlPoolManager::RemovePool("db")`,
   which destroys the shared pool. Instance B's next query fails because
   "its" pool is gone.
3. **Silent-shutdown leak** — `~MSSQLCatalog() = default`, with no cleanup
   hook. When a `DuckDB` instance is destroyed without an explicit `DETACH`
   (common in tests, embedded use, exception unwinding, library teardown),
   the singleton holds the pool indefinitely. TDS sockets stay open against
   the SQL Server; a long-lived process accumulates ghost connections until
   the server-side connection limit is breached.

Spec 047 fixes the ownership model: per-instance / per-catalog resources move
into the catalog itself (RAII via `unique_ptr` members), the singleton goes
away (or shrinks to a thin façade for diagnostics), and a process-wide audit
classifies every remaining piece of global state as **legitimate** or
**migrate**.

## Current state (band-aid)

Commit `70a4d90` (`fix: stale ATTACH state survives DatabaseInstance reuse`)
papered over one of the failure modes — the symptom where
`sqllogictest --force-reload` reuses a freed `DatabaseInstance` pointer and
the new test's `ATTACH AS db` collides with a stale entry in
`g_context_managers`. The band-aid:

- `MSSQLContextManager::RegisterContext` no longer throws on duplicate name;
  it silently evicts the stale entry and replaces it.
- The replacement path also calls `MssqlPoolManager::Instance().RemovePool(name)`
  to clear the dead pool whose connections may be half-broken (e.g. BCP
  mid-stream abort with no `ATTENTION` token sent).

The band-aid is correct ONLY for pointer-reuse-after-destruction. It does
nothing for:

- Two concurrent live instances sharing a pool (no stale entry to detect —
  the second manager is fresh and empty when its `RegisterContext` runs).
- `DETACH` on one instance taking down another's pool (the pool manager
  doesn't know which catalog issued the detach).
- Silent shutdown leaking pool + manager-map entries forever (no destructor
  hook to trigger the band-aid).

Spec 047 retires the band-aid in favour of the structural fix.

## User Scenarios & Testing

### Scenario 1: Two concurrent instances, same alias, different credentials

```cpp
DuckDB db_a, db_b;        // two instances in the same process
Connection conn_a(db_a), conn_b(db_b);

conn_a.Query("ATTACH 'Server=alice.example.com;User=alice;Password=pa'  AS mssql (TYPE mssql)");
conn_b.Query("ATTACH 'Server=bob.example.com;User=bob;Password=pb'      AS mssql (TYPE mssql)");

// MUST succeed independently and route to the correct server.
conn_a.Query("SELECT @@SERVERNAME FROM mssql.dbo.sysobjects");   // expect "alice"
conn_b.Query("SELECT @@SERVERNAME FROM mssql.dbo.sysobjects");   // expect "bob"
```

**Today**: both queries hit `alice.example.com` (the first-registered pool wins).

**After spec 047**: each catalog owns its own pool. Correct routing.

### Scenario 2: DETACH on one, queries continue on the other

```cpp
conn_a.Query("DETACH mssql");
conn_b.Query("SELECT 1 FROM mssql.dbo.X");   // MUST still work
```

**Today**: B's query throws "MSSQL connection pool not initialized" or
similar, because A's `OnDetach` removed the shared pool from the singleton.

**After spec 047**: B's catalog owns its own pool; A's detach can't touch it.

### Scenario 3: Silent shutdown closes sockets

```cpp
{
    DuckDB db;
    Connection conn(db);
    conn.Query("ATTACH '...' AS mssql (TYPE mssql)");
    conn.Query("SELECT 1 FROM mssql.dbo.X");
    // No explicit DETACH; instance goes out of scope.
}
// MUST have closed all TDS sockets to SQL Server.
```

**Today**: pool stays alive in `MssqlPoolManager::Instance().pools_["mssql"]`,
its connections still hold sockets. Server sees them as live for minutes.
Iterate this scenario in a loop → connection-limit exhaustion.

**After spec 047**: `~MSSQLCatalog` drops the owned `unique_ptr<ConnectionPool>`;
RAII closes sockets at end of scope.

## Requirements

### FR-001: Per-catalog pool ownership

`MSSQLCatalog` owns its `tds::ConnectionPool` via `unique_ptr` (today it
holds a `shared_ptr` with a no-op deleter into the singleton's map). The
pool is constructed inside `MSSQLCatalog::Initialize` from connection_info_
and the global `MSSQLPoolConfig` already passed through `MSSQLAttach`.

### FR-002: Remove `MssqlPoolManager` singleton

After FR-001, no production code path needs `MssqlPoolManager::Instance()`.
Hot paths (`ConnectionProvider::GetConnection`, COPY, table scan, DML
executors) already go through `catalog.GetConnectionPool()` — those don't
change. Five remaining call sites either route through the catalog or move
into the catalog itself (see Inventory below).

### FR-003: Diagnostic enumeration via DuckDB catalogs

`mssql_pool_stats()` and `mssql_pool_stats(name)` enumerate by walking
`context.db->GetDatabaseManager().GetDatabases()`, filtering to
`Catalog::GetCatalogType() == "mssql"`, and projecting each catalog's
`GetConnectionPool().GetStats()`. No singleton needed.

### FR-004: Transaction-pinned counter migrates to pool

`MssqlPoolManager::IncrementPinnedCount(name)` / `DecrementPinnedCount(name)`
move to instance methods on `tds::ConnectionPool` (`IncrementPinned()` /
`DecrementPinned()`). Callers in `mssql_transaction.cpp` already have
`catalog_` in hand → `catalog_.GetConnectionPool().IncrementPinned()`.

### FR-005: `~MSSQLCatalog` cleans up without explicit DETACH

`OnDetach` retains its current responsibilities (Azure token-cache invalidation
+ per-instance manager unregistration). `~MSSQLCatalog` is augmented to:

- Run the same unregistration logic if it hasn't already (idempotent flag).
- Allow the `unique_ptr<ConnectionPool>` member to RAII-destroy.

Both explicit `DETACH` and silent destruction end in the same terminal
state: pool gone, sockets closed, manager entry removed.

### FR-006: `g_context_managers` either goes away or its lifecycle is fixed

Two options:

- **(a) Remove it.** Diagnostic functions enumerate via DuckDB's catalog list
  (FR-003). The only remaining consumer is `MSSQLContextManager::Get(db)`
  which today just hands out a per-instance container for `name → ctx`
  shared state. Since each `MSSQLCatalog` would own its own pool + connection
  info, the `MSSQLContext` indirection is no longer load-bearing.
- **(b) Keep it but auto-clean.** Register an `ExtensionCallback::OnDestroyConnection`
  (or whatever DuckDB exposes for `~DatabaseInstance`) to `g_context_managers.erase(db_key)`
  on instance destruction. Pointer-reuse becomes a non-issue.

Option (a) is simpler and removes one more global; recommended.

### FR-007: Audit all process-wide state; classify and document

Spec 047 produces a `state_inventory.md` with every static / singleton state
the extension holds, classified as **legitimate** (one-time OS init, no
ownership ambiguity) or **migrate** (wrong ownership model, must move
per-instance or per-catalog). See Inventory below for the initial pass.

### FR-008: Multi-instance integration test

`test/sql/integration/multi_instance_pool_isolation.test` (or a C++ test if
SQL more clumsy here) exercises Scenarios 1–3 above. The test fails on
`main`-at-kickoff and passes after FR-001..FR-006 land.

## Inventory of process-wide state (current audit)

### Migrate (wrong ownership model)

| Symbol | File | Today | Problem | Fix |
|---|---|---|---|---|
| `MssqlPoolManager` (Meyers singleton via `Instance()`) | `src/connection/mssql_pool_manager.{hpp,cpp}` | Process-wide; `unordered_map<string name, unique_ptr<ConnectionPool>>` keyed only by name | Cross-instance pool sharing; DETACH cross-kill; silent-shutdown leak | Remove. Pool moves into `MSSQLCatalog` as `unique_ptr` member. |
| `g_context_managers` + `g_context_managers_lock` | `src/mssql_storage.cpp:792-793` | `case_insensitive_map_t<unique_ptr<MSSQLContextManager>>` keyed by `(uintptr_t)&DatabaseInstance`; lock | Pointer reuse hazard (band-aid in `RegisterContext`); leaks on `~DatabaseInstance`; redundant once Catalog owns the pool | Remove. Diagnostic enumeration via DuckDB catalog list. |
| `MSSQLConnectionHandleManager` (Meyers singleton) | `src/include/connection/mssql_diagnostic.hpp:15-37`, impl in `mssql_diagnostic.cpp:15` | Process-wide handle counter + `unordered_map<int64_t, shared_ptr<TdsConnection>>` for `mssql_open` / `mssql_close` / `mssql_ping` | Handles persist across `DatabaseInstance` lifecycle; no auto-cleanup on instance destruction; cross-instance handle visibility | Move into `MSSQLContextManager`-equivalent owned by `DatabaseInstance` (extension-state attached via `DatabaseInstance::GetExtensionInfo` or similar). Handles auto-released when instance dies. |
| `MSSQLResultStreamRegistry` (Meyers singleton) | `src/mssql_functions.cpp:41-44`, `src/include/mssql_functions.hpp:115-130` | Process-wide bridge between `Bind` and `InitGlobal` for `mssql_scan` result streams; `unordered_map<uint64_t id, unique_ptr<MSSQLResultStream>>` | Entries removed on `Retrieve`, so usually self-cleaning, BUT abandoned bind (planner rebinds, query cancelled before InitGlobal) leaks stream + its TDS connection until end of process | Move into per-instance state OR add timeout-based eviction. Lower-priority — bounded blast radius vs. pool issues. |
| `mssql::azure::TokenCache` (Meyers singleton) | `src/azure/azure_token.{hpp,cpp}` | Process-wide OAuth2 token cache keyed by `secret_name` | Cross-instance token sharing (probably actually desired — same secret name → same token), but no isolation if two instances mean different things by the same secret name | Keep as-is OR move per-instance with namespacing. Lowest priority — usually a deliberate optimization. Document the cross-instance sharing semantics. |

### Legitimate (one-time OS init, NOT a bug)

| Symbol | File | Why it's fine |
|---|---|---|
| `winsock_init_flag` + `winsock_initialized` | `src/tds/tds_socket.cpp:32-33` | Windows requires `WSAStartup` once per process. `std::once_flag` + `atexit(WSACleanup)`. Cannot be per-instance; this is a Win32 OS resource. |
| OpenSSL global init (implicit, via `OPENSSL_init_ssl` first call) | `src/tds/tls/tds_tls_impl.cpp` | OpenSSL ≥ 1.1 self-initializes on first use; cleanup via library atexit hooks. Per-instance overhead unjustified. |
| `static thread_local CreateSchemaInfo info` | `src/catalog/mssql_schema_entry.cpp:36` | Thread-local scratch buffer for DuckDB Bind callbacks. Scope is one bind operation; thread-local lifetime is bounded by thread, not process. |
| `static thread_local CreateTableInfo info` | `src/catalog/mssql_table_entry.cpp:60` | Same as above. |

## Plan (high level)

### Phase 1 — Move pool ownership to catalog

1. Change `MSSQLCatalog::connection_pool_` type from `shared_ptr` (with no-op deleter)
   to `unique_ptr<tds::ConnectionPool>`.
2. In `MSSQLCatalog::Initialize`, build the pool directly using the existing
   `pool_config` + connection factory logic that's currently in
   `MssqlPoolManager::GetOrCreatePool*` (3 overloads: SQL auth, Azure, integrated).
3. Move the three `MssqlPoolManager::GetOrCreatePool*` bodies into a helper inside
   `mssql_catalog.cpp` (or `connection_provider.cpp`) named `BuildConnectionPool(...)`.
4. Remove the call to `MssqlPoolManager::Instance().GetOrCreatePool*` from `MSSQLAttach`.
   `MSSQLAttach` becomes pure: validate connection, construct `MSSQLCatalog`, return.
5. `MSSQLCatalog::OnDetach` drops the `RemovePool` call (RAII handles it).
6. Migrate `Increment/DecrementPinnedCount` from pool manager to `tds::ConnectionPool`
   instance methods. Update `mssql_transaction.cpp:139,143`.
7. Migrate insert/update executors (`mssql_insert_executor.cpp:74`,
   `mssql_update_executor.cpp:73`) — defensive `GetPool` pre-check goes away
   since `ConnectionProvider::GetConnection` already validates via catalog.
8. Delete `MssqlPoolManager` class + header + .cpp.

### Phase 2 — Per-instance diagnostic enumeration

1. Rewrite `mssql_diagnostic.cpp::MssqlPoolStatsGlobalState` to populate
   `pool_names` by walking `context.db->GetDatabaseManager().GetDatabases()`
   and filtering MSSQL catalogs.
2. Per-pool stats fetched from `catalog.GetConnectionPool().GetStats()`.
3. `mssql_pool_stats('explicit-name')` becomes a single-catalog lookup via
   `Catalog::GetCatalog(context, name).Cast<MSSQLCatalog>()`.

### Phase 3 — Remove `g_context_managers`

1. `MSSQLContextManager` no longer needs to exist as a separate concept —
   `MSSQLCatalog` owns everything formerly stored in `MSSQLContext`.
2. Delete `g_context_managers`, `g_context_managers_lock`,
   `MSSQLContextManager` class.
3. Retire the spec 045 / commit `70a4d90` band-aid (`RegisterContext`
   silent-overwrite) since the pointer-reuse hazard goes away with the
   container.

### Phase 4 — `mssql_open` handle manager → per-instance

1. Define `MSSQLDiagnosticState` attached to `DatabaseInstance` via DuckDB's
   extension-state mechanism (or stored on the catalog of one of the attached
   DBs).
2. Move handle map + counter into it.
3. Handles still globally-unique within the instance but isolated between
   instances. Auto-cleanup on `~DatabaseInstance`.

### Phase 5 — Multi-instance integration test

1. Write the C++ multi-instance test under `test/cpp/test_multi_instance_pool_isolation.cpp`
   (since two `DuckDB` instances in the same process is hard to express in
   sqllogictest). Cover Scenarios 1–3.
2. Add it to `CMakeLists.txt`.
3. Verify it fails on `main`-at-kickoff and passes post-spec-047.

### Phase 6 — Polish

1. Remove the spec 045 band-aid in `MSSQLContextManager::RegisterContext`
   (revert silent-overwrite + conditional pool sweep). The original
   `throw if name already exists` returns, because under the new model the
   name CANNOT pre-exist (each catalog owns its own pool, no shared registry).
2. Update `CLAUDE.md` "Key Architecture Concepts" → "Connection pool"
   bullet to reflect per-catalog ownership.
3. Document `MSSQLResultStreamRegistry` lifecycle (still singleton — Phase 5
   covers it only with audit, not migration; tracked as separate work).
4. `TokenCache` documentation: explicit note that token cache is process-wide
   and that's intentional (deduplicates token acquisition for the same Azure
   secret across instances). If isolation is later required, add per-instance
   namespacing.

## Constraints / non-goals

- **Not changing the public extension API.** All extension functions
  (`mssql_scan`, `mssql_exec`, `mssql_pool_stats`, `mssql_refresh_cache`,
  `mssql_open`, `mssql_close`, `mssql_ping`, etc.) keep their signatures.
- **Not changing TDS protocol behavior.** Pool semantics (acquire / release /
  pin / idle timeout / connection limit) are preserved exactly; only
  ownership and visibility scope change.
- **TokenCache stays singleton** in spec 047 — documented but not migrated.
  Migration is a separate spec if/when an explicit need surfaces.
- **No backward compatibility shims.** The pool manager removal is a clean
  break; once it's gone, it's gone.

## Open questions

- **Does DuckDB expose a `~DatabaseInstance` callback for extensions?** If
  yes, an alternative to FR-006 option (a) is to keep `g_context_managers`
  but wire auto-cleanup. If no, option (a) is the only path.
- **Pool sharing across attachments in the same instance.** If user does
  `ATTACH '<dsn>' AS a; ATTACH '<dsn>' AS b;` to the same SQL Server, today
  they share a pool (same `name` key collision). After spec 047 each gets
  its own pool — 2x connection limit, 2x overhead. Acceptable? (User behavior
  question — likely yes, since they're independent catalogs.)
- **`MSSQLResultStreamRegistry` — defer or include?** Bounded blast radius
  vs. principle-of-no-process-state. Recommend defer for incremental landing.

## References

- Spec 045 commit `70a4d90` — band-aid for `g_context_managers` pointer-reuse
- Spec 045 commit `7bbdf28` — TIMESTAMP_* round-trip (concomitant with pool
  discussion)
- DuckDB AttachedDatabase / DatabaseInstance lifecycle: TBD link
- `src/connection/mssql_connection_provider.cpp:97-149` —
  `ConnectionProvider::GetConnection` (already correct: goes through
  catalog, not singleton)
- `src/copy/copy_function.cpp:248,624,750` — COPY (already correct: catalog)
- `src/table_scan/table_scan.cpp` + `src/mssql_functions.cpp:377` — table
  scan (already correct: catalog via ConnectionProvider)

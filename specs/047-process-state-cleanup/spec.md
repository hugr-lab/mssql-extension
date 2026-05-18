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

## Clarifications

### Session 2026-05-17

- Q: FR-006 — remove `g_context_managers` entirely (option a) or keep it and add an auto-cleanup hook on `~DatabaseInstance` (option b)? → A: Remove entirely. Diagnostic enumeration goes through DuckDB's catalog list; `MSSQLContextManager` class is deleted; spec 045 band-aid (`70a4d90` `RegisterContext` silent-overwrite) is retired in Phase 3.
- Q: `MSSQLResultStreamRegistry` — include migration in spec 047 or defer to a separate spec? → A: Include in 047, but move into `MSSQLCatalog` as a member (NOT into a per-`DatabaseInstance` container). `mssql_scan(context, query)` is catalog-bound by API (the `context` argument names an attached MSSQL catalog), so the registry naturally belongs in the catalog. Catalog destruction auto-zeroes any orphan streams via RAII. One mutex + one `unordered_map` member, no new helper classes.
- Q (revision after Q2 reread): `MSSQLConnectionHandleManager` (`mssql_open` / `mssql_close` / `mssql_ping`) — migrate or stay singleton? → A: Stay singleton; reclassify as **legitimate**. These functions are diagnostic helpers that take a connection string (NOT a catalog name) and return an opaque `BIGINT` handle that survives across SQL statements. The handle map needs to outlive any single function call but is correctly scoped to the process — there is no catalog to attach it to, and `mssql_ping(handle)` / `mssql_close(handle)` need to find the handle without a context discriminator. The state is not load-bearing for any correctness invariant (no cross-pool contamination, no shutdown leak that matters at process scope), so the global singleton is the right answer.
- Addendum: `mssql_open` / `mssql_close` / `mssql_ping` are additionally marked **`[DEPRECATED]`** in spec 047 (see FR-010). Catalog-based alternatives (`ATTACH` + `mssql_scan` / `mssql_exec`) cover the practical use cases; the diagnostic API and its singleton are kept for backward compatibility and slated for removal in a future major release. This makes the legitimate-classification time-bounded: when the functions go, so does the singleton, and process-wide state in the extension drops to the truly-OS-level set (Winsock + OpenSSL + TokenCache).
- Addendum: ATTACH **eager credential validation** (FR-011) is added to spec 047. Today's lazy connect masks bad credentials until the first query (issue surfaced by user during plan review: "ATTACH passes even with wrong password"). After 047, ATTACH synchronously acquires + releases one connection from the pool, propagating any TCP / DNS / LOGIN7 / FEDAUTH / Kerberos error to the user up-front. Opt-out via `lazy_validation=true` for users who need the old behavior (containerized startup, etc.). Default: eager. Bonus benefit: the validation acquire warms one connection in the pool for the first query (zero net cost for the happy path).
- Q: Azure `TokenCache` scope in spec 047 — migrate (per-instance / namespaced) or keep singleton? → A: Keep singleton. Cross-instance token sharing by `secret_name` is a deliberate optimization (avoids redundant device-code / client-secret round-trips for the same secret); the isolation concern is theoretical since secret namespace is already per-instance via DuckDB `CREATE SECRET`. Spec 047 reclassifies TokenCache as **legitimate** with documented sharing semantics.
- Q: Same-DSN attachments within one instance (`ATTACH '<dsn>' AS a; ATTACH '<dsn>' AS b;`) — share a pool or one pool per catalog? → A: One pool per catalog, unconditionally. Spec 047's ownership model is cleaner if unconditional; pool dedup by DSN would re-couple lifetime/transaction state across catalogs. Cost: 2× connection limit + 2× idle overhead per duplicate DSN attach. Document in release notes; surface in `mssql_pool_stats` per-catalog rows.
- Q: Silent-shutdown leak reliability SC — iteration count for the no-leaked-socket test (Scenario 3 gate)? → A: 100 iterations. Fast (~3 sec CI cost), catches per-iteration linear leaks (would produce ~100 ghost sockets — trivially detectable via DMV / netstat). If implementation surfaces slow-leak suspicion during plan/implementation, threshold can be raised in a follow-up before merge.
- Addendum: Related production bug **[issue #96](https://github.com/hugr-lab/mssql-extension/issues/96)** (`Catalog Error: MSSQL Error: Context 'TO_MSSQL' already exists. Use a different name or DETACH first`) is the production manifestation of this spec's bug class. Reproduced in Python / DuckDB.Net 1.5+ via a `duckdb.connect(":memory:")` loop where each iteration ATTACHes the same alias without an explicit DETACH. Added as Scenario 4 + SC-009; spec 047 closes it.

### Session 2026-05-18 (post PR #118 security review)

External security review of PR #118 surfaced a handful of design-level items that needed explicit handling in 047 rather than being deferred. Of the 6 review points, 5 are folded into this spec; 2 are explicitly out-of-scope and tracked elsewhere (see Constraints / non-goals).

- Q: `mssql::azure::TokenCache` key is today `secret_name` (a bare string) — across two DuckDB instances in the same process, two `CREATE SECRET mssql_secret WITH (...)` statements with the same name but different `client_secret` / `tenant` would alias each other's cached OAuth2 tokens. Keep singleton, change key, or document? → A: **Keep singleton; change cache key** to namespace by the owning `DatabaseInstance` (key becomes `(database_instance_id, secret_name)`). Trivial change, eliminates the theoretical-but-real cross-instance token leak. Added as FR-012 + SC-011.
- Q: After 047, `MSSQLConnectionHandleManager` is the only surviving cross-instance credentialed-state holder. Long-running embedding processes that open `mssql_open()` handles and never `mssql_close()` them retain sockets + creds for process lifetime. Add automatic cleanup or just a user-callable bulk-close? → A: **Add `mssql_close_all()` scalar function** (also `[DEPRECATED]` per FR-010 group). Automatic cleanup on `~DatabaseInstance` is fragile (handle map doesn't know which instance opened which handle, and DuckDB has no `~DatabaseInstance` hook for us). `mssql_close_all()` is a tool users can wire into orchestrator shutdown / `atexit` / equivalent. Added as FR-013.
- Q: FR-011 eager validation moves the LOGIN7 error site from deep inside `pool.Acquire()` (where the connection string isn't directly in scope) to `MSSQLAttach` (where it absolutely is). A naive `<<connection_string` in the error wrap → password leaks into `what()` → into user-facing exceptions → into logs. → A: SC-010 gets an explicit assertion: the bad-credentials error message **must not contain** the password substring. Audit `MSSQLAttach` error wrapping accordingly.
- Q: FR-003 rewrites `mssql_pool_stats()` to project directly from per-catalog `connection_info_`, which holds passwords / FEDAUTH tokens / Kerberos keytab paths. Today's singleton output already redacts at the projection layer; the rewrite must preserve that. → A: SC-005 extended with a grep-style assertion: no column in `mssql_pool_stats()` output contains password / token / keytab values for any auth method (SQL / FEDAUTH / Kerberos).
- Q: `~MSSQLCatalog` RAII chain — destructors must be `noexcept` (throw during stack unwind = `std::terminate`). Also: `~ConnectionPool` semantics if connections are still checked out. → A: Polish task (T047a) audits all destructors in the touched ownership chain for `noexcept`. `~ConnectionPool` adds a debug-only assert (no checked-out connections at destruct time — DuckDB contract is that quiescence precedes `~AttachedDatabase`); in release, it forcibly closes underlying sockets without blocking. Doc-comment in `tds_connection_pool.hpp` makes the contract explicit. Graceful in-flight TDS cancellation is **out of scope** (see Constraints / non-goals).

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

### Bundled enhancement: Custom `Application Name` (issue #82)

While we are touching connection-string parsing and the LOGIN7 build path,
spec 047 also resolves [issue #82](https://github.com/hugr-lab/mssql-extension/issues/82):
the extension currently hard-codes `AppName="DuckDB MSSQL Extension"` in
LOGIN7 (and `"DuckDB"` in the three SQL-auth strategies), ignoring any
`Application Name=foo` key the user supplies in the connection string.
SQL Server Profiler / `sys.dm_exec_sessions.program_name` therefore always
shows the hard-coded string, making it impossible for ops to attribute
sessions to a specific application.

The fix is small and fully orthogonal to the ownership refactor (FR-014 /
SC-012 / US-AN below); it shares no files with US1-4. Bundled here because
it touches the same surface (`MSSQLConnectionInfo` parsing + LOGIN7) and
because shipping it as a separate spec would be ceremony for ~50 LOC.

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

The band-aid is correct ONLY for pointer-reuse-after-destruction in a
single-threaded test harness. It does nothing for:

- Two concurrent live instances sharing a pool (no stale entry to detect —
  the second manager is fresh and empty when its `RegisterContext` runs).
- `DETACH` on one instance taking down another's pool (the pool manager
  doesn't know which catalog issued the detach).
- Silent shutdown leaking pool + manager-map entries forever (no destructor
  hook to trigger the band-aid).
- **Production manifestation — issue #96**: short-lived `duckdb.connect(":memory:")`
  in a loop (Python / DuckDB.Net 1.5.0+) where each iteration creates a fresh
  `DatabaseInstance` and ATTACHes under the same alias. On iteration 2+, the
  user hits `Catalog Error: MSSQL Error: Context 'dbalias' already exists.
  Use a different name or DETACH first` — same bug class as the test-only
  `--force-reload` scenario, but in real user code. The band-aid only fires
  inside `MSSQLContextManager::RegisterContext` once a manager has been
  created; if the user's prior iteration left state under the previous
  instance's pointer (without ever issuing `DETACH`), the singleton-keyed
  pool persists and collides on alias name with the new instance.

Spec 047 retires the band-aid in favour of the structural fix; **closes
issue #96** as a side effect.

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

### Scenario 4: Short-lived instances in a loop (issue #96)

Production manifestation reported by the user in [issue #96](https://github.com/hugr-lab/mssql-extension/issues/96).
Verbatim repro (Python / DuckDB.Net 1.5.0+):

```python
import duckdb
sql_script = """
INSTALL mssql FROM community;
LOAD mssql;
CREATE OR REPLACE SECRET mssql_secret (TYPE mssql, HOST 'host', PORT 1433, ...);
ATTACH '' AS TO_MSSQL (TYPE mssql, SECRET mssql_secret);
"""
for i in range(1, 10):
    con = duckdb.connect(database=":memory:")   # fresh DuckDB instance each iteration
    try:
        con.execute(sql_script)                  # MUST succeed every iteration
        print(f"Iteration {i}: ok")
    finally:
        con.close()                              # no explicit DETACH
```

**Today**: iteration 2+ throws `Catalog Error: MSSQL Error: Context
'TO_MSSQL' already exists. Use a different name or DETACH first`. The fresh
instance's `RegisterContext` collides with stale `g_context_managers` /
`MssqlPoolManager` state from the previous iteration (which was never
torn down because the user never issued `DETACH` before `close()`).
Workaround per issue reporter: insert `con.execute("DETACH TO_MSSQL;")`
before `close()`. Workaround is unacceptable in real applications that
can't guarantee paired ATTACH/DETACH (e.g. orchestrator frameworks, ASP.NET
request-scoped lifetimes, exception unwinding).

**After spec 047**: no shared singleton state to collide on. Each
iteration's `~MSSQLCatalog` (via `~DuckDB`'s catalog teardown) drops the
owned pool and closes sockets via RAII. Loop succeeds for all N iterations
without the workaround.

## Requirements

### FR-001: Per-catalog pool ownership

`MSSQLCatalog` owns its `tds::ConnectionPool` via `unique_ptr` (today it
holds a `shared_ptr` with a no-op deleter into the singleton's map). The
pool is constructed inside `MSSQLCatalog::Initialize` from connection_info_
and the global `MSSQLPoolConfig` already passed through `MSSQLAttach`.

**Pool-per-catalog is unconditional.** Two `ATTACH` statements pointing at
the same DSN under different aliases get two independent pools (today they
share one via name collision in the singleton). Cost: 2× connection limit +
2× idle overhead per duplicate-DSN attach within the same instance. Benefit:
no cross-catalog lifetime/transaction coupling; cleaner ownership semantics;
`mssql_pool_stats` reports a row per catalog. Documented in release notes.

**Note on credential footprint:** as a side-effect, duplicate-DSN attaches
also double the in-memory copy count of `Password` / FEDAUTH token / Kerberos
ccache path (one `MSSQLConnectionInfo` per pool instead of one shared entry).
This is an accepted tradeoff in spec 047 — eliminating it requires a separate
`CredentialProvider` redesign that splits secret-backed auth (where the
password can be materialized transiently at LOGIN7 time) from connection-string
auth (where DuckDB itself holds the credential in catalog metadata). Tracked
in [issue #119](https://github.com/hugr-lab/mssql-extension/issues/119) and
scheduled as a future spec; see Constraints / non-goals.

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

### FR-006: Remove `g_context_managers`

`g_context_managers` and `g_context_managers_lock` are deleted. Diagnostic
functions enumerate per-instance state by walking DuckDB's catalog list
(FR-003). The `MSSQLContextManager` class is dissolved: its only remaining
job — handing out a per-instance container for `name → ctx` shared state —
is no longer load-bearing once each `MSSQLCatalog` owns its own pool +
connection info.

The spec 045 band-aid (`70a4d90` `RegisterContext` silent-overwrite +
conditional `RemovePool` sweep) is retired in Phase 3; the pointer-reuse
hazard the band-aid worked around vanishes with the container.

### FR-007: Audit all process-wide state; classify and document

Spec 047 produces a `state_inventory.md` with every static / singleton state
the extension holds, classified as **legitimate** (one-time OS init, no
ownership ambiguity) or **migrate** (wrong ownership model, must move
per-instance or per-catalog). See Inventory below for the initial pass.

### FR-008: Multi-instance integration test

`test/sql/integration/multi_instance_pool_isolation.test` (or a C++ test if
SQL more clumsy here) exercises Scenarios 1–3 above. The test fails on
`main`-at-kickoff and passes after FR-001..FR-006 land.

### FR-009: Result stream registry → catalog member

`MSSQLResultStreamRegistry` (today a process-wide Meyers singleton in
`src/mssql_functions.cpp`) is dissolved as a class; its state — a UUID-keyed
`unordered_map<string, unique_ptr<MSSQLResultStream>>` plus its mutex — moves
into `MSSQLCatalog` as two private members (`streams_mutex_`,
`active_streams_`). `mssql_scan(context, query)` is catalog-bound by API
(the `context` argument names an attached MSSQL catalog), so the bridge state
naturally belongs in the catalog. Bind calls `catalog.RegisterStream(...)`
to get a UUID; `InitGlobal` calls `catalog.RetrieveStream(uuid)` to pull it
back. Catalog destruction (`~MSSQLCatalog` via the `unique_ptr` member chain)
auto-cleans any orphan streams (abandoned-bind leaks from planner rebinds or
query cancellation are bounded by catalog lifetime rather than process
lifetime).

The UUID-based handle (not raw pointer) is retained because DuckDB's
`BindData` is serializable / copyable / comparable — a UUID survives those
operations cleanly while a raw `unique_ptr<MSSQLResultStream>` would not.

### FR-011: Eager connection validation on ATTACH

Today's `MssqlPoolManager::GetOrCreatePool*` constructs a pool with a lazy
connection factory — actual TCP/TDS connection + LOGIN7 (and hence credential
verification) only happens on the first `pool.Acquire()` call, which is
triggered by the first real query. Consequence: `ATTACH 'Server=...;User Id=...;Password=WRONG' AS db (TYPE mssql)`
succeeds; the first `SELECT * FROM db.dbo.X` then fails with a confusing
"login failed for user" error far from the actual statement that caused the
misconfiguration.

After spec 047, `MSSQLCatalog::Initialize` performs **synchronous
validation** right after constructing the pool:

1. Call `pool.Acquire()` once with a short configurable timeout
   (`mssql_attach_validation_timeout`, default = `mssql_connection_timeout`).
2. If acquire succeeds, immediately release the connection back to the pool
   (it stays warm for the first real query).
3. If acquire fails (TCP refused, DNS failure, LOGIN7 rejected, FEDAUTH
   token rejection, Kerberos service ticket failure, etc.), the exception
   propagates out of `Initialize`; `MSSQLAttach` propagates it to the user
   with the verbatim TDS error message attached.

ATTACH then either fully succeeds (catalog ready + credentials confirmed +
one warm connection in the pool) or fully fails (no half-attached state,
no surprise on first query).

Opt-out: new ATTACH option `lazy_validation=true` (and equivalent in
connection string: `LazyValidation=true`) skips the validation step for
users who explicitly want today's lazy behavior (e.g., attaching to a server
expected to come up later, or in containerized startup orchestration where
the SQL Server might not be reachable at attach time but will be at query
time). Default: `lazy_validation=false` (eager).

**Security requirement — no credential leak in error message.** The eager
validation moves the error site from deep inside `pool.Acquire()` (where the
connection string isn't directly in scope) to `MSSQLAttach` (where the full
parsed connection string is in scope, including `Password=…`). The wrapping
code that catches the validation exception and rethrows with context **must
not** concatenate the connection string into the error message. Acceptable:
the verbatim TDS error from SQL Server (`Login failed for user 'sa'`), plus
the catalog alias, plus the host:port. **Not acceptable**: any substring of
the password / FEDAUTH token / Kerberos keytab path. Asserted in SC-010.

### FR-012: TokenCache key namespacing per DatabaseInstance

`mssql::azure::TokenCache` (Meyers singleton) is kept (per clarification Q3 —
legitimate cross-instance optimization), but its cache key changes from
`secret_name` to `(database_instance_id, secret_name)`.

Today's bug: if two DuckDB instances in the same process each define a
secret with the same name (e.g., `mssql_secret`) but different content
(different `tenant_id`, `client_secret`, or `scope`), the second instance's
lookup gets the first instance's cached token. The DuckDB Secret namespace
is per-`DatabaseInstance`, so the names colliding is realistic — many
applications use a fixed secret name like `mssql_secret` in templates.

The `database_instance_id` is the `uintptr_t` cast of the `DatabaseInstance*`
(same identity already used elsewhere in this spec for catalog dispatch).
The same secret-name from two different instances now misses cache and
fetches independently. The optimization clarification Q3 promised
(deduplication across two ATTACHes within one instance using the same
secret) is preserved.

When an instance dies (`~DatabaseInstance`), its entries become unreachable
but stay in the cache map. Spec 047 does not add a per-instance reaper —
entries are evicted lazily on the existing TTL path (cache entries already
have an expiry inherited from the token's `expires_in`). Long-running
processes that create + destroy many `DatabaseInstance`s with rotating
secret names will see modest cache growth bounded by token TTL; documented.

### FR-013: `mssql_close_all()` bulk handle reset

New scalar function `mssql_close_all()` returns `INTEGER` (the count of
handles closed). Internally walks `MSSQLConnectionHandleManager`'s map,
closes every live `TdsConnection`, drops every entry. Atomic under the
manager's mutex; idempotent (returns 0 if no handles).

Marked `[DEPRECATED]` from registration alongside `mssql_open` / `_close` /
`_ping` (it's part of the same diagnostic-helper family). Documented in
`CHANGELOG.md` as the recommended shutdown hook for long-running embedding
processes that use the diagnostic API. Catalog-based usage doesn't need it.

### FR-010: Deprecate `mssql_open` / `mssql_close` / `mssql_ping`

These diagnostic functions stay functional (handle manager singleton kept as
legitimate per the clarification above) but are marked **deprecated** in
this spec:

1. Their `DESCRIPTION` field at registration time (in
   `src/connection/mssql_diagnostic.cpp` `RegisterMSSQLDiagnosticFunctions`)
   gains a `[DEPRECATED]` prefix and a one-line guidance pointing users at
   the catalog-based alternatives (`ATTACH` + `mssql_scan` / `mssql_exec` for
   most use cases; `mssql_pool_stats` for pool diagnostics).
2. `CLAUDE.md` "Extension Functions" table marks the row with `[DEPRECATED]`.
3. A NOTE goes into `CHANGELOG.md` — these functions are kept for
   backward compatibility but should not be used in new code; the handle
   manager singleton is retained only for them and may be removed when the
   functions are removed in a future major release.

No deletion in this spec — only marking. If/when a future spec removes the
functions, the singleton goes with them, ending all process-wide state in
this category.

A companion bulk-close helper `mssql_close_all()` (see FR-013) is added so
that long-running embedding processes have a single call to drop accumulated
diagnostic handles at shutdown — without forcing them to track individual
handle ids.

### FR-014: Custom `Application Name` in connection string (issue #82) — US-AN

Add `application_name` to `MSSQLConnectionInfo` (default empty). Parse the
following connection-string keys (case-insensitive) into that field, in both
ADO.NET and URI parsers, and in the secret reader:

- `Application Name` (with space — ADO.NET / pyodbc canonical form)
- `ApplicationName` (no space — variant accepted by `microsoft/go-mssqldb`)
- `App Name` (short alias)
- `applicationname` (URI / secret underscore-free form)
- `application_name` (secret underscore form, matching existing convention
  for other underscored keys like `krb5_keytabfile`)

If `application_name` is non-empty, propagate it to `AuthOptions.app_name`
in **all** authentication strategies — SQL auth (`sql_auth_strategy.cpp`),
Manual token (`manual_token_strategy.cpp`), FedAuth
(`fedauth_strategy.cpp`), and Integrated/Kerberos/SSPI
(`integrated_auth_strategy.hpp`). Today three of those four strategies
hard-code `"DuckDB"` and the integrated one hard-codes
`"DuckDB MSSQL Extension"` — all four must consult `info.application_name`
first.

If `application_name` is empty (default — current behavior), the four
strategies fall back to a **unified** default string `"DuckDB MSSQL Extension"`
(today three say `"DuckDB"` and one says `"DuckDB MSSQL Extension"` — a
separate bug fixed in passing by this FR; users who saw `"DuckDB"` in their
profiler before will see `"DuckDB MSSQL Extension"` after, matching the
extension's existing public name and the integrated-auth precedent).

LOGIN7 `AppName` field has no length cap from the protocol side, but
SQL Server clamps `program_name` to 128 chars. Truncate at 128 with a
DEBUG-level log line; do not error.

**Out of scope for FR-014**: `WorkstationID` / `Host Name` / `WSID`
connection-string keys (a related issue #82 mentions but not the primary
ask). Tracked as a separate enhancement if requested.

## Success Criteria

Measurable outcomes derived from Requirements + User Scenarios. All criteria
must pass before spec 047 merges.

### SC-001: Multi-instance routing correctness

Scenario 1 test (`test/cpp/test_multi_instance_pool_isolation.cpp`): two
concurrent `DuckDB` instances each `ATTACH` to a distinct SQL Server target
under the same alias `mssql` and issue queries — both queries route to the
correct server (`@@SERVERNAME` matches expected). Fails on `main`-at-kickoff;
passes after FR-001 + FR-002 land.

### SC-002: DETACH-isolation correctness

Scenario 2 test: after instance A issues `DETACH mssql`, instance B's
queries against its own `mssql` catalog still succeed. Fails on
`main`-at-kickoff; passes after FR-001 + FR-005 land.

### SC-003: Silent-shutdown reliability

Scenario 3 test: 100 iterations of `{ DuckDB db; ATTACH; SELECT 1; }` with
no explicit `DETACH`. After loop completes, `sys.dm_exec_connections` (or
netstat for the local case) reports **zero residual TDS connections** from
this process to the SQL Server. Fails on `main`-at-kickoff; passes after
FR-005 lands.

### SC-004: Singleton removal

`grep -rn 'MssqlPoolManager\|MSSQLContextManager\|MSSQLConnectionHandleManager\|MSSQLResultStreamRegistry' src/ include/`
returns **zero matches** (excluding test/ and comment-only mentions). Verifies
FR-002 + FR-006 + FR-009 + Phase 4 cleanup all landed.

### SC-005: Diagnostic enumeration parity + credential redaction

`SELECT * FROM mssql_pool_stats()` on a single-attach instance returns the same
row count and the same column values (modulo per-run numeric stats) before
and after spec 047. Verifies FR-003 doesn't regress observability. With two
attaches to the same DSN under different aliases, returns 2 rows (per
clarification Q4).

**Credential redaction gate (added per PR #118 security review).** For each
of the three auth methods (SQL auth, Azure FEDAUTH, Integrated/Kerberos), the
test attaches a catalog with credentials that include a known-unique sentinel
string in the password / token / keytab path, then queries `mssql_pool_stats()`
and asserts via `string_contains` that no column in any row contains that
sentinel substring. This ensures the FR-003 rewrite (per-catalog projection
from `connection_info_`) doesn't accidentally leak credentials that today's
singleton-keyed output was redacting at the projection layer.

### SC-006: Result stream registry isolation

C++ test: two `DuckDB` instances each ATTACH and run `mssql_scan` against
different MSSQL catalogs concurrently. The UUID returned by Bind in
instance A's catalog must not be retrievable from instance B's catalog
(no cross-catalog stream visibility). Verifies FR-009 (registry on
catalog). Fails on `main`-at-kickoff (singleton registry shares streams
across instances); passes after Phase 4 lands.

### SC-007: No public API regression

All extension functions (`mssql_scan`, `mssql_exec`, `mssql_pool_stats`,
`mssql_refresh_cache`, `mssql_open`, `mssql_close`, `mssql_ping`, etc.)
keep their signatures and observable semantics. Existing test suite
(`make test` + `make integration-test`) stays green at every commit.

### SC-008: Process-wide static state inventory

After implementation, `state_inventory.md` lists every process-wide static
the extension holds. Every "migrate" entry from the initial Inventory is
either resolved (removed or moved) or explicitly retained with rationale
documented in the "legitimate" table. No new "migrate" entries introduced.

### SC-010: ATTACH credential validation

Test `test/sql/attach/attach_validates_credentials.test`:

1. `ATTACH 'Server=localhost,1433;Database=TestDB;User Id=sa;Password=WRONG_PASSWORD_SENTINEL' AS bad (TYPE mssql);` — **MUST** fail with a login error containing the verbatim "Login failed for user 'sa'" string from SQL Server. The error message **MUST NOT** contain the substring `WRONG_PASSWORD_SENTINEL` (asserted via `string_contains` in the test) — preserves the FR-011 security requirement that credentials never leak into user-facing exceptions. **Today**: passes silently; first subsequent query fails.
2. `ATTACH 'Server=unreachable.invalid,1433;Database=db;User Id=u;Password=p' AS unreachable (TYPE mssql);` — **MUST** fail with a connection error (DNS resolution failure / TCP timeout) within the configured `mssql_attach_validation_timeout`. **Today**: passes silently.
3. `ATTACH 'Server=unreachable.invalid,1433;Database=db;User Id=u;Password=p' AS lazy (TYPE mssql, lazy_validation=true);` — **MUST** succeed (opt-out preserves today's behavior); subsequent query fails with the connection error.
4. Existing valid-credential `ATTACH` tests continue to pass with no observable change (the warm connection produced by validation is invisible).

### SC-011: TokenCache cross-instance isolation

C++ test `test/cpp/test_token_cache_isolation.cpp`: construct two `DuckDB`
instances `db_a` and `db_b`. In each instance, `CREATE SECRET mssql_secret`
with the same name but **different** `client_secret` values (or stub
test-only fake secrets that the test harness can distinguish). Trigger token
acquisition in instance A first (via `mssql_azure_auth_test('mssql_secret')`
or equivalent path), then trigger the same in instance B. Assert that
B's acquisition resolves with B's secret value, not A's cached token.

Verifies FR-012 (key is `(database_instance_id, secret_name)`, not bare
`secret_name`). **Today**: fails (B reuses A's cached token because cache
key is bare `secret_name`). **After FR-012**: passes — each instance has
its own cache entry.

### SC-009: Issue #96 regression test

C++ test `test/cpp/test_issue_96_attach_loop.cpp` mirrors the Python
loop from [issue #96](https://github.com/hugr-lab/mssql-extension/issues/96):
construct 100 sequential `DuckDB` instances (`:memory:`), each ATTACH-es
the same alias without an explicit DETACH, runs a smoke query, then
destructs. Every iteration must succeed; no `"Context 'X' already exists"`
errors; no leaked SQL Server connections after the loop completes (verified
via `sys.dm_exec_connections`). Fails on `main`-at-kickoff (reproduces issue
#96); passes after FR-001 + FR-005 + FR-006 land. Closes issue #96.

### SC-012: Custom `Application Name` round-trips to SQL Server (issue #82)

SQL test `test/sql/attach/application_name.test` (requires SQL Server):

1. `ATTACH 'Server=...;Database=...;Application Name=MyHugrApp' AS s (TYPE mssql)`
   → `SELECT APP_NAME() FROM mssql_scan('s', 'SELECT 1')` returns `MyHugrApp`.
2. Same with the variant key spellings (`App Name=`, `ApplicationName=`)
   — each returns the supplied value.
3. ATTACH without `Application Name=` → `APP_NAME()` returns
   `DuckDB MSSQL Extension` (the unified default per FR-014).
4. ATTACH via `CREATE SECRET` carrying `application_name = 'SecretApp'`
   → `APP_NAME()` returns `SecretApp`.
5. 200-character value → `APP_NAME()` returns the first 128 characters
   (SQL Server clamp, verified by `LEN(APP_NAME()) = 128`).

Verifies FR-014 across both connection-string parsers (ADO.NET) and the
secret reader, and across all four auth strategies (the test repeats step 1
for each: SQL auth, manual token, Azure FEDAUTH where Azure secret is
configured, Integrated auth on the Kerberos test rig). **Today**: fails
(steps 1-2 return `DuckDB`; step 3 returns `DuckDB` or `DuckDB MSSQL Extension`
depending on which strategy was used; step 4 same; step 5 N/A — feature
absent). **After FR-014**: all five steps pass. Closes issue #82.

---

## Inventory of process-wide state (current audit)

### Migrate (wrong ownership model)

| Symbol | File | Today | Problem | Fix |
|---|---|---|---|---|
| `MssqlPoolManager` (Meyers singleton via `Instance()`) | `src/connection/mssql_pool_manager.{hpp,cpp}` | Process-wide; `unordered_map<string name, unique_ptr<ConnectionPool>>` keyed only by name | Cross-instance pool sharing; DETACH cross-kill; silent-shutdown leak | Remove. Pool moves into `MSSQLCatalog` as `unique_ptr` member. |
| `g_context_managers` + `g_context_managers_lock` | `src/mssql_storage.cpp:792-793` | `case_insensitive_map_t<unique_ptr<MSSQLContextManager>>` keyed by `(uintptr_t)&DatabaseInstance`; lock | Pointer reuse hazard (band-aid in `RegisterContext`); leaks on `~DatabaseInstance`; redundant once Catalog owns the pool | Remove. Diagnostic enumeration via DuckDB catalog list. |
| `MSSQLResultStreamRegistry` (Meyers singleton) | `src/mssql_functions.cpp:41-44`, `src/include/mssql_functions.hpp:115-130` | Process-wide bridge between `Bind` and `InitGlobal` for `mssql_scan` result streams; `unordered_map<uint64_t id, unique_ptr<MSSQLResultStream>>` | Entries removed on `Retrieve`, so usually self-cleaning, BUT abandoned bind (planner rebinds, query cancelled before InitGlobal) leaks stream + its TDS connection until end of process | Move into `MSSQLCatalog` as members (`streams_mutex_` + `active_streams_`). `mssql_scan` is catalog-bound by API; orphan streams auto-cleaned at `~MSSQLCatalog`. |
### Legitimate (one-time OS init or deliberate process-wide sharing, NOT a bug)

| Symbol | File | Why it's fine |
|---|---|---|
| `winsock_init_flag` + `winsock_initialized` | `src/tds/tds_socket.cpp:32-33` | Windows requires `WSAStartup` once per process. `std::once_flag` + `atexit(WSACleanup)`. Cannot be per-instance; this is a Win32 OS resource. |
| OpenSSL global init (implicit, via `OPENSSL_init_ssl` first call) | `src/tds/tls/tds_tls_impl.cpp` | OpenSSL ≥ 1.1 self-initializes on first use; cleanup via library atexit hooks. Per-instance overhead unjustified. |
| `static thread_local CreateSchemaInfo info` | `src/catalog/mssql_schema_entry.cpp:36` | Thread-local scratch buffer for DuckDB Bind callbacks. Scope is one bind operation; thread-local lifetime is bounded by thread, not process. |
| `static thread_local CreateTableInfo info` | `src/catalog/mssql_table_entry.cpp:60` | Same as above. |
| `mssql::azure::TokenCache` (Meyers singleton) | `src/azure/azure_token.{hpp,cpp}` | Process-wide OAuth2 token cache. Cross-instance sharing is a **deliberate optimization** — same secret name within one instance → same OAuth2 token reused, avoiding redundant device-code / client-secret round-trips. Per PR #118 security review (FR-012), cache key changes from bare `secret_name` to `(database_instance_id, secret_name)` to eliminate the cross-instance leak that the prior "theoretical risk" framing left open. Keep singleton; namespaced key; documented sharing semantics. |
| `MSSQLConnectionHandleManager` (Meyers singleton) | `src/include/connection/mssql_diagnostic.hpp:15-37`, impl in `mssql_diagnostic.cpp:15` | Process-wide handle counter + `unordered_map<int64_t, shared_ptr<TdsConnection>>` for `mssql_open` / `mssql_close` / `mssql_ping`. These functions are diagnostic helpers; `mssql_open(secret_or_dsn)` returns an opaque `BIGINT` handle that survives across SQL statements; subsequent `mssql_ping(handle)` / `mssql_close(handle)` calls have no catalog discriminator by API design (no `context_name` argument). The map IS process-wide state, but it is **not** load-bearing for any cross-instance correctness invariant — there is no pool sharing, no transaction state, no shutdown leak that matters. Static field is the right answer. **Note**: per FR-010 the surrounding functions are marked `[DEPRECATED]` in this spec; the singleton is retained for as long as the functions exist and is slated for removal when a future spec removes the functions. |

## Plan (high level)

### Phase 1 — Move pool ownership to catalog

1. Change `MSSQLCatalog::connection_pool_` type from `shared_ptr<tds::ConnectionPool>`
   (today with a no-op deleter aliasing the singleton's map) to `unique_ptr<tds::ConnectionPool>`
   (real owner; RAII destruction).
2. In `MSSQLCatalog::Initialize`, inline the pool-construction logic from
   `MssqlPoolManager::GetOrCreatePool*` (3 branches by `auth_method`: SQL auth,
   Azure FEDAUTH, Integrated/Kerberos/SSPI). The factory lambda + `PoolConfiguration`
   translation move from the singleton straight into `Initialize`. No new file,
   no new helper module — just delete the singleton wrappers around the existing
   construction code.
3. Remove the `MssqlPoolManager::Instance().GetOrCreatePool*` call from `mssql_storage.cpp`
   (`MSSQLAttach`). The catalog is now constructed and immediately initialized; the
   `MSSQLAttach` function no longer needs to interact with the singleton at all.
4. `MSSQLCatalog::OnDetach` drops its `MssqlPoolManager::Instance().RemovePool(...)`
   call (line 630) — RAII via `~MSSQLCatalog` handles teardown.
5. Migrate pin counter: add `atomic<int64_t> pinned_count_` member to
   `tds::ConnectionPool` + public `IncrementPinned/DecrementPinned/GetPinnedCount`
   methods. Callers (`mssql_transaction.cpp:139,143`) change from
   `MssqlPoolManager::Instance().IncrementPinnedCount(name)` to
   `catalog_.GetConnectionPool().IncrementPinned()`.
6. Fix 4 wrong call sites that bypass catalog (today they work only because
   singleton-pool and catalog-pool point at the same object):
   - `src/dml/insert/mssql_insert_executor.cpp:74` → use `catalog.GetConnectionPool()`
   - `src/dml/update/mssql_update_executor.cpp:73` → same
   - `src/query/mssql_result_stream.cpp:82,89` → same
   - `src/query/mssql_query_executor.cpp:40` → same
7. Delete `MssqlPoolManager` class + `src/connection/mssql_pool_manager.{cpp,hpp}` +
   the `pools_` map + `pinned_counts_` map + `manager_mutex_` + `pinned_mutex_`.
8. Add ATTACH credential validation (FR-011): right after pool construction in
   `Initialize`, do `pool.Acquire()` + immediate release with the configured
   `mssql_attach_validation_timeout` (default = `mssql_connection_timeout`).
   Parse the new ATTACH option `lazy_validation` (and ADO.NET-style alias
   `LazyValidation`) — if `true`, skip the validation acquire.

### Phase 2 — Diagnostic enumeration via DuckDB catalog list

1. Rewrite `mssql_diagnostic.cpp::MssqlPoolStatsGlobalState` to walk
   `context.db->GetDatabaseManager().GetDatabases(context)`, filter to
   `Catalog::GetCatalogType() == "mssql"`, and project per-catalog
   `Cast<MSSQLCatalog>().GetConnectionPool().GetStats()`.
2. `mssql_pool_stats('explicit-name')` becomes a single-catalog lookup via
   `Catalog::GetCatalog(context, name).Cast<MSSQLCatalog>()`.
3. Per-catalog `pinned_count` sourced from the new `GetConnectionPool().GetPinnedCount()`.

### Phase 3 — Remove `g_context_managers`

1. Delete `MSSQLContextManager` class + `g_context_managers` map + `g_context_managers_lock`
   from `src/mssql_storage.cpp`. The class only existed to dispatch `name → MSSQLContext`
   for pool lookup; with the pool owned by `MSSQLCatalog`, the indirection collapses.
2. Retire the spec 045 / commit `70a4d90` band-aid (`RegisterContext` silent-overwrite +
   conditional pool sweep) — the pointer-reuse hazard goes away with the container.
3. Update remaining `MSSQLContextManager::Get(db)` call sites (if any survive after
   Phase 1) to instead reference the catalog directly (callers always have a context
   that gives them access to the catalog list).

### Phase 4 — Result stream registry → catalog

1. Add two private members to `MSSQLCatalog`: `mutex streams_mutex_` +
   `unordered_map<string uuid, unique_ptr<MSSQLResultStream>> active_streams_`.
2. Add public methods `RegisterStream(unique_ptr<MSSQLResultStream>) -> string uuid`
   and `RetrieveStream(const string &uuid) -> unique_ptr<MSSQLResultStream>` to
   `MSSQLCatalog` (mirrors today's `MSSQLResultStreamRegistry::Register/Retrieve`).
3. Update `mssql_scan` `Bind` (line 153) to call `catalog.RegisterStream(...)`
   instead of `MSSQLResultStreamRegistry::Instance().Register(...)`. Get the catalog
   from `context_name` lookup that's already happening at line 121.
4. Update `mssql_scan` `InitGlobal` (line 177) to call `catalog.RetrieveStream(uuid)`
   instead of `MSSQLResultStreamRegistry::Instance().Retrieve(uuid)`.
5. Delete `MSSQLResultStreamRegistry` class from `src/include/mssql_functions.hpp`
   and `src/mssql_functions.cpp:41-64`.

### Phase 5 — Multi-instance integration test

1. Write the C++ multi-instance test under `test/cpp/test_multi_instance_pool_isolation.cpp`
   (since two `DuckDB` instances in the same process is hard to express in
   sqllogictest). Cover Scenarios 1–3.
2. Add it to `CMakeLists.txt`.
3. Write `test/cpp/test_issue_96_attach_loop.cpp` for SC-009 (Scenario 4 from spec).
4. Verify both tests fail on `main`-at-kickoff and pass post-spec-047.

### Phase 6.5 — Bundled: Custom Application Name (issue #82, US-AN)

Fully independent of Phases 1-5 and 7; can land in any order. Touches no
file modified by the singleton-cleanup phases.

1. Add `string application_name;` to `MSSQLConnectionInfo`
   (`src/include/mssql_storage.hpp`); default empty.
2. Extend the ADO.NET parser in `MSSQLConnectionInfo::FromConnectionString`
   (`src/mssql_storage.cpp`) to recognize the five variant keys per FR-014.
3. Extend the URI parser (same file, `FromConnectionString` URI branch) to
   recognize `applicationname=` query parameter.
4. Extend `MSSQLConnectionInfo::FromSecret` (same file) to read the
   `application_name` and `applicationname` secret fields.
5. Update the four auth strategies to consult `info.application_name`
   first, falling back to the unified default `"DuckDB MSSQL Extension"`:
   - `src/tds/auth/sql_auth_strategy.cpp`
   - `src/tds/auth/manual_token_strategy.cpp`
   - `src/tds/auth/fedauth_strategy.cpp`
   - `src/include/tds/auth/integrated_auth_strategy.hpp`
6. Implement 128-char clamp with DEBUG log in the same code path (one place,
   either in the strategies' helper or in `MSSQLConnectionInfo` post-parse
   validation).
7. Write `test/sql/attach/application_name.test` covering SC-012 steps 1-5.
8. Update `CLAUDE.md` "ATTACH Options & Secret Parameters (Catalog Filters)"
   table to add a row for `application_name`; mention `Application Name=`
   syntax in user-facing docs.

### Phase 6 — Polish

1. (Band-aid retirement is part of Phase 3 — the entire `MSSQLContextManager`
   class is deleted, so there's nothing left to revert.)
2. Update `CLAUDE.md` "Key Architecture Concepts" → "Connection pool"
   bullet to reflect per-catalog ownership.
3. Update `CLAUDE.md` "Recent Changes" with spec 047 entry summarizing the
   migrations (3 singletons removed: pool manager, context managers,
   result stream registry; handle manager + TokenCache stay legitimate).
4. Write `state_inventory.md` (FR-007 deliverable) — final classification of
   every process-wide static after implementation. Confirm zero "migrate"
   entries remain; document the `legitimate` set (TokenCache + handle manager +
   Winsock + OpenSSL + thread-locals).
5. `TokenCache` inline comment in `src/azure/azure_token.{hpp,cpp}`: explicit
   note that the cache is process-wide and that's intentional (deduplicates
   token acquisition for the same Azure `secret_name` across instances). If
   isolation is later required, add per-instance namespacing.
6. `MSSQLConnectionHandleManager` inline comment in `src/include/connection/mssql_diagnostic.hpp`:
   explicit note that this singleton is by design — diagnostic helpers have no
   catalog binding by API and the handle map is correctly scoped to the process.
7. Release notes (`CHANGELOG.md`): document the per-catalog pool ownership
   model and the same-DSN-multiple-aliases cost (2× connection limit + 2×
   idle overhead per duplicate-DSN attach within the same instance).

## Constraints / non-goals

- **Not changing the public extension API.** All extension functions
  (`mssql_scan`, `mssql_exec`, `mssql_pool_stats`, `mssql_refresh_cache`,
  `mssql_open`, `mssql_close`, `mssql_ping`, etc.) keep their signatures.
- **Not changing TDS protocol behavior.** Pool semantics (acquire / release /
  pin / idle timeout / connection limit) are preserved exactly; only
  ownership and visibility scope change.
- **TokenCache stays singleton** in spec 047 — reclassified as legitimate
  (deliberate cross-instance token reuse to avoid redundant OAuth2 round-trips
  for the same `secret_name`). Documented but not migrated.
- **No backward compatibility shims.** The pool manager removal is a clean
  break; once it's gone, it's gone.
- **Credential zeroization / lazy materialization is out of scope.** Spec
  047 doubles in-memory credential copies for the same-DSN-multi-alias case
  (one `MSSQLConnectionInfo` per pool instead of one shared entry). Reducing
  the in-memory credential footprint requires a separate `CredentialProvider`
  redesign that splits secret-backed auth (where the password can be
  materialized transiently at LOGIN7 time from DuckDB's Secret store) from
  connection-string auth (where DuckDB itself permanently holds the
  credential in catalog metadata). Tracked in
  [issue #119](https://github.com/hugr-lab/mssql-extension/issues/119);
  scheduled as a follow-up spec (provisionally 049).
- **Graceful in-flight TDS cancellation on DETACH / pool teardown is out of
  scope.** Today's `~MSSQLCatalog` chain (post-047) closes sockets via RAII;
  any in-flight TDS request on another thread observes connection-reset on
  its next read. Sending a TDS ATTENTION packet from the destructor on
  another thread's connection is unsafe (write-while-read race on the TDS
  state machine), and unsolicited ROLLBACK is incorrect (would abort
  user-intended transactions). A proper redesign — cooperative cancellation
  via an atomic flag on `TdsConnection` that the owner thread polls between
  packets — belongs in a future spec covering DuckDB-side `InterruptCheck`
  integration. Spec 047 only documents the contract: destructor closes
  sockets immediately; server-side rollback happens via TCP FIN within
  seconds; no graceful ATTENTION is sent.

## References

- **Issue [#96](https://github.com/hugr-lab/mssql-extension/issues/96)** —
  "Catalog Error: MSSQL Error: Context 'dbalias' already exists" production
  manifestation of the singleton-pool / context-managers bug class. Spec 047
  closes this issue (see Scenario 4 + SC-009).
- **Issue [#82](https://github.com/hugr-lab/mssql-extension/issues/82)** —
  "Application Name in connection string" enhancement request. Bundled into
  spec 047 because it touches the same `MSSQLConnectionInfo` parser surface;
  closed by FR-014 + SC-012 (US-AN).
- Spec 045 commit `70a4d90` — band-aid for `g_context_managers` pointer-reuse
  (partial fix only; full fix in this spec)
- Spec 045 commit `7bbdf28` — TIMESTAMP_* round-trip (concomitant with pool
  discussion)
- DuckDB AttachedDatabase / DatabaseInstance lifecycle: TBD link
- `src/connection/mssql_connection_provider.cpp:97-149` —
  `ConnectionProvider::GetConnection` (already correct: goes through
  catalog, not singleton)
- `src/copy/copy_function.cpp:248,624,750` — COPY (already correct: catalog)
- `src/table_scan/table_scan.cpp` + `src/mssql_functions.cpp:377` — table
  scan (already correct: catalog via ConnectionProvider)

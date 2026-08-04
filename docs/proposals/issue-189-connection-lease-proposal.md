# Issue #189 — No connection lifetime for shared temp tables: analysis & implementation proposal

Status: analysis complete, design settled, implementation proposed. Date: 2026-07-16. Main @ `2c1c4cf`.

Updated 2026-08-04: an alternative fix was evaluated (see "Alternative
(tempdb-qualified table)") and found insufficient for users with no path to any write
grant. The lease design remains the recommended fix for that population.

## TL;DR

1. **The problem is genuine — verified against the code and reproduced empirically.**
   There's no connection lifetime a shared temp table can anchor to: autocommit
   releases flag `RESET_CONNECTION` (drops `#`/`##` tables on next reuse), pinning
   exists only inside explicit transactions (serializes all use to one DuckDB
   connection), and COMMIT/ROLLBACK reset + unpin (D1–D4 below). All four failure modes
   reproduce deterministically against the shipped community binary (R1–R4 below). Two
   red baseline tests are in place, ready to go green once the plan ships.

2. **A user-controllable leased connection is the right shape for the fix.**
   `ConnectionProvider` is already the chokepoint for `mssql_exec`/`mssql_scan`
   connection acquisition and the pinning machinery exists — a catalog-scoped lease
   fits the spec-047 "no process-wide state" direction. Rejected: a `pool_reset=false`
   setting (leaks state unpredictably, still no stable session) and extending the
   deprecated, non-catalog-scoped `mssql_open` handle API.

3. **Three open design points settled**: lease routing covers **`mssql_exec` +
   `mssql_scan` only**; **explicit transactions win** and the lease survives
   COMMIT/ROLLBACK untouched; **`mssql_lease(ctx) → BIGINT` (SPID), strict error on
   double-lease; `mssql_release(ctx) → BOOLEAN`** (false when none held). One lease max
   per (ClientContext, catalog); independent per catalog and per DuckDB connection.

## Reproduction attempts (all four failure modes reproduced)

Environment: `docs/proposals/issue-189-repro/` — a sidecar-only compose file that joins
the repo's existing SQL Server dev container (no second server provisioned) with a
Python sidecar running DuckDB against the community `mssql` binary.

```bash
make docker-up
docker compose -f docs/proposals/issue-189-repro/docker-compose.yml run --rm repro
make docker-down
```

| # | Scenario | Result |
|---|---|---|
| R1 | `mssql_exec` runs `SELECT * INTO ##shared FROM dbo.Source` (autocommit), then poll `tempdb.sys.tables` | **REPRODUCED** — SELECT INTO reports 1000 rows inserted (creation succeeded); table gone after **1** subsequent pool statement (D1's reset fired on first reuse) |
| R2 | Same with `#local`, then `SELECT COUNT(*)` from it in the next statement | **REPRODUCED** — fails with SQL Server error 208 `Invalid object name '#repro189_local'` (D2) |
| R3 | `BEGIN` → create `##tx` → count inside the transaction → `COMMIT` → poll | **REPRODUCED** — count=1000 inside the transaction (pinned connection works); gone within 1 pool statement after COMMIT (D3 + D1) |
| R4 | Create `##locked` in an **open** transaction; a second DuckDB connection reads it while a 3s timer commits | **REPRODUCED** — second pooled connection blocked 3.0s on the Sch-M lock until the COMMIT landed, then read 1000 rows (D3) |

The drop in R1/R3 is not time-based — it fires exactly when the creating physical
connection is next reused, confirming the `RESET_CONNECTION` mechanism rather than any
server-side expiry. Reproduced twice, against two different SQL Server/DuckDB/extension
version combinations, with identical results both times.

## Code audit (main @ `2c1c4cf`)

Audited the full lifecycle of a temp table created through the extension. Each
behavior below is deliberate pooling hygiene individually — **the defect is the
absence of any user-controllable exemption**.

### D1. Autocommit release flags RESET_CONNECTION — `##` tables die at next reuse

`ConnectionProvider::ReleaseConnection` flags `SetNeedsReset(true)` on every autocommit
release (mssql_connection_provider.cpp:224-232), and `~MSSQLResultStream` does the same
on stream teardown (mssql_result_stream.cpp:92). The reset rides the next SQL_BATCH
header, dropping the session's temp tables, `##` globals included — standard
ADO.NET/JDBC-style pool hygiene, but with no opt-out. Reproduced as R1.

### D2. No pinning outside explicit transactions — `#` tables cannot span autocommit statements

Pinning exists only in the explicit-transaction branch of
`ConnectionProvider::GetConnection` (mssql_connection_provider.cpp:127-208); a `#`
table from one autocommit `mssql_exec` is gone for the next statement. Works inside a
transaction, but then all use is confined to one DuckDB connection. Reproduced as R2.

### D3. COMMIT/ROLLBACK reset + unpin — a transaction can't anchor state beyond its own lifetime, and blocks everyone else while open

`CommitTransaction`/`RollbackTransaction` flag reset and return the connection to the
pool (mssql_transaction.cpp:256-266, :307-317). While the transaction is open, the
`SELECT INTO ##...` DDL also holds Sch-M locks until COMMIT. Reproduced as R3
(post-commit drop) and R4 (lock blocking).

### D4. The deprecated handle API is not an escape hatch

`mssql_open` handles support only `mssql_ping`/`mssql_close`/`mssql_close_all` — no
exec surface — and the API is deprecated (spec 047 FR-010). Confirmed dead end.

Safety margin already in place: `TdsConnection` guards state transitions with a CAS
(tds_connection.cpp:884, :994), so a second operation on a busy leased connection fails
with an error rather than corrupting the TDS stream.

## Alternative (tempdb-qualified table): evaluated, insufficient for read-only-only users

An alternative routes around session-scoped temp tables entirely: write to an ordinary,
persistently-named table in `tempdb` instead of a `#`/`##` table, since an ordinary
table has no session affinity and is unaffected by `RESET_CONNECTION`. Two forms — a
COPY-target enhancement (a database segment added to `TargetResolver`'s target string)
and an ATTACH-as-second-catalog workaround (works today, no code change) — turn out to
be the same underlying mechanism. **Both require the identical `CREATE TABLE`
permission in the target database**, confirmed empirically against a genuinely
read-only login: both fail with SQL Server error 262, `CREATE TABLE permission
denied`.

That's a *different* permission from the one that lets `#`/`##` temp table creation
work — SQL Server does not gate temp-table creation through the ordinary per-database
`CREATE TABLE` check at all. "Can create temp tables" does not imply "can create
ordinary tables in `tempdb`."

For users who can get a `tempdb`-scoped grant, this is simpler than the lease design
and worth building. **For users with no possible path to any write grant** (e.g.
regulated environments), it's a hard dead end regardless of scope — the lease design
needs zero additional SQL Server permissions and remains the correct fix for that
population. The two approaches are complementary, not competing.

A lighter-weight "disable connection reset for one dedicated pool" setting was also
considered and rejected: it needs no new scalar functions, but pushes correctness onto
operator discipline (idle-cleanup eviction order isn't identity-aware; a shared pool
leaks `SET`/isolation-level state across unrelated queries) rather than guaranteeing it
by construction the way an explicit lease does.

## Design

Two cleanup owners must both be able to end a lease safely, in either order: the
DuckDB connection (explicit `mssql_release`, or ClientContext destruction) and the
catalog (DETACH while a lease is held — leases must be reclaimed before pool
teardown). Reuses two existing patterns: per-(ClientContext, catalog) state via
`ClientContextState` (same registry-key pattern as `MSSQLBindAnchors::For`,
mssql_bind_anchors.cpp:52-59), and the spec-047/#178 pool lifetime invariant (catalog
holds the sole strong pool reference; the lease holder stores
`shared_ptr<TdsConnection>` + `weak_ptr<ConnectionPool>`, never a strong pool ref —
exactly like `MSSQLResultStream`).

### New class: `MSSQLConnectionLease : ClientContextState`

`src/{include/,}connection/mssql_connection_lease.{hpp,cpp}` (namespace `duckdb`,
`MSSQL` prefix per naming rules):

* `For(context, catalog)` — `registered_state->GetOrCreate`, per-catalog key.
* `Lease(catalog) → int64_t` — pool `Acquire(-1)`, store conn + weak pool handle,
  `IncrementPinned()`, register with catalog, return SPID. Throws if already held.
* `Release() → bool` — idempotent, mutex-guarded; the single teardown routine for all
  three triggers (explicit / destructor / catalog force-release): `Idle` →
  `SetNeedsReset(true)` + pool release; busy → `Close()` + pool release (never pool a
  mid-protocol connection); pool gone → drop the shared_ptr. `DecrementPinned()` if the
  pool is alive.
* `GetConnection()` — nullptr if none; throws if dead or busy ("unfinished mssql_scan
  result stream?"). No silent re-acquire on a dead connection — session state is
  unrecoverable, the user must release + re-lease knowingly.

Verified invariant: `ConnectionPool`'s idle-cleanup thread (`CleanupThreadFunc`,
tds_connection_pool.cpp:334-376) only ever walks `idle_connections_`, which
`Acquire()` removes a connection from for the duration it's checked out. A held lease
is therefore invisible to idle-timeout eviction by construction — no interaction with
`mssql_idle_timeout`/`mssql_min_connections` needed.

### Catalog lease registry (DETACH safety)

Mirrors the spec-047 `RegisterStream`/`RetrieveStream` pattern on `MSSQLCatalog`
(mssql_catalog.hpp:158-163): `RegisterLease(weak_ptr<MSSQLConnectionLease>)` +
`ReleaseAllLeases() noexcept` — a mutex-guarded `vector<weak_ptr<...>>`, expired
entries pruned on access. `~MSSQLCatalog` calls `ReleaseAllLeases()` before pool
teardown (try/catch — the 047 teardown chain is `noexcept`).

### Routing

* `ConnectionProvider::GetConnection(..., bool allow_lease = false)` — new default
  parameter; only `mssql_exec` (mssql_functions.cpp:392) and
  `MSSQLQueryExecutor::Execute` (mssql_query_executor.cpp:71) pass `true`. Checked
  after the transaction branch (transactions win), autocommit only.
* `ConnectionProvider::ReleaseConnection` — no-op if the lease `Owns(conn)` (pointer
  equality); transaction branch untouched. Scans/DML/COPY/CTAS/metadata never see the
  lease.
* `MSSQLResultStream`: rename `transaction_pinned_` → `pinned_`; computed as
  `IsInTransaction(...) || IsLeasedConnection(...)`. Destructor behavior unchanged —
  preserves the #178 worker-thread-destructor fix (pinned streams never touch the pool
  or ClientContext).

### Scalar functions

New `src/mssql_lease_functions.cpp`: bind shaped like `MSSQLExecBind` (constant context
name, catalog resolved + type-checked); `SetVolatile()` + `SPECIAL_HANDLING` like
`mssql_exec`. No read-only restriction on leasing (`mssql_exec` keeps its own). After
DETACH, `mssql_release` fails catalog lookup with "Unknown context" — already
force-released.

### Observability

`Lease()`/`Release()` drive the existing pool `IncrementPinned()`/`DecrementPinned()`
(tds_connection_pool.hpp:98-99), so leases show in `mssql_pool_stats`'s pinned count. A
dedicated `leased_count` column is a follow-up.

## Edge-case matrix

| Case | Behavior |
|---|---|
| Double `mssql_lease` | Error: `lease already held for context '...' (release it with mssql_release)` |
| `mssql_release`, none held | `false` |
| ClientContext dies with lease held | `~MSSQLConnectionLease` → `Release()` |
| DETACH with lease held | `ReleaseAllLeases()` before pool teardown; `D_ASSERT` holds; later `mssql_release` → "Unknown context" |
| Leased connection dies (network / closed by stream teardown) | Next lease use throws; user must release + re-lease |
| `mssql_release` while a scan stream is open on the lease | Connection closed, not pooled; stream errors on next read; server rolls back via TCP FIN |
| Explicit transaction while leased | Txn pins a separate pooled connection; lease bypassed until COMMIT/ROLLBACK, then resumes |
| First statement on a fresh lease | Inherited `needs_reset_` fires → session starts clean (desired) |
| Re-ATTACH under the same alias | Reuses the released state object; `Lease()` just stores the new pool handle — no stale-pointer hazard |
| Two ATTACHes / two DuckDB connections | Independent leases (key includes context name; state is per ClientContext) |
| Concurrent scans on one lease | Busy check → clear error; CAS state machine is the hard guarantee |
| Read-only catalog | Lease allowed; `mssql_exec` still rejected |

## Implementation plan

### Phase 1 — core (self-contained, unit-testable)

`MSSQLConnectionLease` + catalog registry + `~MSSQLCatalog` hook. C++ unit tests
(`test/cpp/`, no SQL Server): registry pruning, `Release()` idempotency, `Owns()`.

A red skeleton exists at `test/cpp/test_connection_lease.cpp` (doesn't compile yet —
the header doesn't exist). Making it compile and pass is this phase's exit criterion;
extend with registry-pruning cases once the catalog side lands, and wire a Makefile
target then (not before, so an uncompilable listed source doesn't break `make test`).

### Phase 2 — routing

`allow_lease` parameter, lease-aware `ReleaseConnection`, `IsLeasedConnection` helper;
call-site updates in `mssql_exec` / `MSSQLQueryExecutor`; stream flag rename.

### Phase 3 — scalar functions
`mssql_lease` / `mssql_release` + registration alongside `RegisterMSSQLFunctions`.

### Phase 4 — integration tests (`test/sql/lease/`, requires `make docker-up`)

1. `lease_basic.test` — already written, red today (fails at the first `mssql_lease`
   call). SPID returned; `##t` via lease survives pool churn; `@@SPID` stable while
   held; double-lease errors; release drops the state. Making it pass is Phases 1–3's
   combined exit criterion.
2. `lease_release_drops_state.test` — retry-loop version of the release-drops-state
   check, for extra confidence beyond `lease_basic.test`'s inline version.
3. `lease_errors.test` — release-none; unknown context; busy-stream error.
4. `lease_transactions.test` — BEGIN/COMMIT around a lease: distinct SPIDs, lease
   state survives commit.
5. `lease_detach.test` — DETACH with held lease; re-ATTACH + re-lease.
6. `lease_multi_catalog.test` — two ATTACHes, independent leases.
7. `lease_pool_stats.test` — pinned count 0 → 1 → 0.

### Phase 5 — docs & finish
* README: "Leased connections / shared temp tables" section with the issue's motivating
  workflow (build `##` tables once, query from many workers).
* CLAUDE.md: add both functions to the Extension Functions table.
* **DATAMODEL.md (required — this alters a pool-layer invariant)**: lease as a third
  pin-owner besides transactions and streams; ownership rules; diagram update.
* docs/transactions.md: lease/transaction precedence note.
* clang-format pass; `make test` + `make integration-test`.

## Out of scope (documented follow-ups)

* Routing table scans / DML / COPY through the lease (settled: exec/scan only).
* Auto-reacquire on dead leased connection (unsafe: silently loses `##` state).
* `leased_count` column in `mssql_pool_stats`.
* Keep-alive pings for long-held leases (client idle cleanup only touches idle pool
  connections, so a held lease is never culled; SQL Server does not kill idle sessions
  by default).

## Artifacts from this investigation

* Reproduction sidecar: `docs/proposals/issue-189-repro/` — see "Reproduction
  attempts" above. Doubles as the fix's acceptance harness: mount a local build and
  extend `repro.py` with the lease-based happy path.
* Two red baseline tests (see Phase 1 / Phase 4 above): `test/sql/lease/lease_basic.test`
  and `test/cpp/test_connection_lease.cpp` — both written against the fix's target API;
  neither passes/compiles yet.

# Issue #189 — No connection lifetime for shared temp tables: analysis & implementation proposal

Status: analysis complete, design settled, implementation proposed. Date: 2026-07-16. Main @ `2c1c4cf`.

## TL;DR

1. **The reported problem is genuine — every claim verified against the code AND
   reproduced empirically.** There is no connection lifetime a shared temp table can
   anchor to: autocommit releases flag `RESET_CONNECTION` (drops session-owned temp
   tables, including `##` globals, on next reuse), pinning exists only inside explicit
   transactions (serializing all use to one DuckDB connection), and COMMIT/ROLLBACK
   reset + unpin (D1–D4 in the code audit). All four failure modes fire
   deterministically against the shipped community binary (0.2.1 on DuckDB 1.5.4) via
   the self-contained compose stack added at `docker/issue-189-repro/` (R1–R4 below).

2. **The proposed fix (a user-controllable leased connection) is the right shape.**
   `ConnectionProvider` is already the single chokepoint for `mssql_exec` / `mssql_scan`
   connection acquisition, the pinning machinery exists, and a catalog-scoped lease fits
   the spec-047 "no process-wide state" direction. Alternatives rejected: a
   `pool_reset=false` setting leaks session state unpredictably AND still gives no
   stable session (idle cleanup can cull the creating connection); extending the
   `mssql_open` handle API contradicts its deprecation (and the handles are not
   catalog-scoped).

3. **Three open design points settled** (maintainer-confirmed): lease routing covers
   **`mssql_exec` + `mssql_scan` only**; **explicit transactions win** and the lease
   survives COMMIT/ROLLBACK untouched; **`mssql_lease(ctx) → BIGINT` (SPID), strict
   error on double-lease; `mssql_release(ctx) → BOOLEAN`** (false when none held). One
   lease max per (ClientContext, catalog); independent per catalog and per DuckDB
   connection.

## Reproduction attempts (all four failure modes reproduced)

Environment: self-contained compose stack at `docker/issue-189-repro/` — SQL Server
2022 (16.0.4262.2, linux/amd64 container) + a Python sidecar running DuckDB 1.5.4 with
the **community `mssql` 0.2.1 binary** (what users actually run; a commented-out mount
+ `MSSQL_EXTENSION_PATH` switches it to a local build). The sidecar populates
`Repro189.dbo.Source` (1000 rows) and drives the scenarios; exit code 0 = all failure
modes present. Verify with:

```bash
cd docker/issue-189-repro
docker compose run --build --rm repro
docker compose down -v
```

| # | Scenario | Result |
|---|---|---|
| R1 | `mssql_exec` runs `SELECT * INTO ##shared FROM dbo.Source` (autocommit), then poll `tempdb.sys.tables` | **REPRODUCED** — SELECT INTO reports 1000 rows inserted (creation succeeded); table gone after **1** subsequent pool statement (D1's reset fired on first reuse) |
| R2 | Same with `#local`, then `SELECT COUNT(*)` from it in the next statement | **REPRODUCED** — fails with SQL Server error 208 `Invalid object name '#repro189_local'` (D2) |
| R3 | `BEGIN` → create `##tx` → count inside the transaction → `COMMIT` → poll | **REPRODUCED** — count=1000 inside the transaction (pinned connection works); gone within 1 pool statement after COMMIT (D3 + D1) |
| R4 | Create `##locked` in an **open** transaction; a second DuckDB connection reads it while a 3s timer commits | **REPRODUCED** — second pooled connection blocked 3.0s on the Sch-M lock until the COMMIT landed, then read 1000 rows (D3) |

Notes:

* The drop in R1/R3 is not time-based — it fires exactly when the creating physical
  connection is next reused (the poll statement itself is the trigger), confirming the
  `RESET_CONNECTION` mechanism rather than any server-side expiry.
* An earlier R4 variant put `SET LOCK_TIMEOUT 2000` in the reader's batch expecting
  error 1222. It never fired: SQL Server compiles the whole batch before executing
  statement 1, so the reader blocks on schema stability during **compilation** —
  before the `SET` runs (session default: infinite) — and hung until the extension's
  own 30s COLMETADATA timeout closed the connection. Blocking confirmed either way;
  the commit-release form is deterministic (and 10× faster per run).
* Sidecar image note: the released 0.2.1 linux binary (pre-spec-053) still hard-links
  `libgssapi_krb5.so.2`, which `python:3.12-slim` lacks — the sidecar Dockerfile
  installs `libgssapi-krb5-2`. Incidental empirical confirmation of the issue #161 /
  spec 053 premise.

## Code audit (main @ `2c1c4cf`)

Audited the full lifecycle of a temp table created through the extension: connection
acquire/release (`ConnectionProvider`), pool release/reuse (`ConnectionPool`,
`RESET_CONNECTION` semantics), transaction pinning and teardown
(`MSSQLTransaction[Manager]`), result-stream release (`MSSQLResultStream`), and the
deprecated handle API. Each behavior below is deliberate pooling hygiene, not a bug in
isolation — **the defect is the absence of any user-controllable exemption**: there is
no connection lifetime a shared temp table can anchor to, exactly as reported.

### D1. Every autocommit release flags RESET_CONNECTION — session-owned temp tables (`##` included) die at the creating connection's next reuse
`ConnectionProvider::ReleaseConnection` flags `SetNeedsReset(true)` on every autocommit
release (mssql_connection_provider.cpp:224-232), and `~MSSQLResultStream` does the same
on stream teardown (mssql_result_stream.cpp:92). The reset bit rides the next
SQL_BATCH header on reuse — SQL Server then drops the session's temp tables, global
`##` tables included. Since which physical connection a later statement reuses is a
pool-internal detail, `##` tables created via `mssql_exec` vanish at an arbitrary later
point. This is standard ADO.NET/JDBC-style pool hygiene (correct as a default) with no
opt-out. Reproduced as R1.

### D2. No pinning outside explicit transactions — `#` temp tables cannot span autocommit statements
Connection pinning exists only in the explicit-transaction branch of
`ConnectionProvider::GetConnection` (mssql_connection_provider.cpp:127-208); autocommit
statements each acquire an arbitrary pool connection (and D1's reset fires between
them). A `#` table created by one `mssql_exec` is gone for the next statement. Inside
a transaction it works — but then all use is confined to one DuckDB connection,
serialized on one TDS connection, defeating the reporter's concurrent-worker design.
Reproduced as R2.

### D3. COMMIT/ROLLBACK reset + unpin — a transaction cannot anchor temp state beyond its own lifetime, and blocks everyone else while it lives
`CommitTransaction` / `RollbackTransaction` flag reset and return the pinned connection
to the pool (mssql_transaction.cpp:256-266, :307-317), so "create the `##` tables in a
pinned transaction, then use them" fails the moment it commits. Worse, while the
transaction is open, the `SELECT INTO ##...` DDL holds Sch-M locks until COMMIT
(standard SQL Server behavior) — other pooled connections block on any access to the
table. Reproduced as R3 (post-commit drop) and R4 (lock blocking).

### D4. The deprecated handle API is not an escape hatch
`mssql_open` handles support only `mssql_ping` / `mssql_close` / `mssql_close_all` —
no exec surface — and the API is `[DEPRECATED]` since spec 047 FR-010 with no catalog
discriminator. Extending it would contradict its deprecation; confirmed dead end.

Safety margin already in place (relevant to the fix): `TdsConnection` guards state
transitions with a CAS (tds_connection.cpp:884, :994), so a second operation on a busy
leased connection fails with an error rather than corrupting the TDS stream.

## Design

Two cleanup owners must both be able to end a lease safely, in either order: the
**DuckDB connection** (explicit `mssql_release`, or ClientContext destruction) and the
**catalog** (DETACH while a lease is held — pool `Shutdown()` debug-asserts
`active_connections_.empty()`, so leases must be reclaimed *before* pool teardown).
The design reuses two reviewed patterns:

* **Per-(ClientContext, catalog) state** via `ClientContextState` with registry key
  `"mssql_lease:<context_name>"` — same as `MSSQLBindAnchors::For`
  (mssql_bind_anchors.cpp:52-59).
* **Pool lifetime invariant (spec 047 / #178)**: catalog holds the SOLE strong pool
  reference. The lease holder stores `shared_ptr<TdsConnection>` +
  `weak_ptr<ConnectionPool>` — never a strong pool ref — exactly like
  `MSSQLResultStream`.

### New class: `MSSQLConnectionLease : ClientContextState`

`src/{include/,}connection/mssql_connection_lease.{hpp,cpp}` (namespace `duckdb`,
`MSSQL` prefix per naming rules):

* `For(context, catalog)` — `registered_state->GetOrCreate`, per-catalog key.
* `Lease(catalog) → int64_t` — pool `Acquire(-1)` (configured acquire timeout), store
  conn + weak pool handle, `IncrementPinned()`, register with catalog, return SPID.
  Throws if already held (strict).
* `Release() → bool` — idempotent, mutex-guarded, the SINGLE teardown routine for all
  three triggers (explicit / `~MSSQLConnectionLease` / catalog force-release):
  conn `Idle` → `SetNeedsReset(true)` + `pool.Release`; conn busy → `Close()` +
  `pool.Release` (never pool a mid-protocol connection "open"); pool weak_ptr dead →
  drop the shared_ptr (`~TdsConnection` closes the socket). `DecrementPinned()` if the
  pool is alive.
* `GetConnection()` — nullptr if none; throws a clear error if the leased connection is
  dead or busy ("unfinished mssql_scan result stream?"). **No silent re-acquire** on a
  dead connection: session state (`##` tables) is unrecoverable, the user must
  release + re-lease knowingly.

Re-ATTACH under the same alias reuses the (released) state object; `Lease()` just
stores the new catalog's pool handle — no stale-pointer hazard.

### Catalog lease registry (DETACH safety)

On `MSSQLCatalog`, mirroring the spec-047 `RegisterStream`/`RetrieveStream` pattern
(mssql_catalog.hpp:158-163): `RegisterLease(weak_ptr<MSSQLConnectionLease>)` +
`ReleaseAllLeases() noexcept` — a mutex-guarded `vector<weak_ptr<...>>`, expired
entries pruned on access. `~MSSQLCatalog` calls `ReleaseAllLeases()` **before** pool
teardown (try/catch — the 047 teardown chain is `noexcept`). `ClientContextState`
objects are `shared_ptr`-owned by `registered_state`, so a dead `weak_ptr` just means
that context already released.

### Routing

* `ConnectionProvider::GetConnection(..., bool allow_lease = false)` — new default
  parameter; only `mssql_exec` (mssql_functions.cpp:392) and
  `MSSQLQueryExecutor::Execute` (mssql_query_executor.cpp:71) pass `true`. Checked
  AFTER the transaction branch (transactions win), only in autocommit.
* `ConnectionProvider::ReleaseConnection` — autocommit branch: if the lease
  `Owns(conn)` (pointer equality), **no-op** (no reset, no pool release). Transaction
  branch untouched. Table scans / DML / COPY / CTAS / metadata never see the lease.
* `MSSQLResultStream`: rename `transaction_pinned_` → `pinned_` ("a longer-lived owner
  — transaction or lease — holds the pin"); executor computes it as
  `IsInTransaction(...) || IsLeasedConnection(...)`. Destructor behavior unchanged —
  pinned streams never touch pool or ClientContext (preserves the #178
  worker-thread-destructor fix).

### Scalar functions

New `src/mssql_lease_functions.cpp`: bind identical in shape to `MSSQLExecBind`
(constant context name, catalog resolved + type-checked, same error text);
`SetVolatile()` + `SPECIAL_HANDLING` like `mssql_exec`. No read-only restriction on
leasing (useful for `mssql_scan` alone; `mssql_exec` keeps its own read-only check).
After DETACH, `mssql_release` fails catalog lookup with the standard "Unknown context"
error — the lease was already force-released.

### Observability

`Lease()`/`Release()` drive the existing `IncrementPinned()`/`DecrementPinned()`
(tds_connection_pool.hpp:98-99) so leases show in `mssql_pool_stats` pinned count
(comment updated to "transactions or held leases"). A dedicated `leased_count` column
is a follow-up — not worth changing the `mssql_pool_stats` schema here.

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
| Two ATTACHes / two DuckDB connections | Independent leases (key includes context name; state is per ClientContext) |
| Concurrent scans on one lease | Busy check → clear error; CAS state machine is the hard guarantee |
| Read-only catalog | Lease allowed; `mssql_exec` still rejected |

## Implementation plan

### Phase 1 — core (self-contained, unit-testable)
`MSSQLConnectionLease` + catalog registry + `~MSSQLCatalog` hook. C++ unit tests
(`test/cpp/`, no SQL Server): registry pruning, `Release()` idempotency, `Owns()`.

### Phase 2 — routing
`allow_lease` parameter, lease-aware `ReleaseConnection`, `IsLeasedConnection` helper;
call-site updates in `mssql_exec` / `MSSQLQueryExecutor`; stream flag rename.

### Phase 3 — scalar functions
`mssql_lease` / `mssql_release` + registration alongside `RegisterMSSQLFunctions`.

### Phase 4 — integration tests (`test/sql/lease/`, requires `make docker-up`)
1. `lease_basic.test` — SPID returned; `SELECT ... INTO ##t` via lease; second
   sqllogictest connection (pooled) sees `##t`; `@@SPID` via lease stable across calls.
2. `lease_release_drops_state.test` — after release, churn the pool, verify `##t` gone
   via `tempdb.sys.tables` (retry loop — drop happens on the physical connection's
   next reuse).
3. `lease_errors.test` — double lease; release-none; unknown context; busy-stream error.
4. `lease_transactions.test` — BEGIN/COMMIT around lease: txn `@@SPID` differs, lease
   SPID and `##t` survive commit.
5. `lease_detach.test` — DETACH with held lease (no crash / assert in debug);
   re-ATTACH + re-lease works.
6. `lease_multi_catalog.test` — two ATTACHes, independent leases.
7. `lease_pool_stats.test` — pinned count 0 → 1 → 0.

### Phase 5 — docs & finish
* README: "Leased connections / shared temp tables" section with the issue's motivating
  workflow (build `##` tables once, query from many workers).
* CLAUDE.md: add both functions to the Extension Functions table.
* **DATAMODEL.md (required — this alters a pool-layer invariant)**: lease as a third
  pin-owner besides transactions and streams; ownership rules (strong conn ref, weak
  pool ref; catalog force-releases before pool teardown); diagram update.
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

* Self-contained reproduction stack: `docker/issue-189-repro/` (compose file, sidecar
  Dockerfile, `repro.py`). `docker compose run --build --rm repro` — exit 0 means the
  limitation is present. Reusable as the fix's acceptance harness: mount a local build
  via `MSSQL_EXTENSION_PATH` and extend `repro.py` with the lease-based happy path
  (lease → create `##` tables → concurrent pooled readers → release → tables dropped).

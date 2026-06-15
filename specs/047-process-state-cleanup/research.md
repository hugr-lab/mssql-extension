# Spec 047 — Research

Resolves the open design decisions not covered by `/speckit-clarify` Q1-Q5. The final scope is **narrower** than the initial draft suggested — see the plan-review correction below.

## 0. Plan-review course correction (READ THIS FIRST)

The first draft of this research / plan introduced a per-`DatabaseInstance` `MSSQLDiagnosticState` container (held via DuckDB `ObjectCache`) + a new `connection_pool_factory.{cpp,hpp}` module. Both were rejected during plan review:

- **`MSSQLDiagnosticState` per-instance container**: overkill. `MSSQLResultStreamRegistry` is catalog-bound by `mssql_scan` API (the `context` argument names an MSSQL catalog) and naturally moves into `MSSQLCatalog` as members. `MSSQLConnectionHandleManager` is **not** catalog-bound (`mssql_open` takes a connection string, returns a `BIGINT` handle that survives across queries with no catalog discriminator) — stays as a singleton, reclassified legitimate, and the surrounding functions are marked deprecated.
- **`connection_pool_factory` module**: overkill. The pool-construction bodies in today's `MssqlPoolManager::GetOrCreatePool*` move directly into `MSSQLCatalog::Initialize` as inline branches by `auth_method`. The existing construction code (PoolConfiguration translation, factory closure, `make_uniq<ConnectionPool>`) stays as-is; only the singleton wrappers (lookup map, lock, insert) get stripped.

The narrowed scope: kill the singleton, put pool ownership in catalog, fix 4 call sites, move stream registry to catalog, mark diagnostic functions deprecated, add ATTACH credential validation.

---

## 1. Per-catalog pool ownership (FR-001)

**Decision**: `MSSQLCatalog::connection_pool_` becomes `unique_ptr<tds::ConnectionPool>` (real owner; RAII destruction). Pool is constructed inline in `MSSQLCatalog::Initialize` using the existing 3-branch logic from today's `MssqlPoolManager::GetOrCreatePool*`. No new file, no factory module — the bodies of the 3 overloads move into `Initialize` and lose their singleton boilerplate (lookup, insert, lock).

**Rationale**:
- Catalog already has the field; today it's a `shared_ptr` with a no-op deleter aliasing the singleton's owned `unique_ptr`. Switching to a real `unique_ptr` is a 1-line type change in the header.
- Pool construction code is ~80 LOC total across the 3 auth methods (SQL auth, Azure FEDAUTH, Integrated). Inlining into `Initialize` adds ~80 LOC to the catalog file; deleting `MssqlPoolManager` removes ~240 LOC. Net win.
- Caller (`mssql_storage.cpp` `MSSQLAttach`) no longer routes pool construction through the singleton — the catalog is constructed and immediately initialized, end of story.

**Alternatives considered**:
- *Extract a `connection_pool_factory.{cpp,hpp}` module with free functions* — rejected during plan review as overengineered. The factory abstraction adds a file without adding clarity; the 3 construction bodies are already self-contained.
- *Keep `MssqlPoolManager` as a stateless static-method class* — rejected. Singleton's only job is ownership/lookup, both of which move to the catalog; stripping out the state leaves an empty class.

## 2. ATTACH credential validation (FR-011 — added during plan review)

**Decision**: `MSSQLCatalog::Initialize` synchronously acquires + releases one connection from the freshly-constructed pool, with timeout = `mssql_attach_validation_timeout` (default = `mssql_connection_timeout`). Failures (TCP refused, DNS failure, LOGIN7 rejected, FEDAUTH token rejected, Kerberos service ticket failure) propagate out as exceptions; `MSSQLAttach` surfaces them to the user with the verbatim TDS error message. Opt-out via ATTACH option `lazy_validation=true` (and `LazyValidation=true` ADO.NET alias) preserves today's lazy-connect behavior.

**Rationale**:
- Today's behavior: `ATTACH '...Password=WRONG' AS db (TYPE mssql)` succeeds; the first `SELECT * FROM db.dbo.X` fails with a confusing login error far from the actual statement at fault. User reported this directly during plan review.
- All 3 auth methods (SQL auth, FEDAUTH, Integrated) do the auth round-trip inside the pool's connection-factory closure. A single `pool.Acquire()` call validates uniformly across all 3.
- Acquire-then-release leaves the connection warm in the pool for the first real query, so eager validation has near-zero net latency cost (one extra round trip on ATTACH, saved on first query).
- Opt-out exists for container orchestration scenarios (attach happens before SQL Server is healthy; first real query is gated by something else).

**Alternatives considered**:
- *Validate by running a benign query like `SELECT 1`* — rejected. The TDS LOGIN7 round trip already validates auth; running an additional query just adds latency.
- *Validate asynchronously in a background thread, fail on first query if the thread fails* — rejected. Defeats the "fail fast at ATTACH" goal; complicates error reporting.
- *Make validation always-on with no opt-out* — rejected. Container/orchestration workflows have legitimate need for deferred connect.

## 3. Result stream registry → catalog member (FR-009)

**Decision**: `MSSQLResultStreamRegistry` class is dissolved; its state (mutex + `unordered_map<string, unique_ptr<MSSQLResultStream>>`) moves into `MSSQLCatalog` as two private members. Bind calls `catalog.RegisterStream(...)` → returns UUID; InitGlobal calls `catalog.RetrieveStream(uuid)`. UUID-based handle retained (DuckDB `BindData` is serializable/copyable/comparable — UUID survives, raw pointer wouldn't).

**Rationale**:
- `mssql_scan(context, query)` is catalog-bound by API — the `context` argument names an attached MSSQL catalog. Bind already does a `HasContext(bind_data->context_name)` check at line 121, then constructs `MSSQLQueryExecutor(context_name)` at line 130. Adding `catalog.RegisterStream(...)` at line 153 is the natural place.
- Catalog destruction auto-cleans orphan streams (abandoned binds from planner rebinds, query cancellation before InitGlobal). Lifetime is bounded by catalog instead of process.
- Same UUID-keyed bridge semantics as today — no API surface change to `mssql_scan`.

**Alternatives considered**:
- *Store stream as `unique_ptr<MSSQLResultStream>` directly in `BindData`* — rejected. DuckDB BindData is copyable (line 73: `result->context_name = context_name`) and comparable (line 83: `Equals` overload); a `unique_ptr` member would break those operations. UUID indirection is the canonical DuckDB pattern.
- *Move to `MSSQLDiagnosticState` per-instance container* — rejected during plan review as overengineered. The catalog is the right scope.

## 4. `MSSQLConnectionHandleManager` retention (legitimate)

**Decision**: Stays as a Meyers singleton. Reclassified from "migrate" to "legitimate" in the inventory. Per FR-010, the surrounding functions (`mssql_open` / `mssql_close` / `mssql_ping`) are marked `[DEPRECATED]` in the function descriptions, CLAUDE.md table, and CHANGELOG entry.

**Rationale**:
- `mssql_open(secret_or_dsn) -> BIGINT` returns an opaque handle with no catalog binding (API design — function takes a connection string or secret name, not an attached database name).
- `mssql_close(handle)` / `mssql_ping(handle)` accept only the BIGINT handle, with no `context_name` discriminator. Storing the handle map on a catalog would require API change.
- The handle map is **not load-bearing** for any cross-instance correctness invariant — there's no shared pool, no shared transaction state. Two `DuckDB` instances calling `mssql_open()` produce two independent connections that can't contaminate each other.
- Catalog-based alternatives (`ATTACH` + `mssql_scan` / `mssql_exec`) cover the practical use cases; the deprecation note steers users away from the diagnostic API for new code.
- When a future spec removes the deprecated functions, the singleton goes with them. Time-bounded legitimacy.

**Alternatives considered**:
- *Change API to `mssql_open(catalog_name)` + relocate handles to catalog* — rejected. Breaking change for diagnostic functions that already work; users would have to update existing scripts.
- *Delete functions outright in this spec* — rejected. Too disruptive for a refactor PR; deprecation gives an off-ramp.

## 5. Diagnostic enumeration via DuckDB catalog list (FR-003)

**Decision**: `mssql_pool_stats(context)` walks `context.db->GetDatabaseManager().GetDatabases(context)`, filters to entries where `AttachedDatabase::GetCatalog().GetCatalogType() == "mssql"`, and projects per-catalog `Cast<MSSQLCatalog>().GetConnectionPool().GetStats()`. Single-name lookup `mssql_pool_stats('name')` uses `Catalog::GetCatalog(context, name).Cast<MSSQLCatalog>()` directly.

**Rationale**:
- Already-public DuckDB API; no singleton needed.
- Discovers ALL attached MSSQL catalogs in the current instance correctly — including same-DSN-different-alias case (clarification Q4: each attach is a separate row).
- Per-instance scope by construction — no cross-instance leakage.

**Alternatives considered**:
- *Keep `MssqlPoolManager::GetAllStats()` as a thin diagnostic shim* — rejected. Defeats the singleton-removal goal.

## 6. Transaction pin counter migration (FR-004)

**Decision**: Move pin counter from `MssqlPoolManager` (today: per-name entry in singleton map) to `tds::ConnectionPool` as a private `std::atomic<int64_t> pinned_count_{0}` member with public `IncrementPinned()` / `DecrementPinned()` / `GetPinnedCount() const` methods. Callers (`mssql_transaction.cpp:139,143`) already have `catalog_` → use `catalog_.GetConnectionPool().IncrementPinned()`.

**Rationale**:
- Counter is logically per-pool. Today's location in the singleton is an accident of the singleton-owning-pool model.
- Atomic counter — no separate lock needed.
- Diagnostic `mssql_pool_stats()` reports `pinned_count` per row; pool's `GetStats()` already returns a struct that this counter slots into.

**Alternatives considered**:
- *Keep counter in a per-instance map alongside pool* — rejected. Re-creates the singleton problem at smaller scale.
- *Make it part of `MSSQLCatalog` instead of pool* — rejected. Pool is the more natural owner since pinning semantics are pool-scoped.

## 7. `~MSSQLCatalog` cleanup (FR-005)

**Decision**: Pure RAII via `unique_ptr<tds::ConnectionPool>` + `unique_ptr<...>` chain through pool → connections → sockets. No explicit cleanup code in `~MSSQLCatalog` beyond default destructor. DuckDB's `DatabaseManager` owns catalogs; when the last reference drops (explicit DETACH or `~DatabaseInstance`), catalog is destroyed; RAII closes everything.

**Rationale**:
- Idiomatic C++ — no destructor logic to keep in sync.
- Symmetric: explicit DETACH and silent destruction terminate identically.
- Testable: Scenario 3 / SC-003 verifies via 100-iteration loop + socket count from `sys.dm_exec_connections`.

**Alternatives considered**: same as before, dropped.

## 8. Multi-instance test harness (SC-001..SC-003, SC-009, SC-010)

**Decision**: New C++ tests under `test/cpp/`:
- `test_multi_instance_pool_isolation.cpp` — Scenarios 1-3 via direct `DuckDB db_a, db_b;` construction. Asserts via result values + `mssql_pool_stats()` output + socket count from `sys.dm_exec_connections`.
- `test_issue_96_attach_loop.cpp` — 100-iteration `duckdb.connect(":memory:")`-equivalent loop in C++.

New SQL test:
- `test/sql/attach/attach_validates_credentials.test` — covers SC-010 (wrong password fails ATTACH; opt-out via `lazy_validation`).

**Rationale**:
- C++ test directly constructs `DuckDB` — sqllogictest is one-process-one-instance.
- Catch2 fixture lets us tag tests as "requires SQL Server" (`MSSQL_TEST_DSN` env var present).
- Socket count via `sys.dm_exec_connections` from a separate verifier connection at end of test.

## 9. Bench parity gate (SC-007 implied)

**Decision**: Run `test/bench/bench_codec_e2e.sh` at 1M rows, min-of-3, within ±5% of pre-spec-047 baseline (capture `main`-at-kickoff as baseline; save to `bench_results.md`).

**Rationale**: Hot paths don't change. Per-row dispatch still goes through `catalog.GetConnectionPool()`. Construction-time difference: one `make_uniq<ConnectionPool>` per ATTACH (same as today) + one validation acquire (+release). Bench shouldn't budge meaningfully.

## 10. Removed-singleton grep gate (SC-004)

**Decision**: `grep -rn 'MssqlPoolManager\|MSSQLContextManager\|MSSQLResultStreamRegistry' src/ src/include/`. Expected: 0 matches. **Note**: `MSSQLConnectionHandleManager` is NOT in the grep set (it's retained per FR-010 + clarification addendum).

**Rationale**:
- Hard-grep on production source is mechanical and clean.
- Handle manager grep would always fail post-implementation since the class stays.

---

## Open items resolved by `/speckit-clarify`

| Question | Resolution |
|---|---|
| FR-006 — remove `g_context_managers` or auto-cleanup? | Remove entirely. |
| `MSSQLResultStreamRegistry` migration scope? | Include in 047, moved into `MSSQLCatalog`. |
| Azure `TokenCache` migration? | Keep singleton; reclassified as legitimate. |
| Same-DSN multiple aliases pool sharing? | One pool per catalog unconditionally. |
| Silent-shutdown SC iteration count? | 100 iterations. |

## Open items resolved during plan review

| Question | Resolution |
|---|---|
| Pool factory location (new module or inline)? | Inline in `MSSQLCatalog::Initialize`; no new module. |
| Per-`DatabaseInstance` state container (`MSSQLDiagnosticState`)? | Not needed — stream registry goes to catalog; handle manager stays singleton + deprecated. |
| `MSSQLConnectionHandleManager` scope? | Stays singleton, reclassified legitimate; functions marked `[DEPRECATED]` (FR-010). |
| ATTACH credential validation? | Eager by default (FR-011); opt-out via `lazy_validation=true`. |

## Outstanding (none)

No NEEDS CLARIFICATION items remain.

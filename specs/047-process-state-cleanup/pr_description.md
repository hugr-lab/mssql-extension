## Summary

Removes three extension-internal process-wide singletons, replaces them with
per-`MSSQLCatalog` ownership via `unique_ptr`, hardens the security model
around ATTACH credentials + Azure token caching, and adds three small
user-facing features that landed naturally on top.

**Closes [#96](https://github.com/hugr-lab/mssql-extension/issues/96)** —
the long-running ATTACH/DETACH-in-Python-loop crash class. **Closes
[#82](https://github.com/hugr-lab/mssql-extension/issues/82)** — custom
LOGIN7 `program_name`. Spawns [#119](https://github.com/hugr-lab/mssql-extension/issues/119)
(→ future spec 049) for lazy credential materialization.

## What's actually deleted vs kept

| Status | Singleton | Replacement |
|---|---|---|
| DELETED | `MssqlPoolManager` | per-`MSSQLCatalog` `unique_ptr<ConnectionPool>` |
| DELETED | `MSSQLContextManager` (spec 045 band-aid `70a4d90`) | direct `Catalog::GetCatalog(context, name)` lookup |
| DELETED | `MSSQLResultStreamRegistry` | per-catalog `MSSQLCatalog::RegisterStream` / `RetrieveStream` (mutex + unordered_map + UUID v4 keys) |
| KEPT + `[DEPRECATED]` | `MSSQLConnectionHandleManager` | none at process scope — the diagnostic API trio (`mssql_open` / `mssql_close` / `mssql_ping`) plus new `mssql_close_all` takes a connection string, not a catalog name. Group scheduled for removal in a future major release; this singleton retires with them. |
| KEPT + namespaced | `mssql::azure::TokenCache` | key changed from `secret_name` to `(uintptr_t(&DatabaseInstance), cache_key)` so two instances sharing a secret name no longer alias — see FR-012 below. |

Full classification (with Winsock / OpenSSL / thread_local scratch / debug-level
caches) is in
[`state_inventory.md`](specs/047-process-state-cleanup/state_inventory.md).

## Production bug classes fixed

Three patterns repeatedly hit by hugr-lab users on the old singleton-pool
codepath; all three pass post-implementation acceptance tests under
`test/cpp/`:

1. **Cross-instance singleton-pool aliasing** (`SC-001`) — two `DuckDB`
   instances ATTACHing different MSSQL targets under the same alias would
   share one pool. Fixed by per-catalog ownership. C++ test:
   `test/cpp/test_multi_instance_pool_isolation.cpp`.
2. **DETACH-then-ATTACH cross-instance pool leak** (`SC-002`) — orphaned
   sockets from instance A surviving DETACH and being handed out to instance
   B's later ATTACH. Fixed by RAII pool teardown in `~MSSQLCatalog`. Same
   test.
3. **100-iteration silent-shutdown socket leak** (`SC-003`, `SC-009`) — the
   verbatim issue #96 repro: 100 iterations of `DuckDB(nullptr) → Connection
   → LOAD mssql → ATTACH → mssql_scan` succeed indefinitely without
   `"Context 'TO_MSSQL' already exists"` regression. C++ test:
   `test/cpp/test_issue_96_attach_loop.cpp`.

## Security hardening (added during PR #118 external review)

- **FR-011 — ATTACH eager credential validation** with `lazy_validation true`
  opt-out (for container/orchestrator scenarios). Wrong password /
  unreachable host now surface as ATTACH errors instead of being deferred to
  the first query. The error message audited to never contain the password
  (`T028a` evidence: positive sentinel-substring assertion in
  `test/sql/attach/attach_validates_credentials.test`).
- **FR-012 — TokenCache per-DatabaseInstance namespacing**. Pre-047, two
  DuckDB instances each defining a secret called `mssql_secret` aliased to
  the same TokenCache row, so instance B silently authenticated with
  instance A's token. Now keyed by `(uintptr_t(&db), cache_key)`. Direct
  acceptance test:
  `test/cpp/test_token_cache_isolation.cpp` (5 sub-assertions including the
  C-never-set negative case + tenant-suffixed-key variant).
- **FR-013 — `mssql_close_all()`** shutdown helper. Single call closes
  every diagnostic-API handle; returns the count of closed handles;
  idempotent. Acceptance: `test/sql/tds_connection/close_all.test` (7
  assertions).
- **SC-005 extension — `mssql_pool_stats` credential redaction grep gate**:
  ATTACH with the SQL-auth password as a sentinel substring; `SELECT *
  FROM mssql_pool_stats()` filtered by `WHERE db LIKE '%' || PASS || '%'`
  must return 0. Vacuity guard: companion `COUNT(*) = 1` confirms the
  zero-match isn't because no rows. Test:
  `test/sql/tds_connection/pool_stats_no_credentials.test`.
- **noexcept teardown audit** (`T046k`): explicit `noexcept` + try/catch on
  the five-destructor chain `~MSSQLCatalog` / `~ConnectionPool` /
  `~TdsConnection` / `~TdsSocket` / `~TlsTdsContext` + `~TlsImpl`. A throw
  during `~AttachedDatabase` unwind would invoke `std::terminate`; this
  surfaces the contract.
- **Pool-destruction invariant** (`T046l`): debug-only
  `D_ASSERT(active_connections_.empty())` at the top of
  `ConnectionPool::Shutdown()`. Surfaces DuckDB-quiescence-contract
  violations as test failures instead of silent UB. Did NOT fire across the
  full integration suite.
- **Class doc-comment** (`T046m`): explicit destruction contract on
  `class ConnectionPool` — sockets close immediately, in-flight TDS sees
  connection-reset on next read, TCP-FIN rollback within seconds, no
  graceful ATTENTION (cross-thread socket write would be unsafe;
  cooperative cancel = follow-up spec, not 047).

## Bonus: custom Application Name (issue #82)

`Application Name=...` (and `ApplicationName=` / `App Name=` /
`application_name=` ADO.NET variants), `?applicationname=` URI query
parameter, and secret `application_name` (with `applicationname` fallback)
now propagate to LOGIN7 `program_name` — visible as `APP_NAME()` /
`sys.dm_exec_sessions.program_name`. 128-char client-side clamp matches SQL
Server's own program_name limit so what the user sees equals what we sent.
Acceptance: `test/sql/attach/application_name.test` (24 assertions across
5 groups including a per-catalog isolation pair).

**Critical deviation note (in commit body for transparency)**: tasks.md
T064 plumbing through the spec-031 `AuthenticationStrategy` framework alone
is dead code — the SQL / FEDAUTH / integrated paths in
`MSSQLCatalog::Initialize` call `TdsConnection::Authenticate*` DIRECTLY,
not via `AuthenticationStrategy::GetLogin7Options()`. Fixed by also
threading `app_name` through the three `TdsConnection::Authenticate*`
methods + both `DoLogin7*` helpers + the three factory closures + the four
`Validate*` paths. Strategy-side plumbing kept for future-proofing.

## Test plan

`make test` (unit, no SQL Server) + `make integration-test` (SQL Server +
`[sql]` groups) + the spec 047 acceptance binaries:

- [x] `make test-spec047-us1` — `test_multi_instance_pool_isolation` +
  `test_issue_96_attach_loop` (SC-001/002/003/009)
- [x] `make test-spec047-us3` — `test_result_stream_registry_isolation`
  (SC-006)
- [x] `make test-spec047-us-sec` — `test_token_cache_isolation` (SC-011)
- [x] `test/sql/attach/attach_validates_credentials.test` — SC-010 + T028a
- [x] `test/sql/tds_connection/close_all.test` — FR-013 smoke (7 asserts)
- [x] `test/sql/tds_connection/pool_stats_no_credentials.test` — SC-005
  redaction (6 asserts)
- [x] `test/sql/attach/application_name.test` — SC-012 (24 asserts across
  5 groups)
- [x] `make integration-test` — final sweep, exit 0 after every spec 047
  commit landed (304 assertions in `[sql]`; `[integration]` half also
  exit 0)
- [ ] **TODO before merge**: `T052` bench parity gate — 3× runs of
  `test/bench/bench_codec_e2e.sh`, min-of-3 per step, ±5% vs baseline
  captured from `main`. SC-007 gate. Currently DRAFT pending that run.

## Files / LOC

20 commits over 4 days; spec design + 6 implementation phases + Polish.
~17 commits ahead of `origin/047-process-state-cleanup` at PR open;
**3 sources singletons gone**, **6 destructors made explicit `noexcept`**,
**~750 LOC net delta** including 8 new test files (4 C++ + 4 SQL).

## Public API surface

- Additive: `mssql_close_all()` scalar (FR-013).
- Additive: `application_name` ATTACH option / URI param / secret field
  (FR-014).
- Additive: `lazy_validation true` ATTACH option +
  `mssql_attach_validation_timeout` setting (FR-011).
- Deprecation (label-only, no behavior change): `mssql_open` / `mssql_close`
  / `mssql_ping` / `mssql_close_all` marked `[DEPRECATED]` in CLAUDE.md
  table + CHANGELOG + source comments at registration sites (FR-010).
- No removals. No behavior change for existing callers.

## Follow-ups (separate work)

- **Lazy credential materialization**
  ([issue #119](https://github.com/hugr-lab/mssql-extension/issues/119))
  → future spec 049: zeroize passwords / tokens at rest in
  `MSSQLConnectionInfo` so a `MSSQLCatalog` instance does not hold
  long-lived plaintext credentials. Spawned by the FR-011 security review.
- **Cooperative TDS cancellation** (future spec): replace
  `~ConnectionPool` immediate-close semantics with atomic flag polled by
  the owning thread so a graceful TDS ATTENTION cancel can be sent.
  Tracked in spec 047 Constraints / non-goals.
- **Removal of `mssql_open` / `mssql_close` / `mssql_ping` /
  `mssql_close_all`**: future major release retires the diagnostic API
  entirely; `MSSQLConnectionHandleManager` goes with them. Reduces
  extension-internal singleton count from 2 to 1 (TokenCache).
- **clang-format-14 sweep** (`T053`) — DRAFT pending; cosmetic; planned
  pre-merge.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

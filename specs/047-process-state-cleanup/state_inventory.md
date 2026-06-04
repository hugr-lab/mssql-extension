# Spec 047 — Process-Wide State Inventory (post-implementation)

Final classification of every process-wide static in the mssql-extension after
spec 047 lands. Closes [FR-007](spec.md), [SC-008](spec.md).

**Scope**: extension-internal state only. DuckDB-side process-wide state
(catalog manager, secret manager, connection pool of the DuckDB engine
itself, etc.) is out of scope.

**Rule**: 0 "migrate" entries below. Every static is either:

- **DELETED** — removed in spec 047, no replacement at process scope.
- **KEEP** — retained intentionally with documented rationale.

## Deleted (3 singletons removed by spec 047)

| Name | Pre-047 file | What it held | Removed by | Replacement |
|------|--------------|--------------|------------|-------------|
| `MssqlPoolManager` | `src/connection/mssql_pool_manager.{cpp,hpp}` | `unordered_map<string, shared_ptr<ConnectionPool>>` keyed by ATTACH context name + per-pool pinned-connection counts | T021 / commit `fede962` | `MSSQLCatalog::connection_pool_` (`unique_ptr<ConnectionPool>`, owned per catalog). Pool lifetime = catalog lifetime. |
| `MSSQLContextManager` + `g_context_managers` + `g_context_managers_lock` | `src/mssql_storage.cpp` (lines ~792-833 pre-removal) | `unordered_map<string, shared_ptr<MSSQLContext>>` keyed by ATTACH context name — band-aid added by spec 045 (`70a4d90`) for cross-callback context lookup | T020 / commit `e6371de` | Direct catalog lookup: `Catalog::GetCatalog(context, name).Cast<MSSQLCatalog>()`. The band-aid is retired entirely. |
| `MSSQLResultStreamRegistry` | `src/mssql_functions.{cpp,hpp}` | `unordered_map<uint64_t, unique_ptr<MSSQLResultStream>>` for mssql_scan Bind→InitGlobal stream handoff | T032-T041 / commit `9d4da5c` | `MSSQLCatalog::active_streams_` (per-catalog map; UUID v4 keys via `UUID::ToString(UUID::GenerateRandomUUID())`; atomic find+erase under one mutex). |

## Kept (legitimate process-wide state)

| Name | File | What it holds | Rationale |
|------|------|---------------|-----------|
| `mssql::azure::TokenCache::Instance()` | `src/azure/azure_token.{cpp,hpp}` | `unordered_map<pair<uintptr_t, string>, CachedToken>` of OAuth2 tokens, namespaced by `(DatabaseInstance address, cache_key)` | Cross-instance token reuse is the explicit feature (Azure OAuth2 tokens are bearer credentials with TTL ≥ 5 min; re-acquiring per-instance burns tokens and rate budget). FR-012 (spec 047 security fold-in) added the namespace key so two instances sharing a secret name no longer alias to the same row. Entries belonging to a destroyed `DatabaseInstance` become unreachable and are evicted by TTL; DuckDB does not expose a `~DatabaseInstance` hook so proactive reaping is impossible without invasive plumbing. |
| `MSSQLConnectionHandleManager::Instance()` | `src/connection/mssql_diagnostic.{cpp,hpp}` | `unordered_map<int64_t, shared_ptr<TdsConnection>>` for `mssql_open` / `mssql_close` / `mssql_ping` | The diagnostic API trio takes a connection string (not a catalog name) by design — no per-catalog owner exists. **[DEPRECATED]** (FR-010 group): the four diagnostic functions (open / close / ping / `mssql_close_all` from FR-013) carry `[DEPRECATED]` markers and are scheduled for removal in a future major release, at which point this singleton goes with them. `mssql_close_all()` provides hosts a deterministic shutdown path before that removal. |
| Winsock `WSAStartup` (Windows only) | `src/tds/tds_socket.cpp` | Process-wide socket library init | OS-level requirement; non-extension-internal. The `WSAStartup` / `WSACleanup` calls match the Windows socket lifecycle and have no replacement at extension scope. |
| OpenSSL global state | `vcpkg` libssl / libcrypto | Library-level cipher tables, RNG seeds, etc. | Library-level state owned by OpenSSL itself; non-extension-internal. Initialization is handled by OpenSSL's library constructors on load. |
| `GetXxxDebugLevel()` function-local statics | `src/*/*.cpp` (~10 sites: mssql_storage, mssql_functions, mssql_connection_provider, table_scan, dml/*, etc.) | `static int level` caching the `MSSQL_DEBUG` env var on first call | Per-process cached env-var reads. Read-once, never mutated after initialization. Not a "singleton state holder" in the cleanup-target sense — purely a performance optimization to avoid `getenv` on every debug-log emission. |
| Thread-local scratch (`thread_local CreateTableInfo info` / `thread_local CreateSchemaInfo info`) | `src/catalog/mssql_table_entry.cpp:60`, `src/catalog/mssql_schema_entry.cpp:36` | Reusable scratch objects for catalog-creation paths | Per-thread, not process-wide. Lives in storage class `static thread_local`; each thread sees its own instance. |

## Verification

SC-004 grep gate (`grep -rn 'MssqlPoolManager|MSSQLContextManager|MSSQLResultStreamRegistry' src/ src/include/`) returns only comment-only history references — the three target identifiers no longer appear in live code. Verified manually post-implementation; final grep dump in `pr_description.md`.

`TokenCache` and `MSSQLConnectionHandleManager` are **intentionally excluded from the SC-004 grep**: they are the two singletons the spec keeps, with the rationale documented above and in CLAUDE.md.

## Follow-ups (not 047 scope)

- **Lazy credential materialization** ([issue #119](https://github.com/hugr-lab/mssql-extension/issues/119) → spec 049): zeroize passwords / tokens at rest in `MSSQLConnectionInfo` so a `MSSQLCatalog` instance does not hold long-lived plaintext credentials. Touches FR-011 surface but is orthogonal to the singleton-cleanup theme of 047.
- **Cooperative TDS cancellation** (future spec): replace `~ConnectionPool` immediate-close semantics with an atomic flag polled by the owning thread so a graceful TDS ATTENTION cancel can be sent. Tracked in spec 047 Constraints / non-goals; out of scope here.
- **Removal of `mssql_open` / `mssql_close` / `mssql_ping` / `mssql_close_all`**: future major release retires the diagnostic API entirely, taking `MSSQLConnectionHandleManager` with it. That brings the extension-internal singleton count from 2 to 1 (TokenCache).

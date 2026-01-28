# Research Findings: Pool Stats and Connection Validation

**Feature**: 023-pool-stats-validation
**Date**: 2026-01-28

## 1. Query Timeout Implementation

### Decision
Implement `mssql_query_timeout` as a DuckDB extension setting that propagates to `MSSQLResultStream::read_timeout_ms_`.

### Rationale
- Socket layer already supports per-operation timeouts via `poll()` (Unix/Linux) and `WSAPoll()` (Windows)
- `MSSQLResultStream` has hardcoded `read_timeout_ms_ = 30000` that can be made configurable
- Existing `DEFAULT_QUERY_TIMEOUT = 30` constant in `tds_types.hpp` confirms 30s is the expected default
- Cancellation flow uses separate short timeouts (10ms polling) and won't be affected

### Alternatives Considered
1. **TDS-level timeout**: Too low-level, would require protocol changes
2. **Per-connection timeout**: Less flexible, can't adjust mid-session
3. **Server-side `SET LOCK_TIMEOUT`**: Different semantics, only affects lock waits

### Key Implementation Points
- Register setting in `mssql_settings.cpp` following existing patterns
- Add `query_timeout` field to `MSSQLPoolConfig` struct
- Pass timeout through `ConnectionProvider` → `MSSQLResultStream` constructor
- Value 0 = infinite timeout (pass large value like `INT_MAX` to socket layer)

### File Locations
| Component | File | Lines |
|-----------|------|-------|
| Constants | `src/include/tds/tds_types.hpp` | 164-169 |
| Socket timeout | `src/tds/tds_socket.cpp` | 694-733 (WaitForReady) |
| Result stream | `src/query/mssql_result_stream.cpp` | 180-186, 375-387 |
| Settings | `src/connection/mssql_settings.cpp` | New registration |

---

## 2. Connection Pinning for Transactions

### Decision
Track pinned connections via a counter in `MssqlPoolManager` that is incremented/decremented by `MSSQLTransaction`.

### Rationale
- `MSSQLTransaction` already tracks `pinned_connection_` member variable
- `MSSQLTransaction::SetPinnedConnection()` is called when connection is pinned/unpinned
- Pool manager is the central point for statistics; adding counter there is cleanest
- Existing mutex protection in transaction class ensures thread safety

### Alternatives Considered
1. **Track in ConnectionPool**: Would require transaction ID awareness in pool
2. **Track in TdsConnection**: Connection doesn't know about transactions
3. **Derive from active_connections**: Impossible to distinguish pinned vs borrowed

### Key Implementation Points
- Add `pinned_connections` field to `PoolStatistics` struct
- Add `IncrementPinnedCount()` / `DecrementPinnedCount()` to `MssqlPoolManager`
- Call from `MSSQLTransaction::SetPinnedConnection()` when pinning/unpinning
- Update `mssql_pool_stats()` in `mssql_diagnostic.cpp` to include new column

### File Locations
| Component | File | Lines |
|-----------|------|-------|
| Transaction pinning | `src/catalog/mssql_transaction.cpp` | 110-133 |
| Pool statistics | `src/include/tds/tds_connection_pool.hpp` | 36-45 |
| Pool manager | `src/connection/mssql_pool_manager.cpp` | 69-76 |
| Diagnostic output | `src/connection/mssql_diagnostic.cpp` | 167-270 |

---

## 3. Catalog-Free Mode

### Decision
Add `catalog` parameter (secret and connection string) that skips `MSSQLCatalog::Initialize()` and guards `EnsureCacheLoaded()`.

### Rationale
- Catalog initialization only queries collation at ATTACH time (single query)
- Metadata loading is lazy (on-demand) via `EnsureCacheLoaded()`
- `mssql_scan` and `mssql_exec` already bypass catalog entirely
- Guard in `EnsureCacheLoaded()` provides clear error when catalog access attempted

### Alternatives Considered
1. **Null catalog**: Would require extensive null checks throughout
2. **Stub catalog**: More complex, same end result
3. **Separate attach function**: Confusing UX, unnecessary duplication

### Key Implementation Points
- Add `MSSQL_SECRET_CATALOG` constant for secret field name
- Parse `catalog=true/false` in `FromSecret()` and `Catalog=yes/no` in `FromConnectionString()`
- Add `catalog_enabled` field to `MSSQLConnectionInfo`
- In `MSSQLAttach()`: if `!catalog_enabled`, skip `catalog->Initialize()` call
- In `MSSQLCatalog::EnsureCacheLoaded()`: throw if `!catalog_enabled_`

### File Locations
| Component | File | Lines |
|-----------|------|-------|
| Secret parsing | `src/mssql_secret.cpp` | New field |
| Connection parsing | `src/mssql_storage.cpp` | 66-73, 316-343 |
| Catalog init | `src/catalog/mssql_catalog.cpp` | 64-81 |
| Lazy loading | `src/catalog/mssql_catalog.cpp` | 609-637 |
| mssql_scan | `src/mssql_functions.cpp` | 68-248 (no changes) |

---

## 4. TLS Default Behavior

### Decision
Change default `use_encrypt` from `false` to `true`. Users must explicitly set `use_encrypt=false` for non-TLS connections.

### Rationale
- Modern SQL Server deployments generally require/support TLS
- Security-first approach aligns with constitution principle III (Correctness over Convenience)
- ADO.NET driver defaults to encryption in modern versions
- No fallback protects against TLS downgrade attacks

### Alternatives Considered
1. **TLS-first with fallback**: Convenient but vulnerable to downgrade attacks
2. **Non-TLS default**: Backward compatible but insecure
3. **Auto-detect from server**: Complex, still vulnerable if server compromised

### Key Implementation Points
- Change `use_encrypt = false` to `use_encrypt = true` in `MSSQLConnectionInfo` struct
- Update `FromSecret()`: default to `true` when `use_encrypt` not specified
- Update `FromConnectionString()`: default `encrypt_value` and `trust_cert_value` to `true`
- Existing error handling for `ENCRYPT_REQ` vs `use_encrypt=false` already exists

### File Locations
| Component | File | Lines |
|-----------|------|-------|
| Struct default | `src/include/mssql_storage.hpp` | Line 32 |
| Secret parsing | `src/mssql_storage.cpp` | 66-73 |
| Connection string | `src/mssql_storage.cpp` | 316-343 |
| PRELOGIN handling | `src/tds/tds_connection.cpp` | 135-193 |

---

## 5. TLS Validation at ATTACH

### Decision
Execute `SELECT 1` validation query in `ValidateConnection()` after successful TLS authentication.

### Rationale
- Current validation only tests TCP connect + authentication
- TLS issues can appear later during actual query execution
- Simple validation query verifies full TLS data path works
- Error translation already exists in `TranslateConnectionError()`

### Alternatives Considered
1. **Ping only**: Doesn't verify TLS data transfer
2. **Full metadata query**: Too slow, unnecessary
3. **Deferred validation**: Defeats purpose of fail-fast

### Key Implementation Points
- After `conn.Authenticate()` succeeds in `ValidateConnection()`
- If `info.use_encrypt`, execute `SELECT 1` using `ExecuteBatch()`
- Catch exceptions, translate to user-friendly TLS error messages
- Add TLS-specific patterns to `TranslateConnectionError()`

### File Locations
| Component | File | Lines |
|-----------|------|-------|
| Validation | `src/mssql_storage.cpp` | 474-508 |
| Error translation | `src/mssql_storage.cpp` | 425-472 |
| ExecuteBatch | `src/tds/tds_connection.cpp` | 389-507 |

---

## Dependencies Between Features

```
┌─────────────────────┐
│ TLS by Default (P3) │
└──────────┬──────────┘
           │ enables
           ▼
┌─────────────────────────────┐
│ TLS Validation at ATTACH    │
│ (P1) - validates TLS works  │
└─────────────────────────────┘

┌─────────────────────┐     ┌─────────────────────┐
│ Query Timeout (P1)  │     │ Catalog-Free (P2)   │
└─────────────────────┘     └─────────────────────┘
        │                           │
        │ independent               │ independent
        ▼                           ▼
┌─────────────────────────────────────────────────┐
│             Extended Pool Stats (P2)            │
│     (pinned tracking independent of above)      │
└─────────────────────────────────────────────────┘
```

All features are independently implementable and testable.

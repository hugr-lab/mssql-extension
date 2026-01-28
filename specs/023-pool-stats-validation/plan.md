# Implementation Plan: Extend Pool Stats and Connection Validation

**Branch**: `023-pool-stats-validation` | **Date**: 2026-01-28 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/023-pool-stats-validation/spec.md`

## Summary

This feature extends the mssql-extension with five improvements:
1. **TLS Validation at ATTACH** - Execute a validation query (`SELECT 1`) after TLS handshake to verify TLS functionality early
2. **Query Timeout Setting** - Add configurable `mssql_query_timeout` setting (default: 30s) propagated to TDS layer
3. **Catalog-Free Mode** - Add `catalog` parameter to skip schema discovery for serverless/restricted databases
4. **Extended Pool Stats** - Add `active_connections` and `pinned_connections` columns to `mssql_pool_stats()`
5. **TLS by Default** - Change default encryption behavior to TLS-on (no fallback to non-TLS)

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (via vcpkg for TLS)
**Storage**: In-memory (connection pool state, metadata cache)
**Testing**: SQLLogicTest (integration), C++ unit tests (no SQL Server required)
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW)
**Project Type**: Single project (DuckDB extension)
**Performance Goals**: TLS validation query < 1s, no measurable overhead for query timeout propagation
**Constraints**: Maintain backward compatibility for explicit `use_encrypt=false` settings
**Scale/Scope**: Extension-level changes affecting ~15 source files

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Evidence |
|-----------|--------|----------|
| I. Native and Open | PASS | Uses existing native TDS implementation, no new dependencies |
| II. Streaming First | PASS | Query timeout affects socket read operations, no buffering changes |
| III. Correctness over Convenience | PASS | TLS-by-default improves security; explicit opt-out required for non-TLS |
| IV. Explicit State Machines | PASS | No changes to connection state machine |
| V. DuckDB-Native UX | PASS | New setting follows DuckDB patterns; catalog-free mode provides clear error messages |
| VI. Incremental Delivery | PASS | Each feature independently usable and testable |

**Post-Design Re-check**: All principles remain satisfied.

## Project Structure

### Documentation (this feature)

```text
specs/023-pool-stats-validation/
├── plan.md              # This file
├── research.md          # Phase 0 research findings
├── data-model.md        # Phase 1 entity definitions
├── quickstart.md        # Phase 1 usage guide
├── contracts/           # Phase 1 API contracts
└── tasks.md             # Phase 2 task breakdown
```

### Source Code (repository root)

```text
src/
├── include/
│   ├── tds/
│   │   ├── tds_types.hpp              # Add DEFAULT_QUERY_TIMEOUT constant reference
│   │   └── tds_connection_pool.hpp    # Extend PoolStatistics struct
│   ├── connection/
│   │   ├── mssql_settings.hpp         # Add query_timeout to pool config
│   │   └── mssql_pool_manager.hpp     # Add pinned tracking method
│   ├── catalog/
│   │   ├── mssql_catalog.hpp          # Add catalog_enabled flag
│   │   └── mssql_transaction.hpp      # Expose pinned connection state
│   └── mssql_storage.hpp              # Add catalog_enabled to MSSQLConnectionInfo
│   └── mssql_secret.hpp               # Add catalog secret field constant
├── connection/
│   ├── mssql_settings.cpp             # Register mssql_query_timeout setting
│   ├── mssql_pool_manager.cpp         # Track pinned connections
│   └── mssql_diagnostic.cpp           # Add columns to mssql_pool_stats()
├── catalog/
│   ├── mssql_catalog.cpp              # Skip initialization when catalog disabled
│   └── mssql_transaction.cpp          # Report pinned state to pool manager
├── tds/
│   └── tds_connection_pool.cpp        # Track active/pinned in statistics
├── query/
│   └── mssql_result_stream.cpp        # Accept configurable timeout
├── mssql_storage.cpp                  # Parse catalog param, TLS default, validation query
├── mssql_secret.cpp                   # Parse catalog field from secret
└── mssql_functions.cpp                # Pass timeout to query execution

test/
├── sql/
│   ├── attach/
│   │   ├── attach_tls_validation.test     # TLS validation at attach time
│   │   ├── attach_catalog_disabled.test   # Catalog-free mode tests
│   │   └── attach_tls_default.test        # TLS-by-default tests
│   ├── integration/
│   │   ├── query_timeout.test             # Query timeout configuration
│   │   └── pool_stats_extended.test       # Extended pool statistics
│   └── tds_connection/
│       └── pool_stats_pinned.test         # Pinned connection tracking
└── cpp/
    └── test_pool_statistics.cpp           # Unit tests for statistics struct
```

**Structure Decision**: Single project structure - all changes within existing `src/` and `test/` directories following established DuckDB extension conventions.

## Complexity Tracking

No constitution violations requiring justification.

## Implementation Phases

### Phase 1: Query Timeout Setting (P1)

**Scope**: FR-004 through FR-007

1. Add `mssql_query_timeout` setting to `mssql_settings.cpp`
2. Add `query_timeout` field to `MSSQLPoolConfig` struct
3. Propagate timeout through `ConnectionProvider` → `MSSQLResultStream`
4. Update `MSSQLResultStream` constructor to accept configurable timeout (replace hardcoded `read_timeout_ms_ = 30000`)
5. Support value 0 for infinite timeout

**Key Files**:
- `src/connection/mssql_settings.cpp` (register setting)
- `src/include/connection/mssql_settings.hpp` (add to config struct)
- `src/query/mssql_result_stream.cpp` (accept timeout parameter)
- `src/include/query/mssql_result_stream.hpp` (update constructor)

### Phase 2: TLS Validation at ATTACH (P1)

**Scope**: FR-001 through FR-003

1. In `ValidateConnection()`, after successful authentication with TLS enabled, execute `SELECT 1` validation query
2. Catch and translate TLS-specific errors to user-friendly messages
3. Detect `ENCRYPT_REQ` response when `use_encrypt=false` and provide actionable error

**Key Files**:
- `src/mssql_storage.cpp` (extend `ValidateConnection()`)
- Error messages integrated into existing `TranslateConnectionError()` function

### Phase 3: TLS by Default (P3)

**Scope**: FR-016 through FR-019

1. Change default `use_encrypt` from `false` to `true` in:
   - `MSSQLConnectionInfo` struct initialization
   - `FromSecret()` when `use_encrypt` not specified
   - `FromConnectionString()` when encrypt/trustservercertificate not specified
2. Ensure explicit `use_encrypt=false` still works for backward compatibility
3. Add clear error when user sets `use_encrypt=false` but server requires TLS

**Key Files**:
- `src/include/mssql_storage.hpp` (change default)
- `src/mssql_storage.cpp` (change defaults in FromSecret/FromConnectionString)

### Phase 4: Catalog-Free Mode (P2)

**Scope**: FR-008 through FR-012

1. Add `MSSQL_SECRET_CATALOG` constant to `mssql_secret.hpp`
2. Parse `catalog` field from secret in `CreateMSSQLSecretFromConfig()`
3. Parse `Catalog` parameter from connection string in `FromConnectionString()`
4. Add `catalog_enabled` field to `MSSQLConnectionInfo`
5. In `MSSQLAttach()`, skip catalog initialization when disabled
6. In `MSSQLCatalog::EnsureCacheLoaded()`, throw clear error if catalog access attempted while disabled
7. Verify `mssql_scan` and `mssql_exec` continue to work (they already bypass catalog)

**Key Files**:
- `src/include/mssql_secret.hpp` (add constant)
- `src/mssql_secret.cpp` (parse catalog field)
- `src/mssql_storage.cpp` (parse from connection string, skip initialization)
- `src/include/mssql_storage.hpp` (add catalog_enabled field)
- `src/catalog/mssql_catalog.cpp` (guard EnsureCacheLoaded)

### Phase 5: Extended Pool Statistics (P2)

**Scope**: FR-013 through FR-015

1. Extend `PoolStatistics` struct with `pinned_connections` field
2. Add pinned connection tracking to `ConnectionPool`:
   - Track when `Acquire()` is called with transaction context
   - Update count when connections are pinned/unpinned
3. Expose tracking from `MSSQLTransaction` to pool manager
4. Update `mssql_pool_stats()` table function to include new columns

**Key Files**:
- `src/include/tds/tds_connection_pool.hpp` (extend struct)
- `src/tds/tds_connection_pool.cpp` (track pinned count)
- `src/connection/mssql_diagnostic.cpp` (add columns to output)
- `src/include/connection/mssql_pool_manager.hpp` (add pinned tracking interface)
- `src/connection/mssql_pool_manager.cpp` (implement tracking)

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| Breaking existing users with TLS-by-default | SC-006 explicitly tests backward compatibility; explicit `use_encrypt=false` preserved |
| Query timeout affecting cancellation flow | Cancellation uses separate short timeouts (10ms polling); query timeout only affects data receive |
| Catalog-free mode confusion | Clear error messages when catalog access attempted; documentation in quickstart.md |
| Pool stats accuracy under concurrency | Use existing mutex-protected statistics updates; pinned count uses atomic tracking |

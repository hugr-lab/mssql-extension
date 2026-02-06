# Implementation Plan: Connection & FEDAUTH Refactoring

**Branch**: `031-connection-fedauth-refactor` | **Date**: 2026-02-06 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/031-connection-fedauth-refactor/spec.md`

## Summary

Fix critical bugs affecting FEDAUTH connections (BCP state corruption, transaction INSERT failures, token expiration handling) and refactor the connection/authentication layer to reduce code duplication. Phase 0 fixes are blocking and must be completed before refactoring phases.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (vcpkg), libcurl (vcpkg for Azure OAuth2)
**Storage**: In-memory (connection pool state, token cache)
**Testing**: SQLLogicTest (integration), C++ unit tests (no SQL Server), Azure Warehouse manual testing
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW/Rtools 4.2)
**Project Type**: Single C++ extension project
**Performance Goals**: No regression in connection acquisition (< 5% increase)
**Constraints**: Must work with SQL Server 2019+, Azure SQL, Fabric Warehouse; token TTL ~1 hour
**Scale/Scope**: 6 critical bugs (Phase 0), 5 refactoring phases

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | Uses native TDS implementation, no ODBC/JDBC |
| II. Streaming First | PASS | BCP/COPY uses streaming, refactoring preserves this |
| III. Correctness over Convenience | PASS | Fixing state corruption bugs improves correctness |
| IV. Explicit State Machines | PASS | Phase 4 formalizes connection state machine |
| V. DuckDB-Native UX | PASS | ATTACH validation improves catalog experience |
| VI. Incremental Delivery | PASS | Phase 0 bugs first, refactoring phases incremental |

**Row Identity Model**: Not affected by this feature (connection layer, not DML).

**Version Baseline**: SQL Server 2019+ requirement maintained.

## Project Structure

### Documentation (this feature)

```text
specs/031-connection-fedauth-refactor/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output (state machine, error types)
├── quickstart.md        # Phase 1 output (debug/test guide)
├── contracts/           # Phase 1 output (not applicable - internal refactor)
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
src/
├── azure/                    # Azure authentication
│   ├── azure_token.cpp       # TokenCache + token acquisition
│   ├── azure_fedauth.cpp     # FEDAUTH token encoding
│   └── azure_secret_reader.cpp
├── connection/               # Connection management
│   ├── mssql_connection_provider.cpp  # Transaction pinning
│   └── mssql_pool_manager.cpp         # Pool lifecycle
├── copy/
│   └── bcp_writer.cpp        # BCP error handling (Bug 0.1)
├── tds/
│   ├── tds_connection.cpp    # State machine, auth flows
│   ├── tds_connection_pool.cpp # Pool acquire/release (Bug 0.6)
│   └── auth/                 # NEW: Phase 1 auth strategies
└── include/                  # Headers (mirrors src/)

test/
├── sql/
│   ├── azure/                # FEDAUTH tests
│   │   ├── azure_service_principal.test
│   │   └── azure_transaction_insert.test  # NEW: Bug 0.3
│   └── copy/
│       └── bcp_error_recovery.test        # NEW: Bug 0.1
└── cpp/
    └── test_fedauth_encoding.cpp
```

**Structure Decision**: Single project structure preserved. New auth/ subdirectory for Phase 1 strategy pattern.

## Complexity Tracking

> No constitution violations requiring justification.

## Phase 0: Bug Analysis

### Bug 0.1: BCP State Corruption

**Root Cause Identified**: In `bcp_writer.cpp:261`, when BCP throws an exception on error, line 269 (`TransitionState(Executing, Idle)`) is never reached. The connection stays in `Executing` state but gets returned to the pool.

**Pool Validation Gap**: `ConnectionPool::Release()` (line 130-167) only checks `IsAlive()`, not connection state. A connection in `Executing` state passes `IsAlive()` check and gets returned to idle pool.

**Fix Strategy**:
1. Transition to `Idle` or `Close` connection before throwing in BCP error path
2. Add state validation in `Release()` - close connections not in `Idle` state

### Bug 0.2: Excessive Connection Acquires

**Root Cause Hypothesis**: Each metadata lookup during CTAS may acquire a new connection. Need to trace connection acquisition during CTAS to identify specific lookup points.

**Investigation Needed**: Add temporary debug logging to track cache hits/misses.

### Bug 0.3: INSERT in Transaction Fails

**Root Cause Hypothesis**: Schema lookup code path doesn't check for pinned transaction connection before acquiring from pool. Needs investigation in `mssql_connection_provider.cpp`.

### Bug 0.4: Token Expiration

**Current State**: `TokenCache::GetToken()` correctly checks `IsValid()` which considers expiration with 5-minute margin. However, the issue is that pool connections are created at ATTACH time with a pre-encoded token that's captured in the factory closure - this token never gets refreshed.

**Fix Strategy**:
1. On auth failure, invalidate cached token and retry once with fresh token
2. On DETACH, call `TokenCache::Invalidate()` for the context's Azure secret

### Bug 0.5: FEDAUTH ATTACH Validation

**Current State**: Need to verify if SQL auth path does SELECT 1 validation and if FEDAUTH path skips it.

### Bug 0.6: Printf Debugging

**Current State**: Line 60 in `tds_connection_pool.cpp` has unconditional `fprintf`. Should use existing `GetMssqlDebugLevel()` pattern.

## Research Tasks (Phase 0)

1. **Trace CTAS connection acquisition** - Add debug logging to identify all acquire calls during a single CTAS
2. **Verify INSERT transaction pinning** - Check if schema lookup uses pinned connection
3. **Compare SQL vs FEDAUTH ATTACH paths** - Verify validation step presence
4. **Test token refresh on auth failure** - Confirm retry logic would work

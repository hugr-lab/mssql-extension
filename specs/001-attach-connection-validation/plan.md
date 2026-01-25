# Implementation Plan: Attach Connection Validation

**Branch**: `001-attach-connection-validation` | **Date**: 2026-01-25 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/001-attach-connection-validation/spec.md`

## Summary

Add connection validation during ATTACH to fail fast when credentials are invalid, instead of deferring failure to subsequent catalog queries. Also add `TrustServerCertificate` as an alias for `use_encrypt` for ADO.NET compatibility.

Currently, the `MSSQLAttach` function creates a connection pool factory and catalog without actually testing the connection. Invalid credentials are only discovered later when catalog queries trigger connection acquisition, resulting in confusing "IO Error: Failed to acquire connection" messages.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB main branch (catalog API), existing TDS layer (specs 001-012)
**Storage**: In-memory (connection pool, metadata cache)
**Testing**: DuckDB SQLLogicTest format
**Target Platform**: Linux, macOS, Windows (cross-platform)
**Project Type**: Single (DuckDB extension)
**Performance Goals**: <1 second overhead for connection validation on valid credentials
**Constraints**: 30 second default timeout for unreachable servers, 5 second target for auth failures
**Scale/Scope**: Single connection test sufficient for validation

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | ✅ PASS | Uses existing native TDS implementation |
| II. Streaming First | ✅ N/A | No result streaming involved |
| III. Correctness over Convenience | ✅ PASS | Fail fast on invalid credentials instead of silent failure |
| IV. Explicit State Machines | ✅ PASS | Uses existing TdsConnection state machine |
| V. DuckDB-Native UX | ✅ PASS | ATTACH fails immediately with clear error, no orphaned catalogs |
| VI. Incremental Delivery | ✅ PASS | Adds validation without changing existing connection infrastructure |

**Row Identity Model**: N/A - not a DML feature

## Project Structure

### Documentation (this feature)

```text
specs/001-attach-connection-validation/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (N/A - internal change)
└── tasks.md             # Phase 2 output
```

### Source Code (repository root)

```text
src/
├── mssql_storage.cpp        # MODIFY: Add connection validation in MSSQLAttach
├── include/
│   └── mssql_storage.hpp    # MODIFY: Add TrustServerCertificate parsing
└── connection/
    └── mssql_pool_manager.cpp  # REVIEW: Connection factory error handling

test/sql/
└── attach/                  # NEW: Connection validation tests
    ├── attach_validation.test
    └── attach_trust_cert.test
```

**Structure Decision**: Minimal changes to existing structure. Main modification in `mssql_storage.cpp` for validation logic.

## Complexity Tracking

No constitution violations. The implementation adds validation to existing ATTACH flow without introducing new complexity.

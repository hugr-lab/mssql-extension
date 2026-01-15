# Implementation Plan: DuckDB Surface API

**Branch**: `002-duckdb-surface-api` | **Date**: 2026-01-15 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/002-duckdb-surface-api/spec.md`

## Summary

Define and implement the public DuckDB interface for the MSSQL extension including:
- SECRET TYPE `mssql` with host, port, database, user, password fields
- ATTACH/DETACH handlers for connection context management
- `mssql_execute` and `mssql_scan` table functions

This phase creates stub implementations (no real network traffic) to stabilize the API surface before protocol implementation.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB main branch (extension API)
**Storage**: N/A (connection metadata in memory, secrets via DuckDB's secret manager)
**Testing**: DuckDB SQL test framework (`.test` files in `test/sql/`)
**Target Platform**: Linux (x64, arm64), macOS (arm64), Windows (x64)
**Project Type**: Single (DuckDB extension)
**Performance Goals**: N/A for stub phase
**Constraints**: No external dependencies (native TDS per constitution)
**Scale/Scope**: API surface definition only

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

### Pre-Design Check

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | No external dependencies; stub implementation |
| II. Streaming First | N/A | No data streaming in this phase (stubs only) |
| III. Correctness over Convenience | PASS | Explicit errors for invalid input |
| IV. Explicit State Machines | DEFERRED | Connection state machine designed but not implemented |
| V. DuckDB-Native UX | PASS | Uses standard DuckDB patterns (secrets, attach, table functions) |
| VI. Incremental Delivery | PASS | Read-first approach; stub before real implementation |

**Pre-Design Gate Status**: PASS

### Post-Design Check (Phase 1 Complete)

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | Only DuckDB APIs used; KeyValueSecret, StorageExtension, TableFunction |
| II. Streaming First | PASS | mssql_scan uses TableFunction with chunked output pattern |
| III. Correctness over Convenience | PASS | All validation errors explicit with actionable messages |
| IV. Explicit State Machines | PASS | MSSQLContext tracks state (Created→Attached→Connected→Detached) |
| V. DuckDB-Native UX | PASS | Standard CREATE SECRET, ATTACH, DETACH, table function syntax |
| VI. Incremental Delivery | PASS | Stub phase delivers testable API before protocol implementation |

**Post-Design Gate Status**: PASS - Design fully compliant with constitution.

## Project Structure

### Documentation (this feature)

```text
specs/002-duckdb-surface-api/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (API specifications)
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
src/
├── include/
│   ├── mssql_extension.hpp      # Extension class declaration
│   ├── mssql_secret.hpp         # Secret type registration (NEW)
│   ├── mssql_storage.hpp        # Storage extension / attach handler (NEW)
│   └── mssql_functions.hpp      # Table functions (NEW)
├── mssql_extension.cpp          # Extension entry point (MODIFY)
├── mssql_secret.cpp             # Secret implementation (NEW)
├── mssql_storage.cpp            # Attach/detach implementation (NEW)
└── mssql_functions.cpp          # mssql_scan, mssql_execute (NEW)

test/sql/
├── mssql_version.test           # Existing
├── mssql_secret.test            # Secret creation/validation (NEW)
├── mssql_attach.test            # Attach/detach tests (NEW)
├── mssql_execute.test           # mssql_execute tests (NEW)
└── mssql_scan.test              # mssql_scan tests (NEW)
```

**Structure Decision**: Single project structure. Extension code in `src/`, tests in `test/sql/`. New files follow existing patterns from `mssql_extension.cpp`.

## Complexity Tracking

> No violations requiring justification.

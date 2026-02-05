# Implementation Plan: CTAS Fixes - IF NOT EXISTS and Auto-TABLOCK

**Branch**: `030-ctas-fixes` | **Date**: 2026-02-05 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/030-ctas-fixes/spec.md`

## Summary

Fix two issues with CREATE TABLE AS SELECT (CTAS) operations:
1. **Issue #44 (Bug)**: `CREATE TABLE IF NOT EXISTS` throws error when table exists instead of silently succeeding
2. **Issue #45 (Enhancement)**: Automatically use TABLOCK hint when creating new tables for 15-30% performance improvement

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB main branch, existing TDS protocol layer (Specs 001-029), OpenSSL (vcpkg)
**Storage**: N/A (remote SQL Server connection)
**Testing**: SQLLogicTest for integration tests, C++ unit tests
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW)
**Project Type**: Single DuckDB extension project
**Performance Goals**: 15-30% improvement in bulk load operations via automatic TABLOCK
**Constraints**: Must maintain backward compatibility with existing CTAS/COPY behavior
**Scale/Scope**: Small bugfix + enhancement, affects ~5-8 files

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | ✅ PASS | No new external dependencies; uses existing native TDS |
| II. Streaming First | ✅ PASS | No change to streaming behavior |
| III. Correctness over Convenience | ✅ PASS | IF NOT EXISTS follows SQL standard semantics |
| IV. Explicit State Machines | ✅ PASS | No new state machines; reuses existing CTAS state |
| V. DuckDB-Native UX | ✅ PASS | Standard DuckDB `IF NOT EXISTS` syntax support |
| VI. Incremental Delivery | ✅ PASS | Two independent fixes, can be delivered separately |

**Gate Result**: ✅ PASS - Proceed to Phase 0

## Project Structure

### Documentation (this feature)

```text
specs/030-ctas-fixes/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output
└── tasks.md             # Phase 2 output (via /speckit.tasks)
```

### Source Code (repository root)

```text
src/
├── dml/ctas/
│   ├── mssql_ctas_planner.cpp      # MODIFY: Extract if_not_exists flag
│   ├── mssql_physical_ctas.cpp     # MODIFY: Handle IF NOT EXISTS and auto-TABLOCK
│   └── mssql_ctas_executor.cpp     # MODIFY: Auto-TABLOCK logic for BCP
├── catalog/
│   └── mssql_schema_entry.cpp      # MODIFY: Handle IF NOT EXISTS for non-CTAS CREATE TABLE
├── copy/
│   └── copy_function.cpp           # MODIFY: Auto-TABLOCK for COPY TO
├── include/dml/ctas/
│   ├── mssql_ctas_types.hpp        # MODIFY: Add if_not_exists to CTASTarget
│   └── mssql_ctas_config.hpp       # MODIFY: Add is_new_table flag for auto-TABLOCK
└── include/copy/
    └── bcp_config.hpp              # MODIFY: Add is_new_table flag

test/
├── sql/ctas/
│   └── if_not_exists.test          # NEW: IF NOT EXISTS test cases
└── sql/integration/
    └── auto_tablock.test           # NEW: Auto-TABLOCK verification
```

**Structure Decision**: Single project layout following existing DuckDB extension conventions. Modifications focused on CTAS/COPY subsystems.

## Constitution Check (Post-Design)

*Re-evaluated after Phase 1 design artifacts.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | ✅ PASS | No new dependencies added |
| II. Streaming First | ✅ PASS | Streaming behavior unchanged |
| III. Correctness over Convenience | ✅ PASS | IF NOT EXISTS provides correct SQL standard behavior |
| IV. Explicit State Machines | ✅ PASS | Added `skipped` state is explicit and documented |
| V. DuckDB-Native UX | ✅ PASS | Supports standard DuckDB syntax |
| VI. Incremental Delivery | ✅ PASS | Issue #44 (P1) and #45 (P2) can be delivered independently |

**Post-Design Gate Result**: ✅ PASS - Ready for task generation

## Complexity Tracking

No constitution violations requiring justification.

# Implementation Plan: Code Cleanup and Directory Reorganization

**Branch**: `001-code-cleanup` | **Date**: 2026-01-26 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/001-code-cleanup/spec.md`

## Summary

Refactoring to improve codebase organization: rename table_scan files for consistency, remove unused code (functions and fields) with incremental testing, consolidate DML directories (insert/update/delete → dml), clean up unnecessary comments, and update documentation.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB main branch (extension API), existing TDS layer
**Storage**: N/A (code refactoring only, no new data storage)
**Testing**: DuckDB SQLLogicTest framework (make test)
**Target Platform**: macOS ARM64, Linux x86_64
**Project Type**: Single project (DuckDB extension)
**Performance Goals**: N/A (no performance changes expected)
**Constraints**: All 1371+ tests must pass after each change
**Scale/Scope**: ~50 source files affected by directory restructuring

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | ✅ Pass | No external dependencies added |
| II. Streaming First | ✅ Pass | No changes to streaming behavior |
| III. Correctness over Convenience | ✅ Pass | Incremental testing ensures correctness |
| IV. Explicit State Machines | ✅ Pass | No state machine changes |
| V. DuckDB-Native UX | ✅ Pass | No user-facing API changes |
| VI. Incremental Delivery | ✅ Pass | Each change tested independently |

**Gate Status**: PASS - All principles satisfied. This is a pure refactoring with no functional changes.

## Project Structure

### Documentation (this feature)

```text
specs/001-code-cleanup/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output (minimal for refactoring)
├── quickstart.md        # Phase 1 output (verification steps)
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (current → target)

```text
# CURRENT STRUCTURE
src/
├── table_scan/
│   └── mssql_table_scan.cpp    # → rename to table_scan.cpp
├── insert/                      # → move to dml/
├── update/                      # → move to dml/
├── delete/                      # → move to dml/
└── include/
    ├── table_scan/
    │   └── mssql_table_scan.hpp # → rename to table_scan.hpp
    ├── insert/                  # → move to dml/
    ├── update/                  # → move to dml/
    └── delete/                  # → move to dml/

# TARGET STRUCTURE
src/
├── table_scan/
│   └── table_scan.cpp          # Renamed
├── dml/                         # Consolidated
│   ├── insert/                  # From src/insert/
│   ├── update/                  # From src/update/
│   └── delete/                  # From src/delete/
└── include/
    ├── table_scan/
    │   └── table_scan.hpp      # Renamed
    └── dml/                     # Consolidated
        ├── insert/              # From src/include/insert/
        ├── update/              # From src/include/update/
        └── delete/              # From src/include/delete/
```

**Structure Decision**: Consolidating DML operations under a single `dml/` parent directory improves discoverability and reflects the logical grouping of INSERT, UPDATE, DELETE operations.

## Complexity Tracking

No constitution violations - no complexity justification needed.

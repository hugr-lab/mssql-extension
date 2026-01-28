# Implementation Plan: Improve Multi-Resultset Error Messages

**Branch**: `021-multi-resultset-error` | **Date**: 2026-01-28 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/021-multi-resultset-error/spec.md`

## Summary

When a multi-statement SQL batch executed via `mssql_scan()` produces more than one result set, the system crashes with an internal type assertion error (`Expected vector of type INT16, but found vector of type INT32`). The fix detects a second `COLMETADATA` token during row streaming in `FillChunk()`, raises a clear error message, and drains remaining TDS tokens to leave the connection in a clean state.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB main branch (extension API, DataChunk), existing TDS layer (specs 001-020)
**Storage**: In-memory (result streaming, no intermediate buffering)
**Testing**: DuckDB extension test framework (`make test`), integration tests (require SQL Server)
**Target Platform**: macOS, Linux, Windows
**Project Type**: Single project (DuckDB extension)
**Performance Goals**: No performance impact on single-result-set queries
**Constraints**: Error detection must happen before any type mismatch crash; connection must remain reusable after error
**Scale/Scope**: Single file change (`mssql_result_stream.cpp`), ~20 lines of new code

## Constitution Check

*No constitution file found. Proceeding with standard project conventions.*

## Project Structure

### Documentation (this feature)

```text
specs/021-multi-resultset-error/
├── plan.md              # This file
├── research.md          # Root cause analysis and fix design
├── quickstart.md        # Exact code changes needed
├── checklists/
│   └── requirements.md  # Specification quality checklist
└── tasks.md             # Task breakdown (created by /speckit.tasks)
```

### Source Code (repository root)

```text
src/
├── query/
│   └── mssql_result_stream.cpp    # FillChunk() — add ColMetadata case + DrainRemaining helper
└── include/
    └── query/
        └── mssql_result_stream.hpp  # Add DrainRemaining() declaration (if needed)

tests/
└── integration/
    └── (existing tests verify no regression)
```

**Structure Decision**: This is a minimal bug fix touching one source file (`mssql_result_stream.cpp`). The fix adds a new `case` handler in the `FillChunk()` switch statement and a helper method to drain remaining tokens after error detection.

## Complexity Tracking

No complexity violations. This is a targeted fix adding a single case handler and error path.

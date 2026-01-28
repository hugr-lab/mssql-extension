# Implementation Plan: Support Multi-Statement SQL in mssql_scan

**Branch**: `020-multi-statement-scan` | **Date**: 2026-01-27 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/020-multi-statement-scan/spec.md`

## Summary

The `mssql_scan` table function fails when executing multi-statement SQL batches where the first statement doesn't return a result set (e.g., `SELECT INTO #t ...; SELECT * FROM #t`). The root cause is that `MSSQLResultStream::Initialize()` treats any DONE token received before COLMETADATA as end-of-stream, without checking the `DONE_MORE` flag that indicates more results follow.

The fix has two parts:
1. **Multi-statement token handling**: Modify `Initialize()` to check `DoneToken::IsFinal()` and continue reading tokens when more results are expected.
2. **Connection pool cleanup**: Execute `sp_reset_connection` before returning connections to the pool (autocommit mode only) to clear session artifacts like temp tables, variables, and SET options. Connections pinned to explicit transactions are NOT reset until after commit/rollback.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB (main branch), existing TDS layer (specs 001-019)
**Storage**: In-memory (result streaming, connection pool state)
**Testing**: DuckDB sqllogictest framework, C++ unit tests (Catch2)
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW)
**Project Type**: Single (DuckDB extension)
**Performance Goals**: N/A (fix existing behavior, no new performance requirements)
**Constraints**: Must not break existing 108 test cases / 2741 assertions
**Scale/Scope**: 3 files modified, ~50 lines changed

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
| --------- | ------ | ----- |
| I. Native and Open | PASS | No external drivers; uses native TDS protocol |
| II. Streaming First | PASS | No buffering change; streams from first result-producing statement |
| III. Correctness over Convenience | PASS | Fixes incorrect behavior (premature stream termination); errors still reported |
| IV. Explicit State Machines | PASS | Initialize() state transitions remain explicit; new path for non-final DONE |
| V. DuckDB-Native UX | PASS | No change to catalog UX |
| VI. Incremental Delivery | PASS | Standalone fix, independently testable |

All gates pass. No violations.

## Project Structure

### Documentation (this feature)

```text
specs/020-multi-statement-scan/
├── spec.md              # Feature specification
├── plan.md              # This file
├── research.md          # Phase 0 output
├── quickstart.md        # Phase 1 output
└── checklists/
    └── requirements.md  # Quality checklist
```

### Source Code (repository root)

```text
src/
├── query/
│   └── mssql_result_stream.cpp   # MODIFY: Handle non-final DONE tokens in Initialize()
├── tds/
│   └── tds_connection_pool.cpp   # MODIFY: Add sp_reset_connection on Release()
└── connection/
    └── mssql_connection_provider.cpp  # MODIFY: Reset after commit/rollback before pool return
```

**Structure Decision**: Three files modified. The Initialize() fix is in the result stream layer. The connection reset is in the pool release path (autocommit) and the transaction commit/rollback path (explicit transactions).

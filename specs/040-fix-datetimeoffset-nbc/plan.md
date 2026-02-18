# Implementation Plan: Fix DATETIMEOFFSET in NBC Row Reader

**Branch**: `040-fix-datetimeoffset-nbc` | **Date**: 2026-02-18 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/040-fix-datetimeoffset-nbc/spec.md`

## Summary

Add the missing `TDS_TYPE_DATETIMEOFFSET` case to `ReadValueNBC()` in `tds_row_reader.cpp`, and add comprehensive integration tests covering all scale-dependent datetime types (TIME, DATETIME2, DATETIMEOFFSET) at scales 0, 3, 7 in both standard ROW and NBCROW encoding paths.

## Technical Context

**Language/Version**: C++17 (C++11-compatible for ODR on Linux)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (vcpkg), custom TDS protocol layer
**Storage**: N/A (remote SQL Server via TDS protocol)
**Testing**: SQLLogicTest (integration, requires SQL Server), C++ unit tests (Catch2, no SQL Server needed)
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW)
**Project Type**: DuckDB extension (single shared library)
**Performance Goals**: N/A (bug fix, no performance change expected)
**Constraints**: Must use `GEN=ninja` for builds; new .cpp files must be added to CMakeLists.txt
**Scale/Scope**: 1 code fix (single `case` block), 1 test init SQL update, 2 integration test files

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
| --------- | :----: | ----- |
| I. Native and Open | PASS | Fix is within our native TDS row reader, no external libraries |
| II. Streaming First | PASS | No buffering changes; fix reads data inline from TDS stream |
| III. Correctness over Convenience | PASS | Fix eliminates a runtime error for a supported type; all scales handled correctly |
| IV. Explicit State Machines | PASS | No state machine changes; fix is within existing value-read dispatch |
| V. DuckDB-Native UX | PASS | DATETIMEOFFSET already maps to TIMESTAMP_TZ; fix enables it in NBC path |
| VI. Incremental Delivery | PASS | Fix is independently testable and deployable |

No violations. No complexity tracking needed.

## Project Structure

### Documentation (this feature)

```text
specs/040-fix-datetimeoffset-nbc/
├── spec.md              # Feature specification
├── plan.md              # This file
├── research.md          # Phase 0 output (minimal — no unknowns)
├── checklists/
│   └── requirements.md  # Spec quality checklist
└── tasks.md             # Phase 2 output (/speckit.tasks)
```

### Source Code (files to modify)

```text
src/tds/tds_row_reader.cpp              # Add TDS_TYPE_DATETIMEOFFSET case to ReadValueNBC()
docker/init/init.sql                     # Add NullableDatetimeScales test table
test/sql/catalog/datetimeoffset_nbc.test # New: NBCROW integration tests for DATETIMEOFFSET
test/sql/integration/datetime_scales.test # New: comprehensive scale tests for TIME, DATETIME2, DATETIMEOFFSET
```

**Structure Decision**: This is a targeted bug fix within the existing TDS row reader. No new source files, no new classes, no architectural changes. Only one line-range change in `tds_row_reader.cpp`, plus test infrastructure additions.

## Constitution Re-Check (Post Phase 1 Design)

| Principle | Status | Notes |
| --------- | :----: | ----- |
| I. Native and Open | PASS | No external dependencies introduced |
| II. Streaming First | PASS | No buffering; NBC read is inline with existing stream |
| III. Correctness over Convenience | PASS | All scales handled; no silent truncation or data loss beyond existing microsecond precision |
| IV. Explicit State Machines | PASS | No state changes |
| V. DuckDB-Native UX | PASS | TIMESTAMP_TZ mapping preserved |
| VI. Incremental Delivery | PASS | Bug fix + tests are independently shippable |

All gates pass. No violations found post-design.

## Implementation Tasks Overview

### Task 1: Add DATETIMEOFFSET to ReadValueNBC (P1, ~5 min)

Add `case TDS_TYPE_DATETIMEOFFSET` to `ReadValueNBC()` in `src/tds/tds_row_reader.cpp` between lines 726-728, using the identical 1-byte-length-prefix pattern already used by DATE, TIME, DATETIME2, UNIQUEIDENTIFIER.

### Task 2: Add NullableDatetimeScales test table (P1, ~10 min)

Add new table to `docker/init/init.sql` with:
- TIME(0), TIME(3), TIME(7) — all nullable
- DATETIME2(0), DATETIME2(3), DATETIME2(7) — all nullable
- DATETIMEOFFSET(0), DATETIMEOFFSET(3), DATETIMEOFFSET(7) — all nullable
- 12 padding nullable INT columns to guarantee NBCROW encoding
- 5 test data rows: all non-null, all-datetime-null, mixed, dto-only, different offsets

### Task 3: Add DATETIMEOFFSET NBC integration test (P1, ~15 min)

New `test/sql/catalog/datetimeoffset_nbc.test`:
- Creates inline test table (via mssql_exec) with many nullable columns + DATETIMEOFFSET at scales 0, 3, 7
- Tests non-NULL values at each scale with UTC conversion
- Tests NULL values
- Tests mixed NULL/non-NULL rows
- Cleans up test table

### Task 4: Add comprehensive datetime scale tests (P2, ~15 min)

New `test/sql/integration/datetime_scales.test` (or extend existing):
- Tests TIME(0), TIME(3), TIME(7) in standard ROW path
- Tests DATETIMEOFFSET(0), DATETIMEOFFSET(3), DATETIMEOFFSET(7) in standard ROW path
- Tests all scale-dependent types in NBCROW path via NullableDatetimeScales table
- Verifies DATETIME2 scales still pass (regression check)

### Task 5: Build and validate (P1, ~5 min)

- `GEN=ninja make` — verify build succeeds
- `./build/release/test/unittest` — verify no C++ test regressions
- `make integration-test` — verify all integration tests pass (requires docker-up)

## Artifacts Generated

| Artifact | Path | Status |
| -------- | ---- | :----: |
| Spec | specs/040-fix-datetimeoffset-nbc/spec.md | Complete |
| Plan | specs/040-fix-datetimeoffset-nbc/plan.md | Complete |
| Research | specs/040-fix-datetimeoffset-nbc/research.md | Complete |
| Data Model | specs/040-fix-datetimeoffset-nbc/data-model.md | Complete |
| Contract | specs/040-fix-datetimeoffset-nbc/contracts/readvalue-nbc-contract.md | Complete |
| Quickstart | specs/040-fix-datetimeoffset-nbc/quickstart.md | Complete |
| Checklist | specs/040-fix-datetimeoffset-nbc/checklists/requirements.md | Complete |
| Tasks | specs/040-fix-datetimeoffset-nbc/tasks.md | Pending (/speckit.tasks) |

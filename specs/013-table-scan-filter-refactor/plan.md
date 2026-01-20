# Implementation Plan: Table Scan and Filter Pushdown Refactoring

**Branch**: `013-table-scan-filter-refactor` | **Date**: 2026-01-20 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/013-table-scan-filter-refactor/spec.md`

## Summary

Refactor the table scan implementation by moving code from `mssql_functions.cpp` to a dedicated `src/table_scan/` module, removing unused `src/pushdown/` directory, and implementing an enhanced filter encoder that supports LIKE patterns, ILIKE (via LOWER), function expressions (LOWER, UPPER, LEN, TRIM), date/time functions (YEAR, MONTH, DAY, HOUR, MINUTE, SECOND, DATEDIFF, DATEADD, DATEPART), CASE expressions, arithmetic operations, and reversed comparisons. The filter encoder will use a "correctness first" strategy: push down supported filters to SQL Server, and re-apply all filters in DuckDB whenever any expression cannot be fully pushed down.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB main branch (extension API, catalog API, DataChunk), existing TDS layer (specs 001-012), OpenSSL (via vcpkg)
**Storage**: In-memory (no intermediate buffering per Streaming First principle)
**Testing**: DuckDB extension test framework (`make test`), integration tests requiring SQL Server
**Target Platform**: Linux (x64, arm64), macOS (x64, arm64), Windows (x64)
**Project Type**: Single project - DuckDB loadable extension
**Performance Goals**: Filter pushdown should not add measurable latency to query planning; generated T-SQL should execute efficiently on SQL Server
**Constraints**: Memory bounded by DuckDB chunk size; no full result buffering
**Scale/Scope**: Refactoring ~600 lines from mssql_functions.cpp; adding ~900-1100 lines for enhanced filter encoder (including date/time function support)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Evidence |
|-----------|--------|----------|
| I. Native and Open | PASS | No external drivers used; native TDS implementation preserved |
| II. Streaming First | PASS | Filter pushdown reduces data transfer; no result buffering added |
| III. Correctness over Convenience | PASS | FR-037 mandates no incorrect results; unsupported expressions fail explicitly |
| IV. Explicit State Machines | PASS | Filter encoder has explicit supported/unsupported states; re-filter flag is explicit |
| V. DuckDB-Native UX | PASS | Filter pushdown is transparent to users; standard DuckDB query syntax |
| VI. Incremental Delivery | PASS | Refactoring maintains existing functionality; new filter types added incrementally |

**Gate Result**: PASS - No violations. Proceed to Phase 0.

## Project Structure

### Documentation (this feature)

```text
specs/013-table-scan-filter-refactor/
├── plan.md              # This file
├── spec.md              # Feature specification
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (internal APIs)
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
src/
├── table_scan/                    # NEW: Refactored table scan module
│   ├── mssql_table_scan.hpp       # Public interface for catalog integration
│   ├── mssql_table_scan.cpp       # Scan function registration
│   ├── table_scan_bind.hpp        # Bind data structures
│   ├── table_scan_bind.cpp        # Bind function implementation
│   ├── table_scan_state.hpp       # Global/local state structures
│   ├── table_scan_state.cpp       # State management
│   ├── table_scan_execute.cpp     # Scan execution (FillChunk)
│   ├── filter_encoder.hpp         # Filter encoder interface
│   ├── filter_encoder.cpp         # Filter → T-SQL conversion
│   └── function_mapping.hpp       # DuckDB → SQL Server function map
├── include/table_scan/            # Public headers (if needed)
├── catalog/
│   └── mssql_table_entry.cpp      # Updated to use new table_scan module
├── mssql_functions.cpp            # MODIFIED: Remove table scan code, keep mssql_scan()
└── pushdown/                      # REMOVED: Unused placeholder files

tests/
├── cpp/                           # Unit tests
│   └── filter_encoder_test.cpp    # NEW: Filter encoder unit tests
└── integration/                   # Integration tests (require SQL Server)
    └── filter_pushdown_test.cpp   # NEW/MODIFIED: Filter pushdown tests
```

**Structure Decision**: Single project structure. New `src/table_scan/` module contains all refactored code. Existing `mssql_functions.cpp` retains the ad-hoc `mssql_scan()` function but delegates catalog scan to the new module.

## Complexity Tracking

> No constitution violations requiring justification.

## Post-Design Constitution Re-Check

*Re-evaluated after Phase 1 design completion.*

| Principle | Status | Post-Design Evidence |
|-----------|--------|---------------------|
| I. Native and Open | PASS | FilterEncoder uses only standard C++; no external libraries |
| II. Streaming First | PASS | Filter pushdown reduces transferred data; no buffering in encoder |
| III. Correctness over Convenience | PASS | `needs_duckdb_filter` flag ensures DuckDB re-applies filters when needed |
| IV. Explicit State Machines | PASS | Filter encoding states documented in data-model.md state diagram |
| V. DuckDB-Native UX | PASS | Users write standard DuckDB SQL; pushdown is transparent |
| VI. Incremental Delivery | PASS | Refactoring preserves existing filters; new types added incrementally |

**Post-Design Gate Result**: PASS - Design complies with all constitution principles.

## Generated Artifacts

| Artifact | Path | Description |
|----------|------|-------------|
| Research | [research.md](./research.md) | DuckDB filter API analysis, function mappings, design decisions |
| Data Model | [data-model.md](./data-model.md) | C++ structures, state transitions, validation rules |
| Filter Encoder Contract | [contracts/filter_encoder.hpp](./contracts/filter_encoder.hpp) | FilterEncoder class interface |
| Table Scan Contract | [contracts/table_scan.hpp](./contracts/table_scan.hpp) | Table scan function interfaces |
| Quickstart | [quickstart.md](./quickstart.md) | Usage examples and migration guide |

## Next Steps

Run `/speckit.tasks` to generate implementation tasks from this plan.

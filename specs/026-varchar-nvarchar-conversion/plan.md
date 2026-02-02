# Implementation Plan: VARCHAR to NVARCHAR Conversion

**Branch**: `026-varchar-nvarchar-conversion` | **Date**: 2026-02-02 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/026-varchar-nvarchar-conversion/spec.md`

## Summary

Convert non-UTF8 VARCHAR/CHAR columns to NVARCHAR in generated table scan queries by wrapping them in `CAST(column AS NVARCHAR(n))`. This leverages SQL Server's server-side encoding conversion to ensure data arrives as UTF-16LE, which the extension already decodes correctly.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB (main branch), existing TDS protocol layer, OpenSSL (vcpkg)
**Storage**: SQL Server 2019+ (remote), in-memory (result streaming, connection pool state)
**Testing**: SQLLogicTest (integration tests requiring SQL Server), C++ unit tests
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW)
**Project Type**: Single project (DuckDB extension)
**Performance Goals**: No measurable regression in existing test suite; rely on SQL Server CAST efficiency
**Constraints**: Silent truncation for VARCHAR >4000 chars; documented limitation
**Scale/Scope**: All table scan queries with VARCHAR/CHAR columns

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | No external libraries; uses existing TDS protocol |
| II. Streaming First | PASS | No buffering change; query modification only |
| III. Correctness over Convenience | PASS | Fixes silent data corruption; truncation is documented |
| IV. Explicit State Machines | N/A | No state machine changes |
| V. DuckDB-Native UX | PASS | Transparent to users; table scans work correctly |
| VI. Incremental Delivery | PASS | Read-path fix only; no write-path changes |

**Gate Result**: PASS - All applicable principles satisfied.

## Project Structure

### Documentation (this feature)

```text
specs/026-varchar-nvarchar-conversion/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
└── tasks.md             # Phase 2 output (via /speckit.tasks)
```

### Source Code (repository root)

```text
src/
├── include/
│   ├── catalog/
│   │   └── mssql_column_info.hpp    # Has IsUTF8Collation() - reuse
│   ├── mssql_functions.hpp          # MSSQLCatalogScanBindData - add mssql_columns
│   └── table_scan/
│       └── table_scan.hpp           # Interface (unchanged)
├── catalog/
│   └── mssql_column_info.cpp        # Has IsUTF8Collation() - reuse
└── table_scan/
    └── table_scan.cpp               # Modify query generation (lines 134-208)

test/
├── sql/
│   └── catalog/
│       └── varchar_encoding.test    # New integration test
└── cpp/                             # Existing unit tests (unchanged)
```

**Structure Decision**: Single project structure. Changes are localized to table_scan.cpp query generation and minimal additions to bind data. No new directories needed.

## Complexity Tracking

No constitution violations to justify.

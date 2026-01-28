# Implementation Plan: CTAS for MSSQL

**Branch**: `022-mssql-ctas` | **Date**: 2026-01-28 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/022-mssql-ctas/spec.md`

## Summary

Implement CREATE TABLE AS SELECT (CTAS) for MSSQL-attached databases using a two-phase approach: (1) translate DuckDB output schema to SQL Server DDL and execute CREATE TABLE remotely, (2) insert the query result set using the existing batched bulk insert path (Mode A). Supports `OR REPLACE` modifier and emits debug-level observability logs.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (vcpkg), existing TDS protocol layer
**Storage**: SQL Server 2019+ (remote), in-memory (result streaming, connection pool state)
**Testing**: SQLLogicTest (integration, requires SQL Server), C++ unit tests (no SQL Server)
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW/Rtools 4.2)
**Project Type**: DuckDB extension (single project)
**Performance Goals**: 1M rows CTAS with stable memory usage, batched insert throughput
**Constraints**: Memory bounded by chunk size (not result size), existing batch size limits apply
**Scale/Scope**: Single feature addition to existing extension (~500-800 LOC new code)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | Uses existing native TDS implementation, no external drivers |
| II. Streaming First | PASS | Source query results streamed through DuckDB pipelines, batched insert prevents full buffering |
| III. Correctness over Convenience | PASS | Fails explicitly on unsupported types, schema validation errors, existing table conflicts |
| IV. Explicit State Machines | PASS | Uses existing connection state machine, transaction pinning patterns |
| V. DuckDB-Native UX | PASS | CTAS exposed through standard DuckDB catalog DDL syntax |
| VI. Incremental Delivery | PASS | CTAS adds write capability progressively; leverages existing DDL and INSERT paths |

**Post-Design Re-check**: All principles remain satisfied. OR REPLACE uses explicit DROP + CREATE sequence with documented non-atomic semantics (III). Debug logging integrates with existing MSSQL_DEBUG infrastructure (IV).

## Project Structure

### Documentation (this feature)

```text
specs/022-mssql-ctas/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (DDL contracts)
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
src/
├── catalog/
│   ├── mssql_catalog.cpp           # MODIFY: Add PlanCreateTableAs() override
│   ├── mssql_ddl_translator.hpp    # MODIFY: Add CTAS type mapping helpers
│   └── mssql_ddl_translator.cpp    # MODIFY: Add TranslateCreateTableFromSchema()
├── dml/
│   └── ctas/                       # NEW: CTAS-specific implementation
│       ├── mssql_ctas_planner.hpp  # CTAS planning logic
│       ├── mssql_ctas_planner.cpp
│       ├── mssql_ctas_executor.hpp # Two-phase execution (DDL + INSERT)
│       └── mssql_ctas_executor.cpp
├── connection/
│   └── mssql_settings.cpp          # MODIFY: Add CTAS settings
└── include/
    ├── catalog/
    │   └── mssql_ddl_translator.hpp
    ├── dml/
    │   └── ctas/
    │       ├── mssql_ctas_planner.hpp
    │       └── mssql_ctas_executor.hpp
    └── connection/
        └── mssql_settings.hpp

test/
├── sql/
│   └── ctas/                       # NEW: CTAS integration tests
│       ├── ctas_basic.test
│       ├── ctas_types.test
│       ├── ctas_large.test
│       ├── ctas_or_replace.test
│       ├── ctas_failure.test
│       └── ctas_transaction.test
└── cpp/
    └── test_ctas_type_mapping.cpp  # NEW: Unit tests for type mapping
```

**Structure Decision**: Follows existing DuckDB extension layout. CTAS code placed in `src/dml/ctas/` parallel to existing `src/dml/insert/`, `src/dml/update/`, `src/dml/delete/` directories. Integration tests in `test/sql/ctas/` following existing test organization.

## Complexity Tracking

> No constitution violations requiring justification.

| Concern | Resolution |
|---------|------------|
| Two-phase non-atomic semantics | Documented in spec (FR-020). Matches SQL Server DDL auto-commit behavior. |
| OR REPLACE data loss on CREATE failure | Documented non-atomic behavior. `mssql_ctas_drop_on_failure` only applies to insert phase, not OR REPLACE DROP. |

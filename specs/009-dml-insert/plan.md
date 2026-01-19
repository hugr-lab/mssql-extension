# Implementation Plan: High-Performance DML INSERT

**Branch**: `009-dml-insert` | **Date**: 2026-01-19 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/009-dml-insert/spec.md`

## Summary

Implement SQL Batch-based INSERT from DuckDB into SQL Server with two modes: bulk insert (maximum throughput) and insert with RETURNING (via OUTPUT INSERTED). The implementation uses batched SQL text generation with configurable limits (2000 rows, 8MB default), T-SQL literal encoding for all DuckDB types, and Unicode literals (N'...') for server-side collation handling.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB main branch (catalog API, DataChunk), existing TDS layer (specs 001-007), mbedTLS 3.6.4 (via vcpkg for loadable)
**Storage**: In-memory (no intermediate buffering per Streaming First principle)
**Testing**: Catch2 (C++ unit tests), DuckDB integration tests
**Target Platform**: Cross-platform (Linux, macOS, Windows) via DuckDB extension system
**Project Type**: Single project (DuckDB extension)
**Performance Goals**: 10M+ rows insertable without memory exhaustion; streaming batches to minimize memory footprint
**Constraints**: Max 8MB SQL statement size (configurable), max 2000 rows per statement (configurable), statement-level atomicity
**Scale/Scope**: High-volume ETL workloads (10M+ rows per operation)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Compliance | Notes |
|-----------|------------|-------|
| I. Native and Open | PASS | Uses existing native TDS implementation; no ODBC/JDBC |
| II. Streaming First | PASS | Batched execution without full result buffering; memory bounded by batch size |
| III. Correctness over Convenience | PASS | Statement-level atomicity; explicit errors on failures; no unstable identifiers |
| IV. Explicit State Machines | PASS | Connection state machine already exists; INSERT uses existing Idle→Executing flow |
| V. DuckDB-Native UX | PASS | INSERT via catalog write hooks; standard DuckDB INSERT syntax |
| VI. Incremental Delivery | PASS | Read-only implemented first (specs 001-008); writes added progressively |

**Row Identity Model Compliance**: INSERT does not use rowid; creates new rows with server-generated identity values. RETURNING uses OUTPUT INSERTED which returns actual column values, not physical locators.

## Project Structure

### Documentation (this feature)

```text
specs/009-dml-insert/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
src/
├── insert/                          # NEW: Insert implementation
│   ├── mssql_insert_executor.cpp/hpp    # Main insert orchestration
│   ├── mssql_insert_statement.cpp/hpp   # SQL statement generation
│   ├── mssql_value_serializer.cpp/hpp   # DuckDB value → T-SQL literal
│   ├── mssql_batch_builder.cpp/hpp      # Row batching logic
│   └── mssql_returning_parser.cpp/hpp   # OUTPUT INSERTED result parsing
├── catalog/
│   ├── mssql_catalog.cpp/hpp            # MODIFY: Implement PlanInsert()
│   └── mssql_transaction.cpp/hpp        # MODIFY: Enable write transactions
├── connection/
│   └── mssql_settings.cpp/hpp           # MODIFY: Add insert settings
└── tds/
    └── encoding/
        └── type_converter.cpp/hpp       # EXTEND: Add reverse conversion helpers

tests/
├── cpp/
│   ├── test_insert_executor.cpp         # NEW: Insert unit tests
│   ├── test_value_serializer.cpp        # NEW: Value encoding tests
│   └── test_batch_builder.cpp           # NEW: Batching tests
└── integration/
    └── test_insert_integration.cpp      # NEW: End-to-end insert tests
```

**Structure Decision**: Single project structure following existing extension layout. New `src/insert/` module for INSERT-specific code; extends existing catalog and connection modules.

## Complexity Tracking

No constitution violations requiring justification. Design follows established patterns from SELECT implementation.

## Post-Design Constitution Re-Check

*Verified after Phase 1 design completion.*

| Principle | Post-Design Status | Design Evidence |
|-----------|-------------------|-----------------|
| I. Native and Open | PASS | SQL Batch via TDS; no external drivers |
| II. Streaming First | PASS | `MSSQLBatchBuilder` processes chunks without buffering; memory bounded by `max_sql_bytes` |
| III. Correctness over Convenience | PASS | `InsertError` with row offsets; explicit identity column rejection; NaN/Inf rejection |
| IV. Explicit State Machines | PASS | `InsertBatch.State` enum with documented transitions |
| V. DuckDB-Native UX | PASS | `MSSQLCatalog::PlanInsert()` enables standard SQL syntax |
| VI. Incremental Delivery | PASS | INSERT builds on specs 001-008; future BCP spec deferred |

**Row Identity Model**: Verified. INSERT creates new rows; RETURNING uses `OUTPUT INSERTED` for actual values, not physical locators.

## Generated Artifacts

- `research.md` - 10 design decisions documented
- `data-model.md` - 8 entities defined with validation rules
- `contracts/insert-api.md` - 5 class interfaces specified
- `quickstart.md` - Usage examples and configuration guide

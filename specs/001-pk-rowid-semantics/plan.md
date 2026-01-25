# Implementation Plan: MSSQL rowid Semantics (PK-based Row Identity)

**Branch**: `001-pk-rowid-semantics` | **Date**: 2026-01-25 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/001-pk-rowid-semantics/spec.md`

## Summary

Implement `rowid` pseudo-column support for MSSQL tables by mapping DuckDB's row identity semantics to SQL Server primary keys. This enables DuckDB to reference specific rows for future UPDATE/DELETE operations (covered in spec 05.04b).

Key approach:
- Discover PK metadata from SQL Server system views (`sys.key_constraints`, `sys.index_columns`)
- Cache PK info in `MSSQLTableEntry` with lazy loading
- Expose `rowid` as a virtual column in scan projections
- Map scalar PKs directly to rowid type; composite PKs to DuckDB STRUCT

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB main branch (catalog API, DataChunk), existing TDS layer (specs 001-012)
**Storage**: In-memory (PK metadata cache per table entry)
**Testing**: DuckDB SQLLogicTest framework (`make test`, `make integration-test`)
**Target Platform**: Linux (primary), macOS, Windows (MSVC/MinGW)
**Project Type**: Single (DuckDB extension)
**Performance Goals**: PK discovery same latency as existing metadata queries; no rowid overhead when not projected
**Constraints**: PK-based identity only (no %%physloc%%); existing cache invalidation mechanism

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Evidence |
|-----------|--------|----------|
| I. Native and Open | ✅ PASS | Uses native TDS protocol, no ODBC/JDBC |
| II. Streaming First | ✅ PASS | Rowid computed during streaming, no extra buffering |
| III. Correctness over Convenience | ✅ PASS | PK-based identity only; %%physloc%% explicitly forbidden; clear errors for no-PK tables |
| IV. Explicit State Machines | ✅ PASS | PK cache has explicit states (not_loaded, loaded); integrates with existing cache mechanism |
| V. DuckDB-Native UX | ✅ PASS | rowid exposed as standard DuckDB pseudo-column via TableCatalogEntry |
| VI. Incremental Delivery | ✅ PASS | This spec (rowid read) is independent from 05.04b (UPDATE/DELETE) |

**Row Identity Model Compliance**: ✅ Directly implements the constitution's Row Identity Model:
- DuckDB `rowid` maps to primary key values
- Single-column PK → scalar `rowid`
- Composite PK → `STRUCT` rowid
- Tables without PK do NOT support rowid (and by extension, catalog-based UPDATE/DELETE)

## Project Structure

### Documentation (this feature)

```text
specs/001-pk-rowid-semantics/
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
│   │   ├── mssql_table_entry.hpp      # MODIFY: Add PrimaryKeyInfo cache
│   │   ├── mssql_primary_key.hpp      # NEW: PK metadata structures
│   │   └── mssql_metadata_cache.hpp   # MODIFY: Add PK discovery query
│   └── table_scan/
│       └── mssql_table_scan.hpp       # NO CHANGE (interface)
├── catalog/
│   ├── mssql_table_entry.cpp          # MODIFY: Lazy PK loading, rowid type
│   ├── mssql_primary_key.cpp          # NEW: PK discovery implementation
│   └── mssql_metadata_cache.cpp       # MODIFY: Add LoadPrimaryKey method
└── table_scan/
    └── mssql_table_scan.cpp           # MODIFY: Handle COLUMN_IDENTIFIER_ROW_ID

test/
├── sql/
│   └── rowid/                         # NEW: rowid test directory
│       ├── scalar_pk_rowid.test       # Scalar PK tests
│       ├── composite_pk_rowid.test    # Composite PK tests
│       ├── no_pk_rowid.test           # Tables without PK
│       └── view_rowid.test            # View error handling
└── integration/
    └── rowid_integration.test         # End-to-end integration tests

docker/init/
└── init.sql                           # MODIFY: Add test tables for rowid
```

**Structure Decision**: Extends existing catalog module. New `mssql_primary_key.hpp/cpp` for PK-specific logic keeps concerns separated.

## Constitution Re-Check (Post-Design)

*Phase 1 design complete. Re-validating against constitution principles.*

| Principle | Status | Post-Design Evidence |
|-----------|--------|---------------------|
| I. Native and Open | ✅ PASS | PK discovery uses native TDS queries to sys.* views |
| II. Streaming First | ✅ PASS | Rowid constructed in FillChunk during streaming; no intermediate storage |
| III. Correctness over Convenience | ✅ PASS | Design rejects rowid for no-PK tables and views; explicit error messages |
| IV. Explicit State Machines | ✅ PASS | PrimaryKeyInfo has `loaded` and `exists` flags; clear state transitions |
| V. DuckDB-Native UX | ✅ PASS | rowid exposed via standard `COLUMN_IDENTIFIER_ROW_ID` mechanism |
| VI. Incremental Delivery | ✅ PASS | rowid read-only (this spec) independent from DML (spec 05.04b) |

**Row Identity Model**: Design fully implements constitution's Row Identity Model section.

## Complexity Tracking

> No violations - design aligns with constitution principles.

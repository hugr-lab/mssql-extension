# Implementation Plan: Catalog-Driven DDL and Statistics

**Branch**: `008-catalog-ddl-statistics` | **Date**: 2026-01-18 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/008-catalog-ddl-statistics/spec.md`

## Summary

Implement catalog-driven DDL for the DuckDB MSSQL extension by translating DuckDB catalog DDL operations (CREATE/DROP SCHEMA, CREATE/DROP/RENAME TABLE, ADD/RENAME/DROP/ALTER COLUMN) into remote T-SQL and executing them on SQL Server. Additionally, implement SQL Server-backed statistics (row count from DMVs, optional column stats via DBCC) for DuckDB's optimizer, proper table/view distinction in the catalog, and READ_ONLY attach mode for production safety.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB main branch (extension API, catalog API, DataChunk), existing TDS layer (specs 001-007), mbedTLS 3.6.4 (via vcpkg for loadable)
**Storage**: In-memory (metadata cache with TTL), DuckDB secret manager for credentials
**Testing**: DuckDB test framework (`.test` files in `/test/sql/`), C++ unit tests (Catch2 via DuckDB)
**Target Platform**: Linux, macOS, Windows (DuckDB-supported platforms)
**Project Type**: Single project (DuckDB storage extension)
**Performance Goals**: DDL operations < 5 seconds, statistics cache hit < 100ms latency
**Constraints**: No ODBC/JDBC/FreeTDS, streaming-first (no full result buffering), PK-based row identity for future writes
**Scale/Scope**: SQL Server 2019+, UTF-8 collations, tables up to millions of rows

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Evidence |
|-----------|--------|----------|
| I. Native and Open | ✅ PASS | Uses existing native TDS implementation, no ODBC/JDBC/FreeTDS |
| II. Streaming First | ✅ PASS | Statistics queries return scalar values, DDL has no result streaming |
| III. Correctness over Convenience | ✅ PASS | DDL fails with explicit SQL Server errors, no silent corruption |
| IV. Explicit State Machines | ✅ PASS | Extends existing TdsConnection state machine, cache state machine |
| V. DuckDB-Native UX | ✅ PASS | DDL via DuckDB catalog commands, not raw SQL parsing |
| VI. Incremental Delivery | ✅ PASS | Read-only already works, this adds DDL incrementally |

**Row Identity Model**: Not directly applicable to DDL; statistics are table-level, not row-level.

## Project Structure

### Documentation (this feature)

```text
specs/008-catalog-ddl-statistics/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (internal C++ APIs)
└── tasks.md             # Phase 2 output (/speckit.tasks)
```

### Source Code (repository root)

```text
src/
├── include/
│   ├── catalog/
│   │   ├── mssql_catalog.hpp        # Add DDL hook methods, read_only flag
│   │   ├── mssql_schema_entry.hpp   # Add CreateTable, DropEntry methods
│   │   ├── mssql_table_entry.hpp    # Add GetStatistics, view flag
│   │   ├── mssql_view_entry.hpp     # NEW: View catalog entry
│   │   ├── mssql_statistics.hpp     # NEW: Statistics provider
│   │   └── mssql_ddl_translator.hpp # NEW: DDL to T-SQL translation
│   ├── connection/
│   │   └── mssql_settings.hpp       # Add statistics settings
│   └── mssql_functions.hpp          # Add mssql_exec function
├── catalog/
│   ├── mssql_catalog.cpp            # DDL hook implementations
│   ├── mssql_schema_entry.cpp       # CreateTable, DropEntry
│   ├── mssql_table_entry.cpp        # GetStatistics implementation
│   ├── mssql_view_entry.cpp         # NEW: View entry implementation
│   ├── mssql_statistics.cpp         # NEW: Statistics fetching/caching
│   └── mssql_ddl_translator.cpp     # NEW: DDL translation logic
├── connection/
│   └── mssql_settings.cpp           # Statistics settings registration
└── mssql_functions.cpp              # mssql_exec implementation

test/
├── sql/
│   ├── catalog/
│   │   ├── ddl_schema.test          # NEW: Schema DDL tests
│   │   ├── ddl_table.test           # NEW: Table DDL tests
│   │   ├── ddl_column.test          # NEW: Column DDL tests
│   │   ├── statistics.test          # NEW: Statistics tests
│   │   ├── view_distinction.test    # NEW: Table/view tests
│   │   └── read_only.test           # NEW: READ_ONLY mode tests
│   └── mssql_exec.test              # NEW: mssql_exec function tests
└── cpp/
    └── test_ddl_translator.cpp      # NEW: Unit tests for DDL translation
```

**Structure Decision**: Extends existing single-project structure. New files are added to existing directories following established patterns. DDL translation is a new module (`mssql_ddl_translator`) that generates T-SQL from DuckDB catalog objects.

## Complexity Tracking

No violations to justify. Implementation follows existing patterns:
- DDL hooks follow DuckDB's Catalog API patterns
- Statistics caching extends existing MSSQLMetadataCache pattern
- Type mapping uses existing type converter

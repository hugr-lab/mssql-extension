# Implementation Plan: BCP Improvements

**Branch**: `025-bcp-improvements` | **Date**: 2026-01-30 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/025-bcp-improvements/spec.md`

## Summary

This feature adds four improvements to the existing COPY TO functionality:
1. Support empty schema syntax for temp tables (`mssql://db//#temp`, `db..#temp`)
2. Fix connection leaks on COPY errors
3. Improve column type mismatch error messages for existing tables
4. Add optional INSERT method as alternative to BCP protocol

All changes extend the existing `src/copy/` module without architectural changes.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (via vcpkg for TLS)
**Storage**: SQL Server 2019+ (remote target), in-memory (connection pool state)
**Testing**: SQLLogicTest (integration), C++ unit tests (no SQL Server required)
**Target Platform**: Linux x86_64, macOS ARM64, Windows x64
**Project Type**: Single DuckDB extension
**Performance Goals**: BCP: ~300K rows/s, INSERT: uses existing INSERT batching
**Constraints**: Connection pool must not leak; bounded memory for streaming
**Scale/Scope**: Feature extends existing COPY functionality, ~4 files modified

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | ✅ PASS | All changes use existing native TDS implementation |
| II. Streaming First | ✅ PASS | BCP streaming unchanged; INSERT uses existing batched approach |
| III. Correctness over Convenience | ✅ PASS | Clear error messages for type mismatches; no silent failures |
| IV. Explicit State Machines | ✅ PASS | Connection cleanup follows existing state machine |
| V. DuckDB-Native UX | ✅ PASS | COPY syntax unchanged; new options follow DuckDB conventions |
| VI. Incremental Delivery | ✅ PASS | Each user story independently testable and deployable |

**Gate Result**: PASS - No violations

## Project Structure

### Documentation (this feature)

```text
specs/025-bcp-improvements/
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
├── copy/
│   ├── bcp_config.cpp        # Config parsing (add METHOD option)
│   ├── bcp_writer.cpp        # BCP protocol writer (unchanged)
│   ├── copy_function.cpp     # Main COPY callbacks (error handling, INSERT method)
│   └── target_resolver.cpp   # URL/catalog parsing (empty schema support)
├── include/copy/
│   ├── bcp_config.hpp        # Config struct (add method field)
│   ├── copy_function.hpp     # Function declarations
│   └── target_resolver.hpp   # Target struct (unchanged)
├── dml/insert/               # Existing INSERT infrastructure (reused)
│   ├── mssql_insert_executor.cpp
│   └── mssql_batch_builder.cpp

test/
├── sql/copy/
│   ├── copy_empty_schema.test    # New: empty schema syntax tests
│   ├── copy_connection_leak.test # New: connection leak tests
│   ├── copy_type_mismatch.test   # New: type mismatch error tests
│   └── copy_insert_method.test   # New: INSERT method tests
└── cpp/
    └── test_target_resolver.cpp  # Extended: empty schema parsing
```

**Structure Decision**: Extends existing `src/copy/` module. No new directories needed. All changes are additive to existing files.

## Complexity Tracking

> No violations to justify - all changes follow existing patterns.

## Post-Design Constitution Re-Check

*Re-evaluated after Phase 1 design artifacts completed.*

| Principle | Status | Design Verification |
|-----------|--------|---------------------|
| I. Native and Open | ✅ PASS | INSERT method uses existing TDS-based INSERT, not ODBC |
| II. Streaming First | ✅ PASS | BCP unchanged; INSERT batches bounded by `mssql_insert_batch_size` |
| III. Correctness over Convenience | ✅ PASS | Type mismatch errors explicit with column name and types |
| IV. Explicit State Machines | ✅ PASS | Connection lifecycle documented in data-model.md |
| V. DuckDB-Native UX | ✅ PASS | METHOD option follows DuckDB COPY option patterns |
| VI. Incremental Delivery | ✅ PASS | 4 independent user stories, each testable separately |

**Post-Design Gate Result**: PASS - Design validated against constitution

# Implementation Plan: Incremental Catalog Cache with TTL and Point Invalidation

**Branch**: `029-incremental-catalog-cache` | **Date**: 2026-02-05 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/029-incremental-catalog-cache/spec.md`

## Summary

Transform the MSSQL extension's catalog cache from eager full-refresh to incremental lazy loading with granular TTL. Currently, `EnsureCacheLoaded()` triggers `Refresh()` which queries ALL schemas, ALL tables, and ALL columns upfront. This feature enables:

1. **Lazy Loading**: Load schema list only when first schema accessed; load table list per-schema on first access; load columns per-table on first query/bind
2. **Granular TTL**: Separate TTL for schema metadata (table lists) and table metadata (columns)
3. **Point Invalidation**: DDL via DuckDB Catalog (CREATE/DROP/ALTER TABLE, CREATE/DROP SCHEMA) invalidates only affected cache entries, not entire cache

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (vcpkg), existing TDS protocol layer
**Storage**: In-memory (metadata cache in `MSSQLMetadataCache` class)
**Testing**: SQLLogicTest (`test/sql/`), C++ unit tests (`test/cpp/`), Azure SQL via `AZURE_SQL_TEST_DSN`
**Target Platform**: Linux, macOS, Windows (matches DuckDB platforms)
**Project Type**: Single project (DuckDB extension)
**Performance Goals**: ATTACH with 500+ tables completes with 0 `sys.columns` queries; first table query loads only that table's columns
**Constraints**: Thread-safe (existing mutex patterns), uses existing `mssql_catalog_cache_ttl` setting, backward compatible with `mssql_refresh_cache()`
**Scale/Scope**: Large databases with 500+ tables benefit most; memory scales with accessed tables, not total tables

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | ✅ PASS | Uses existing TDS implementation, no new dependencies |
| II. Streaming First | ✅ PASS | Metadata queries already stream via `MSSQLSimpleQuery::ExecuteWithCallback` |
| III. Correctness over Convenience | ✅ PASS | Lazy loading is additive; no change to PK-based rowid semantics |
| IV. Explicit State Machines | ✅ PASS | Extends `MSSQLCacheState` with per-schema/table loading states |
| V. DuckDB-Native UX | ✅ PASS | Transparent to users; catalog queries work identically |
| VI. Incremental Delivery | ✅ PASS | P1 lazy loading → P2 TTL/invalidation → P3 schema DDL |

## Project Structure

### Documentation (this feature)

```text
specs/029-incremental-catalog-cache/
├── plan.md              # This file
├── research.md          # Phase 0: Architecture analysis and approach
├── data-model.md        # Phase 1: Entity definitions and state machines
├── quickstart.md        # Phase 1: Implementation guide
├── contracts/           # Phase 1: API contracts
└── tasks.md             # Phase 2: Implementation tasks (via /speckit.tasks)
```

### Source Code (repository root)

```text
src/
├── catalog/
│   ├── mssql_metadata_cache.cpp    # PRIMARY: Lazy loading logic, per-level TTL
│   ├── mssql_catalog.cpp           # EnsureCacheLoaded integration, DDL hooks
│   ├── mssql_schema_entry.cpp      # Point invalidation on DDL
│   └── mssql_table_set.cpp         # Column lazy loading integration
├── include/catalog/
│   ├── mssql_metadata_cache.hpp    # Extended state machine, new structs
│   ├── mssql_catalog.hpp           # New TTL settings accessors
│   └── mssql_schema_metadata.hpp   # NEW: Per-schema metadata with load state
├── connection/
│   └── mssql_settings.cpp          # Uses existing mssql_catalog_cache_ttl

test/
├── sql/
│   ├── catalog/
│   │   ├── lazy_loading.test       # NEW: Lazy loading behavior tests
│   │   └── incremental_ttl.test    # NEW: Per-level TTL tests
│   └── azure/
│       └── azure_lazy_loading.test # NEW: Azure SQL specific tests
└── cpp/
    └── test_incremental_cache.cpp  # NEW: Unit tests for cache state machine
```

**Structure Decision**: Single project structure following existing extension layout. New files for schema-level metadata struct and Azure-specific tests. Existing files modified to add lazy loading and point invalidation.

## Complexity Tracking

No constitution violations. Implementation follows existing patterns:
- State machine extension follows `MSSQLCacheState` pattern
- Thread-safety uses existing mutex approach
- DDL hooks use existing `CreateTable`/`DropEntry` entry points

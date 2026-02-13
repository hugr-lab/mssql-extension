# Implementation Plan: DDL Schema Support

**Branch**: `035-ddl-schema-support` | **Date**: 2026-02-13 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/035-ddl-schema-support/spec.md`

## Summary

Add `CREATE SCHEMA IF NOT EXISTS` support by checking schema existence before executing DDL (issue #54). Also handle `DROP SCHEMA IF EXISTS` and verify cache invalidation after schema DDL operations. Follows the same `OnCreateConflict` pattern already used for `CREATE TABLE IF NOT EXISTS`.

## Technical Context

**Language/Version**: C++17 (C++11-compatible for ODR on Linux)
**Primary Dependencies**: DuckDB v1.5-variegata (extension API)
**Storage**: N/A (remote SQL Server via TDS protocol)
**Testing**: DuckDB SQLLogicTest (integration tests requiring SQL Server)
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC/MinGW)
**Project Type**: DuckDB C++ extension
**Performance Goals**: Schema existence check should add negligible overhead (single cache lookup or SQL query)
**Constraints**: Must use DuckDB's `OnCreateConflict` / `OnEntryNotFound` enums, not custom logic
**Scale/Scope**: 2 files modified, ~30 lines of code changes

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | Uses native TDS, no external drivers |
| II. Streaming First | N/A | DDL operations, no result streaming |
| III. Correctness over Convenience | PASS | Explicit existence check before DDL; error preserved for non-IF-NOT-EXISTS case |
| IV. Explicit State Machines | PASS | Cache state transitions (LOADED/NOT_LOADED) remain explicit |
| V. DuckDB-Native UX | PASS | `CREATE SCHEMA IF NOT EXISTS` is standard DuckDB SQL syntax |
| VI. Incremental Delivery | PASS | Small self-contained change, independently testable |

## Project Structure

### Documentation (this feature)

```text
specs/035-ddl-schema-support/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── spec.md              # Feature specification
├── checklists/
│   └── requirements.md  # Spec quality checklist
└── tasks.md             # Phase 2 output (from /speckit.tasks)
```

### Source Code (files to modify)

```text
src/
├── catalog/
│   └── mssql_catalog.cpp          # CreateSchema() and DropSchema() — add on_conflict/if_not_found handling
test/
└── sql/
    └── catalog/
        └── ddl_schema.test        # Integration tests for CREATE/DROP SCHEMA IF [NOT] EXISTS
```

**Structure Decision**: Minimal change — only `mssql_catalog.cpp` needs modification. The `CreateSchema` method gains an existence check via `on_conflict`, and `DropSchema` gains an existence check via `if_not_found`. No new files, no new classes.

## Implementation Approach

### CreateSchema (IF NOT EXISTS)

In `MSSQLCatalog::CreateSchema()`, before executing the DDL:

1. Check `info.on_conflict`
2. If `IGNORE_ON_CONFLICT` (IF NOT EXISTS):
   - Try to get the schema via `GetSchema()` which already handles cache + SQL Server lookup
   - If schema exists, return the existing schema entry without executing DDL
   - If schema doesn't exist, proceed with normal CREATE SCHEMA
3. If `ERROR_ON_CONFLICT` (default): preserve existing behavior (let SQL Server error)

### DropSchema (IF EXISTS)

In `MSSQLCatalog::DropSchema()`, before executing the DDL:

1. Check `info.if_not_found`
2. If `RETURN_NULL` (IF EXISTS):
   - Check if schema exists via cache or catalog lookup
   - If schema doesn't exist, return silently
   - If schema exists, proceed with normal DROP SCHEMA
3. If `THROW_EXCEPTION` (default): preserve existing behavior

### Cache Invalidation

The current `InvalidateAll()` calls after CREATE/DROP SCHEMA are already correct. Verify via integration tests that:
- After CREATE SCHEMA, the schema appears in catalog queries
- After DROP SCHEMA, the schema disappears from catalog queries
- After DROP + re-CREATE, the schema is visible again

## Complexity Tracking

No constitution violations. No complexity justifications needed.

# Specification Quality Checklist: Fix Partitioned Table Catalog

**Purpose**: Validate specification completeness and quality before proceeding to implementation
**Created**: 2026-05-19
**Feature**: [spec.md](../spec.md)

## Content Quality

- [x] No implementation details (languages, frameworks, APIs) in spec.md user stories — implementation details live in plan.md/contracts/.
- [x] Focused on user value (`SELECT *` works; row counts are accurate) and business need (unblocks log/event tables in DuckDB).
- [x] Written so a non-engineer can read US1-US3 and understand what's broken and why it matters.
- [x] All mandatory sections completed: User Scenarios & Testing, Requirements, Success Criteria.

## Requirement Completeness

- [x] No [NEEDS CLARIFICATION] markers remain in spec.md.
- [x] Requirements are testable and unambiguous (FR-001 through FR-012 each map to an SC or a test artifact).
- [x] Success criteria are measurable (SC-001 names the query that must succeed; SC-002 names the equality the row count must satisfy).
- [x] Success criteria are technology-agnostic to the extent meaningful (the spec is about a SQL Server interop fix, so SQL Server constructs are unavoidable; no DuckDB-internal API names appear in SC).
- [x] All three user stories have acceptance scenarios in Given/When/Then form.
- [x] Edge cases are identified (heap + partitions, empty table, view, columnstore, 0-row partition, concurrent split).
- [x] Scope is bounded: three SQL templates in one file + one test fixture + one test file. Anything else is out of scope.
- [x] Dependencies (SQL Server `sys.partitions`) and assumptions (`approx_row_count` is an estimate) are documented.

## Feature Readiness

- [x] Every functional requirement has a corresponding acceptance criterion or success criterion.
- [x] User scenarios cover the primary read path (US1), the metadata enumeration path (US2), and the bulk preload path (US3).
- [x] Success criteria match Measurable Outcomes in spec.md.
- [x] No implementation details leak into spec.md user stories or success criteria (SQL appears only in the "Key Entities" reference subsection, which is informational).

## Bug-fix Audit

- [x] Root cause identified and documented (per-partition row multiplication via un-aggregated `LEFT JOIN sys.partitions`).
- [x] All affected code paths enumerated (three SQL templates, one C++ caller `MakeTableInfo`).
- [x] Adjacent code paths confirmed unaffected (`src/copy/target_resolver.cpp` queries `sys.columns` without joining `sys.partitions` — verified during investigation).
- [x] Silent-correctness bug identified alongside the loud crash (row-count miscount in `TABLE_DISCOVERY_SQL_TEMPLATE`).
- [x] Backward-compatibility analysis: non-partitioned tables (single heap/clustered partition) MUST report bit-for-bit identical `approx_rows` after the fix.

## Test Plan Completeness

- [x] Integration test creates a partitioned table with ≥3 partitions and ≥2 columns (matches the reported reproducer shape).
- [x] Integration test asserts column metadata is non-duplicated AND row count is correctly aggregated.
- [x] Integration test exercises the bulk preload path (`mssql_preload_catalog`), not just single-table load.
- [x] Test fixture is isolated in its own init SQL file so failures don't cascade into the main test database.
- [x] Optional C++ unit-test guard is specified so a future refactor that drops aggregation fails fast.

## Notes

- All items pass validation.
- Spec is intentionally scoped to the bug fix — no opportunistic refactoring of the metadata cache (that path would belong in a separate refactoring spec, e.g., a hypothetical "050-metadata-query-builder").
- Issue #85 reporter (`@keen85`) reproduced the bug with a `DATETIME2(7)` partition key; the fix is partition-key-type-agnostic, so the test fixture uses `INT` for simplicity while the spec narrative references the user's `DATETIME2` example for context.

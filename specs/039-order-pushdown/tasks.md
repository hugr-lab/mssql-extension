# Tasks: ORDER BY Pushdown with TOP N

**Input**: Design documents from `/specs/039-order-pushdown/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US5)
- Include exact file paths in descriptions

---

## Phase 1: Setup

**Purpose**: Register OptimizerExtension infrastructure and add new source files

- [X] T001 Add `src/table_scan/mssql_optimizer.cpp` to EXTENSION_SOURCES list in `CMakeLists.txt`
- [X] T002 Create `src/include/table_scan/mssql_optimizer.hpp` with `MSSQLOptimizer` class declaration (static `Optimize()` method)
- [X] T003 Create `src/table_scan/mssql_optimizer.cpp` with empty `Optimize()` stub that iterates plan children
- [X] T004 Register `OptimizerExtension` with `optimize_function = MSSQLOptimizer::Optimize` in `src/mssql_extension.cpp` `LoadInternal()`
- [X] T005 Build and verify extension loads without errors (`GEN=ninja make`)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Setting, ATTACH option, and bind data fields that all user stories depend on

- [X] T006 [P] Add `mssql_order_pushdown` boolean setting (default `false`) in `src/connection/mssql_settings.cpp`
- [X] T007 [P] Add `order_pushdown` ATTACH option parsing in `src/mssql_storage.cpp` `MSSQLAttach()` (erase from options map, store in catalog attach info)
- [X] T008 Add `order_by_clause` (string) and `top_n` (int64_t, default 0) fields to `MSSQLCatalogScanBindData` in `src/include/mssql_functions.hpp`
- [X] T009 In `src/table_scan/table_scan.cpp` query assembly, append `ORDER BY` clause from `bind_data.order_by_clause` after WHERE clause; prepend `TOP N` to SELECT when `bind_data.top_n > 0`
- [X] T010 Build and verify all changes compile (`GEN=ninja make`)

**Checkpoint**: Infrastructure ready — ORDER BY pushdown logic can now be implemented

---

## Phase 3: User Story 5 - Configuration Control (Priority: P1)

**Goal**: Users can enable/disable ORDER BY pushdown via setting and ATTACH option

**Independent Test**: Toggle setting and ATTACH option, verify optimizer callback respects the effective value

### Implementation for User Story 5

- [X] T011 [US5] In `MSSQLOptimizer::Optimize()`, read effective pushdown config: check ATTACH option from `MSSQLCatalogScanBindData` (if set), otherwise read `mssql_order_pushdown` setting via `context.TryGetCurrentSetting()` in `src/table_scan/mssql_optimizer.cpp`
- [X] T012 [US5] Early-return from `Optimize()` when pushdown is disabled for the detected MSSQL scan
- [X] T013 [US5] Add integration test for configuration control in `test/sql/catalog/order_pushdown.test`: verify default (disabled) produces no pushdown, `SET mssql_order_pushdown = true` enables it, ATTACH option overrides setting

**Checkpoint**: Configuration control works — pushdown can be toggled on/off

---

## Phase 4: User Story 1 - Simple ORDER BY Pushdown (Priority: P1) MVP

**Goal**: Push ORDER BY with simple column references to SQL Server

**Independent Test**: Run `SELECT * FROM db.dbo.T ORDER BY col1 ASC` with pushdown enabled and verify results match non-pushdown ordering

### Implementation for User Story 1

- [X] T014 [US1] Implement pattern detection in `MSSQLOptimizer::Optimize()`: detect `LogicalOrder → LogicalGet` where `get.function.name == "mssql_catalog_scan"` in `src/table_scan/mssql_optimizer.cpp`
- [X] T015 [US1] Implement expression resolution: map `BoundReferenceExpression.index` to column name via `LogicalGet.GetColumnIds()` and `bind_data.all_column_names` in `src/table_scan/mssql_optimizer.cpp`
- [X] T016 [US1] Implement NULL order validation: check `BoundOrderByNode.null_order` vs SQL Server defaults (ASC=NULLS_FIRST, DESC=NULLS_LAST), skip column if mismatch and `mssql_columns[i].is_nullable == true` in `src/table_scan/mssql_optimizer.cpp`
- [X] T017 [US1] Build ORDER BY clause string from validated columns (e.g., `[col1] ASC, [col2] DESC`), store in `bind_data.order_by_clause` in `src/table_scan/mssql_optimizer.cpp`
- [X] T018 [US1] Implement plan modification: on full pushdown (all columns pushed), replace `LogicalOrder` with its child in the plan tree; on partial pushdown, keep `LogicalOrder` in `src/table_scan/mssql_optimizer.cpp`
- [X] T019 [US1] Add integration tests for simple ORDER BY pushdown in `test/sql/catalog/order_pushdown.test`: single column ASC/DESC, multi-column, verify result ordering matches expected

**Checkpoint**: Simple ORDER BY pushdown works end-to-end

---

## Phase 5: User Story 2 - Partial ORDER BY Pushdown (Priority: P2)

**Goal**: When some ORDER BY columns can't be pushed, send the pushable prefix for pre-sort benefit

**Independent Test**: Run `ORDER BY col1 ASC, (col1 + col2) DESC` and verify col1 is pushed, DuckDB still sorts, results are correct

### Implementation for User Story 2

- [X] T020 [US2] Ensure expression classification correctly rejects non-`BoundReferenceExpression` nodes (complex expressions, casts, nested functions) and marks them as non-pushable in `src/table_scan/mssql_optimizer.cpp`
- [X] T021 [US2] Ensure partial pushdown: when some columns are non-pushable, build ORDER BY from pushable prefix only and keep `LogicalOrder` in plan in `src/table_scan/mssql_optimizer.cpp`
- [X] T022 [US2] Add integration tests for partial pushdown in `test/sql/catalog/order_pushdown.test`: mixed pushable/non-pushable columns, verify result order is correct

**Checkpoint**: Partial pushdown works — pre-sort benefit without correctness loss

---

## Phase 6: User Story 3 - TOP N Pushdown (Priority: P2)

**Goal**: When ORDER BY + LIMIT are both fully pushable, generate `SELECT TOP N ... ORDER BY ...`

**Independent Test**: Run `SELECT * FROM db.dbo.T ORDER BY col1 LIMIT 10` and verify only 10 rows returned with correct ordering

### Implementation for User Story 3

- [X] T023 [US3] Detect `LogicalLimit → LogicalOrder → LogicalGet` pattern in `MSSQLOptimizer::Optimize()` in `src/table_scan/mssql_optimizer.cpp`
- [X] T024 [US3] Detect `LogicalTop → LogicalGet` pattern (DuckDB's merged ORDER BY + LIMIT) in `src/table_scan/mssql_optimizer.cpp`
- [X] T025 [US3] Extract constant LIMIT value from `LogicalLimit.limit_val` or `LogicalTop`, store in `bind_data.top_n` in `src/table_scan/mssql_optimizer.cpp`
- [X] T026 [US3] On full ORDER BY + LIMIT pushdown, remove both `LogicalOrder` and `LogicalLimit` (or `LogicalTop`) from plan in `src/table_scan/mssql_optimizer.cpp`
- [X] T027 [US3] Add integration tests for TOP N pushdown in `test/sql/catalog/order_pushdown.test`: ORDER BY + LIMIT, verify row count and ordering

**Checkpoint**: TOP N pushdown works — minimal data transfer from SQL Server

---

## Phase 7: User Story 4 - ORDER BY with Function Expressions (Priority: P3)

**Goal**: Push ORDER BY with simple function expressions like `YEAR(date_col)` to SQL Server

**Independent Test**: Run `SELECT * FROM db.dbo.T ORDER BY YEAR(date_col)` and verify function is translated and pushed

### Implementation for User Story 4

- [X] T028 [US4] In expression classification, detect `BoundFunctionExpression` nodes and check if function name exists in `GetFunctionMapping()` from `src/table_scan/function_mapping.cpp` in `src/table_scan/mssql_optimizer.cpp`
- [X] T029 [US4] For supported functions, build T-SQL fragment using function mapping template (e.g., `YEAR([col])`) and include in ORDER BY clause in `src/table_scan/mssql_optimizer.cpp`
- [X] T030 [US4] Add integration tests for function expression pushdown in `test/sql/catalog/order_pushdown.test`: ORDER BY YEAR(date_col), ORDER BY with unsupported function (verify fallback to partial pushdown)

**Checkpoint**: Function expression pushdown works for supported functions

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Validation, edge cases, and cleanup

- [X] T031 Add integration test for non-MSSQL table regression in `test/sql/catalog/order_pushdown.test`: verify ORDER BY on DuckDB-local table is unaffected
- [X] T032 Add integration test for NULL ordering edge cases in `test/sql/catalog/order_pushdown.test`: nullable column with non-default null order (should skip pushdown), NOT NULL column (should always push)
- [X] T033 Build release and run full test suite (`GEN=ninja make && GEN=ninja make test`)
- [X] T034 Verify quickstart.md examples work end-to-end against test SQL Server

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion — BLOCKS all user stories
- **US5 Configuration (Phase 3)**: Depends on Foundational — should be done first (other stories need pushdown enabled)
- **US1 Simple ORDER BY (Phase 4)**: Depends on US5 (needs pushdown enabled to test)
- **US2 Partial Pushdown (Phase 5)**: Depends on US1 (extends expression classification)
- **US3 TOP N (Phase 6)**: Depends on US1 (extends pattern detection)
- **US4 Function Expressions (Phase 7)**: Depends on US1 (extends expression resolution)
- **Polish (Phase 8)**: Depends on all user stories

### Within Each User Story

- Implementation tasks are sequential (each builds on previous)
- Integration test is last task in each story

### Parallel Opportunities

- T006 and T007 (setting + ATTACH option) can run in parallel
- US3 (TOP N) and US4 (Function Expressions) can run in parallel after US1 completes
- US2 (Partial) can run in parallel with US3/US4 after US1 completes

---

## Implementation Strategy

### MVP First (User Story 5 + User Story 1)

1. Complete Phase 1: Setup (T001-T005)
2. Complete Phase 2: Foundational (T006-T010)
3. Complete Phase 3: Configuration Control (T011-T013)
4. Complete Phase 4: Simple ORDER BY Pushdown (T014-T019)
5. **STOP and VALIDATE**: Test with `SET mssql_order_pushdown = true; SELECT * FROM db.dbo.T ORDER BY col1`
6. Build and run tests

### Incremental Delivery

1. MVP: Configuration + Simple ORDER BY → functional pushdown
2. Add Partial Pushdown → graceful degradation for complex queries
3. Add TOP N → optimized ORDER BY + LIMIT queries
4. Add Function Expressions → extended pushdown coverage
5. Polish → edge cases, regression tests, validation

---

## Notes

- All new code in `src/table_scan/mssql_optimizer.cpp` — single file for optimizer logic
- `GEN=ninja` required for all builds (user preference)
- Integration tests require SQL Server (`require-env MSSQL_TESTDB_DSN`)
- Use `MSSQL_DEBUG=1` to verify generated SQL includes ORDER BY / TOP N

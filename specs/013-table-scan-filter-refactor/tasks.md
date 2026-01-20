# Tasks: Table Scan and Filter Pushdown Refactoring

**Input**: Design documents from `/specs/013-table-scan-filter-refactor/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/

**Tests**: No test tasks included (tests not explicitly requested in specification)

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- **Single project**: `src/` at repository root
- Table scan module: `src/table_scan/`
- Catalog integration: `src/catalog/`
- Namespace: `duckdb::mssql` (no MSSQL prefix for types within this namespace)

---

## Phase 1: Setup (Directory Structure)

**Purpose**: Create module structure and prepare for code migration

- [x] T001 Create `src/table_scan/` directory for the new module
- [x] T002 [P] Create header file `src/table_scan/mssql_table_scan.hpp` with public interface declarations
- [x] T003 [P] Create header file `src/table_scan/table_scan_bind.hpp` with TableScanBindData struct declaration
- [x] T004 [P] Create header file `src/table_scan/table_scan_state.hpp` with TableScanGlobalState and TableScanLocalState struct declarations
- [x] T005 [P] Create header file `src/table_scan/filter_encoder.hpp` with FilterEncoder class declaration per contracts/filter_encoder.hpp
- [x] T006 [P] Create header file `src/table_scan/function_mapping.hpp` with FunctionMapping struct and lookup declarations

---

## Phase 2: Foundational (Core Infrastructure)

**Purpose**: Core components that ALL user stories depend on - MUST complete before any filter pushdown work

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [ ] T007 Implement `src/table_scan/table_scan_bind.cpp` with TableScanBindData struct and GetFullTableName/IsValidColumnIndex methods
- [ ] T008 Implement `src/table_scan/table_scan_state.cpp` with TableScanGlobalState and TableScanLocalState structs
- [ ] T009 Implement `src/table_scan/function_mapping.cpp` with string function mappings (LOWER, UPPER, LEN, TRIM, LTRIM, RTRIM)
- [ ] T010 Implement `src/table_scan/filter_encoder.cpp` with base FilterEncoder::Encode() method and existing filter type support (CONSTANT_COMPARISON, IS_NULL, IS_NOT_NULL, IN_FILTER)
- [ ] T011 Implement FilterEncoder::EncodeConjunctionAnd() with partial pushdown support in `src/table_scan/filter_encoder.cpp`
- [ ] T012 Implement FilterEncoder::EncodeConjunctionOr() with all-or-nothing semantics in `src/table_scan/filter_encoder.cpp`
- [ ] T013 Implement FilterEncoder utility methods in `src/table_scan/filter_encoder.cpp`: ValueToSQLLiteral, EscapeStringLiteral, EscapeBracketIdentifier, GetComparisonOperator
- [ ] T014 Migrate table scan bind logic from `src/mssql_functions.cpp` to `src/table_scan/table_scan_bind.cpp`
- [ ] T015 Implement `src/table_scan/table_scan_execute.cpp` with TableScanExecute function (migrate from mssql_functions.cpp)
- [ ] T016 Implement `src/table_scan/mssql_table_scan.cpp` with GetCatalogScanFunction(), TableScanBind(), TableScanInitGlobal(), TableScanInitLocal()
- [ ] T017 Implement BuildSelectQuery() in `src/table_scan/mssql_table_scan.cpp` with column projection and WHERE clause integration
- [ ] T018 Update `src/catalog/mssql_table_entry.cpp` to use new GetCatalogScanFunction() from table_scan module
- [ ] T019 Remove `src/pushdown/` directory and its placeholder files

**Checkpoint**: Foundation ready - basic table scan works with existing filter types, user story implementation can begin

---

## Phase 3: User Story 1 - LIKE Pattern Filtering (Priority: P1) 🎯 MVP

**Goal**: Push down LIKE pattern filters (prefix, suffix, contains, ILIKE) to SQL Server

**Independent Test**: Execute `SELECT * FROM mssql.schema.table WHERE name LIKE 'John%'` and verify the generated T-SQL includes `WHERE [name] LIKE N'John%'`

### Implementation for User Story 1

- [ ] T020 [US1] Add prefix/suffix/contains function mappings to `src/table_scan/function_mapping.cpp`
- [ ] T021 [US1] Implement FilterEncoder::EncodeLikePattern() in `src/table_scan/filter_encoder.cpp` for prefix, suffix, contains patterns
- [ ] T022 [US1] Implement FilterEncoder::EscapeLikePattern() in `src/table_scan/filter_encoder.cpp` to escape %, _, [ characters
- [ ] T023 [US1] Add iprefix/isuffix/icontains function mappings (ILIKE support using LOWER()) to `src/table_scan/function_mapping.cpp`
- [ ] T024 [US1] Implement ILIKE encoding in FilterEncoder::EncodeLikePattern() to apply LOWER() to both column and pattern in `src/table_scan/filter_encoder.cpp`
- [ ] T025 [US1] Implement FilterEncoder::EncodeExpressionFilter() to handle EXPRESSION_FILTER type in `src/table_scan/filter_encoder.cpp`
- [ ] T026 [US1] Implement FilterEncoder::EncodeFunctionExpression() for function call expressions in `src/table_scan/filter_encoder.cpp`

**Checkpoint**: LIKE pattern pushdown works - queries with prefix, suffix, contains, ILIKE patterns are pushed to SQL Server

---

## Phase 4: User Story 2 - Function Expressions and CASE (Priority: P1)

**Goal**: Push down SQL-compatible function expressions (LOWER, UPPER, LEN, TRIM) and CASE expressions to SQL Server

**Independent Test**: Execute `SELECT * FROM table WHERE LOWER(name) = 'test'` and verify the LOWER function appears in generated T-SQL

### Implementation for User Story 2

- [ ] T027 [P] [US2] Implement FilterEncoder::EncodeExpression() dispatcher in `src/table_scan/filter_encoder.cpp` to route to appropriate encoder
- [ ] T028 [P] [US2] Implement FilterEncoder::EncodeColumnRef() for column reference expressions in `src/table_scan/filter_encoder.cpp`
- [ ] T029 [P] [US2] Implement FilterEncoder::EncodeConstant() for constant value expressions in `src/table_scan/filter_encoder.cpp`
- [ ] T030 [US2] Implement FilterEncoder::EncodeComparisonExpression() for comparison expressions in `src/table_scan/filter_encoder.cpp`
- [ ] T031 [US2] Implement reversed comparison support (constant on left) in EncodeComparisonExpression() in `src/table_scan/filter_encoder.cpp`
- [ ] T032 [US2] Implement nested function expression support via recursive EncodeExpression() calls in `src/table_scan/filter_encoder.cpp`
- [ ] T033 [US2] Implement FilterEncoder::EncodeCaseExpression() for CASE WHEN expressions in `src/table_scan/filter_encoder.cpp`
- [ ] T034 [US2] Add depth tracking to ExpressionEncodeContext and max depth check (100 levels) in `src/table_scan/filter_encoder.cpp`

**Checkpoint**: Function expressions and CASE pushdown works - queries with LOWER, UPPER, LEN, TRIM, and CASE are pushed to SQL Server

---

## Phase 5: User Story 3 - Date/Time Functions (Priority: P2)

**Goal**: Push down date/time function expressions (YEAR, MONTH, DAY, DATEDIFF, DATEADD, DATEPART) to SQL Server

**Independent Test**: Execute `SELECT * FROM table WHERE YEAR(created_at) = 2024` and verify the YEAR function appears in generated T-SQL

### Implementation for User Story 3

- [ ] T035 [P] [US3] Add YEAR, MONTH, DAY function mappings to `src/table_scan/function_mapping.cpp`
- [ ] T036 [P] [US3] Add HOUR, MINUTE, SECOND function mappings (using DATEPART) to `src/table_scan/function_mapping.cpp`
- [ ] T037 [P] [US3] Add date_diff → DATEDIFF function mapping to `src/table_scan/function_mapping.cpp`
- [ ] T038 [P] [US3] Add date_add → DATEADD function mapping (with parameter reorder) to `src/table_scan/function_mapping.cpp`
- [ ] T039 [P] [US3] Add date_part → DATEPART function mapping to `src/table_scan/function_mapping.cpp`
- [ ] T040 [US3] Add current_date and current_timestamp constant mappings to `src/table_scan/filter_encoder.cpp`
- [ ] T041 [US3] Add date part string mappings (year, month, day, hour, minute, second, week, quarter) to `src/table_scan/function_mapping.cpp`

**Checkpoint**: Date/time function pushdown works - queries with date extraction and arithmetic functions are pushed to SQL Server

---

## Phase 6: User Story 4 - Arithmetic Expressions (Priority: P2)

**Goal**: Push down arithmetic expressions (+, -, *, /) to SQL Server

**Independent Test**: Execute `SELECT * FROM table WHERE price * quantity > 1000` and verify the arithmetic expression appears in generated T-SQL

### Implementation for User Story 4

- [ ] T042 [US4] Implement FilterEncoder::EncodeOperatorExpression() for arithmetic operators in `src/table_scan/filter_encoder.cpp`
- [ ] T043 [US4] Implement FilterEncoder::GetArithmeticOperator() mapping for +, -, *, /, % in `src/table_scan/filter_encoder.cpp`
- [ ] T044 [US4] Add parenthesization for arithmetic expressions to preserve operator precedence in `src/table_scan/filter_encoder.cpp`

**Checkpoint**: Arithmetic expression pushdown works - queries with +, -, *, / in filters are pushed to SQL Server

---

## Phase 7: User Story 5 - OR Condition Handling (Priority: P2)

**Goal**: Correctly handle OR conditions with unsupported branches - reject entire OR if any branch unsupported

**Independent Test**: Execute query with mixed supported/unsupported OR branches and verify correct filtering behavior

### Implementation for User Story 5

- [ ] T045 [US5] Implement FilterEncoder::EncodeConjunctionExpression() for AND/OR in expression trees in `src/table_scan/filter_encoder.cpp`
- [ ] T046 [US5] Verify EncodeConjunctionOr() rejects entire OR when any child is unsupported in `src/table_scan/filter_encoder.cpp`
- [ ] T047 [US5] Verify EncodeConjunctionAnd() allows partial pushdown (skip unsupported children) in `src/table_scan/filter_encoder.cpp`
- [ ] T048 [US5] Ensure needs_duckdb_filter flag is correctly set when any filter not fully pushed in `src/table_scan/filter_encoder.cpp`

**Checkpoint**: OR/AND semantics correct - partial pushdown for AND, all-or-nothing for OR

---

## Phase 8: User Story 7 - Consistent Filter Application Strategy (Priority: P1)

**Goal**: Ensure correct results regardless of which filters are pushed down

**Independent Test**: Compare query results with and without filter pushdown, verify identical results

### Implementation for User Story 7

- [ ] T049 [US7] Implement filter pushdown integration in TableScanInitGlobal() to call FilterEncoder::Encode() in `src/table_scan/mssql_table_scan.cpp`
- [ ] T050 [US7] Store filter_pushdown_applied and needs_duckdb_filter flags in TableScanGlobalState in `src/table_scan/table_scan_state.cpp`
- [ ] T051 [US7] Ensure DuckDB re-applies all filters when needs_duckdb_filter is true in `src/table_scan/table_scan_execute.cpp`

**Checkpoint**: Filter strategy correct - results always accurate regardless of pushdown

---

## Phase 9: User Story 6 - Code Organization (Priority: P3)

**Goal**: Clean code organization in src/table_scan/ with clear separation of concerns

**Independent Test**: Verify table scan code is in src/table_scan/ with separate files for bind, state, execute, filter encoding

### Implementation for User Story 6

- [ ] T052 [US6] Remove table scan code from `src/mssql_functions.cpp` (keep mssql_scan() for ad-hoc queries)
- [ ] T053 [US6] Update CMakeLists.txt to include new `src/table_scan/` source files
- [ ] T054 [US6] Verify all existing integration tests pass after refactoring
- [ ] T055 [US6] Update include guards and namespace declarations in all table_scan headers

**Checkpoint**: Code organization complete - table scan in dedicated module, old pushdown directory removed

---

## Phase 10: Polish & Cross-Cutting Concerns

**Purpose**: Debug logging, final validation, and cleanup

- [ ] T056 [P] Implement MSSQL_DEBUG logging for filter pushdown decisions in `src/table_scan/filter_encoder.cpp`
- [ ] T057 [P] Add debug logging for generated T-SQL WHERE clause in `src/table_scan/mssql_table_scan.cpp`
- [ ] T058 [P] Add debug logging for needs_duckdb_filter flag status in `src/table_scan/mssql_table_scan.cpp`
- [ ] T059 Code review and cleanup of table_scan module
- [ ] T060 Run quickstart.md validation scenarios manually
- [ ] T061 Final verification: all success criteria from spec.md are met

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phases 3-9)**: All depend on Foundational phase completion
  - US1 (LIKE): Can start immediately after Foundational
  - US2 (Functions/CASE): Can start after Foundational, benefits from US1 completion
  - US3 (Date/Time): Independent of US1/US2, just needs Foundational
  - US4 (Arithmetic): Independent of US1-US3, just needs Foundational
  - US5 (OR Handling): Depends on US1-US4 for full testing coverage
  - US6 (Code Org): Can start after Foundational, completes after all other US
  - US7 (Filter Strategy): Depends on US1-US5 for integration
- **Polish (Phase 10)**: Depends on all user stories being complete

### User Story Dependencies

- **User Story 1 (LIKE) - P1**: No dependencies on other stories
- **User Story 2 (Functions/CASE) - P1**: No strict dependency, shares FilterEncoder infrastructure
- **User Story 3 (Date/Time) - P2**: No dependencies, parallel to US4/US5
- **User Story 4 (Arithmetic) - P2**: No dependencies, parallel to US3/US5
- **User Story 5 (OR Handling) - P2**: Benefits from US1-US4 for testing edge cases
- **User Story 6 (Code Org) - P3**: Final cleanup after all implementation
- **User Story 7 (Filter Strategy) - P1**: Integrates with all filter encoding work

### Parallel Opportunities

**Phase 1 (Setup)**: T002, T003, T004, T005, T006 can run in parallel

**Phase 2 (Foundational)**: Sequential - establishes core infrastructure

**Phase 3 (US1 - LIKE)**: Sequential within phase, parallel with other stories after T025-T026

**Phase 4 (US2 - Functions)**: T027, T028, T029 can run in parallel

**Phase 5 (US3 - Date/Time)**: T035, T036, T037, T038, T039 can run in parallel

**Phase 6 (US4 - Arithmetic)**: Sequential within phase

**Phase 10 (Polish)**: T056, T057, T058 can run in parallel

---

## Parallel Example: Phase 1 Setup

```bash
# Launch all header file creation tasks together:
Task: "Create header file src/table_scan/mssql_table_scan.hpp"
Task: "Create header file src/table_scan/table_scan_bind.hpp"
Task: "Create header file src/table_scan/table_scan_state.hpp"
Task: "Create header file src/table_scan/filter_encoder.hpp"
Task: "Create header file src/table_scan/function_mapping.hpp"
```

## Parallel Example: Phase 5 Date/Time Functions

```bash
# Launch all date/time function mapping tasks together:
Task: "Add YEAR, MONTH, DAY function mappings to function_mapping.cpp"
Task: "Add HOUR, MINUTE, SECOND function mappings to function_mapping.cpp"
Task: "Add date_diff → DATEDIFF function mapping to function_mapping.cpp"
Task: "Add date_add → DATEADD function mapping to function_mapping.cpp"
Task: "Add date_part → DATEPART function mapping to function_mapping.cpp"
```

---

## Implementation Strategy

### MVP First (User Stories 1, 2, 7)

1. Complete Phase 1: Setup (T001-T006)
2. Complete Phase 2: Foundational (T007-T019)
3. Complete Phase 3: User Story 1 - LIKE Patterns (T020-T026)
4. Complete Phase 4: User Story 2 - Functions/CASE (T027-T034)
5. Complete Phase 8: User Story 7 - Filter Strategy (T049-T051)
6. **STOP and VALIDATE**: Basic filter pushdown with LIKE, functions, CASE works correctly
7. Deploy/demo MVP

### Incremental Delivery

1. **Foundation**: Setup + Foundational → Basic table scan works
2. **MVP (P1 stories)**: US1 + US2 + US7 → LIKE and function pushdown works
3. **Enhanced (P2 stories)**: US3 + US4 + US5 → Date/time, arithmetic, OR handling
4. **Complete (P3)**: US6 + Polish → Code organization and cleanup

### Single Developer Strategy

1. Complete phases sequentially: Setup → Foundational → US1 → US2 → US7 → US3 → US4 → US5 → US6 → Polish
2. Each checkpoint validates incremental progress
3. MVP deliverable after US1 + US2 + US7

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Types in `duckdb::mssql` namespace do NOT use MSSQL prefix
- Types in `duckdb` namespace MUST use MSSQL prefix (e.g., MSSQLResultStream)
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- Avoid: vague tasks, same file conflicts, cross-story dependencies that break independence

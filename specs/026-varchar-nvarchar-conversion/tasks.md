# Tasks: VARCHAR to NVARCHAR Conversion

**Input**: Design documents from `/specs/026-varchar-nvarchar-conversion/`
**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: Integration tests ARE required (spec requests testing with extended ASCII data).

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3, US4)
- Include exact file paths in descriptions

## Path Conventions

- **Project type**: Single project (DuckDB C++ extension)
- **Source**: `src/` at repository root
- **Tests**: `test/sql/` for SQLLogicTest, `test/cpp/` for unit tests

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: No setup needed - this feature modifies existing code only

- [x] T001 Review existing code in src/table_scan/table_scan.cpp to understand column list generation (lines 68-208)
- [x] T002 Review existing MSSQLColumnInfo in src/include/catalog/mssql_column_info.hpp for available fields

**Checkpoint**: Code locations verified, ready for implementation

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Add metadata plumbing required by all user stories

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [x] T003 Add `vector<MSSQLColumnInfo> mssql_columns` field to MSSQLCatalogScanBindData in src/include/mssql_functions.hpp
- [x] T004 Populate mssql_columns in MSSQLTableEntry::GetScanFunction() in src/catalog/mssql_table_entry.cpp
- [x] T005 Add helper function `NeedsNVarcharConversion(const MSSQLColumnInfo &col)` in src/table_scan/table_scan.cpp
- [x] T006 Add helper function `GetNVarcharLength(int16_t max_length)` in src/table_scan/table_scan.cpp
- [x] T007 Add helper function `BuildColumnExpression(const MSSQLColumnInfo &col, const string &col_name)` in src/table_scan/table_scan.cpp

**Checkpoint**: Foundation ready - helper functions exist, metadata flows through bind data

---

## Phase 3: User Story 1 - Query Tables with Extended ASCII (Priority: P1) 🎯 MVP

**Goal**: Users can query VARCHAR columns with extended ASCII characters (é, ñ, ü) without UTF-8 errors

**Independent Test**: Create table with Latin1 collation VARCHAR, insert extended ASCII, query and verify correct decoding

### Implementation for User Story 1

- [x] T008 [US1] Modify column list generation at line ~139 (rowid path, projected columns) in src/table_scan/table_scan.cpp to use BuildColumnExpression()
- [x] T009 [US1] Modify column list generation at line ~155 (rowid path, PK columns) in src/table_scan/table_scan.cpp to use BuildColumnExpression()
- [x] T010 [US1] Modify column list generation at line ~204 (standard path, no rowid) in src/table_scan/table_scan.cpp to use BuildColumnExpression()
- [x] T011 [US1] Add debug logging for NVARCHAR conversion decisions (controlled by MSSQL_DEBUG env)

### Tests for User Story 1

- [x] T012 [US1] Create integration test file test/sql/catalog/varchar_encoding.test with test table setup
- [x] T013 [US1] Add test case: VARCHAR with extended ASCII characters returns correct data
- [x] T014 [US1] Add test case: CHAR with extended ASCII characters returns correct data
- [x] T015 [US1] Add test case: NULL values in VARCHAR columns preserved correctly

**Checkpoint**: User Story 1 complete - basic VARCHAR/CHAR with extended ASCII works

---

## Phase 4: User Story 2 - Mixed Column Types (Priority: P2)

**Goal**: Tables with mixed VARCHAR/NVARCHAR/INT columns handled correctly - conversion only where needed

**Independent Test**: Create table with INT, VARCHAR (Latin1), and NVARCHAR columns, verify all return correct data

### Implementation for User Story 2

- [x] T016 [US2] Verify BuildColumnExpression() returns unmodified `[column]` for non-string types
- [x] T017 [US2] Verify BuildColumnExpression() returns unmodified `[column]` for NVARCHAR/NCHAR columns

### Tests for User Story 2

- [x] T018 [US2] Add test case: Mixed INT, VARCHAR, NVARCHAR table returns correct data for all columns
- [x] T019 [US2] Add test case: Projection of specific columns (SELECT id, name) works correctly

**Checkpoint**: User Story 2 complete - mixed column types work correctly

---

## Phase 5: User Story 3 - VARCHAR(MAX) Handling (Priority: P2)

**Goal**: VARCHAR(MAX) columns correctly converted to NVARCHAR(MAX)

**Independent Test**: Create VARCHAR(MAX) column with >4000 chars of extended ASCII, verify full data returned

### Implementation for User Story 3

- [x] T020 [US3] Verify GetNVarcharLength() returns "MAX" when max_length == -1
- [x] T021 [US3] Verify generated SQL uses NVARCHAR(MAX) for VARCHAR(MAX) columns

### Tests for User Story 3

- [x] T022 [US3] Add test case: VARCHAR(MAX) with extended ASCII >4000 chars returns full data
- [x] T023 [US3] Add test case: VARCHAR(MAX) preserves NULL values

**Checkpoint**: User Story 3 complete - VARCHAR(MAX) columns work correctly

---

## Phase 6: User Story 4 - Length Boundary Handling (Priority: P3)

**Goal**: VARCHAR columns >4000 chars truncated to NVARCHAR(4000); lengths ≤4000 preserved

**Independent Test**: Create VARCHAR(8000) column with >4000 chars, verify truncation to 4000

### Implementation for User Story 4

- [x] T024 [US4] Verify GetNVarcharLength() returns "4000" when max_length > 4000 (and not -1)
- [x] T025 [US4] Verify GetNVarcharLength() returns original length when max_length ≤ 4000

### Tests for User Story 4

- [x] T026 [US4] Add test case: VARCHAR(4000) column returns full 4000 characters
- [x] T027 [US4] Add test case: VARCHAR(8000) with 5000 chars truncates to 4000 characters
- [x] T028 [US4] Add test case: VARCHAR(5000) column definition generates NVARCHAR(4000) CAST

**Checkpoint**: User Story 4 complete - length boundaries handled correctly

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Documentation and validation

- [x] T029 [P] Add VARCHAR encoding limitation section to README.md documenting truncation behavior
- [x] T030 [P] Add mssql_scan() encoding note to README.md (raw SQL requires manual CAST)
- [x] T031 Run existing test suite to verify no regressions (make test-all)
- [x] T032 Run quickstart.md validation - verify example queries work

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - code review only
- **Foundational (Phase 2)**: Depends on Setup - adds bind data fields and helper functions
- **User Stories (Phase 3-6)**: All depend on Foundational phase completion
  - US1 (P1): Can start immediately after Foundational
  - US2 (P2): Can start in parallel with US1 (different test cases)
  - US3 (P2): Can start in parallel with US1/US2 (different test cases)
  - US4 (P3): Can start in parallel with US1/US2/US3 (different test cases)
- **Polish (Phase 7)**: Depends on all user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: After Foundational - Core implementation, MUST complete first
- **User Story 2 (P2)**: After Foundational - Independent, tests mixed types
- **User Story 3 (P2)**: After Foundational - Independent, tests VARCHAR(MAX)
- **User Story 4 (P3)**: After Foundational - Independent, tests length boundaries

### Within Each User Story

- Implementation tasks modify the same file (table_scan.cpp) - cannot run in parallel
- Test tasks for different user stories CAN run in parallel (different test scenarios)
- Each user story is independently testable after its implementation tasks complete

### Parallel Opportunities

- T001 and T002 (Setup review) can run in parallel
- T029 and T030 (README updates) can run in parallel
- Test tasks across different user stories can run in parallel (different test files/scenarios)
- Note: Implementation tasks T008-T010 modify the same function and CANNOT run in parallel

---

## Parallel Example: Test Tasks

```bash
# After all implementation is complete, run all test scenarios in parallel:
Task: "Test VARCHAR with extended ASCII" (T013)
Task: "Test CHAR with extended ASCII" (T014)
Task: "Test mixed column types" (T018)
Task: "Test VARCHAR(MAX)" (T022)
Task: "Test length truncation" (T027)
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (code review)
2. Complete Phase 2: Foundational (add bind data, helpers)
3. Complete Phase 3: User Story 1 (core VARCHAR conversion)
4. **STOP and VALIDATE**: Test with extended ASCII data
5. This alone fixes the UTF-8 error for most users

### Incremental Delivery

1. Setup + Foundational → Helpers ready
2. User Story 1 → Core fix deployed (MVP!)
3. User Story 2 → Mixed types verified
4. User Story 3 → VARCHAR(MAX) verified
5. User Story 4 → Edge cases verified
6. Each story adds confidence without breaking previous functionality

### Single Developer Strategy

Since all implementation tasks modify the same file (table_scan.cpp):

1. Complete Setup + Foundational sequentially
2. Implement all modifications to table_scan.cpp (T008-T010) in one session
3. Run test suite after each user story's tests are added
4. Polish phase last

---

## Notes

- All implementation tasks modify src/table_scan/table_scan.cpp - coordinate carefully
- Helper functions (T005-T007) should be static local functions, not exposed in header
- MSSQLColumnInfo already has is_unicode, is_utf8, max_length fields - no new fields needed
- Test file test/sql/catalog/varchar_encoding.test requires SQL Server running (make docker-up)
- Debug logging controlled by MSSQL_DEBUG environment variable (existing pattern)

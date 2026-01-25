# Tasks: MSSQL rowid Semantics (PK-based Row Identity)

**Input**: Design documents from `/specs/001-pk-rowid-semantics/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/

**Tests**: Integration tests are included as this is a DuckDB extension with SQLLogicTest framework.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3, US4)
- Include exact file paths in descriptions

## Path Conventions

- **C++ Extension**: `src/`, `src/include/`, `test/sql/` at repository root
- Paths follow existing project structure per plan.md

---

## Phase 1: Setup (Test Infrastructure)

**Purpose**: Create test tables in SQL Server for rowid testing

- [ ] T001 Add rowid test tables to docker/init/init.sql (scalar PK tables, composite PK tables, no-PK table, view)

**Test tables to create**:

```sql
-- Scalar PK tables (different types)
CREATE TABLE RowidTestInt (id INT PRIMARY KEY, name NVARCHAR(100));
CREATE TABLE RowidTestBigint (id BIGINT PRIMARY KEY, data VARCHAR(50));
CREATE TABLE RowidTestVarchar (code VARCHAR(20) PRIMARY KEY, description NVARCHAR(200));

-- Composite PK tables
CREATE TABLE RowidTestComposite2 (tenant_id INT, id BIGINT, value DECIMAL(10,2), PRIMARY KEY (tenant_id, id));
CREATE TABLE RowidTestComposite3 (region VARCHAR(10), year INT, seq INT, data NVARCHAR(100), PRIMARY KEY (region, year, seq));

-- No PK table
CREATE TABLE RowidTestNoPK (log_time DATETIME, message NVARCHAR(MAX));

-- View (for view error test)
CREATE VIEW RowidTestView AS SELECT id, name FROM RowidTestInt;
```

---

## Phase 2: Foundational (Core PK Infrastructure)

**Purpose**: Implement PK metadata structures and discovery that ALL user stories depend on

**CRITICAL**: No user story work can begin until this phase is complete

- [ ] T002 [P] Create PKColumnInfo and PrimaryKeyInfo structs in src/include/catalog/mssql_primary_key.hpp
- [ ] T003 [P] Implement PK discovery SQL query constant in src/catalog/mssql_primary_key.cpp
- [ ] T004 Implement PrimaryKeyInfo::Discover() method in src/catalog/mssql_primary_key.cpp
- [ ] T005 Implement PrimaryKeyInfo::ComputeRowIdType() for scalar and STRUCT types in src/catalog/mssql_primary_key.cpp
- [ ] T006 Add PrimaryKeyInfo cache field to MSSQLTableEntry in src/include/catalog/mssql_table_entry.hpp
- [ ] T007 Implement EnsurePKLoaded() lazy loading method in src/catalog/mssql_table_entry.cpp
- [ ] T008 Implement GetRowIdType(), HasPrimaryKey(), GetPrimaryKeyInfo() methods in src/catalog/mssql_table_entry.cpp
- [ ] T009 Add debug logging for PK discovery operations (MSSQL_DEBUG support) in src/catalog/mssql_primary_key.cpp

**Checkpoint**: PK discovery infrastructure ready - user story implementation can now begin

---

## Phase 3: User Story 1 - Scalar PK rowid (Priority: P1) MVP

**Goal**: Query rowid from tables with single-column primary key

**Independent Test**: `SELECT rowid, * FROM mssql.dbo.RowidTestInt LIMIT 5` returns rowid matching id column

### Tests for User Story 1

- [ ] T010 [P] [US1] Create test file test/sql/rowid/scalar_pk_rowid.test with INT PK tests
- [ ] T011 [P] [US1] Add BIGINT PK tests to test/sql/rowid/scalar_pk_rowid.test
- [ ] T012 [P] [US1] Add VARCHAR PK tests to test/sql/rowid/scalar_pk_rowid.test

### Implementation for User Story 1

- [ ] T013 [US1] Detect COLUMN_IDENTIFIER_ROW_ID in TableScanInitGlobal column_ids in src/table_scan/mssql_table_scan.cpp
- [ ] T014 [US1] Add rowid_requested and pk_info fields to MSSQLCatalogScanBindData in src/include/mssql_functions.hpp
- [ ] T015 [US1] Populate PK info in GetScanFunction() for rowid support in src/catalog/mssql_table_entry.cpp
- [ ] T016 [US1] Include PK columns in SELECT when rowid requested in TableScanInitGlobal in src/table_scan/mssql_table_scan.cpp
- [ ] T017 [US1] Implement scalar rowid vector population in FillChunk path in src/table_scan/mssql_table_scan.cpp

**Checkpoint**: Scalar PK rowid fully functional - can test with `SELECT rowid, * FROM table_with_int_pk`

---

## Phase 4: User Story 2 - Composite PK rowid (Priority: P1)

**Goal**: Query rowid from tables with multi-column primary key as STRUCT

**Independent Test**: `SELECT rowid FROM mssql.dbo.RowidTestComposite2 LIMIT 5` returns STRUCT with tenant_id and id fields

### Tests for User Story 2

- [ ] T018 [P] [US2] Create test file test/sql/rowid/composite_pk_rowid.test with 2-column PK tests
- [ ] T019 [P] [US2] Add 3-column PK tests to test/sql/rowid/composite_pk_rowid.test
- [ ] T020 [P] [US2] Add STRUCT field access tests (rowid.field_name) to test/sql/rowid/composite_pk_rowid.test

### Implementation for User Story 2

- [ ] T021 [US2] Implement STRUCT vector creation for composite PK in TableScanExecute in src/table_scan/mssql_table_scan.cpp
- [ ] T022 [US2] Implement StructVector child population from PK columns in src/table_scan/mssql_table_scan.cpp
- [ ] T023 [US2] Verify PK ordinal order matches STRUCT field order in src/catalog/mssql_primary_key.cpp

**Checkpoint**: Composite PK rowid fully functional - can test with `SELECT rowid.tenant_id, rowid.id FROM composite_pk_table`

---

## Phase 5: User Story 3 - No rowid Overhead (Priority: P2)

**Goal**: Queries without rowid projection do not fetch unnecessary PK columns

**Independent Test**: Debug log shows SELECT without PK columns when rowid not requested

### Tests for User Story 3

- [ ] T024 [P] [US3] Create test file test/sql/rowid/no_rowid_overhead.test verifying queries without rowid work correctly

### Implementation for User Story 3

- [ ] T025 [US3] Verify PK columns only added to SELECT when rowid in column_ids in src/table_scan/mssql_table_scan.cpp
- [ ] T026 [US3] Add debug log showing generated SQL for verification in src/table_scan/mssql_table_scan.cpp
- [ ] T027 [US3] Handle case where PK column is already in user's projection (no duplication) in src/table_scan/mssql_table_scan.cpp

**Checkpoint**: No overhead for non-rowid queries verified

---

## Phase 6: User Story 4 - No PK Error Handling (Priority: P2)

**Goal**: Clear error when rowid requested from table without PK, while allowing regular queries

**Independent Test**: `SELECT rowid FROM no_pk_table` throws "MSSQL: rowid requires a primary key" but `SELECT * FROM no_pk_table` works

### Tests for User Story 4

- [ ] T028 [P] [US4] Create test file test/sql/rowid/no_pk_rowid.test with error expectation
- [ ] T029 [P] [US4] Add success tests for non-rowid queries on no-PK tables to test/sql/rowid/no_pk_rowid.test

### Implementation for User Story 4

- [ ] T030 [US4] Throw "MSSQL: rowid requires a primary key" when rowid requested and pk_info.exists=false in src/table_scan/mssql_table_scan.cpp
- [ ] T031 [US4] Ensure non-rowid queries work normally for no-PK tables in src/table_scan/mssql_table_scan.cpp

**Checkpoint**: No-PK error handling complete

---

## Phase 7: View Error Handling (Edge Case)

**Goal**: Distinct error when rowid requested from SQL Server view

**Independent Test**: `SELECT rowid FROM view` throws "MSSQL: rowid not supported for views"

### Tests for Views

- [ ] T032 [P] Create test file test/sql/rowid/view_rowid.test with view error expectation
- [ ] T033 [P] Add success tests for non-rowid queries on views to test/sql/rowid/view_rowid.test

### Implementation for Views

- [ ] T034 Check object_type_ == VIEW and throw distinct error in GetRowIdType() in src/catalog/mssql_table_entry.cpp

**Checkpoint**: View error handling complete

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Final validation and edge cases

- [ ] T035 [P] Add NULL PK value detection and error in FillChunk in src/table_scan/mssql_table_scan.cpp
- [ ] T036 [P] Create integration test test/sql/rowid/rowid_integration.test combining all scenarios
- [ ] T037 Update quickstart.md with final test commands in specs/001-pk-rowid-semantics/quickstart.md
- [ ] T038 Run full test suite: `make integration-test`

---

## Dependencies & Execution Order

### Phase Dependencies

```
Phase 1: Setup ──────────────┐
                             ├──► Phase 2: Foundational ──► All User Stories
                             │
                             │    ┌─► Phase 3: US1 (Scalar PK)
                             │    │
Phase 2 Complete ────────────┼────┼─► Phase 4: US2 (Composite PK)
                             │    │
                             │    ├─► Phase 5: US3 (No Overhead)
                             │    │
                             │    └─► Phase 6: US4 (No PK Error)
                             │
                             └──► Phase 7: View Error ──► Phase 8: Polish
```

### User Story Dependencies

- **User Story 1 (P1)**: Depends on Phase 2 (Foundational) - Core rowid path
- **User Story 2 (P1)**: Depends on Phase 2 - Can parallel with US1 (different code paths)
- **User Story 3 (P2)**: Depends on US1 or US2 - Verification of no-overhead
- **User Story 4 (P2)**: Depends on Phase 2 - Error path only

### Within Each User Story

- Tests can run in parallel ([P])
- Implementation tasks are sequential (build on each other)

### Parallel Opportunities

Within Phase 2 (Foundational):

```bash
# These can run in parallel:
Task T002: "Create PKColumnInfo and PrimaryKeyInfo structs"
Task T003: "Implement PK discovery SQL query constant"
```

Within Phase 3 (US1 Tests):

```bash
# These can run in parallel:
Task T010: "Create test file with INT PK tests"
Task T011: "Add BIGINT PK tests"
Task T012: "Add VARCHAR PK tests"
```

User Stories 1 and 2 can be worked on in parallel once Phase 2 is complete.

---

## Implementation Strategy

### MVP First (User Story 1 + 2)

1. Complete Phase 1: Setup test tables
2. Complete Phase 2: Foundational PK infrastructure
3. Complete Phase 3: User Story 1 (Scalar PK)
4. Complete Phase 4: User Story 2 (Composite PK)
5. **STOP and VALIDATE**: Test both scalar and composite PK rowid
6. Deploy/merge if ready

### Incremental Delivery

1. Setup + Foundational → PK discovery working
2. Add US1 (Scalar) → Test independently → Most common case covered
3. Add US2 (Composite) → Test independently → Full rowid semantics
4. Add US3 + US4 → Error handling and optimization
5. Add View handling → Edge case complete
6. Polish → Production ready

---

## Summary

| Phase | Tasks | Parallel | Description |
|-------|-------|----------|-------------|
| 1: Setup | 1 | - | Test table creation |
| 2: Foundational | 8 | 2 | PK infrastructure |
| 3: US1 Scalar PK | 8 | 3 | Scalar rowid (MVP) |
| 4: US2 Composite PK | 6 | 3 | STRUCT rowid |
| 5: US3 No Overhead | 4 | 1 | Performance verification |
| 6: US4 No PK Error | 4 | 2 | Error handling |
| 7: View Error | 3 | 2 | Edge case |
| 8: Polish | 4 | 2 | Final validation |
| **Total** | **38** | **15** | |

---

## Notes

- [P] tasks = different files, no dependencies on other tasks in same phase
- [USn] label maps task to specific user story for traceability
- All test files use DuckDB SQLLogicTest format
- Run tests with `make integration-test` (requires SQL Server via `make docker-up`)
- Commit after each task or logical group
- Stop at any checkpoint to validate independently

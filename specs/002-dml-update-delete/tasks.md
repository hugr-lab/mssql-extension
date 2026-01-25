# Tasks: DML UPDATE/DELETE using PK-based rowid

**Input**: Design documents from `/specs/002-dml-update-delete/`
**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: Integration tests are included as requested in the specification Testing section.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2)
- Include exact file paths in descriptions

## Path Conventions

- **Single project**: `src/`, `test/` at repository root
- Extension follows DuckDB extension layout

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create directory structure and shared DML configuration

- [x] T001 Create directory structure: `src/include/dml/`, `src/include/update/`, `src/include/delete/`, `src/dml/`, `src/update/`, `src/delete/`
- [x] T002 [P] Create `MSSQLDMLConfig` struct in `src/include/dml/mssql_dml_config.hpp`
- [x] T003 [P] Implement `MSSQLDMLConfig` loading from settings in `src/dml/mssql_dml_config.cpp`
- [x] T004 Register DML settings (`mssql_dml_batch_size`, `mssql_dml_use_prepared`, `mssql_dml_max_parameters`) in `src/connection/mssql_settings.cpp`
- [x] T005 Update `CMakeLists.txt` to include new source directories and files

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure for UPDATE and DELETE operations that MUST be complete before user story implementation

**Critical**: No user story work can begin until this phase is complete

- [x] T006 Create `ExtractPKFromRowid()` utility function for scalar/composite PK extraction in `src/include/dml/mssql_rowid_extractor.hpp`
- [x] T007 Implement `ExtractPKFromRowid()` with STRUCT support in `src/dml/mssql_rowid_extractor.cpp`
- [x] T008 Create `MSSQLDMLBatch` struct in `src/include/dml/mssql_dml_batch.hpp`
- [x] T009 Create `MSSQLDMLResult` struct in `src/include/dml/mssql_dml_result.hpp`
- [x] T010 Create test directory structure: `test/sql/dml/`

**Checkpoint**: Foundation ready - user story implementation can now begin

---

## Phase 3: User Story 1 - Update Rows via Standard SQL UPDATE (Priority: P1)

**Goal**: Enable `UPDATE table SET col=val WHERE condition` on MSSQL tables with scalar PK using batched execution

**Independent Test**: Run `UPDATE mssql.dbo.products SET price = 99.99 WHERE id = 1` and verify the row is updated

### Implementation for User Story 1

- [x] T011 [P] [US1] Create `MSSQLUpdateColumn` struct in `src/include/update/mssql_update_column.hpp`
- [x] T012 [P] [US1] Create `MSSQLUpdateTarget` struct in `src/include/update/mssql_update_target.hpp`
- [x] T013 [US1] Create `MSSQLUpdateStatement` class for SQL generation in `src/include/update/mssql_update_statement.hpp`
- [x] T014 [US1] Implement `MSSQLUpdateStatement::Build()` with VALUES join pattern in `src/update/mssql_update_statement.cpp`
- [x] T015 [P] [US1] Create `MSSQLUpdateExecutor` class in `src/include/update/mssql_update_executor.hpp`
- [x] T016 [US1] Implement `MSSQLUpdateExecutor::Execute()` for batch accumulation in `src/update/mssql_update_executor.cpp`
- [x] T017 [US1] Implement `MSSQLUpdateExecutor::Finalize()` for flush and result in `src/update/mssql_update_executor.cpp`
- [x] T018 [P] [US1] Create `MSSQLPhysicalUpdate` operator class in `src/include/update/mssql_physical_update.hpp`
- [x] T019 [P] [US1] Create `MSSQLUpdateGlobalSinkState` in `src/include/update/mssql_physical_update.hpp`
- [x] T020 [US1] Implement `MSSQLPhysicalUpdate::Sink()` in `src/update/mssql_physical_update.cpp`
- [x] T021 [US1] Implement `MSSQLPhysicalUpdate::Finalize()` in `src/update/mssql_physical_update.cpp`
- [x] T022 [US1] Implement `MSSQLPhysicalUpdate::GetData()` to return row count in `src/update/mssql_physical_update.cpp`
- [x] T023 [US1] Implement `MSSQLCatalog::PlanUpdate()` for scalar PK tables in `src/catalog/mssql_catalog.cpp`
- [x] T024 [US1] Create integration test for scalar PK UPDATE in `test/sql/dml/update_scalar_pk.test`
- [x] T025 [US1] Create integration test for bulk UPDATE (1000 rows) in `test/sql/dml/update_bulk.test`

**Checkpoint**: User Story 1 complete - scalar PK UPDATE operations work end-to-end

---

## Phase 4: User Story 2 - Delete Rows via Standard SQL DELETE (Priority: P1)

**Goal**: Enable `DELETE FROM table WHERE condition` on MSSQL tables with scalar PK using batched execution

**Independent Test**: Run `DELETE FROM mssql.dbo.logs WHERE id = 12345` and verify the row is deleted

### Implementation for User Story 2

- [x] T026 [P] [US2] Create `MSSQLDeleteTarget` struct in `src/include/delete/mssql_delete_target.hpp`
- [x] T027 [US2] Create `MSSQLDeleteStatement` class for SQL generation in `src/include/delete/mssql_delete_statement.hpp`
- [x] T028 [US2] Implement `MSSQLDeleteStatement::Build()` with VALUES join pattern in `src/delete/mssql_delete_statement.cpp`
- [x] T029 [P] [US2] Create `MSSQLDeleteExecutor` class in `src/include/delete/mssql_delete_executor.hpp`
- [x] T030 [US2] Implement `MSSQLDeleteExecutor::Execute()` for batch accumulation in `src/delete/mssql_delete_executor.cpp`
- [x] T031 [US2] Implement `MSSQLDeleteExecutor::Finalize()` for flush and result in `src/delete/mssql_delete_executor.cpp`
- [x] T032 [P] [US2] Create `MSSQLPhysicalDelete` operator class in `src/include/delete/mssql_physical_delete.hpp`
- [x] T033 [P] [US2] Create `MSSQLDeleteGlobalSinkState` in `src/include/delete/mssql_physical_delete.hpp`
- [x] T034 [US2] Implement `MSSQLPhysicalDelete::Sink()` in `src/delete/mssql_physical_delete.cpp`
- [x] T035 [US2] Implement `MSSQLPhysicalDelete::Finalize()` in `src/delete/mssql_physical_delete.cpp`
- [x] T036 [US2] Implement `MSSQLPhysicalDelete::GetData()` to return row count in `src/delete/mssql_physical_delete.cpp`
- [x] T037 [US2] Implement `MSSQLCatalog::PlanDelete()` for scalar PK tables in `src/catalog/mssql_catalog.cpp`
- [x] T038 [US2] Create integration test for scalar PK DELETE in `test/sql/dml/delete_scalar_pk.test`
- [x] T039 [US2] Create integration test for bulk DELETE (5000 rows) in `test/sql/dml/delete_bulk.test`

**Checkpoint**: User Story 2 complete - scalar PK DELETE operations work end-to-end

---

## Phase 5: User Story 3 - Composite PK Support (Priority: P2)

**Goal**: Extend UPDATE/DELETE to support tables with multi-column primary keys using STRUCT rowid

**Independent Test**: Run UPDATE/DELETE on table with composite PK `(tenant_id, id)` and verify correct rows affected

### Implementation for User Story 3

- [x] T040 [US3] Extend `MSSQLUpdateStatement` to handle composite PK ON clause in `src/update/mssql_update_statement.cpp`
- [x] T041 [US3] Extend `MSSQLDeleteStatement` to handle composite PK ON clause in `src/delete/mssql_delete_statement.cpp`
- [x] T042 [US3] Extend `MSSQLUpdateExecutor` to extract STRUCT rowid fields in `src/update/mssql_update_executor.cpp`
- [x] T043 [US3] Extend `MSSQLDeleteExecutor` to extract STRUCT rowid fields in `src/delete/mssql_delete_executor.cpp`
- [x] T044 [US3] Create integration test for composite PK UPDATE in `test/sql/dml/update_composite_pk.test`
- [x] T045 [US3] Create integration test for composite PK DELETE in `test/sql/dml/delete_composite_pk.test`

**Checkpoint**: User Story 3 complete - composite PK UPDATE/DELETE work with STRUCT rowid

---

## Phase 6: User Story 4 - Tables Without Primary Key (Priority: P2)

**Goal**: Reject UPDATE/DELETE on tables without PK with clear error message

**Independent Test**: Attempt UPDATE/DELETE on heap table and verify error "MSSQL: UPDATE/DELETE requires a primary key"

### Implementation for User Story 4

- [ ] T046 [US4] Add PK existence check in `MSSQLCatalog::PlanUpdate()` in `src/catalog/mssql_catalog.cpp`
- [ ] T047 [US4] Add PK existence check in `MSSQLCatalog::PlanDelete()` in `src/catalog/mssql_catalog.cpp`
- [ ] T048 [US4] Create integration test for UPDATE error on no-PK table in `test/sql/dml/update_no_pk_error.test`
- [ ] T049 [US4] Create integration test for DELETE error on no-PK table in `test/sql/dml/delete_no_pk_error.test`

**Checkpoint**: User Story 4 complete - clear errors for PK-less tables

---

## Phase 7: User Story 5 - Reject Primary Key Updates (Priority: P2)

**Goal**: Reject attempts to modify PK columns with clear error message

**Independent Test**: Attempt `UPDATE table SET pk_column = value` and verify error "MSSQL: updating primary key columns is not supported"

### Implementation for User Story 5

- [ ] T050 [US5] Add PK column detection in `MSSQLCatalog::PlanUpdate()` in `src/catalog/mssql_catalog.cpp`
- [ ] T051 [US5] Create integration test for PK column update rejection in `test/sql/dml/update_pk_column_error.test`

**Checkpoint**: User Story 5 complete - PK column updates rejected at planning time

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Improvements that affect multiple user stories

- [ ] T052 Add debug logging for generated SQL in `MSSQLUpdateExecutor` when MSSQL_DEBUG enabled
- [ ] T053 Add debug logging for generated SQL in `MSSQLDeleteExecutor` when MSSQL_DEBUG enabled
- [ ] T054 [P] Create batching verification test (batch_size respected) in `test/sql/dml/batching.test`
- [ ] T055 [P] Create NULL value handling test for UPDATE SET in `test/sql/dml/update_null.test`
- [ ] T056 [P] Create large-scale test (100K rows) in `test/sql/dml/large_scale.test`
- [ ] T057 Update `CMakeLists.txt` with final source list for all new files
- [ ] T058 Validate quickstart.md examples work with implementation

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3-7)**: All depend on Foundational phase completion
  - US1 and US2 can proceed in parallel (UPDATE and DELETE are independent)
  - US3, US4, US5 depend on US1/US2 being complete (they extend functionality)
- **Polish (Phase 8)**: Depends on all user stories being complete

### User Story Dependencies

- **User Story 1 (UPDATE - P1)**: Can start after Foundational (Phase 2)
- **User Story 2 (DELETE - P1)**: Can start after Foundational (Phase 2), parallel with US1
- **User Story 3 (Composite PK - P2)**: Requires US1 and US2 complete
- **User Story 4 (No PK Error - P2)**: Requires US1 and US2 complete (extends PlanUpdate/PlanDelete)
- **User Story 5 (PK Update Error - P2)**: Requires US1 complete (extends PlanUpdate only)

### Within Each User Story

- Headers/structs before implementation
- Statement generators before executors
- Executors before physical operators
- Physical operators before catalog integration
- Catalog integration before tests

### Parallel Opportunities

- T002, T003 can run in parallel (different files)
- T011, T012, T015, T018, T019 can run in parallel (header files)
- T026, T029, T032, T033 can run in parallel (DELETE header files)
- US1 and US2 phases can run in parallel (UPDATE vs DELETE)
- T052, T053, T054, T055, T056 can run in parallel (independent tests)

---

## Parallel Example: User Story 1 + User Story 2

```bash
# These can run in parallel after Foundational phase:

# Team Member A: User Story 1 (UPDATE)
Task: T011-T025

# Team Member B: User Story 2 (DELETE)
Task: T026-T039
```

---

## Implementation Strategy

### MVP First (User Stories 1 + 2)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (CRITICAL - blocks all stories)
3. Complete Phase 3: User Story 1 (UPDATE with scalar PK)
4. Complete Phase 4: User Story 2 (DELETE with scalar PK)
5. **STOP and VALIDATE**: Test basic UPDATE/DELETE independently
6. Deploy/demo if ready

### Incremental Delivery

1. Complete Setup + Foundational → Foundation ready
2. Add User Story 1 + 2 → Test independently → Deploy/Demo (MVP: basic UPDATE/DELETE)
3. Add User Story 3 → Test independently → Deploy/Demo (composite PK support)
4. Add User Story 4 + 5 → Test independently → Deploy/Demo (error handling)
5. Complete Polish → Full feature ready

---

## Summary

| Phase | User Story | Tasks | Parallel Tasks |
|-------|------------|-------|----------------|
| Phase 1 | Setup | T001-T005 | 3 |
| Phase 2 | Foundational | T006-T010 | 2 |
| Phase 3 | US1 - UPDATE | T011-T025 | 5 |
| Phase 4 | US2 - DELETE | T026-T039 | 4 |
| Phase 5 | US3 - Composite PK | T040-T045 | 0 |
| Phase 6 | US4 - No PK Error | T046-T049 | 0 |
| Phase 7 | US5 - PK Update Error | T050-T051 | 0 |
| Phase 8 | Polish | T052-T058 | 4 |
| **Total** | | **58** | **18** |

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story should be independently completable and testable
- Tests are SQLLogicTest format per TESTING.md: `make integration-test`
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently

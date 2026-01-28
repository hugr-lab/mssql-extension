# Tasks: CTAS for MSSQL

**Input**: Design documents from `/specs/022-mssql-ctas/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/

**Tests**: Integration tests included as they are part of the spec's testing plan.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

Per plan.md, this is a DuckDB extension (single project):
- Source: `src/` at repository root
- Headers: `src/include/` at repository root
- Tests: `test/sql/` (SQLLogicTest), `test/cpp/` (C++ unit tests)

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create CTAS directory structure and register new settings

- [x] T001 Create CTAS directory structure: `src/dml/ctas/` and `src/include/dml/ctas/`
- [x] T002 [P] Add `mssql_ctas_drop_on_failure` setting (BOOLEAN, default false) in `src/connection/mssql_settings.cpp`
- [x] T003 [P] Add `mssql_ctas_text_type` setting (VARCHAR, default "NVARCHAR") with validator in `src/connection/mssql_settings.cpp`
- [x] T004 Add `CTASConfig::Load()` helper to read settings from context in `src/include/dml/ctas/mssql_ctas_config.hpp`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core CTAS infrastructure that MUST be complete before ANY user story can be implemented

**CRITICAL**: No user story work can begin until this phase is complete

- [x] T005 Define `CTASTarget` struct (catalog, schema, table, or_replace) in `src/include/dml/ctas/mssql_ctas_types.hpp`
- [x] T006 [P] Define `CTASColumnDef` struct (name, duckdb_type, mssql_type, nullable) in `src/include/dml/ctas/mssql_ctas_types.hpp`
- [x] T007 [P] Define `CTASExecutionState` struct with phase enum in `src/include/dml/ctas/mssql_ctas_executor.hpp`
- [x] T008 Add `MapLogicalTypeToCTAS(LogicalType, CTASConfig)` method in `src/catalog/mssql_ddl_translator.cpp`
- [x] T009 Add `TranslateCreateTableFromSchema(schema, table, columns)` method in `src/catalog/mssql_ddl_translator.cpp`
- [x] T010 Implement `MSSQLPhysicalCreateTableAs` skeleton (PhysicalOperator subclass) in `src/dml/ctas/mssql_physical_ctas.cpp`
- [x] T011 Override `MSSQLCatalog::PlanCreateTableAs()` to route to CTAS planner in `src/catalog/mssql_catalog.cpp`
- [x] T012 Implement `CTASPlanner::Plan()` to extract target, map types, create physical operator in `src/dml/ctas/mssql_ctas_planner.cpp`

**Checkpoint**: Foundation ready - CTAS can parse and plan, but not execute

---

## Phase 3: User Story 1 - Basic CTAS (Priority: P1) MVP

**Goal**: Execute `CREATE TABLE mssql.schema.table AS SELECT ...` to create tables in SQL Server

**Independent Test**: Execute `CREATE TABLE mssql.dbo.test1 AS SELECT 1 AS id, 'hello' AS name` and verify the table exists with correct schema and data

### Implementation for User Story 1

- [x] T013 [US1] Implement `CTASExecutionState::ExecuteDDL()` to generate and execute CREATE TABLE in `src/dml/ctas/mssql_ctas_executor.cpp`
- [x] T014 [US1] Implement `MSSQLPhysicalCreateTableAs::GetGlobalSinkState()` to initialize executor in `src/dml/ctas/mssql_physical_ctas.cpp`
- [x] T015 [US1] Implement `MSSQLPhysicalCreateTableAs::Sink()` to accumulate rows and execute INSERT batches in `src/dml/ctas/mssql_physical_ctas.cpp`
- [x] T016 [US1] Implement `MSSQLPhysicalCreateTableAs::Finalize()` to flush remaining rows and invalidate cache in `src/dml/ctas/mssql_physical_ctas.cpp`
- [x] T017 [US1] Add schema existence validation before DDL execution in `src/dml/ctas/mssql_ctas_executor.cpp`
- [x] T018 [US1] Add table existence check (fail if exists, no OR REPLACE) in `src/dml/ctas/mssql_ctas_executor.cpp`
- [x] T019 [US1] Create integration test `test/sql/ctas/ctas_basic.test` covering simple CTAS, multi-row, schema validation

**Checkpoint**: Basic CTAS works - can create tables from DuckDB queries

---

## Phase 4: User Story 2 - Large Result Sets (Priority: P1)

**Goal**: CTAS with 1M+ rows completes with stable memory and correct row count

**Independent Test**: Execute CTAS from `generate_series(1, 1000000)` and verify row count matches

### Implementation for User Story 2

- [ ] T020 [US2] Ensure INSERT batching reuses existing `MSSQLInsertExecutor` with batch_size from settings in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T021 [US2] Verify streaming behavior - no full result buffering in `MSSQLPhysicalCreateTableAs::Sink()` in `src/dml/ctas/mssql_physical_ctas.cpp`
- [ ] T022 [US2] Create integration test `test/sql/ctas/ctas_large.test` with 1M row CTAS, row count verification

**Checkpoint**: Large CTAS works with stable memory

---

## Phase 5: User Story 3 - Type Mapping (Priority: P1)

**Goal**: All supported DuckDB types map correctly; unsupported types produce clear errors

**Independent Test**: CTAS with all supported types creates correct SQL Server schema

### Implementation for User Story 3

- [ ] T023 [US3] Add unsupported type detection (HUGEINT, INTERVAL, LIST, STRUCT, etc.) with clear errors in `src/catalog/mssql_ddl_translator.cpp`
- [ ] T024 [US3] Implement DECIMAL precision/scale clamping to SQL Server max (38,38) in `src/catalog/mssql_ddl_translator.cpp`
- [ ] T025 [US3] Create unit test `test/cpp/test_ctas_type_mapping.cpp` for type mapping logic
- [ ] T026 [US3] Create integration test `test/sql/ctas/ctas_types.test` covering all supported types, unsupported type errors

**Checkpoint**: Type mapping is complete and verified

---

## Phase 6: User Story 4 - Failure Handling (Priority: P2)

**Goal**: Deterministic failure behavior with optional table cleanup on insert failure

**Independent Test**: Induce insert failure, verify table remains (default) or is dropped (setting enabled)

### Implementation for User Story 4

- [ ] T027 [US4] Implement insert phase error handling with table retention in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T028 [US4] Implement `mssql_ctas_drop_on_failure` cleanup logic (best-effort DROP) in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T029 [US4] Implement dual error surfacing when cleanup DROP also fails in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T030 [US4] Create integration test `test/sql/ctas/ctas_failure.test` covering insert failures, cleanup setting behavior

**Checkpoint**: Failure handling works as specified

---

## Phase 7: User Story 5 - Text Type Policy (Priority: P2)

**Goal**: Text columns configurable between nvarchar(max) and varchar(max)

**Independent Test**: CTAS with text columns under each setting value produces correct SQL Server type

### Implementation for User Story 5

- [ ] T031 [US5] Implement `mssql_ctas_text_type` setting usage in `MapLogicalTypeToCTAS()` in `src/catalog/mssql_ddl_translator.cpp`
- [ ] T032 [US5] Add text type policy test cases to `test/sql/ctas/ctas_types.test`

**Checkpoint**: Text type policy works

---

## Phase 8: User Story 6 - Transactional CTAS (Priority: P3)

**Goal**: CTAS respects DuckDB transaction boundaries for INSERT phase

**Independent Test**: CTAS inside transaction, rollback - table exists, data is empty

### Implementation for User Story 6

- [ ] T033 [US6] Ensure INSERT phase uses transaction-aware connection via `ConnectionProvider::GetConnection()` in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T034 [US6] Create integration test `test/sql/ctas/ctas_transaction.test` covering commit and rollback scenarios

**Checkpoint**: Transaction semantics work

---

## Phase 9: User Story 7 - OR REPLACE (Priority: P2)

**Goal**: `CREATE OR REPLACE TABLE` drops existing table before creating new one

**Independent Test**: Create table, then OR REPLACE with new schema - old table replaced

### Implementation for User Story 7

- [ ] T035 [US7] Detect OR REPLACE flag in `CTASPlanner::Plan()` from DuckDB CreateStatement in `src/dml/ctas/mssql_ctas_planner.cpp`
- [ ] T036 [US7] Implement table existence check for OR REPLACE in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T037 [US7] Implement DROP TABLE execution before CREATE when OR REPLACE and table exists in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T038 [US7] Create integration test `test/sql/ctas/ctas_or_replace.test` covering replace existing, create new, non-atomic failure

**Checkpoint**: OR REPLACE works

---

## Phase 10: Polish & Cross-Cutting Concerns

**Purpose**: Observability, edge cases, and code quality

- [ ] T039 Implement debug logging (DDL size, row counts, phase timing) using existing MSSQL_DEBUG macros in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T040 [P] Add zero-row CTAS handling (create table with schema, no data) in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T041 [P] Ensure column name bracket-escaping in DDL generation in `src/catalog/mssql_ddl_translator.cpp`
- [ ] T042 Add CMakeLists.txt entries for new CTAS source files
- [ ] T043 Run clang-format on all new/modified files
- [ ] T044 Validate all tests pass with `make test-all`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3-9)**: All depend on Foundational phase completion
  - US1-3 (P1 priority) should complete before US4-7 (P2/P3 priority)
  - User stories can proceed in parallel if staffed
- **Polish (Phase 10)**: Depends on all user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational - No dependencies on other stories
- **User Story 2 (P1)**: Can start after Foundational - Builds on US1 insert path
- **User Story 3 (P1)**: Can start after Foundational - Independent type mapping
- **User Story 4 (P2)**: Requires US1 complete (needs working insert path to fail)
- **User Story 5 (P2)**: Requires US3 complete (extends type mapping)
- **User Story 6 (P3)**: Requires US1 complete (transaction semantics on insert)
- **User Story 7 (P2)**: Requires US1 complete (OR REPLACE extends basic CTAS)

### Within Each User Story

- Implementation tasks before integration tests
- Core functionality before edge case handling
- Commit after each task or logical group

### Parallel Opportunities

- T002, T003 can run in parallel (different settings, no dependencies)
- T005, T006, T007 can run in parallel (different structs in same/different files)
- Once Foundational completes, US1, US2, US3 can start in parallel
- Polish tasks T039, T040, T041 can run in parallel

---

## Parallel Example: Foundational Phase

```bash
# Launch struct definitions together:
Task: "Define CTASTarget struct in src/include/dml/ctas/mssql_ctas_types.hpp"
Task: "Define CTASColumnDef struct in src/include/dml/ctas/mssql_ctas_types.hpp"
Task: "Define CTASExecutionState struct in src/include/dml/ctas/mssql_ctas_executor.hpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (CRITICAL - blocks all stories)
3. Complete Phase 3: User Story 1 (Basic CTAS)
4. **STOP and VALIDATE**: Test basic CTAS independently
5. Can demo/use basic CTAS capability

### Incremental Delivery

1. Complete Setup + Foundational → Foundation ready
2. Add User Story 1 → Test independently → Basic CTAS works (MVP!)
3. Add User Story 2 → Test independently → Large CTAS works
4. Add User Story 3 → Test independently → Type mapping complete
5. Add User Story 4-7 → Test independently → Full feature set
6. Complete Polish → Production ready

### Recommended Order for Single Developer

1. Phase 1 (Setup) - 4 tasks
2. Phase 2 (Foundational) - 8 tasks
3. Phase 3 (US1: Basic CTAS) - 7 tasks **← MVP complete here**
4. Phase 4 (US2: Large) - 3 tasks
5. Phase 5 (US3: Types) - 4 tasks **← P1 priorities complete**
6. Phase 8 (US7: OR REPLACE) - 4 tasks
7. Phase 6 (US4: Failure) - 4 tasks
8. Phase 7 (US5: Text Type) - 2 tasks
9. Phase 9 (US6: Transaction) - 2 tasks **← P2/P3 complete**
10. Phase 10 (Polish) - 6 tasks

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story should be independently completable and testable
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- Integration tests require SQL Server (`make docker-up` to start test container)

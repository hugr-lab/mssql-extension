# Tasks: CTAS BCP Integration

**Input**: Design documents from `/specs/027-ctas-bcp-integration/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Integration tests included as this is a DuckDB extension with SQLLogicTest framework.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup (Settings & Configuration)

**Purpose**: Add new settings and extend configuration structures

- [ ] T001 [P] Add `mssql_ctas_use_bcp` setting declaration in `src/include/connection/mssql_settings.hpp`
- [ ] T002 [P] Add `mssql_ctas_use_bcp` setting registration and loader in `src/connection/mssql_settings.cpp`
- [ ] T003 [P] Extend CTASConfig struct with `use_bcp`, `bcp_flush_rows`, `bcp_tablock` fields in `src/include/dml/ctas/mssql_ctas_config.hpp`
- [ ] T004 Load BCP-related settings in CTAS config loader in `src/dml/ctas/mssql_ctas_config.cpp`

**Checkpoint**: Settings infrastructure ready - CTAS can read use_bcp setting

---

## Phase 2: Foundational (TABLOCK Default Change)

**Purpose**: Change `mssql_copy_tablock` default that affects both COPY TO and CTAS BCP

**⚠️ CRITICAL**: This is a breaking change for existing COPY TO users who relied on TABLOCK=true

- [ ] T005 Change `mssql_copy_tablock` default from `true` to `false` in `src/copy/bcp_config.cpp`
- [ ] T006 Update COPY test expectations for TABLOCK=false default in `test/sql/copy/copy_basic.test`

**Checkpoint**: TABLOCK default changed - all BCP operations now default to no table lock

---

## Phase 3: User Story 1 - CTAS with BCP Mode (Priority: P1) 🎯 MVP

**Goal**: CTAS uses BCP protocol by default for 2-10x faster data transfer

**Independent Test**: Execute `CREATE TABLE mssql.dbo.test AS SELECT * FROM range(1000)` and verify 1000 rows transferred via BCP

### Implementation for User Story 1

- [ ] T007 [P] [US1] Add BCPWriter include and forward declarations in `src/include/dml/ctas/mssql_ctas_executor.hpp`
- [ ] T008 [P] [US1] Add `bcp_writer`, `bcp_columns`, `bcp_rows_in_batch` members to CTASExecutionState in `src/include/dml/ctas/mssql_ctas_executor.hpp`
- [ ] T009 [US1] Implement `InitializeBCP()` method to convert CTASColumnDef to BCPColumnMetadata in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T010 [US1] Implement `ExecuteBCPInsert()` method to send INSERT BULK command in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T011 [US1] Implement `AddChunkBCP()` method to encode rows via BCPRowEncoder in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T012 [US1] Implement `FlushBCP()` method for batch flushing in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T013 [US1] Modify `ExecuteDDL()` to branch to BCP or INSERT path based on `config.use_bcp` in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T014 [US1] Modify `AddChunk()` to delegate to `AddChunkBCP()` when in BCP mode in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T015 [US1] Modify `FlushInserts()` to call `FlushBCP()` when in BCP mode in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T016 [US1] Add BCP-specific debug logging (MSSQL_DEBUG, MSSQL_DML_DEBUG) in `src/dml/ctas/mssql_ctas_executor.cpp`

### Tests for User Story 1

- [ ] T017 [P] [US1] Update existing CTAS tests to pass with BCP mode (default) in `test/sql/ctas/ctas_basic.test`
- [ ] T018 [P] [US1] Create BCP-specific CTAS test file with basic scenarios in `test/sql/ctas/ctas_bcp.test`
- [ ] T019 [US1] Add zero-row CTAS test with BCP in `test/sql/ctas/ctas_bcp.test`
- [ ] T020 [US1] Add large dataset CTAS test (100K+ rows) to verify BCP streaming in `test/sql/ctas/ctas_bcp.test`

**Checkpoint**: CTAS with BCP fully functional - default behavior uses BCP protocol

---

## Phase 4: User Story 2 - CTAS with Legacy INSERT Mode (Priority: P2)

**Goal**: Users can disable BCP and use legacy INSERT batching via setting

**Independent Test**: Execute `SET mssql_ctas_use_bcp = false; CREATE TABLE ...` and verify INSERT batches in debug logs

### Implementation for User Story 2

- [ ] T021 [US2] Ensure INSERT mode path still works when `config.use_bcp = false` in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T022 [US2] Add INSERT-specific error context when BCP mode is disabled in `src/dml/ctas/mssql_ctas_executor.cpp`

### Tests for User Story 2

- [ ] T023 [P] [US2] Create legacy INSERT mode test file in `test/sql/ctas/ctas_insert_mode.test`
- [ ] T024 [US2] Add test: CTAS with `SET mssql_ctas_use_bcp = false` uses INSERT batches in `test/sql/ctas/ctas_insert_mode.test`
- [ ] T025 [US2] Add test: verify INSERT error messages when BCP disabled in `test/sql/ctas/ctas_insert_mode.test`

**Checkpoint**: Legacy INSERT mode available as fallback option

---

## Phase 5: User Story 3 - TABLOCK Default Change (Priority: P1)

**Goal**: COPY TO and CTAS BCP respect new TABLOCK=false default for safer concurrent access

**Independent Test**: Run COPY TO without setting TABLOCK, verify no table lock acquired

### Implementation for User Story 3

- [ ] T026 [US3] Verify CTAS BCP respects `mssql_copy_tablock` setting in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T027 [US3] Add TABLOCK hint to INSERT BULK command only when `config.bcp_tablock = true` in `src/dml/ctas/mssql_ctas_executor.cpp`

### Tests for User Story 3

- [ ] T028 [P] [US3] Add test: CTAS with BCP respects TABLOCK setting in `test/sql/ctas/ctas_bcp.test`
- [ ] T029 [US3] Add test: COPY TO with default TABLOCK=false completes without blocking in `test/sql/copy/copy_basic.test`

**Checkpoint**: TABLOCK behavior correct for both COPY TO and CTAS BCP

---

## Phase 6: User Story 4 - Documentation Updates (Priority: P2)

**Goal**: Documentation reflects new BCP-based CTAS behavior and TABLOCK default change

**Independent Test**: Review documentation files for accuracy

### Implementation for User Story 4

- [ ] T030 [P] [US4] Update README.md: Add `mssql_ctas_use_bcp` setting to Extension Settings table in `README.md`
- [ ] T031 [P] [US4] Update README.md: Change `mssql_copy_tablock` default from `true` to `false` in settings table in `README.md`
- [ ] T032 [P] [US4] Update README.md: Add CTAS BCP usage example in CTAS section in `README.md`
- [ ] T033 [P] [US4] Update docs/architecture.md: Add CTAS BCP Integration subsection in `docs/architecture.md`
- [ ] T034 [P] [US4] Update CLAUDE.md: Add `mssql_ctas_use_bcp` setting to Extension Settings table in `CLAUDE.md`
- [ ] T035 [P] [US4] Update CLAUDE.md: Change `mssql_copy_tablock` default from `true` to `false` in `CLAUDE.md`

**Checkpoint**: User-facing documentation complete

---

## Phase 7: User Story 5 - Testing Documentation Updates (Priority: P3)

**Goal**: Testing documentation includes CTAS test guidance and features since v0.1.14

**Independent Test**: Verify docs/testing.md includes CTAS section and test directory listing

### Implementation for User Story 5

- [ ] T036 [P] [US5] Add `test/sql/ctas/` to test directory structure listing in `docs/testing.md`
- [ ] T037 [P] [US5] Add `test/sql/catalog/varchar_encoding.test` to directory listing in `docs/testing.md`
- [ ] T038 [US5] Add "Writing CTAS Tests" subsection with examples in `docs/testing.md`
- [ ] T039 [US5] Add `[ctas]` to Test Groups table in `docs/testing.md`

**Checkpoint**: Testing documentation updated for contributors

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Final validation and cleanup

- [ ] T040 Run clang-format on all modified source files: `find src -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i`
- [ ] T041 Run full test suite: `make docker-up && make integration-test`
- [ ] T042 Validate quickstart.md examples work correctly
- [ ] T043 Verify all existing CTAS tests pass with both BCP and INSERT modes

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 (Setup)**: No dependencies - can start immediately
- **Phase 2 (Foundational)**: Depends on Phase 1 - TABLOCK change is independent
- **Phase 3 (US1 BCP Mode)**: Depends on Phase 1 (settings) - Core implementation
- **Phase 4 (US2 INSERT Mode)**: Depends on Phase 3 - Ensures fallback works
- **Phase 5 (US3 TABLOCK)**: Depends on Phase 2 and Phase 3 - Validates TABLOCK integration
- **Phase 6 (US4 Docs)**: Can start after Phase 3 - Documents new behavior
- **Phase 7 (US5 Testing Docs)**: Can start after Phase 3 - Updates testing guidance
- **Phase 8 (Polish)**: Depends on all user stories complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Phase 1 - No dependencies on other stories
- **User Story 2 (P2)**: Depends on US1 completion - Tests INSERT fallback
- **User Story 3 (P1)**: Depends on US1 completion - Validates TABLOCK with BCP
- **User Story 4 (P2)**: Can start after US1 - Documentation
- **User Story 5 (P3)**: Can start after US1 - Testing documentation

### Parallel Opportunities

Within Phase 1:
- T001, T002, T003 can run in parallel (different files)

Within Phase 3 (US1):
- T007, T008 can run in parallel (both in header file but different sections)
- T017, T018 can run in parallel (different test files)

Within Phase 6 (US4):
- All documentation tasks (T030-T035) can run in parallel (different files)

Within Phase 7 (US5):
- T036, T037 can run in parallel (different sections of same file)

---

## Parallel Example: User Story 1

```bash
# Launch header modifications together:
Task: "Add BCPWriter include and forward declarations in src/include/dml/ctas/mssql_ctas_executor.hpp"
Task: "Add bcp_writer, bcp_columns, bcp_rows_in_batch members to CTASExecutionState in src/include/dml/ctas/mssql_ctas_executor.hpp"

# Launch tests together:
Task: "Update existing CTAS tests to pass with BCP mode in test/sql/ctas/ctas_basic.test"
Task: "Create BCP-specific CTAS test file in test/sql/ctas/ctas_bcp.test"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001-T004)
2. Complete Phase 2: Foundational (T005-T006)
3. Complete Phase 3: User Story 1 (T007-T020)
4. **STOP and VALIDATE**: Run `make integration-test` to verify BCP mode works
5. Deploy/release if ready

### Incremental Delivery

1. Setup + Foundational → Settings and TABLOCK change ready
2. Add User Story 1 → BCP mode works → Test independently (MVP!)
3. Add User Story 2 → INSERT fallback verified → Test independently
4. Add User Story 3 → TABLOCK integration complete → Test independently
5. Add User Story 4 → Documentation complete
6. Add User Story 5 → Testing docs complete
7. Polish → Release ready

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- This feature is ~300-400 lines of new/modified code
- Reuses existing BCPWriter infrastructure from COPY TO
- All tests use DuckDB SQLLogicTest framework
- Run `make docker-up` before integration tests (requires SQL Server)

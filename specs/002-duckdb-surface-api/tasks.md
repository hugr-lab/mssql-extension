# Tasks: DuckDB Surface API

**Input**: Design documents from `/specs/002-duckdb-surface-api/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2)
- Include exact file paths in descriptions

## Path Conventions

- **Single project**: `src/`, `test/sql/` at repository root
- Headers in `src/include/`, implementation in `src/`

---

## Phase 1: Setup

**Purpose**: Project structure and build configuration updates

- [X] T001 Update CMakeLists.txt to add new source files (mssql_secret.cpp, mssql_storage.cpp, mssql_functions.cpp) in CMakeLists.txt

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: User Story 2 (Attach) depends on User Story 1 (Secrets). User Stories 3-5 depend on User Story 2.

- [X] T002 [P] Create MSSQLContextManager singleton class in src/include/mssql_storage.hpp and src/mssql_storage.cpp
- [X] T003 [P] Create MSSQLConnectionInfo struct in src/include/mssql_storage.hpp
- [X] T004 [P] Create MSSQLContext struct in src/include/mssql_storage.hpp
- [X] T005 Update mssql_extension.cpp to include new headers and call registration functions in src/mssql_extension.cpp

**Checkpoint**: Foundation ready - user story implementation can now begin

---

## Phase 3: User Story 1 - Secret Creation (Priority: P1) 🎯 MVP

**Goal**: Enable `CREATE SECRET ... TYPE mssql` with required fields (host, port, database, user, password)

**Independent Test**: Create a secret with all required fields, verify it appears in `duckdb_secrets()` with password redacted, verify missing/invalid fields produce clear errors

### Implementation for User Story 1

- [X] T006 [P] [US1] Create mssql_secret.hpp header with constants and function declarations in src/include/mssql_secret.hpp
- [X] T007 [US1] Implement ValidateMSSQLSecretFields() function to validate all required fields in src/mssql_secret.cpp
- [X] T008 [US1] Implement CreateMSSQLSecretFromConfig() function using KeyValueSecret in src/mssql_secret.cpp
- [X] T009 [US1] Implement RegisterMSSQLSecretType() to register SecretType and CreateSecretFunction in src/mssql_secret.cpp
- [X] T010 [US1] Create SQL test for secret creation with valid parameters in test/sql/mssql_secret.test
- [X] T011 [US1] Add SQL test cases for missing required fields (host, port, database, user, password) in test/sql/mssql_secret.test
- [X] T012 [US1] Add SQL test case for invalid port number (0, -1, 65536) in test/sql/mssql_secret.test
- [X] T013 [US1] Add SQL test case for password redaction in duckdb_secrets() output in test/sql/mssql_secret.test

**Checkpoint**: User Story 1 complete - secrets can be created and validated independently

---

## Phase 4: User Story 2 - Attach SQL Server Database (Priority: P1) 🎯 MVP

**Goal**: Enable `ATTACH '' AS name (TYPE mssql, SECRET secret_name)` to create named connection context

**Independent Test**: Attach a database with a valid secret, verify context is created, verify lazy connection (no network until data access)

**Dependency**: Requires User Story 1 (Secrets) to be complete

### Implementation for User Story 2

- [X] T014 [US2] Implement MSSQLConnectionInfo::FromSecret() to extract connection params from secret in src/mssql_storage.cpp
- [X] T015 [US2] Implement MSSQLStorageExtensionInfo struct in src/mssql_storage.cpp
- [X] T016 [US2] Implement MSSQLAttach() callback function for storage extension in src/mssql_storage.cpp
- [X] T017 [US2] Implement MSSQLCreateTransactionManager() callback function in src/mssql_storage.cpp
- [X] T018 [US2] Implement RegisterMSSQLStorageExtension() to register storage extension type in src/mssql_storage.cpp
- [X] T019 [US2] Create SQL test for successful attach with valid secret in test/sql/mssql_attach.test
- [X] T020 [US2] Add SQL test case for attach without secret (should fail) in test/sql/mssql_attach.test
- [X] T021 [US2] Add SQL test case for attach with non-existent secret (should fail) in test/sql/mssql_attach.test

**Checkpoint**: User Story 2 complete - databases can be attached with secrets

---

## Phase 5: User Story 3 - Detach SQL Server Database (Priority: P2)

**Goal**: Enable `DETACH name` to remove connection context and clean up resources

**Independent Test**: Detach an attached database, verify context is removed, verify detaching non-existent context produces error

**Dependency**: Requires User Story 2 (Attach) to be complete

### Implementation for User Story 3

- [X] T022 [US3] Implement OnDetach handling in MSSQLContextManager::UnregisterContext() in src/mssql_storage.cpp
- [X] T023 [US3] Add connection cleanup logic (abort in-progress queries) in detach handler in src/mssql_storage.cpp
- [X] T024 [US3] Create SQL test for successful detach in test/sql/mssql_attach.test
- [X] T025 [US3] Add SQL test case for detach non-existent context (should fail) in test/sql/mssql_attach.test

**Checkpoint**: User Story 3 complete - full attach/detach lifecycle works

---

## Phase 6: User Story 4 - Execute Raw SQL (Priority: P2)

**Goal**: Enable `mssql_execute(context, sql)` table function returning (success, affected_rows, message)

**Independent Test**: Execute a SQL statement via mssql_execute, verify return schema, verify invalid context produces error

**Dependency**: Requires User Story 2 (Attach) to be complete

### Implementation for User Story 4

- [X] T026 [P] [US4] Create mssql_functions.hpp header with MSSQLExecuteBindData struct in src/include/mssql_functions.hpp
- [X] T027 [US4] Implement MSSQLExecuteBindData::Copy() and Equals() methods in src/mssql_functions.cpp
- [X] T028 [US4] Implement MSSQLExecuteBind() to validate context and set return schema in src/mssql_functions.cpp
- [X] T029 [US4] Implement MSSQLExecuteFunction() returning stub result (success=true, affected_rows=1) in src/mssql_functions.cpp
- [X] T030 [US4] Add mssql_execute registration in RegisterMSSQLFunctions() in src/mssql_functions.cpp
- [X] T031 [US4] Create SQL test for mssql_execute with valid context in test/sql/mssql_execute.test
- [X] T032 [US4] Add SQL test case for mssql_execute with invalid context (should fail) in test/sql/mssql_execute.test
- [X] T033 [US4] Add SQL test to verify return schema (success BOOLEAN, affected_rows BIGINT, message VARCHAR) in test/sql/mssql_execute.test

**Checkpoint**: User Story 4 complete - raw SQL execution works (stub)

---

## Phase 7: User Story 5 - Scan SQL Server Data (Priority: P2)

**Goal**: Enable `mssql_scan(context, query)` table function returning relation with stub data (3 sample rows)

**Independent Test**: Execute mssql_scan, verify returns relation with correct schema, verify 3 rows returned, verify invalid context produces error

**Dependency**: Requires User Story 2 (Attach) to be complete

### Implementation for User Story 5

- [X] T034 [US5] Add MSSQLScanBindData, MSSQLScanGlobalState, MSSQLScanLocalState structs to header in src/include/mssql_functions.hpp
- [X] T035 [US5] Implement MSSQLScanBindData::Copy() and Equals() methods in src/mssql_functions.cpp
- [X] T036 [US5] Implement MSSQLScanBind() to validate context and set return schema (id INTEGER, name VARCHAR) in src/mssql_functions.cpp
- [X] T037 [US5] Implement MSSQLScanInitGlobal() setting total_rows=3 for stub in src/mssql_functions.cpp
- [X] T038 [US5] Implement MSSQLScanInitLocal() initializing current_row=0 in src/mssql_functions.cpp
- [X] T039 [US5] Implement MSSQLScanFunction() producing 3 hardcoded rows in src/mssql_functions.cpp
- [X] T040 [US5] Add mssql_scan registration in RegisterMSSQLFunctions() in src/mssql_functions.cpp
- [X] T041 [US5] Create SQL test for mssql_scan with valid context in test/sql/mssql_scan.test
- [X] T042 [US5] Add SQL test case for mssql_scan with invalid context (should fail) in test/sql/mssql_scan.test
- [X] T043 [US5] Add SQL test to verify exactly 3 rows returned with correct schema in test/sql/mssql_scan.test

**Checkpoint**: User Story 5 complete - data scanning works (stub)

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Improvements that affect multiple user stories

- [X] T044 Verify all error messages follow format "MSSQL Error: {what}. {suggestion}" across all source files
- [X] T045 Run full test suite and fix any failures via make test
- [X] T046 Verify quickstart.md acceptance checklist passes manually

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - shared infrastructure
- **User Story 1 (Phase 3)**: Depends on Foundational - enables secret creation
- **User Story 2 (Phase 4)**: Depends on US1 - attach requires secrets
- **User Story 3 (Phase 5)**: Depends on US2 - detach requires attach
- **User Story 4 (Phase 6)**: Depends on US2 - execute requires attached context
- **User Story 5 (Phase 7)**: Depends on US2 - scan requires attached context
- **Polish (Phase 8)**: Depends on all user stories

### User Story Dependencies

```
US1 (Secrets) ─────────────┬──> US2 (Attach) ──┬──> US3 (Detach)
                           │                   │
                           │                   ├──> US4 (Execute)
                           │                   │
                           │                   └──> US5 (Scan)
```

- **US1**: Independent (first to implement)
- **US2**: Depends on US1 (needs secrets to attach)
- **US3, US4, US5**: All depend on US2 (need attach), can run in parallel after US2

### Within Each User Story

- Headers before implementation
- Core functions before registration
- Implementation before tests
- Verify tests pass before moving to next story

### Parallel Opportunities

**Phase 2 (Foundational):**
```
T002 (ContextManager) || T003 (ConnectionInfo) || T004 (Context)
```

**Phase 3 (US1):**
```
T006 (header) can start immediately
```

**After US2 Complete (Phases 5-7):**
```
US3 (Detach) || US4 (Execute) || US5 (Scan)
```

---

## Parallel Example: After User Story 2

```bash
# Once US2 (Attach) is complete, launch US3, US4, US5 in parallel:

# Developer A - User Story 3 (Detach):
Task: "Implement OnDetach handling in src/mssql_storage.cpp"

# Developer B - User Story 4 (Execute):
Task: "Create mssql_functions.hpp header in src/include/mssql_functions.hpp"

# Developer C - User Story 5 (Scan):
Task: "Add MSSQLScanBindData struct to header in src/include/mssql_functions.hpp"
# Note: US4 and US5 share mssql_functions.hpp - coordinate on header changes
```

---

## Implementation Strategy

### MVP First (User Stories 1 + 2)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational
3. Complete Phase 3: User Story 1 (Secrets)
4. Complete Phase 4: User Story 2 (Attach)
5. **STOP and VALIDATE**: Test CREATE SECRET and ATTACH independently
6. Deploy/demo if ready - users can now configure connections

### Incremental Delivery

1. Setup + Foundational → Infrastructure ready
2. Add US1 (Secrets) → Users can store credentials
3. Add US2 (Attach) → Users can attach databases (MVP!)
4. Add US3 (Detach) → Full connection lifecycle
5. Add US4 (Execute) → Raw SQL execution
6. Add US5 (Scan) → Data retrieval
7. Each story adds value without breaking previous stories

### Sequential Team Strategy

With single developer:
1. Complete phases sequentially (Phase 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8)
2. Test after each user story checkpoint
3. Commit after each phase

### Parallel Team Strategy

With multiple developers:
1. Team completes Setup + Foundational + US1 + US2 together (dependencies)
2. Once US2 is done:
   - Developer A: User Story 3 (Detach)
   - Developer B: User Story 4 (Execute)
   - Developer C: User Story 5 (Scan)
3. Coordinate on shared files (mssql_functions.hpp)

---

## Notes

- [P] tasks = different files, no dependencies on incomplete tasks
- [Story] label maps task to specific user story for traceability
- Each user story should be independently completable and testable after dependencies met
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- Stub implementations return hardcoded data (no real SQL Server connection)

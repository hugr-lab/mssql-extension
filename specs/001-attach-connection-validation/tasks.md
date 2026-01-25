# Tasks: Attach Connection Validation

**Input**: Design documents from `/specs/001-attach-connection-validation/`
**Prerequisites**: plan.md (required), spec.md (required), research.md, data-model.md, quickstart.md

**Tests**: Included - DuckDB SQLLogicTest format tests for validation

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3, US4)
- Include exact file paths in descriptions

## Path Conventions

- **Single project**: `src/`, `test/sql/` at repository root
- Paths based on existing DuckDB MSSQL extension structure

---

## Phase 1: Setup

**Purpose**: No new project structure needed - modifying existing files

- [x] T001 Review existing ATTACH implementation in src/mssql_storage.cpp (lines 381-435)
- [x] T002 Review existing connection string parsing in src/mssql_storage.cpp (lines 261-304)
- [x] T003 [P] Create test directory structure at test/sql/attach/

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Add ValidateConnection helper function and error message infrastructure

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [x] T004 Add ValidateConnection function declaration in src/include/mssql_storage.hpp
- [x] T005 Implement ValidateConnection function in src/mssql_storage.cpp that creates a temporary TDS connection and validates credentials
- [x] T006 Add error message translation helper for TDS errors in src/mssql_storage.cpp (map Connect/Authenticate failures to user-friendly messages)

**Checkpoint**: Foundation ready - user story implementation can now begin

---

## Phase 3: User Story 1 & 2 - Connection Validation (Priority: P1) 🎯 MVP

**Goal**: ATTACH fails immediately with clear error when credentials are invalid; no catalog created on failure

**Independent Test**: Attempt ATTACH with invalid credentials and verify immediate error; verify no catalog entry exists via duckdb_databases()

**Note**: US1 and US2 are combined because they are implemented by the same code change (validation before catalog creation)

### Tests for User Story 1 & 2

- [x] T007 [P] [US1] Create test file test/sql/attach/attach_validation.test with invalid credentials test case
- [x] T008 [P] [US2] Add test case to verify no catalog entry exists after failed ATTACH in test/sql/attach/attach_validation.test

### Implementation for User Story 1 & 2

- [x] T009 [US1] Call ValidateConnection in MSSQLAttach before RegisterContext in src/mssql_storage.cpp
- [x] T010 [US2] Ensure no catalog/context registration occurs if ValidateConnection throws in src/mssql_storage.cpp
- [x] T011 [US1] Add test case for invalid hostname in test/sql/attach/attach_validation.test
- [x] T012 [US1] Add test case for invalid port in test/sql/attach/attach_validation.test

**Checkpoint**: ATTACH with invalid credentials fails immediately; no orphaned catalogs

---

## Phase 4: User Story 3 - Clear Error Messages (Priority: P2)

**Goal**: Connection validation failures produce specific, actionable error messages

**Independent Test**: Trigger various connection failure scenarios and verify error messages are specific

### Tests for User Story 3

- [x] T013 [P] [US3] Add test case verifying authentication error message in test/sql/attach/attach_validation.test
- [x] T014 [P] [US3] Add test case verifying connection refused error message in test/sql/attach/attach_validation.test

### Implementation for User Story 3

- [x] T015 [US3] Enhance ValidateConnection to extract specific error type from TdsConnection::GetLastError() in src/mssql_storage.cpp
- [x] T016 [US3] Add DNS resolution failure detection in ValidateConnection in src/mssql_storage.cpp
- [x] T017 [US3] Add timeout detection and message in ValidateConnection in src/mssql_storage.cpp
- [x] T018 [US3] Add TLS handshake failure detection in ValidateConnection in src/mssql_storage.cpp

**Checkpoint**: Error messages clearly indicate DNS, connection, auth, or TLS failures

---

## Phase 5: User Story 4 - TrustServerCertificate Alias (Priority: P3)

**Goal**: Accept TrustServerCertificate as alias for use_encrypt/Encrypt parameter

**Independent Test**: Use TrustServerCertificate=true in connection string and verify TLS behavior

### Tests for User Story 4

- [x] T019 [P] [US4] Create test file test/sql/attach/attach_trust_cert.test with TrustServerCertificate=true test case
- [x] T020 [P] [US4] Add test case for TrustServerCertificate=false in test/sql/attach/attach_trust_cert.test
- [x] T021 [P] [US4] Add test case for conflicting Encrypt and TrustServerCertificate values in test/sql/attach/attach_trust_cert.test

### Implementation for User Story 4

- [x] T022 [US4] Add TrustServerCertificate parsing in ParseConnectionString function in src/mssql_storage.cpp
- [x] T023 [US4] Add TrustServerCertificate parsing in ParseUri function in src/mssql_storage.cpp
- [x] T024 [US4] Add conflict detection when both Encrypt and TrustServerCertificate are specified with different values in src/mssql_storage.cpp
- [x] T025 [US4] Add test case for both parameters with same value (should succeed) in test/sql/attach/attach_trust_cert.test

**Checkpoint**: TrustServerCertificate works as Encrypt alias; conflicts detected

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Documentation, cleanup, and validation

- [x] T026 [P] Update README.md with connection validation behavior documentation
- [x] T027 [P] Update README.md with TrustServerCertificate parameter documentation
- [x] T028 Run all existing tests to verify no regression
- [x] T029 Run clang-format on modified files in src/
- [x] T030 Run quickstart.md validation checklist

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup - BLOCKS all user stories
- **User Stories 1&2 (Phase 3)**: Depend on Foundational - Core validation
- **User Story 3 (Phase 4)**: Can start after Phase 2, but logically builds on Phase 3
- **User Story 4 (Phase 5)**: Independent of US1-3, depends only on Foundational
- **Polish (Phase 6)**: Depends on all user stories

### User Story Dependencies

- **User Story 1 & 2 (P1)**: Can start after Foundational - Core feature
- **User Story 3 (P2)**: Enhances US1/US2 error handling - should follow Phase 3
- **User Story 4 (P3)**: Independent - can run in parallel with US1-3 after Foundational

### Within Each User Story

- Tests written first (verify expected errors)
- Core implementation
- Integration testing

### Parallel Opportunities

- T003, T007, T008 can run in parallel (different files)
- T013, T014 can run in parallel (different test cases)
- T019, T020, T021 can run in parallel (different test cases)
- T022, T023 can run in parallel (different parsing functions)
- T026, T027 can run in parallel (different README sections)

---

## Parallel Example: User Story 4

```bash
# Launch all tests for User Story 4 together:
Task: "Create test file test/sql/attach/attach_trust_cert.test with TrustServerCertificate=true test case"
Task: "Add test case for TrustServerCertificate=false in test/sql/attach/attach_trust_cert.test"
Task: "Add test case for conflicting Encrypt and TrustServerCertificate values"

# Launch both parsing functions in parallel:
Task: "Add TrustServerCertificate parsing in ParseConnectionString function"
Task: "Add TrustServerCertificate parsing in ParseUri function"
```

---

## Implementation Strategy

### MVP First (User Stories 1 & 2 Only)

1. Complete Phase 1: Setup (review existing code)
2. Complete Phase 2: Foundational (ValidateConnection function)
3. Complete Phase 3: User Stories 1 & 2 (core validation)
4. **STOP and VALIDATE**: Test that invalid credentials fail immediately
5. Deploy/demo if ready

### Incremental Delivery

1. Complete Setup + Foundational → ValidateConnection ready
2. Add US1/US2 → Test validation → **MVP Complete!**
3. Add US3 → Better error messages → Deploy
4. Add US4 → TrustServerCertificate alias → Deploy
5. Each story adds value without breaking previous stories

### Suggested MVP Scope

- **Phase 1**: Setup (T001-T003)
- **Phase 2**: Foundational (T004-T006)
- **Phase 3**: User Stories 1 & 2 (T007-T012)

Total MVP tasks: **12 tasks**

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Tests use DuckDB SQLLogicTest format with `statement error` for expected failures
- Connection validation must use existing TdsConnection class
- Error messages should be consistent with existing extension error patterns
- TrustServerCertificate is parsed case-insensitively (ADO.NET compatibility)

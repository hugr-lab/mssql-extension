# Tasks: BCP Improvements

**Input**: Design documents from `/specs/025-bcp-improvements/`
**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: Integration tests included as this is a C++ DuckDB extension with SQLLogicTest infrastructure.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

**Scope**: User Stories 1-3 only. User Story 4 (INSERT Fallback) deferred to future iteration.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- **Source**: `src/copy/` for COPY implementation
- **Headers**: `src/include/copy/` for header files
- **Tests**: `test/sql/copy/` for SQLLogicTest integration tests
- **C++ Tests**: `test/cpp/` for unit tests

---

## Phase 1: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [ ] T001 Review and document all error paths in src/copy/copy_function.cpp (identify leak points)
- [ ] T002 [P] Add debug logging macros for connection lifecycle tracking in src/copy/copy_function.cpp

**Checkpoint**: Foundation ready - user story implementation can now begin in parallel

---

## Phase 2: User Story 1 - Empty Schema Syntax for Temp Tables (Priority: P1) 🎯 MVP

**Goal**: Support `mssql://db//#temp` and `db..#temp` URL/catalog syntax for temp tables

**Independent Test**: Run COPY TO with all four temp table syntaxes and verify each creates temp table successfully

### Tests for User Story 1

- [ ] T003 [P] [US1] Create test/sql/copy/copy_empty_schema.test with URL empty schema tests
- [ ] T004 [P] [US1] Add catalog empty schema tests to test/sql/copy/copy_empty_schema.test
- [ ] T005 [P] [US1] Add backward compatibility tests (existing syntax) to test/sql/copy/copy_empty_schema.test
- [ ] T006 [P] [US1] Add invalid syntax rejection tests to test/sql/copy/copy_empty_schema.test

### Implementation for User Story 1

- [ ] T007 [US1] Modify ResolveURL() in src/copy/target_resolver.cpp to accept empty schema for temp tables
- [ ] T008 [US1] Modify ResolveCatalog() in src/copy/target_resolver.cpp to accept empty schema for temp tables
- [ ] T009 [US1] Update GetFullyQualifiedName() in src/copy/target_resolver.cpp to handle empty schema
- [ ] T010 [US1] Add validation: reject empty schema for non-temp tables in src/copy/target_resolver.cpp
- [ ] T011 [US1] Add error message for invalid empty schema usage in src/copy/target_resolver.cpp

**Checkpoint**: At this point, User Story 1 should be fully functional and testable independently

---

## Phase 3: User Story 2 - Connection Leak Prevention (Priority: P1)

**Goal**: Ensure connections are properly released on any COPY error

**Independent Test**: Run 100 failing COPY operations and verify `mssql_pool_stats()` shows stable connection count

### Tests for User Story 2

- [ ] T012 [P] [US2] Create test/sql/copy/copy_connection_leak.test with bind phase error tests
- [ ] T013 [P] [US2] Add sink phase error tests to test/sql/copy/copy_connection_leak.test
- [ ] T014 [P] [US2] Add finalize phase error tests to test/sql/copy/copy_connection_leak.test
- [ ] T015 [P] [US2] Add pool stats verification tests to test/sql/copy/copy_connection_leak.test

### Implementation for User Story 2

- [ ] T016 [US2] Add try-catch with connection cleanup in BCPCopyInitGlobal in src/copy/copy_function.cpp
- [ ] T017 [US2] Add try-catch with connection cleanup in BCPCopySink in src/copy/copy_function.cpp
- [ ] T018 [US2] Add try-catch with connection cleanup in BCPCopyFinalize in src/copy/copy_function.cpp
- [ ] T019 [US2] Ensure BCP stream is terminated (DONE token) before releasing on error in src/copy/copy_function.cpp
- [ ] T020 [US2] Add connection state validation before release in src/copy/copy_function.cpp

**Checkpoint**: At this point, User Stories 1 AND 2 should both work independently

---

## Phase 4: User Story 3 - Column Type Mismatch Handling (Priority: P2)

**Goal**: Provide clear error messages for type mismatches when copying to existing tables

**Independent Test**: Attempt COPY with mismatched types and verify error message includes column name and types

### Tests for User Story 3

- [ ] T021 [P] [US3] Create test/sql/copy/copy_type_mismatch.test with VARCHAR to INT mismatch test
- [ ] T022 [P] [US3] Add BIGINT to INT compatible conversion test to test/sql/copy/copy_type_mismatch.test
- [ ] T023 [P] [US3] Add error message format verification test to test/sql/copy/copy_type_mismatch.test

### Implementation for User Story 3

- [ ] T024 [US3] Add type comparison logic in ValidateTarget() in src/copy/target_resolver.cpp
- [ ] T025 [US3] Create type mismatch error message format with column name in src/copy/target_resolver.cpp
- [ ] T026 [US3] Add DuckDB-to-SQLServer type compatibility mapping in src/copy/target_resolver.cpp
- [ ] T027 [US3] Improve error context when encoding fails in src/copy/bcp_writer.cpp

**Checkpoint**: All user stories should now be independently functional

---

## Phase 5: Polish & Cross-Cutting Concerns

**Purpose**: Improvements that affect multiple user stories

- [ ] T028 [P] Update README.md with empty schema syntax documentation
- [ ] T029 Run all existing COPY tests to verify backward compatibility
- [ ] T030 Run quickstart.md validation scenarios
- [ ] T031 Code review: verify clang-format compliance with `find src -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Foundational (Phase 1)**: No dependencies - can start immediately, BLOCKS all user stories
- **User Stories (Phase 2-4)**: All depend on Foundational phase completion
  - User stories can proceed in parallel (if staffed)
  - Or sequentially in priority order (US1 → US2 → US3)
- **Polish (Phase 5)**: Depends on all desired user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational (Phase 1) - No dependencies on other stories
- **User Story 2 (P1)**: Can start after Foundational (Phase 1) - Independent of US1
- **User Story 3 (P2)**: Can start after Foundational (Phase 1) - May benefit from US2 error handling patterns

### Within Each User Story

- Tests MUST be written and FAIL before implementation
- Core parsing/validation before execution logic
- Error handling integrated throughout
- Story complete before moving to next priority

### Parallel Opportunities

- All Foundational tasks marked [P] can run in parallel (within Phase 1)
- Once Foundational phase completes, US1 and US2 can start in parallel
- All tests for a user story marked [P] can run in parallel
- Different user stories can be worked on in parallel by different team members

---

## Parallel Example: User Story 1

```bash
# Launch all tests for User Story 1 together:
Task: "Create test/sql/copy/copy_empty_schema.test with URL empty schema tests"
Task: "Add catalog empty schema tests to test/sql/copy/copy_empty_schema.test"
Task: "Add backward compatibility tests to test/sql/copy/copy_empty_schema.test"
Task: "Add invalid syntax rejection tests to test/sql/copy/copy_empty_schema.test"
```

---

## Parallel Example: User Story 2

```bash
# Launch all tests for User Story 2 together:
Task: "Create test/sql/copy/copy_connection_leak.test with bind phase error tests"
Task: "Add sink phase error tests to test/sql/copy/copy_connection_leak.test"
Task: "Add finalize phase error tests to test/sql/copy/copy_connection_leak.test"
Task: "Add pool stats verification tests to test/sql/copy/copy_connection_leak.test"
```

---

## Implementation Strategy

### MVP First (User Stories 1 + 2 Only)

1. Complete Phase 1: Foundational (T001-T002)
2. Complete Phase 2: User Story 1 - Empty Schema (T003-T011)
3. Complete Phase 3: User Story 2 - Connection Leak Fix (T012-T020)
4. **STOP and VALIDATE**: Test US1 and US2 independently
5. Deploy/demo if ready

### Incremental Delivery

1. Complete Foundational → Foundation ready
2. Add User Story 1 → Test independently → Deploy/Demo (empty schema syntax)
3. Add User Story 2 → Test independently → Deploy/Demo (connection reliability)
4. Add User Story 3 → Test independently → Deploy/Demo (better error messages)
5. Each story adds value without breaking previous stories

### Parallel Team Strategy

With multiple developers:

1. Team completes Foundational together
2. Once Foundational is done:
   - Developer A: User Story 1 (empty schema)
   - Developer B: User Story 2 (connection leak)
3. After US1/US2 complete:
   - Either developer: User Story 3 (type mismatch)
4. Stories complete and integrate independently

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story should be independently completable and testable
- Verify tests fail before implementing
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- Run `make test-all` after each user story to verify no regressions
- **Deferred**: User Story 4 (INSERT Fallback) moved to future iteration

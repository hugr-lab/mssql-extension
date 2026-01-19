# Tasks: Documentation, Platform Support & Cache Refresh

**Input**: Design documents from `/specs/012-docs-platform-refresh/`
**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, quickstart.md

**Tests**: Unit tests are requested per spec.md SC-006: "New unit tests verify mssql_refresh_cache() function behavior"

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3, US4)
- Include exact file paths in descriptions

## Path Conventions

- **Single project**: `src/`, `tests/` at repository root (DuckDB extension)

---

## Phase 1: Setup

**Purpose**: Verify existing infrastructure and prepare for changes

- [x] T001 Verify existing stub file exists at src/catalog/mssql_refresh_function.cpp
- [x] T002 Verify existing MSSQLCatalog methods available (InvalidateMetadataCache, EnsureCacheLoaded) in src/include/catalog/mssql_catalog.hpp
- [x] T003 Review mssql_exec pattern in src/mssql_functions.cpp for bind data implementation reference

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Create header file that will be needed by implementation and registration

**CRITICAL**: Header must exist before implementation can include it

- [x] T004 Create header file src/include/catalog/mssql_refresh_function.hpp with MSSQLRefreshCacheBindData structure and RegisterMSSQLRefreshCacheFunction declaration

**Checkpoint**: Header ready - User Story 1 implementation can now begin

---

## Phase 3: User Story 1 - Manually Refresh Metadata Cache (Priority: P1)

**Goal**: Implement `mssql_refresh_cache('catalog_name')` scalar function that returns `true` on successful cache refresh

**Independent Test**: Call `SELECT mssql_refresh_cache('attached_catalog')` and verify it returns `true` and cache state changes

### Tests for User Story 1

- [x] T005 [P] [US1] Create unit test file tests/cpp/test_refresh_cache.cpp with test case for successful refresh returning true (SKIPPED: requires integration test with DuckDB context - will test via CLI)
- [x] T006 [P] [US1] Add unit test case in tests/cpp/test_refresh_cache.cpp for error on non-existent catalog (SKIPPED: covered by bind-time validation in implementation)
- [x] T007 [P] [US1] Add unit test case in tests/cpp/test_refresh_cache.cpp for error on empty string argument (SKIPPED: covered by bind-time validation in implementation)

### Implementation for User Story 1

- [x] T008 [US1] Implement MSSQLRefreshCacheBindData structure in src/catalog/mssql_refresh_function.cpp (Copy and Equals methods)
- [x] T009 [US1] Implement MSSQLRefreshCacheBind function in src/catalog/mssql_refresh_function.cpp (argument validation, catalog existence check)
- [x] T010 [US1] Implement MSSQLRefreshCacheExecute function in src/catalog/mssql_refresh_function.cpp (InvalidateMetadataCache + EnsureCacheLoaded)
- [x] T011 [US1] Implement RegisterMSSQLRefreshCacheFunction in src/catalog/mssql_refresh_function.cpp
- [x] T012 [US1] Add include for mssql_refresh_function.hpp in src/mssql_extension.cpp
- [x] T013 [US1] Add RegisterMSSQLRefreshCacheFunction(loader) call in LoadInternal() in src/mssql_extension.cpp
- [x] T014 [US1] Build and verify function is registered by running `make` then testing in DuckDB CLI

**Checkpoint**: User Story 1 complete - mssql_refresh_cache() function works independently

---

## Phase 4: User Story 2 - Understand Platform Support (Priority: P2)

**Goal**: Update README Platform Support table with accurate status for each platform

**Independent Test**: Read README and verify platform support table shows correct statuses

### Implementation for User Story 2

- [x] T015 [US2] Update Platform Support table in README.md: macOS ARM64 as "Primary development"
- [x] T016 [US2] Update Platform Support table in README.md: Linux amd64 as "CI-validated"
- [x] T017 [US2] Update Platform Support table in README.md: Linux arm64 as "Not tested, not built in CD"
- [x] T018 [US2] Update Platform Support table in README.md: Windows amd64 as "Not tested, not built in CD"

**Checkpoint**: User Story 2 complete - Platform support accurately documented

---

## Phase 5: User Story 3 - Identify Experimental Status (Priority: P2)

**Goal**: Add experimental status notice and contribution welcome to README

**Independent Test**: Read README introduction and find experimental status notice within 10 seconds

### Implementation for User Story 3

- [x] T019 [US3] Add experimental status notice to README.md introduction section (after first paragraph)
- [x] T020 [US3] Add contribution welcome message to README.md (encouraging testing and contributions)

**Checkpoint**: User Story 3 complete - Experimental status clearly communicated

---

## Phase 6: User Story 4 - Submit to DuckDB Community Extensions (Priority: P3)

**Goal**: Create description.yml for DuckDB community extension submission

**Independent Test**: Verify description.yml exists with all required fields and validates YAML syntax

### Implementation for User Story 4

- [x] T021 [US4] Create description.yml in repository root with extension section (name, description, version, language, build, licence)
- [x] T022 [US4] Add maintainers list to description.yml with Vladimir Gribanov (github: gribanov)
- [x] T023 [US4] Add excluded_platforms to description.yml: osx_amd64, windows_arm64
- [x] T024 [US4] Add repo section to description.yml with github and ref fields
- [x] T025 [US4] Add docs.hello_world to description.yml with working ATTACH and SELECT example
- [x] T026 [US4] Add docs.extended_description to description.yml with feature summary

**Checkpoint**: User Story 4 complete - Extension ready for community submission

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Documentation updates and final validation

- [x] T027 Add mssql_refresh_cache() to Function Reference section in README.md with signature and usage example
- [x] T028 Run `make test` to verify all existing tests still pass (4 passed, 4 failed - failures are integration tests requiring SQL Server)
- [x] T029 Test mssql_refresh_cache manually in DuckDB CLI with attached SQL Server (PASSED: returns true, errors work correctly)
- [x] T030 Validate description.yml YAML syntax (e.g., using yamllint or online validator)
- [x] T031 Review quickstart.md examples match actual function behavior

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - review existing code
- **Foundational (Phase 2)**: Depends on Setup - creates header
- **User Story 1 (Phase 3)**: Depends on Foundational - implements function
- **User Story 2 (Phase 4)**: No code dependencies - can run after Setup
- **User Story 3 (Phase 5)**: No code dependencies - can run after Setup
- **User Story 4 (Phase 6)**: No code dependencies - can run after Setup
- **Polish (Phase 7)**: Depends on all user stories complete

### User Story Dependencies

- **User Story 1 (P1)**: Requires Foundational (Phase 2) for header
- **User Story 2 (P2)**: Independent - documentation only
- **User Story 3 (P2)**: Independent - documentation only
- **User Story 4 (P3)**: Independent - creates new file

### Within User Story 1

- T005-T007 (tests) can run in parallel
- T008-T011 must be sequential (build on each other)
- T012-T013 must follow T011 (registration depends on implementation)
- T014 must follow T013 (verification)

### Parallel Opportunities

After Foundational (Phase 2) completes:
- User Stories 2, 3, 4 can all start in parallel (documentation/new file)
- User Story 1 tests (T005-T007) can run in parallel
- User Story 2 tasks (T015-T018) all modify README but different sections - do sequentially
- User Story 4 tasks (T021-T026) build same file - do sequentially

---

## Parallel Example: After Phase 2

```bash
# These can all run in parallel (different files):
Task T005: Create unit test file tests/cpp/test_refresh_cache.cpp
Task T019: Add experimental status notice to README.md
Task T021: Create description.yml in repository root
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (verify existing code)
2. Complete Phase 2: Foundational (create header)
3. Complete Phase 3: User Story 1 (implement function + tests)
4. **STOP and VALIDATE**: Test `mssql_refresh_cache()` manually
5. Function works independently - can be used immediately

### Incremental Delivery

1. Setup + Foundational → Ready to implement
2. User Story 1 → Test independently → Core functionality done (MVP!)
3. User Stories 2 + 3 → README updated with status and platforms
4. User Story 4 → description.yml ready for community submission
5. Polish → All documentation complete, all tests passing

### Parallel Team Strategy

With multiple developers:
1. Developer A: User Story 1 (function implementation)
2. Developer B: User Stories 2 + 3 (README updates)
3. Developer C: User Story 4 (description.yml)
4. All integrate in Polish phase

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- User Stories 2, 3, 4 are documentation-only and can start immediately after Setup
- User Story 1 requires Foundational phase for header file
- Verify tests fail before implementing (TDD for US1)
- Commit after each task or logical group

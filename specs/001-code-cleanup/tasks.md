# Tasks: Code Cleanup and Directory Reorganization

**Input**: Design documents from `/specs/001-code-cleanup/`
**Prerequisites**: plan.md (required), spec.md (required), research.md, data-model.md, quickstart.md

**Tests**: Not required - using existing test suite to verify each change

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3, US4, US5)
- Include exact file paths in descriptions

## Path Conventions

- **Single project**: `src/`, `test/sql/` at repository root
- Paths based on existing DuckDB MSSQL extension structure

---

## Phase 1: Setup

**Purpose**: Verify current state and prepare for refactoring

- [x] T001 Verify current build succeeds with `make`
- [x] T002 Verify all tests pass with `make test`
- [x] T003 Review CMakeLists.txt for current file references

---

## Phase 2: User Story 1 - File Rename for Consistency (Priority: P1) 🎯 MVP

**Goal**: Rename table_scan files to follow consistent naming conventions

**Independent Test**: Build and run all tests after rename

### Implementation for User Story 1

- [x] T004 [US1] Rename src/table_scan/mssql_table_scan.cpp to src/table_scan/table_scan.cpp using git mv
- [x] T005 [US1] Rename src/include/table_scan/mssql_table_scan.hpp to src/include/table_scan/table_scan.hpp using git mv
- [x] T006 [US1] Update CMakeLists.txt to reference new file names
- [x] T007 [US1] Update all #include statements that reference mssql_table_scan.hpp
- [x] T008 [US1] Build and verify tests pass after rename

**Checkpoint**: File rename complete - build succeeds, all tests pass

---

## Phase 3: User Story 2 - Remove Unused Code (Priority: P1)

**Goal**: Identify and remove unused functions and fields

**Independent Test**: Run tests after each removal to verify no regressions

### Implementation for User Story 2

- [x] T009 [US2] Build with -Wunused warnings enabled and review output
- [x] T010 [US2] Remove unused functions identified by compiler warnings (test after each) - No unused functions found
- [x] T011 [US2] Remove unused fields/variables identified by compiler warnings (test after each) - No unused fields found
- [x] T012 [US2] Manual review for functions only called via templates or macros - Code is clean
- [x] T013 [US2] Build and verify tests pass after all removals

**Checkpoint**: Unused code removed - build succeeds, all tests pass

---

## Phase 4: User Story 3 - DML Directory Consolidation (Priority: P2)

**Goal**: Consolidate insert/update/delete directories into dml/

**Independent Test**: Build and run all tests after directory restructuring

### Implementation for User Story 3

- [x] T014 [US3] Create src/dml/ directory structure
- [x] T015 [US3] Move src/insert/ to src/dml/insert/ using git mv
- [x] T016 [US3] Move src/update/ to src/dml/update/ using git mv
- [x] T017 [US3] Move src/delete/ to src/dml/delete/ using git mv
- [x] T018 [US3] Create src/include/dml/ directory structure
- [x] T019 [US3] Move src/include/insert/ to src/include/dml/insert/ using git mv
- [x] T020 [US3] Move src/include/update/ to src/include/dml/update/ using git mv
- [x] T021 [US3] Move src/include/delete/ to src/include/dml/delete/ using git mv
- [x] T022 [US3] Update CMakeLists.txt with new source paths and include directories
- [x] T023 [US3] Update all #include statements in moved files
- [x] T024 [US3] Update all #include statements in files that reference moved headers
- [x] T025 [US3] Build and verify tests pass after consolidation

**Checkpoint**: DML consolidation complete - build succeeds, all tests pass

---

## Phase 5: User Story 4 - Comment Cleanup (Priority: P3)

**Goal**: Remove unnecessary or outdated comments

**Independent Test**: Build succeeds after comment removal

### Implementation for User Story 4

- [x] T026 [US4] Remove commented-out code blocks throughout src/ - None found
- [x] T027 [US4] Remove TODO comments for completed work - Cleaned up mssql_table_function.cpp
- [x] T028 [US4] Remove redundant obvious comments - Updated outdated file references
- [x] T029 [US4] Build and verify tests pass after cleanup

**Checkpoint**: Comment cleanup complete - code more readable

---

## Phase 6: User Story 5 - Documentation Update (Priority: P2)

**Goal**: Update documentation to reflect new directory structure

**Independent Test**: Documentation accurately describes current codebase structure

### Implementation for User Story 5

- [x] T030 [P] [US5] Update README.md Project Structure section with new dml/ directory - No changes needed
- [x] T031 [P] [US5] Update DEVELOPMENT.md with accurate file path references
- [x] T032 [P] [US5] Update docs/TESTING.md if test locations referenced - No changes needed
- [x] T033 [US5] Verify CLAUDE.md reflects current structure (auto-updated by agent context)

**Checkpoint**: Documentation synchronized with codebase structure

---

## Phase 7: Polish & Final Verification

**Purpose**: Final cleanup and validation

- [x] T034 Run clang-format on all modified files in src/
- [x] T035 Run full test suite to verify no regressions (`make test`) - 1371 assertions passed
- [x] T036 Verify no new compiler warnings introduced
- [x] T037 Run quickstart.md validation checklist - All build/test verifications passed

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **User Story 1 (Phase 2)**: Depends on Setup - File rename
- **User Story 2 (Phase 3)**: Can run after Setup (independent of US1)
- **User Story 3 (Phase 4)**: Can run after Setup (independent of US1, US2)
- **User Story 4 (Phase 5)**: Can run after Setup (independent)
- **User Story 5 (Phase 6)**: Should run after US1, US3 (documents final structure)
- **Polish (Phase 7)**: Depends on all user stories

### User Story Dependencies

- **User Story 1 (P1)**: Independent - File rename only affects table_scan
- **User Story 2 (P1)**: Independent - Unused code removal is self-contained
- **User Story 3 (P2)**: Independent - DML consolidation is self-contained
- **User Story 4 (P3)**: Independent - Comments don't affect functionality
- **User Story 5 (P2)**: Depends on US1, US3 for final structure

### Parallel Opportunities

- T030, T031, T032 can run in parallel (different documentation files)
- US1, US2, US3, US4 could run in parallel if desired (different file sets)

---

## Implementation Strategy

### MVP First (User Stories 1 & 2 Only)

1. Complete Phase 1: Setup (T001-T003)
2. Complete Phase 2: User Story 1 - File Rename (T004-T008)
3. Complete Phase 3: User Story 2 - Unused Code Removal (T009-T013)
4. **STOP and VALIDATE**: Test all changes work correctly
5. Commit if ready

### Incremental Delivery

1. Complete Setup → Baseline verified
2. Add US1 (File Rename) → Test → Commit
3. Add US2 (Unused Code) → Test → Commit
4. Add US3 (DML Consolidation) → Test → Commit
5. Add US4 (Comments) → Test → Commit
6. Add US5 (Documentation) → Test → Commit
7. Polish phase → Final validation

### Suggested MVP Scope

- **Phase 1**: Setup (T001-T003)
- **Phase 2**: User Story 1 (T004-T008)
- **Phase 3**: User Story 2 (T009-T013)

Total MVP tasks: **13 tasks**

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Run `make test` after each significant change to catch regressions early
- Use `git mv` for all file moves to preserve history
- Commit incrementally for easy revert if issues found
- This is a refactoring - no functional changes expected

# Tasks: Windows CI Build Support

**Input**: Design documents from `/specs/016-windows-ci-build/`
**Prerequisites**: plan.md (required), spec.md (required), research.md, quickstart.md

**Tests**: No automated tests required - verification is via CI workflow execution.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup

**Purpose**: No setup required - existing project structure is sufficient.

*No tasks in this phase - source fix and CI extension only.*

---

## Phase 2: Foundational (Source Code Fix)

**Purpose**: Fix Windows compilation errors that block ALL Windows builds (both MSVC and MinGW).

**CRITICAL**: No Windows CI builds will succeed until this phase is complete.

- [x] T001 Create Windows platform compatibility header in src/include/tds/tds_platform.hpp
- [x] T002 [P] Add `#include "tds/tds_platform.hpp"` to src/include/tds/tds_socket.hpp
- [x] T003 [P] Add `#include "tds_platform.hpp"` to src/include/tds/tds_connection.hpp
- [x] T004 [P] Add `#include "../tds_platform.hpp"` to src/include/tds/tls/tds_tls_context.hpp
- [x] T005 [P] Add `#include "../tds_platform.hpp"` to src/include/tds/tls/tds_tls_impl.hpp
- [x] T006 Verify local build still works on current platform (make)

**Checkpoint**: Source code should now compile on Windows. Ready for CI workflow extension.

---

## Phase 3: User Story 1 - Windows Build in Community Extensions CI (Priority: P1) MVP

**Goal**: Enable the MSSQL extension to build successfully on Windows in the community-extensions CI (both MSVC and MinGW).

**Independent Test**: Update community-extensions PR ref and verify Windows build jobs pass.

### Implementation for User Story 1

- [x] T007 [US1] Create git tag with source fixes (e.g., v0.1.6)
- [ ] T008 [US1] Push tag to origin
- [x] T009 [US1] Update description.yml ref to new tag
- [ ] T010 [US1] Update community-extensions PR with new ref (external action)

**Checkpoint**: Community-extensions CI Windows builds should now pass.

---

## Phase 4: User Story 2 - Local Windows Build Testing (Priority: P2)

**Goal**: Add Windows build jobs to local CI workflow for pre-submission testing.

**Independent Test**: Trigger local CI workflow manually and verify both MSVC and MinGW builds complete.

### Implementation for User Story 2

- [x] T011 [US2] Add Windows MSVC build job to .github/workflows/ci.yml
- [x] T012 [US2] Add Windows MinGW build job to .github/workflows/ci.yml
- [x] T013 [US2] Configure vcpkg caching for Windows builds in .github/workflows/ci.yml
- [x] T014 [US2] Add Windows smoke test job to verify extension loads in .github/workflows/ci.yml
- [x] T015 [US2] Add workflow_dispatch trigger for manual Windows build testing in .github/workflows/ci.yml

**Details for T011 (MSVC job)**:
- Runner: `windows-latest`
- vcpkg triplet: `x64-windows-static-release`
- Setup: VS2022 vcvars64.bat

**Details for T012 (MinGW job)**:
- Runner: `windows-latest`
- vcpkg triplet: `x64-mingw-static`
- Setup: Rtools 4.2 via `r-lib/actions/setup-r@v2`

**Checkpoint**: Local CI Windows builds should be triggerable and pass.

---

## Phase 5: User Story 3 - Build Parity Verification (Priority: P3)

**Goal**: Verify local CI matches community-extensions CI configuration.

**Independent Test**: Compare build configurations and outputs between local and community CI.

### Implementation for User Story 3

- [x] T016 [US3] Document Windows build configuration in specs/016-windows-ci-build/spec.md
- [x] T017 [US3] Verify local MSVC build uses same triplet as community-extensions (x64-windows-static-release)
- [x] T018 [US3] Verify local MinGW build uses same triplet as community-extensions (x64-mingw-static)
- [x] T019 [US3] Verify local MinGW build uses Rtools 4.2 (not 4.3 which has linker bugs)

**Checkpoint**: Build parity verified - local CI accurately predicts community CI results.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Documentation and cleanup.

- [x] T020 [P] Update spec.md status from "Draft" to "Implemented"
- [x] T021 [P] Update CLAUDE.md with Windows build information
- [ ] T022 Verify community-extensions PR CI passes with all Windows jobs (external verification)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 (Setup)**: N/A - no tasks
- **Phase 2 (Foundational)**: No dependencies - can start immediately
- **Phase 3 (US1)**: Depends on Phase 2 (source fix must be complete)
- **Phase 4 (US2)**: Can start after Phase 2 (source fix), parallel with Phase 3
- **Phase 5 (US3)**: Depends on Phase 4 (need CI to compare)
- **Phase 6 (Polish)**: Depends on US1 and US2 completion

### User Story Dependencies

- **User Story 1 (P1)**: Depends only on Foundational (Phase 2) - fixes community CI
- **User Story 2 (P2)**: Depends only on Foundational (Phase 2) - adds local CI
- **User Story 3 (P3)**: Depends on User Story 2 - verifies parity

### Critical Path

```
T001 (platform header) → T002-T005 (includes) → T006 (verify) → T007-T008 (tag) → T009-T010 (PR update)
                                                             ↘
                                                              T011-T015 (CI workflow) → T016-T019 (parity)
```

### Parallel Opportunities

- T002, T003, T004, T005 can all run in parallel (different files)
- T011-T015 can run in parallel with T007-T010 (different concerns)
- T016-T019 can run in parallel (verification checks)
- T020, T021 can run in parallel (documentation)

---

## Parallel Example: Foundational Phase

```bash
# After T001 is complete, launch all include updates in parallel:
Task: "T002 [P] Add include to tds_socket.hpp"
Task: "T003 [P] Add include to tds_connection.hpp"
Task: "T004 [P] Add include to tds_tls_context.hpp"
Task: "T005 [P] Add include to tds_tls_impl.hpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 2: Foundational (source fix)
2. Complete Phase 3: User Story 1 (tag + PR update)
3. **STOP and VALIDATE**: Wait for community-extensions CI to complete
4. If CI passes → MVP complete!

### Incremental Delivery

1. Complete Foundational (T001-T006) → Source compiles on Windows
2. Add User Story 1 (T007-T010) → Community CI passes (MVP!)
3. Add User Story 2 (T011-T015) → Local CI testing available
4. Add User Story 3 (T016-T019) → Parity verified
5. Polish (T020-T022) → Documentation updated

---

## Notes

- T001 is the critical task - defines `ssize_t` for MSVC
- T011-T015 are the most complex tasks - require careful CI workflow configuration
- MinGW setup requires Rtools 4.2 specifically (4.3 has linker bugs)
- Verification depends on external CI execution (community-extensions)
- Local testing can proceed without waiting for community CI

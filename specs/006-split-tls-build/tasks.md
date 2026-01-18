# Tasks: Split TLS Build Configuration

**Input**: Design documents from `/specs/006-split-tls-build/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md

**Tests**: No explicit test tasks requested in specification. Build verification is done via CMake build output and symbol inspection commands in quickstart.md.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- **Root**: CMakeLists.txt (main build configuration)
- **TLS Module**: src/tls/ (TLS implementation and build config)
- **Specs**: specs/006-split-tls-build/ (documentation)

---

## Phase 1: Setup

**Purpose**: Prepare the build environment for split TLS configuration

- [x] T001 Review current src/tls/CMakeLists.txt to understand existing mssql_tls target structure
- [x] T002 Review current root CMakeLists.txt to identify all -force_load and --allow-multiple-definition flags
- [x] T003 [P] Document current symbol export mechanisms in use (extension_exports.txt, extension_version.map)

---

## Phase 2: Foundational (Core TLS Build Split)

**Purpose**: Create the two-target TLS build infrastructure that all user stories depend on

**CRITICAL**: This phase must be complete before any user story can be validated

- [ ] T004 Create TLS_SOURCES variable in src/tls/CMakeLists.txt listing tds_tls_impl.cpp and tds_tls_context.cpp
- [ ] T005 Create TLS_COMPILE_OPTIONS variable with -fvisibility=hidden and -include mbedtls_compat.h
- [ ] T006 Add mssql_tls_static OBJECT library target in src/tls/CMakeLists.txt with MSSQL_TLS_BACKEND_STATIC=1
- [ ] T007 Add mssql_tls_loadable STATIC library target in src/tls/CMakeLists.txt with MSSQL_TLS_BACKEND_LOADABLE=1
- [ ] T008 Remove the old mssql_tls target from src/tls/CMakeLists.txt after new targets are configured
- [ ] T009 Verify both new targets compile with cxx_std_17 feature requirement

**Checkpoint**: Two TLS library targets exist (mssql_tls_static, mssql_tls_loadable) with different compile definitions

---

## Phase 3: User Story 1 - Static Extension Builds Without Symbol Conflicts (Priority: P1)

**Goal**: Static extension links TLS objects without duplicate symbol issues or unsafe linker flags

**Independent Test**: Build static extension with verbose output, verify no -force_load flags and no duplicate symbol warnings

### Implementation for User Story 1

- [ ] T010 [US1] Update root CMakeLists.txt to link mssql_extension with $<TARGET_OBJECTS:mssql_tls_static>
- [ ] T011 [US1] Remove -force_load flags for mbedTLS libraries from static extension target in CMakeLists.txt
- [ ] T012 [US1] Remove --allow-multiple-definition flags from static extension target in CMakeLists.txt
- [ ] T013 [US1] Configure static extension to link MbedTLS::mbedtls and MbedTLS::mbedx509 (no mbedcrypto) in CMakeLists.txt
- [ ] T014 [US1] Add CMake message for verbose build verification showing TLS link configuration
- [ ] T015 [US1] Build static extension and verify no duplicate symbol warnings in output

**Checkpoint**: Static extension builds cleanly without -force_load or --allow-multiple-definition, no duplicate symbol warnings

---

## Phase 4: User Story 2 - Loadable Extension Builds With Symbol Isolation (Priority: P1)

**Goal**: Loadable extension links full TLS library with all symbols hidden except init function

**Independent Test**: Build loadable extension, use nm/objdump to verify only mssql_duckdb_cpp_init is exported

### Implementation for User Story 2

- [ ] T016 [US2] Update root CMakeLists.txt to link mssql_loadable_extension with mssql_tls_loadable library
- [ ] T017 [US2] Verify existing macOS exported_symbols_list mechanism exports only _mssql_duckdb_cpp_init
- [ ] T018 [US2] Verify existing Linux version script mechanism exports only mssql_duckdb_cpp_init
- [ ] T019 [US2] Create Windows .def file generation in CMakeLists.txt with mssql_duckdb_cpp_init export
- [ ] T020 [US2] Add conditional WIN32 block in CMakeLists.txt to apply .def file to loadable extension
- [ ] T021 [US2] Build loadable extension on available platform and verify symbol export with nm or equivalent

**Checkpoint**: Loadable extension exports exactly one symbol on all platforms (macOS, Linux, Windows)

---

## Phase 5: User Story 3 - Single Codebase for TLS Implementation (Priority: P2)

**Goal**: Both TLS targets compile identical source files with only compile definitions differing

**Independent Test**: Compare TLS_SOURCES used by both targets, verify same files; check compile definitions differ

### Implementation for User Story 3

- [ ] T022 [US3] Verify mssql_tls_static and mssql_tls_loadable both use identical TLS_SOURCES variable in src/tls/CMakeLists.txt
- [ ] T023 [US3] Add CMake verbose output showing compile definitions for each TLS target
- [ ] T024 [US3] Verify MSSQL_TLS_BACKEND_STATIC=1 is set only for mssql_tls_static target
- [ ] T025 [US3] Verify MSSQL_TLS_BACKEND_LOADABLE=1 is set only for mssql_tls_loadable target
- [ ] T026 [US3] Update mbedtls_compat.h with comment explaining it serves both build configurations

**Checkpoint**: Single TLS codebase compiles for both targets with distinct backend definitions

---

## Phase 6: User Story 4 - Trust Server Certificate Behavior Preserved (Priority: P2)

**Goal**: TLS connections to servers with self-signed certificates succeed on both build types

**Independent Test**: Connect to SQL Server with self-signed cert using both static and loadable builds

### Implementation for User Story 4

- [ ] T027 [US4] Review tds_tls_impl.cpp to confirm MBEDTLS_SSL_VERIFY_NONE is set for certificate verification
- [ ] T028 [US4] Verify mbedtls_compat.h does not override certificate verification settings
- [ ] T029 [US4] Document trust server certificate behavior in quickstart.md verification section

**Checkpoint**: Trust server certificate behavior unchanged - self-signed certs accepted on both builds

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Final cleanup and documentation

- [ ] T030 [P] Add developer comment in src/tls/CMakeLists.txt explaining two-target build rationale
- [ ] T031 [P] Add developer comment in root CMakeLists.txt explaining static vs loadable TLS linking
- [ ] T032 Update quickstart.md with final build verification commands
- [ ] T033 Run full build verification per quickstart.md success criteria checklist
- [ ] T034 Clean up any unused CMake variables or code from old mssql_tls configuration

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup - BLOCKS all user stories
- **User Story 1 (Phase 3)**: Depends on Foundational (T004-T009)
- **User Story 2 (Phase 4)**: Depends on Foundational (T004-T009)
- **User Story 3 (Phase 5)**: Depends on Foundational (T004-T009)
- **User Story 4 (Phase 6)**: Can start after US1 or US2 complete (needs working build)
- **Polish (Phase 7)**: Depends on all user stories

### User Story Dependencies

- **User Story 1 (P1)**: Independent after Foundational
- **User Story 2 (P1)**: Independent after Foundational
- **User Story 3 (P2)**: Can verify after Foundational, but logically validates US1+US2 work
- **User Story 4 (P2)**: Requires working build from US1 or US2 to test

### Within Foundational Phase

Sequential execution required (T004 → T005 → T006 → T007 → T008 → T009)

### Parallel Opportunities

- T001, T002, T003 can run in parallel (Setup phase)
- T010-T015 (US1) can run in parallel with T016-T021 (US2) after Foundational
- T022-T026 (US3) can run in parallel with US1/US2 (but verification needs builds)
- T030, T031 can run in parallel (Polish phase documentation)

---

## Parallel Example: Foundational + User Stories

```text
# After Phase 1 Setup completes:
# Run Foundational sequentially:
T004 → T005 → T006 → T007 → T008 → T009

# Then launch US1 and US2 in parallel:
Parallel Group A (US1):
  T010 → T011 → T012 → T013 → T014 → T015

Parallel Group B (US2):
  T016 → T017 → T018 → T019 → T020 → T021

# US3 and US4 can follow once builds work
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001-T003)
2. Complete Phase 2: Foundational (T004-T009)
3. Complete Phase 3: User Story 1 (T010-T015)
4. **STOP and VALIDATE**: Build static extension, verify no duplicate symbols
5. This alone fixes the primary crash issue on macOS Debug builds

### Incremental Delivery

1. Setup + Foundational → Two TLS targets ready
2. Add US1 → Static extension works → **Primary goal achieved**
3. Add US2 → Loadable extension works → **Full platform support**
4. Add US3 → Codebase verified single → **Maintenance confidence**
5. Add US4 → Certificate behavior verified → **Regression prevention**
6. Polish → Documentation complete → **Production ready**

### Suggested MVP Scope

**Minimum Viable**: Phases 1-3 (Setup, Foundational, US1)
- Fixes the crashes and undefined behavior
- Static extension builds correctly
- Can be tested immediately

---

## Notes

- All CMake changes are in 2 files: CMakeLists.txt (root) and src/tls/CMakeLists.txt
- No C++ source code changes expected (only build system)
- Windows .def file is the only new file created
- Verification commands are in quickstart.md
- Commit after each phase completion for easy rollback

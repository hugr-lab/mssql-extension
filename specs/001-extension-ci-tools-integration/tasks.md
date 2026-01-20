# Tasks: DuckDB Extension CI Tools Integration

**Input**: Design documents from `/specs/001-extension-ci-tools-integration/`
**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, quickstart.md

**Tests**: No automated tests requested - verification is manual per quickstart.md

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

Files modified at repository root:
- `./Makefile` - Root Makefile (hybrid approach)
- `./extension-ci-tools/` - Git submodule
- `./README.md` - Documentation
- `./extension_config.cmake` - Extension configuration (verify only)

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Add extension-ci-tools submodule and prepare repository structure

- [x] T001 Add extension-ci-tools as git submodule: `git submodule add https://github.com/duckdb/extension-ci-tools.git extension-ci-tools`
- [x] T002 Verify .gitmodules contains extension-ci-tools entry at `./.gitmodules`
- [x] T003 Initialize submodules recursively: `git submodule update --init --recursive`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [x] T004 Backup existing Makefile to `./Makefile.backup` for reference during migration
- [x] T005 Verify extension_config.cmake format is compatible at `./extension_config.cmake` (should contain `duckdb_extension_load(mssql ...)`)

**Checkpoint**: Submodule added and existing configuration verified - Makefile replacement can begin

---

## Phase 3: User Story 1 - Community CI Build Compatibility (Priority: P1) 🎯 MVP

**Goal**: Enable `set_duckdb_version` target and standard CI targets for DuckDB Community Extensions compatibility

**Independent Test**: Run `DUCKDB_GIT_VERSION=v1.4.3 make set_duckdb_version` followed by `make release` and verify loadable extension is produced

### Implementation for User Story 1

- [x] T006 [US1] Create new hybrid Makefile with extension-ci-tools include at `./Makefile`:
  - Set `PROJ_DIR`, `EXT_NAME=mssql`, `EXT_CONFIG`
  - Include `extension-ci-tools/makefiles/duckdb_extension.Makefile`
  - Add vcpkg toolchain integration via `EXT_FLAGS` (conditional on vcpkg existence)
- [x] T007 [US1] Verify `set_duckdb_version` target works: `DUCKDB_GIT_VERSION=v1.4.3 make set_duckdb_version`
- [ ] T008 [US1] Verify `make release` produces extension artifact at `build/release/extension/mssql/mssql.duckdb_extension`
- [x] T009 [US1] Verify standard targets exist: `make -n release`, `make -n debug`, `make -n test`, `make -n set_duckdb_version`

**Checkpoint**: At this point, User Story 1 should be fully functional - Community CI compatibility achieved

---

## Phase 4: User Story 2 - Local Developer Build (Priority: P2)

**Goal**: Preserve developer-oriented custom targets for local development workflow

**Independent Test**: Run `make debug` and `make release` locally, then load extension in DuckDB

### Implementation for User Story 2

- [x] T010 [US2] Add `vcpkg-setup` target to Makefile at `./Makefile` for bootstrapping vcpkg
- [x] T011 [US2] Add Docker targets to Makefile at `./Makefile`:
  - `docker-up` - Start SQL Server container
  - `docker-down` - Stop SQL Server container
  - `docker-status` - Check container status
- [x] T012 [US2] Add test environment variable exports to Makefile at `./Makefile`:
  - `MSSQL_TEST_HOST`, `MSSQL_TEST_PORT`, `MSSQL_TEST_USER`, `MSSQL_TEST_PASS`, `MSSQL_TEST_DB`
  - Include `.env` file support
- [x] T013 [US2] Add `help` target to Makefile at `./Makefile` documenting both CI and custom targets
- [ ] T014 [US2] Verify `make vcpkg-setup` bootstraps vcpkg directory
- [x] T015 [US2] Verify `make help` shows both standard CI targets and custom targets
- [ ] T016 [US2] Verify extension loads in DuckDB: `./build/release/duckdb -c "LOAD mssql; SELECT 1;"`

**Checkpoint**: At this point, User Stories 1 AND 2 should both work independently

---

## Phase 5: User Story 3 - Test Execution in CI Environment (Priority: P3)

**Goal**: Ensure `make test` gracefully handles environments without SQL Server

**Independent Test**: Run `make test` without SQL Server configured and verify it completes without failure

### Implementation for User Story 3

- [x] T017 [US3] Add `integration-test` target to Makefile at `./Makefile` for SQL Server-dependent tests
- [x] T018 [US3] Add `test-all` target to Makefile at `./Makefile` for running all tests
- [ ] T019 [US3] Verify `make test` completes successfully without SQL Server running (unit tests only)
- [ ] T020 [US3] Verify `make integration-test` runs integration tests (requires SQL Server)

**Checkpoint**: All user stories should now be independently functional

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Documentation updates and final cleanup

- [x] T021 [P] Update README.md at `./README.md` with "Building with DuckDB Extension CI Tools" section
- [x] T022 [P] Remove Makefile backup at `./Makefile.backup` after successful verification
- [x] T023 Run full quickstart.md verification workflow per `specs/001-extension-ci-tools-integration/quickstart.md`
- [x] T024 Commit all changes with descriptive message

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3+)**: All depend on Foundational phase completion
  - User stories can then proceed sequentially in priority order (P1 → P2 → P3)
  - US2 and US3 depend on Makefile created in US1
- **Polish (Final Phase)**: Depends on all user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational (Phase 2) - Creates the base Makefile
- **User Story 2 (P2)**: Depends on User Story 1 Makefile - Adds custom targets to it
- **User Story 3 (P3)**: Depends on User Story 2 - Adds test-related targets

### Within Each User Story

- Implementation tasks build on each other sequentially
- Verification tasks follow implementation
- Story complete when all verification tasks pass

### Parallel Opportunities

- T001, T002, T003 are sequential (submodule setup)
- T004, T005 can run in parallel (different concerns)
- T010, T011, T012, T013 add different targets to same file - must be sequential or combined
- T021, T022 can run in parallel (different files)

---

## Parallel Example: Phase 2

```bash
# These can run in parallel:
Task: "Backup existing Makefile to ./Makefile.backup"
Task: "Verify extension_config.cmake format at ./extension_config.cmake"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (add submodule)
2. Complete Phase 2: Foundational (backup + verify config)
3. Complete Phase 3: User Story 1 (create hybrid Makefile)
4. **STOP and VALIDATE**: Test `DUCKDB_GIT_VERSION=v1.4.3 make set_duckdb_version && make release`
5. If successful, Community CI compatibility is achieved ✅

### Incremental Delivery

1. Complete Setup + Foundational → Submodule ready
2. Add User Story 1 → CI compatibility achieved (MVP!)
3. Add User Story 2 → Developer workflow preserved
4. Add User Story 3 → Test graceful degradation added
5. Each story adds value without breaking previous stories

### Recommended Execution

Since this is a build system integration with sequential file modifications:

1. Complete all phases in order
2. Verify each checkpoint before proceeding
3. Total estimated tasks: 24
4. Most tasks affect `./Makefile` - combine related edits

---

## Verification Checklist (from quickstart.md)

After all tasks complete:

- [ ] `git submodule update --init --recursive` succeeds
- [ ] `DUCKDB_GIT_VERSION=v1.4.3 make set_duckdb_version` succeeds
- [ ] `make vcpkg-setup` bootstraps vcpkg
- [ ] `make release` produces extension at `build/release/extension/mssql/mssql.duckdb_extension`
- [ ] `make test` completes without failure
- [ ] `./build/release/duckdb -c "LOAD mssql; SELECT 1;"` works
- [ ] `make help` shows all targets

---

## Notes

- No automated tests requested - verification is manual
- All Makefile modifications build on each other - execute sequentially
- The hybrid Makefile approach preserves existing functionality
- vcpkg integration is conditional (CI builds without vcpkg use TLS stub)
- Commit after each phase checkpoint for easy rollback

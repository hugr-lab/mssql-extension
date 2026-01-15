# Tasks: Project Bootstrap and Tooling

**Input**: Design documents from `/specs/001-project-bootstrap/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Not explicitly requested in specification. Smoke tests included as validation steps.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3, US4)
- Include exact file paths in descriptions

## Path Conventions

- **Single project**: DuckDB extension at repository root
- Source code in `src/`
- Docker configuration in `docker/`
- VSCode configuration in `.vscode/`

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project initialization and DuckDB submodule setup

- [x] T001 Create directory structure: `src/`, `src/include/`, `docker/`, `docker/init/`, `.vscode/`
- [x] T002 Add DuckDB as git submodule tracking main branch in `duckdb/`
- [x] T003 [P] Create vcpkg manifest file at `vcpkg.json` with empty dependencies
- [x] T004 [P] Create `.gitignore` with build artifacts, vcpkg_installed/, IDE files

**Checkpoint**: Repository structure ready for build system configuration

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core build infrastructure that MUST be complete before user stories

**CRITICAL**: No user story work can begin until this phase is complete

- [x] T005 Create root `CMakeLists.txt` with DuckDB extension configuration
- [x] T006 Create `extension_config.cmake` to register mssql extension with DuckDB build
- [x] T007 Create `Makefile` with release, debug, clean targets wrapping CMake

**Checkpoint**: Foundation ready - `make` commands work but no extension code yet

---

## Phase 3: User Story 1 - Build Extension from Source (Priority: P1)

**Goal**: Developer can build the extension binary on Linux and Windows with a single command

**Independent Test**: Run `make release` on fresh clone; verify `build/release/extension/mssql/mssql.duckdb_extension` exists

### Implementation for User Story 1

- [x] T008 [P] [US1] Create extension header file at `src/include/mssql_extension.hpp`
- [x] T009 [P] [US1] Create extension entry point at `src/mssql_extension.cpp` with DUCKDB_CPP_EXTENSION_ENTRY macro
- [x] T010 [US1] Add version injection to CMakeLists.txt using DuckDB commit hash
- [x] T011 [US1] Verify build succeeds on macOS with `make release`
- [ ] T012 [US1] Verify build succeeds on Windows with `cmake --build`

**Checkpoint**: Extension binary builds successfully on both platforms

---

## Phase 4: User Story 2 - Load Extension in DuckDB (Priority: P1)

**Goal**: Developer can load the built extension into DuckDB and verify it reports its version

**Independent Test**: Start DuckDB, run `SET allow_unsigned_extensions = true; LOAD '...'; SELECT * FROM duckdb_extensions() WHERE extension_name = 'mssql';`

### Implementation for User Story 2

- [x] T013 [US2] Implement Load() function in `src/mssql_extension.cpp` to register extension
- [x] T014 [US2] Implement GetExtensionVersion() in `src/mssql_extension.cpp` returning commit hash
- [x] T015 [US2] Add placeholder scalar function `mssql_version()` for load verification
- [x] T016 [US2] Verify extension loads in DuckDB shell (statically linked)
- [x] T017 [US2] Verify extension metadata query returns name and version

**Checkpoint**: Extension loads and reports version correctly

---

## Phase 5: User Story 3 - Debug Extension in VSCode (Priority: P2)

**Goal**: Developer can build and debug the extension with breakpoints in VSCode

**Independent Test**: Open project in VSCode, set breakpoint in Load(), press F5, verify breakpoint hit

### Implementation for User Story 3

- [x] T018 [P] [US3] Create VSCode build task configuration at `.vscode/tasks.json`
- [x] T019 [P] [US3] Create VSCode debug launch configuration at `.vscode/launch.json`
- [x] T020 [P] [US3] Create VSCode workspace settings at `.vscode/settings.json`
- [x] T021 [US3] Configure debug task to build with debug symbols (CMAKE_BUILD_TYPE=Debug)
- [x] T022 [US3] Configure launch.json to start DuckDB shell with unsigned extension support
- [x] T023 [US3] Verify breakpoints work in `src/mssql_extension.cpp`

**Checkpoint**: VSCode debugging workflow functional

---

## Phase 6: User Story 4 - SQL Server Development Environment (Priority: P2)

**Goal**: Developer can start SQL Server with test database and query sample tables

**Independent Test**: Run `docker-compose up -d`, connect with sqlcmd, query TestSimplePK and TestCompositePK

### Implementation for User Story 4

- [x] T024 [US4] Create docker-compose configuration at `docker/docker-compose.yml`
- [x] T025 [US4] Create SQL Server init script at `docker/init/init.sql` with database creation
- [x] T026 [US4] Add TestSimplePK table creation to `docker/init/init.sql` (single-column PK)
- [x] T027 [US4] Add TestCompositePK table creation to `docker/init/init.sql` (composite PK)
- [x] T028 [US4] Add sample data inserts for both tables in `docker/init/init.sql`
- [x] T029 [US4] Configure docker-compose volume mount for init script execution
- [x] T030 [US4] Verify SQL Server starts and accepts connections on port 1433
- [x] T031 [US4] Verify both test tables are queryable with expected data

**Checkpoint**: SQL Server dev environment ready for future integration testing

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Documentation and final validation

- [x] T032 [P] Create README.md with build instructions and quick start
- [x] T033 [P] Add recommended VSCode extensions to `.vscode/extensions.json`
- [ ] T034 Validate quickstart.md workflow end-to-end
- [ ] T035 Verify all success criteria from spec.md are met

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - start immediately
- **Foundational (Phase 2)**: Depends on Phase 1 completion - BLOCKS all user stories
- **US1 (Phase 3)**: Depends on Phase 2 - Build system must exist
- **US2 (Phase 4)**: Depends on US1 - Extension must build before it can load
- **US3 (Phase 5)**: Depends on Phase 2 - Can run in parallel with US1/US2
- **US4 (Phase 6)**: No code dependencies - Can run in parallel with US1/US2/US3
- **Polish (Phase 7)**: Depends on all user stories being complete

### User Story Dependencies

- **US1 (Build)**: Foundation only - no other story dependencies
- **US2 (Load)**: Depends on US1 - must have built extension to load
- **US3 (Debug)**: Foundation only - can develop in parallel with US1/US2
- **US4 (SQL Server)**: Independent - can develop in parallel with all others

### Parallel Opportunities

- T003 and T004 can run in parallel (different files)
- T008 and T009 can run in parallel (header and cpp)
- T018, T019, T020 can run in parallel (separate VSCode config files)
- T032 and T033 can run in parallel (different files)
- US3 and US4 can be developed in parallel after Phase 2

---

## Parallel Example: Phase 1 Setup

```bash
# These tasks can run in parallel:
Task T003: "Create vcpkg manifest file at vcpkg.json"
Task T004: "Create .gitignore with build artifacts"
```

## Parallel Example: User Story 3 (VSCode)

```bash
# These tasks can run in parallel:
Task T018: "Create VSCode build task configuration at .vscode/tasks.json"
Task T019: "Create VSCode debug launch configuration at .vscode/launch.json"
Task T020: "Create VSCode workspace settings at .vscode/settings.json"
```

---

## Implementation Strategy

### MVP First (User Stories 1 + 2)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational
3. Complete Phase 3: User Story 1 (Build)
4. Complete Phase 4: User Story 2 (Load)
5. **STOP and VALIDATE**: Extension builds and loads successfully
6. This is the minimal viable bootstrap

### Incremental Delivery

1. Setup + Foundational → Build system ready
2. US1 (Build) → Extension binary exists
3. US2 (Load) → Extension loads in DuckDB (MVP complete!)
4. US3 (Debug) → Developer productivity enhancement
5. US4 (SQL Server) → Integration testing foundation
6. Polish → Documentation and validation

### Parallel Team Strategy

With multiple developers after Phase 2:

- Developer A: US1 → US2 (sequential, US2 depends on US1)
- Developer B: US3 (VSCode setup, independent)
- Developer C: US4 (Docker/SQL Server, independent)

---

## Notes

- [P] tasks = different files, no dependencies on incomplete tasks
- [Story] label maps task to specific user story for traceability
- US1 and US2 are both P1 priority but US2 depends on US1
- US3 and US4 are both P2 priority and can run in parallel
- No test tasks generated (tests not explicitly requested in spec)
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently

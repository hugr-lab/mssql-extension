# Tasks: CI/CD Release Workflows

**Input**: Design documents from `/specs/011-ci-release-workflows/`
**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, quickstart.md

**Tests**: Not explicitly requested - no test tasks included.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

Based on plan.md, files are organized as:

```text
.github/workflows/          # GitHub Actions workflow files
scripts/ci/                 # CI helper scripts
scripts/sql/                # SQL test files
docker/                     # Docker Compose (existing)
README.md                   # Documentation updates
```

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create directory structure and shared helper scripts

- [x] T001 Create scripts/ci/ directory for CI helper scripts
- [x] T002 Create scripts/sql/ directory for SQL test files
- [x] T003 [P] Create fetch_duckdb.sh script in scripts/ci/fetch_duckdb.sh with version argument support

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core helper scripts that MUST be complete before ANY user story workflow can be implemented

**CRITICAL**: No workflow implementation can begin until this phase is complete

- [x] T004 Implement fetch_duckdb.sh in scripts/ci/fetch_duckdb.sh to clone DuckDB by tag or main branch and output commit hash
- [x] T005 [P] Create smoke_test.sh in scripts/ci/smoke_test.sh for load-only extension verification
- [x] T006 [P] Create smoke_test.sql in scripts/sql/smoke_test.sql with SQL Server ATTACH, SELECT, DDL, and DML tests
- [x] T007 Create integration_test.sh in scripts/ci/integration_test.sh for full SQL Server integration testing on Linux

**Checkpoint**: Helper scripts ready - workflow implementation can now begin

---

## Phase 3: User Story 1 - Release Build and Publish (Priority: P1)

**Goal**: Implement release workflow that builds 12 artifacts (4 platforms × 3 DuckDB versions) and publishes to GitHub Releases on `v*` tags

**Independent Test**: Push a tag `v0.0.1-test` and verify all 12 artifacts appear on the GitHub Release with correct naming and SHA256SUMS.txt

### Implementation for User Story 1

- [x] T008 [US1] Create release.yml workflow file in .github/workflows/release.yml with tag trigger on `v*`
- [x] T009 [US1] Add lint job to release.yml in .github/workflows/release.yml with clang-format check
- [x] T010 [US1] Define build matrix in release.yml in .github/workflows/release.yml with 4 platforms (linux_amd64, linux_arm64, osx_arm64, windows_amd64) × 3 DuckDB versions (1.4.1, 1.4.2, 1.4.3)
- [x] T011 [US1] Implement build job in release.yml in .github/workflows/release.yml that fetches DuckDB by tag, builds extension, logs commit hash
- [x] T012 [US1] Add artifact naming logic to release.yml in .github/workflows/release.yml using pattern `mssql-{EXT_VERSION}-duckdb-{DUCKDB_VERSION}-{PLATFORM}.duckdb_extension`
- [x] T013 [US1] Add smoke test job to release.yml in .github/workflows/release.yml (load-only on all platforms)
- [x] T014 [US1] Add release job to release.yml in .github/workflows/release.yml that collects artifacts, generates SHA256SUMS.txt, uploads to GitHub Release using softprops/action-gh-release

**Checkpoint**: Release workflow complete - tagging `v*` triggers full build matrix and publishes to GitHub Releases

---

## Phase 4: User Story 2 - PR Validation (Priority: P2)

**Goal**: Update CI workflow to build against DuckDB nightly on linux_amd64 and osx_arm64 for PR validation

**Independent Test**: Open a PR with a trivial change and verify CI builds succeed on both platforms against DuckDB nightly

### Implementation for User Story 2

- [x] T015 [US2] Update existing ci.yml in .github/workflows/ci.yml to trigger on pull_request and push to main branch
- [x] T016 [US2] Modify build matrix in ci.yml in .github/workflows/ci.yml to use linux_amd64 (ubuntu-22.04) and osx_arm64 (macos-14) only
- [x] T017 [US2] Update ci.yml build job in .github/workflows/ci.yml to fetch DuckDB main branch (nightly) and log commit hash
- [x] T018 [US2] Add artifact upload step to ci.yml in .github/workflows/ci.yml for PR build artifacts

**Checkpoint**: PR validation workflow complete - PRs trigger builds against DuckDB nightly

---

## Phase 5: User Story 3 - Linux Integration Smoke Test (Priority: P2)

**Goal**: Run full SQL Server integration tests on Linux runners using Docker container

**Independent Test**: Trigger CI on linux_amd64 and verify SQL Server container starts, extension ATTACHes, and SELECT/DDL/DML execute successfully

### Implementation for User Story 3

- [x] T019 [US3] Add SQL Server service container to ci.yml in .github/workflows/ci.yml for linux runners using docker-compose
- [x] T020 [US3] Add integration test step to ci.yml in .github/workflows/ci.yml that waits for SQL Server health, then runs integration_test.sh
- [x] T021 [US3] Add integration test step to release.yml in .github/workflows/release.yml for linux platform smoke tests
- [x] T022 [US3] Update smoke_test.sql in scripts/sql/smoke_test.sql to include CREATE TABLE and INSERT verification

**Checkpoint**: Linux integration tests complete - SQL Server connectivity verified on Linux runners

---

## Phase 6: User Story 4 - Load-Only Smoke Test on macOS/Windows (Priority: P3)

**Goal**: Run load-only smoke tests on macOS and Windows where Docker is unavailable

**Independent Test**: Trigger CI/release on osx_arm64 or windows_amd64 and verify extension loads successfully with clear log indicating integration test skipped

### Implementation for User Story 4

- [x] T023 [US4] Add platform detection to smoke_test.sh in scripts/ci/smoke_test.sh to log "Integration test skipped - Docker not available" when appropriate
- [x] T024 [US4] Add conditional smoke test logic to ci.yml in .github/workflows/ci.yml to run load-only test on macOS
- [x] T025 [US4] Add conditional smoke test logic to release.yml in .github/workflows/release.yml to run load-only test on macOS and Windows

**Checkpoint**: Load-only smoke tests complete on all platforms - binary validity verified

---

## Phase 7: User Story 5 - README Documentation Updates (Priority: P3)

**Goal**: Update README with minimum DuckDB version, platform support status, and license information

**Independent Test**: Read README and verify it contains: minimum DuckDB 1.4.1, Windows caveat, MIT license

### Implementation for User Story 5

- [x] T026 [US5] Update Prerequisites section in README.md to specify minimum DuckDB version 1.4.1
- [x] T027 [P] [US5] Add Platform Support section to README.md noting Windows is not fully tested with contribution note
- [x] T028 [US5] Update License section in README.md to state MIT license

**Checkpoint**: README documentation complete - users can find compatibility and license info

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Final validation, cleanup, and cross-cutting improvements

- [x] T029 Validate all workflows by reviewing YAML syntax and job dependencies in .github/workflows/
- [x] T030 [P] Add workflow concurrency controls to ci.yml and release.yml in .github/workflows/ to cancel duplicate runs
- [x] T031 Run quickstart.md validation by following local testing instructions in specs/011-ci-release-workflows/quickstart.md
- [ ] T032 Create test tag `v0.0.1-test` to validate full release workflow end-to-end

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phases 3-7)**: All depend on Foundational phase completion
  - US1 (Release) can proceed first as it's highest priority
  - US2 (PR Validation) and US3 (Linux Integration) can proceed in parallel after US1
  - US4 (Load-Only) depends on smoke test scripts from Phase 2
  - US5 (README) has no code dependencies, can proceed in parallel
- **Polish (Phase 8)**: Depends on all user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational - No dependencies on other stories
- **User Story 2 (P2)**: Can start after Foundational - Uses same helper scripts as US1
- **User Story 3 (P2)**: Can start after Foundational - Adds integration tests to workflows from US1/US2
- **User Story 4 (P3)**: Can start after Foundational - Adds conditional logic to smoke tests
- **User Story 5 (P3)**: Can start after Setup - No code dependencies, documentation only

### Within Each User Story

- Workflow structure before job implementations
- Jobs in dependency order: lint → build → smoke-test → release
- Cross-platform considerations before platform-specific logic

### Parallel Opportunities

- T003 can run in parallel with T001, T002 (different directories)
- T005, T006 can run in parallel (different files, no dependencies)
- T027 can run in parallel with T026 (different README sections)
- T030 can run in parallel with T029 (different workflow aspects)

---

## Parallel Example: User Story 1 (Release)

```bash
# After Foundational phase, these tasks can be launched together:
# T008 creates the workflow file first

# Then these can run in parallel after T008:
Task: "Add lint job to release.yml in .github/workflows/release.yml" (T009)
Task: "Define build matrix in release.yml in .github/workflows/release.yml" (T010)

# Then T011-T014 proceed sequentially as they build on the matrix
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001-T003)
2. Complete Phase 2: Foundational (T004-T007)
3. Complete Phase 3: User Story 1 - Release (T008-T014)
4. **STOP and VALIDATE**: Test with a `v0.0.1-test` tag
5. Release workflow is functional - basic releases possible

### Incremental Delivery

1. Complete Setup + Foundational → Helper scripts ready
2. Add User Story 1 (Release) → Test with tag → Core release functionality
3. Add User Story 2 (PR Validation) → Test with PR → CI validation working
4. Add User Story 3 (Linux Integration) → SQL Server tests in CI
5. Add User Story 4 (Load-Only) → Complete platform coverage
6. Add User Story 5 (README) → Documentation complete
7. Each story adds value without breaking previous stories

### Parallel Team Strategy

With multiple developers:

1. Team completes Setup + Foundational together
2. Once Foundational is done:
   - Developer A: User Story 1 (Release workflow)
   - Developer B: User Story 5 (README - no code deps)
3. After US1 complete:
   - Developer A: User Stories 2 & 4 (CI improvements)
   - Developer B: User Story 3 (Integration tests)

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story should be independently completable and testable
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- Avoid: vague tasks, same file conflicts, cross-story dependencies that break independence

## Summary

| Metric | Value |
|--------|-------|
| Total Tasks | 32 |
| Setup Tasks | 3 |
| Foundational Tasks | 4 |
| US1 (Release) Tasks | 7 |
| US2 (PR Validation) Tasks | 4 |
| US3 (Linux Integration) Tasks | 4 |
| US4 (Load-Only) Tasks | 3 |
| US5 (README) Tasks | 3 |
| Polish Tasks | 4 |
| Parallelizable Tasks | 8 |

**MVP Scope**: Complete Phases 1-3 (Tasks T001-T014) for working release workflow

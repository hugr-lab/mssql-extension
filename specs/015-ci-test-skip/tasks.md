# Tasks: Skip Tests in Community Extensions CI

**Input**: Design documents from `/specs/015-ci-test-skip/`
**Prerequisites**: plan.md (required), spec.md (required), research.md, quickstart.md

**Tests**: No tests required - this is a configuration-only change verified via external CI.

**Organization**: Tasks are minimal due to the single-file configuration change scope.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup

**Purpose**: No setup required - existing project structure is sufficient.

*No tasks in this phase - configuration change only.*

---

## Phase 2: Foundational

**Purpose**: No foundational work required - single configuration change.

*No tasks in this phase - configuration change only.*

---

## Phase 3: User Story 1 - Community Extensions CI Success (Priority: P1) 🎯 MVP

**Goal**: Enable the MSSQL extension to pass the community-extensions CI workflow by skipping tests that require SQL Server.

**Independent Test**: Submit/update PR to community-extensions repository and verify CI workflow completes successfully with "Tests are skipped in this run..." message.

### Implementation for User Story 1

- [ ] T001 [US1] Add `test_config` field to extension section in description.yml

**Details for T001**:
```yaml
extension:
  # ... existing fields ...
  test_config: '{"test_env_variables": {"SKIP_TESTS": "1"}}'
```

- [ ] T002 [US1] Create git tag for new release (e.g., v0.1.1) after committing changes
- [ ] T003 [US1] Update community-extensions PR ref to point to new tag in duckdb/community-extensions PR#1104

**Checkpoint**: Community-extensions CI should now pass. Tests display "Tests are skipped in this run..." message.

---

## Phase 4: User Story 2 - Local Development Testing Preserved (Priority: P2)

**Goal**: Verify that local development testing continues to work unaffected by the configuration change.

**Independent Test**: Run `make integration-test` locally with Docker SQL Server running.

### Verification for User Story 2

- [ ] T004 [US2] Verify `make test` runs normally when SKIP_TESTS is NOT set locally
- [ ] T005 [US2] Verify `make integration-test` works with Docker SQL Server (docker-up first)

**Checkpoint**: Local testing workflow unchanged - `description.yml` test_config only affects CI.

---

## Phase 5: User Story 3 - Graceful Test Skip (Priority: P3)

**Goal**: Confirm tests with `require-env` skip gracefully when environment variables are not set.

**Independent Test**: Run tests without `MSSQL_TEST_DSN` set and verify skip behavior.

### Verification for User Story 3

- [ ] T006 [US3] Verify tests without require-env (mssql_version.test, mssql_secret.test) still run locally
- [ ] T007 [US3] Verify tests with require-env are skipped when MSSQL_TEST_DSN is not set

**Checkpoint**: Test behavior is correct - SQL Server-independent tests run, others skip gracefully.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Documentation and cleanup.

- [ ] T008 [P] Update specs/015-ci-test-skip/spec.md status from "Draft" to "Implemented"
- [ ] T009 Verify community-extensions PR CI passes after all changes

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 (Setup)**: N/A - no tasks
- **Phase 2 (Foundational)**: N/A - no tasks
- **Phase 3 (US1)**: No dependencies - can start immediately
- **Phase 4 (US2)**: Depends on T001 being committed (to verify local behavior unchanged)
- **Phase 5 (US3)**: No dependencies on US1/US2 - parallel verification possible
- **Phase 6 (Polish)**: Depends on US1 completion and CI verification

### User Story Dependencies

- **User Story 1 (P1)**: Independent - core implementation
- **User Story 2 (P2)**: Verification only - confirms US1 doesn't break local dev
- **User Story 3 (P3)**: Verification only - confirms existing test skip behavior

### Critical Path

```
T001 (description.yml) → T002 (tag) → T003 (PR update) → T009 (CI verify)
```

### Parallel Opportunities

- T004-T007 (verification tasks) can run in parallel after T001 is committed
- T008 can run in parallel with other polish tasks

---

## Parallel Example: All Verification Tasks

```bash
# After T001 is committed, launch all verification in parallel:
Task: "T004 [US2] Verify make test runs normally"
Task: "T005 [US2] Verify make integration-test works"
Task: "T006 [US3] Verify tests without require-env run"
Task: "T007 [US3] Verify tests with require-env skip"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete T001: Add test_config to description.yml
2. Complete T002: Create git tag
3. Complete T003: Update community-extensions PR
4. **STOP and VALIDATE**: Wait for CI to complete
5. If CI passes → MVP complete!

### Incremental Verification

1. Complete US1 (T001-T003) → CI should pass
2. Verify US2 (T004-T005) → Local dev unchanged
3. Verify US3 (T006-T007) → Test skip behavior correct
4. Polish (T008-T009) → Documentation and final verification

---

## Notes

- This is a minimal configuration change - single line addition to YAML file
- No code changes required
- External verification via community-extensions CI
- Local development workflow completely unaffected
- Can be easily reverted by removing the `test_config` line

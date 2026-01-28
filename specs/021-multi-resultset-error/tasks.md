# Tasks: Improve Multi-Resultset Error Messages

**Input**: Design documents from `/specs/021-multi-resultset-error/`
**Prerequisites**: plan.md, spec.md, research.md, quickstart.md

**Tests**: No test tasks included (not explicitly requested in spec).

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2)
- Include exact file paths in descriptions

## Phase 1: Setup

**Purpose**: No setup needed — this is a targeted fix in an existing codebase on an existing branch.

(No tasks in this phase.)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Add the `DrainRemainingTokens()` helper method that both user stories depend on.

- [x] T001 Declare `DrainRemainingTokens()` private method in `src/include/query/mssql_result_stream.hpp`
- [x] T002 Implement `DrainRemainingTokens()` in `src/query/mssql_result_stream.cpp` after `DrainAfterCancel()` — enable skip mode, loop parsing tokens and reading data until final DONE or timeout, transition connection to Idle on success or Close on timeout

**Checkpoint**: Helper method compiles and is available for use in FillChunk.

---

## Phase 3: User Story 1 — Clear Error on Multiple Result Sets (Priority: P1) MVP

**Goal**: Detect a second COLMETADATA token during row streaming and throw a clear, actionable error message instead of crashing with an internal type assertion.

**Independent Test**: Execute `FROM mssql_scan('db', 'SELECT * FROM dbo.test; SELECT ''hello''')` and verify the system returns a clear error message mentioning multiple result sets instead of an internal crash.

### Implementation for User Story 1

- [x] T003 [US1] Add `case tds::ParsedTokenType::ColMetadata` in `FillChunk()` switch statement in `src/query/mssql_result_stream.cpp` (before the `default:` case, around line 297) — set `state_` to `Error`, call `DrainRemainingTokens()`, throw `InvalidInputException` with message: "MSSQL Error: The SQL batch produced multiple result sets. Only one result-producing statement is allowed per mssql_scan() call. Ensure your batch contains only one SELECT or other result-producing statement, or use separate mssql_scan() calls for multiple result sets."
- [x] T004 [US1] Build the extension (`make debug`) and verify compilation succeeds with no errors or warnings
- [x] T005 [US1] Run existing test suite (`make test`) and verify zero regressions — all unit tests, integration tests, and SQL tests must pass

**Checkpoint**: Multi-resultset batches produce a clear error message. Single-statement queries and multi-statement batches with only one result-producing statement continue working.

---

## Phase 4: User Story 2 — Connection Remains Clean After Error (Priority: P2)

**Goal**: After the multiple-result-set error is thrown, the connection must be in a clean Idle state so subsequent queries on the same pooled connection work correctly.

**Independent Test**: Execute a multi-result-set batch that triggers the error, then run a normal query on the same connection and verify it succeeds.

### Implementation for User Story 2

- [x] T006 [US2] Verify `DrainRemainingTokens()` in `src/query/mssql_result_stream.cpp` correctly transitions connection to `Idle` state after draining (already implemented in T002) — review that the `connection_->TransitionState(Executing, Idle)` call happens on final DONE, and `connection_->Close()` on timeout
- [ ] T007 [US2] Manual integration test: trigger the multi-resultset error, then execute a normal `mssql_scan` query on the same attached database and verify it succeeds without connection errors

**Checkpoint**: Connection is reusable after multi-resultset error. No connection pool corruption.

---

## Phase 5: Polish & Cross-Cutting Concerns

**Purpose**: Edge case verification and final validation.

- [ ] T008 Manual test: verify identical-schema result sets still trigger the error (`SELECT * FROM dbo.test; SELECT * FROM dbo.test`)
- [ ] T009 Manual test: verify DML between two result-producing statements triggers the error correctly (`SELECT * INTO #t FROM dbo.test; INSERT INTO #t VALUES(1); SELECT * FROM #t; SELECT 'extra'`)
- [ ] T010 Manual test: verify single-statement DML with no result set works correctly (`FROM mssql_exec('db', 'UPDATE dbo.test SET col1 = 1')`)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Foundational (Phase 2)**: No dependencies — can start immediately
- **User Story 1 (Phase 3)**: Depends on Phase 2 (T001, T002)
- **User Story 2 (Phase 4)**: Depends on Phase 3 (T003) — connection cleanup is tested after error detection works
- **Polish (Phase 5)**: Depends on Phase 3 and Phase 4

### User Story Dependencies

- **User Story 1 (P1)**: Depends only on Foundational phase (T001, T002)
- **User Story 2 (P2)**: Depends on User Story 1 (T003) being complete — can't test connection cleanup without the error being thrown

### Within Each User Story

- Implementation before build verification
- Build verification before test suite validation
- All automated tests pass before manual testing

### Parallel Opportunities

- T001 and T002 can be done together (header + implementation) since they're in different files
- T008, T009, T010 (manual edge case tests) can run in parallel

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 2: Add `DrainRemainingTokens()` helper
2. Complete Phase 3: Add `ColMetadata` case in `FillChunk()`
3. **STOP and VALIDATE**: Build, test, manual verification
4. Commit and push

### Incremental Delivery

1. T001 + T002 → Helper method ready
2. T003 + T004 + T005 → Error detection works, all tests pass (MVP!)
3. T006 + T007 → Connection cleanup verified
4. T008-T010 → Edge cases validated

---

## Notes

- Total: 10 tasks (2 foundational, 3 US1, 2 US2, 3 polish)
- This is a minimal, targeted fix — primary code change is ~30 lines in one file
- The `DrainRemainingTokens()` helper follows the same pattern as existing `DrainAfterCancel()` but without ATTENTION signal
- Connection cleanup (US2) is largely handled by `DrainRemainingTokens()` which is part of the foundational phase

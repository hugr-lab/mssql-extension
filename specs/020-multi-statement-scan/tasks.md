# Tasks: Support Multi-Statement SQL in mssql_scan

**Input**: Design documents from `/specs/020-multi-statement-scan/`
**Prerequisites**: plan.md, spec.md, research.md, quickstart.md

**Tests**: Not explicitly requested in spec. No test tasks generated.

**Organization**: Tasks grouped by user story. US1/US2/US3/US4 share the same code change (Phase 2). US5 is an independent change.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup

**Purpose**: Verify branch and clean working tree.

- [x] T001 Verify current branch is `020-multi-statement-scan` and clean working tree

---

## Phase 2: Foundational — Multi-Statement Token Handling (Blocking)

**Purpose**: Fix `Initialize()` to handle non-final DONE tokens. All user stories 1-4 depend on this.

**CRITICAL**: No user story work can begin until this phase is complete.

- [x] T002 Modify `ParsedTokenType::Done` case in `MSSQLResultStream::Initialize()` in `src/query/mssql_result_stream.cpp` — retrieve `DoneToken` via `parser_.GetDone()`, check `IsFinal()`: if NOT final (DONE_MORE set), `break` to continue the token loop; if final with no columns, transition to Complete as before. Keep existing error check for accumulated errors.
- [x] T003 Build release to verify compilation: `GEN=ninja make release`
- [x] T004 Run full test suite to verify no regression: `make test && make integration-test` with `MSSQL_TEST_DSN_TLS` exported — all 108 test cases, 2741 assertions must pass

**Checkpoint**: Multi-statement token handling is in place. Single-statement queries still work identically.

---

## Phase 3: User Story 1 — Temp Table Queries (Priority: P1) MVP

**Goal**: Users can execute `SELECT INTO #t ...; SELECT * FROM #t` via `mssql_scan`.

**Independent Test**: `FROM mssql_scan('db', 'select * into #t from dbo.test; select * from #t')` returns correct columns and data.

### Implementation for User Story 1

- [x] T005 [US1] Manual test: execute `FROM mssql_scan('db', 'select * into #t from dbo.test; select * from #t')` in DuckDB CLI and verify it returns expected rows
- [x] T006 [US1] Manual test: execute `FROM mssql_scan('db', 'CREATE TABLE #t (id INT, name VARCHAR(100)); INSERT INTO #t VALUES (1, ''test''); SELECT * FROM #t')` and verify it returns the inserted row

**Checkpoint**: US1 complete — temp table pattern works.

---

## Phase 4: User Story 2 — DML Followed by SELECT (Priority: P2)

**Goal**: Users can execute DML + SELECT batches via `mssql_scan`.

**Independent Test**: `FROM mssql_scan('db', 'SET NOCOUNT ON; SELECT * FROM dbo.test')` returns results.

### Implementation for User Story 2

No additional code changes needed — the Phase 2 fix covers this. DONE tokens from DML statements have DONE_MORE set when followed by more statements.

- [x] T007 [US2] Manual test: execute `FROM mssql_scan('db', 'SET NOCOUNT ON; SELECT * FROM dbo.test')` and verify results are returned

**Checkpoint**: US2 covered by same fix as US1.

---

## Phase 5: User Story 3 — Error in Intermediate Statement (Priority: P1)

**Goal**: Errors in intermediate statements are reported to the user.

**Independent Test**: `FROM mssql_scan('db', 'SELECT * FROM nonexistent_table; SELECT 1 AS x')` reports an error.

### Implementation for User Story 3

No additional code changes needed — the Phase 2 fix checks `errors_` on non-final DONE tokens and throws if errors accumulated.

- [x] T008 [US3] Manual test: execute `FROM mssql_scan('db', 'SELECT * FROM nonexistent_table_xyz; SELECT 1 AS x')` and verify it reports the SQL Server error message about the nonexistent table

**Checkpoint**: US3 inherently covered by the error check in Phase 2.

---

## Phase 6: User Story 4 — No Result Set in Entire Batch (Priority: P2)

**Goal**: Batches with no result set complete gracefully.

**Independent Test**: `FROM mssql_scan('db', 'DECLARE @x INT = 1')` completes without error.

### Implementation for User Story 4

No additional code changes needed — when all DONE tokens are processed and the final one is reached without COLMETADATA, the stream transitions to Complete (existing behavior, now only on truly final DONE).

- [x] T009 [US4] Manual test: execute `FROM mssql_scan('db', 'DECLARE @x INT = 1')` and verify it completes gracefully (empty result)

**Checkpoint**: US4 covered.

---

## Phase 7: User Story 5 — Connection Reset on Pool Return (Priority: P1)

**Goal**: Session state is reset when connections return to the pool, preventing temp table leaks.

**Independent Test**: After a multi-statement batch creating temp tables, the next query on the same pooled connection should not see those temp tables.

### Implementation for User Story 5

- [x] T010 [US5] Add connection reset in `ConnectionProvider::ReleaseConnection()` in `src/connection/mssql_connection_provider.cpp` — before calling `pool.Release()` in the autocommit path, execute `conn->ExecuteAndDrain("exec sp_reset_connection")`. If reset fails, close the connection instead of returning it dirty.
- [x] T011 [US5] Add connection reset after commit/rollback in `src/connection/mssql_connection_provider.cpp` (or `src/catalog/mssql_transaction.cpp`, wherever `CommitTransaction`/`RollbackTransaction` call `pool.Release()`) — execute `sp_reset_connection` on the pinned connection before releasing it to the pool.
- [x] T012 [US5] Build and run full test suite: `GEN=ninja make release && make test && make integration-test` — all 108 test cases must pass
- [x] T013 [US5] Manual test: execute multi-statement batch creating temp table, then run another query on same pool and verify temp table does not exist

**Checkpoint**: Connection pool returns clean connections. No session state leakage.

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: CI verification and commit.

- [x] T014 Run full test suite with TLS: `make test && make integration-test` with `MSSQL_TEST_DSN_TLS` exported
- [ ] T015 Commit changes and create PR

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies
- **Foundational (Phase 2)**: Depends on Phase 1 — T002 is the core code change, T003-T004 verify it
- **User Stories 1-4 (Phases 3-6)**: All depend on Phase 2 completion — these are verification tasks, not code changes
- **User Story 5 (Phase 7)**: Independent of Phases 3-6, depends only on Phase 1 — separate code change in different files
- **Polish (Phase 8)**: Depends on all phases being complete

### User Story Dependencies

- **US1-US4**: All share the same code change (Phase 2 T002). Can be verified in parallel after Phase 2.
- **US5**: Independent code change in `mssql_connection_provider.cpp`. Can be implemented in parallel with Phase 2.

### Parallel Opportunities

- T002 and T010-T011 can be implemented in parallel (different files)
- T005, T006, T007, T008, T009 can all be verified in parallel after Phase 2
- T012 runs after T010-T011

---

## Implementation Strategy

### MVP First (Phase 2 Only)

1. Complete T001 (verify branch)
2. Complete T002 (fix Initialize() token handling)
3. Complete T003-T004 (build and test)
4. **STOP and VALIDATE**: Manual test with temp table query (T005)
5. This is the core fix — 1 code change in 1 file

### Incremental Delivery

1. Phase 2: Fix multi-statement token handling (~5 lines in 1 file)
2. Phases 3-6: Verify all user stories (manual tests)
3. Phase 7: Add connection reset (~15 lines in 1-2 files)
4. Phase 8: Final test and PR

---

## Notes

- This feature has 2 implementation tasks (T002, T010-T011) and the rest are verification
- The multi-statement fix (T002) is ~5 lines changed in one `case` block
- The connection reset (T010-T011) is ~15 lines across 1-2 files
- US1-US4 are all satisfied by the same single code change
- US5 is independent and can be done in parallel

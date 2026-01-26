# Tasks: MSSQL Transactions (DuckDB-Compatible MVP)

**Input**: Design documents from `/specs/001-mssql-transactions/`
**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md

**Tests**: Tests ARE included as this feature requires SQL Server integration testing.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3, US4, US5, US6)
- Include exact file paths in descriptions

## Path Conventions

- **Language**: C++17 (DuckDB extension standard)
- **Source**: `src/` at repository root
- **Tests**: `test/sql/` at repository root (SQLLogicTest format)
- **Headers**: `src/include/` at repository root

---

## Phase 1: Setup

**Purpose**: Create test infrastructure and directory structure for transaction feature

- [ ] T001 Create transaction test directory `test/sql/transaction/`
- [ ] T002 [P] Add test table creation script for transaction tests in `docker/init/init-transaction-tests.sql`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core transaction infrastructure that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

### MSSQLTransaction Class Modification

- [ ] T003 Add pinned connection field (`shared_ptr<TdsConnection>`) to `MSSQLTransaction` class in `src/include/catalog/mssql_transaction.hpp`
- [ ] T004 Add connection mutex field (`std::mutex`) to `MSSQLTransaction` class in `src/include/catalog/mssql_transaction.hpp`
- [ ] T005 Add SQL Server transaction state flag (`bool sql_server_transaction_active_`) to `MSSQLTransaction` class in `src/include/catalog/mssql_transaction.hpp`
- [ ] T006 Add savepoint counter field (`uint32_t savepoint_counter_`) to `MSSQLTransaction` class in `src/include/catalog/mssql_transaction.hpp`
- [ ] T007 Add public accessor methods (`GetPinnedConnection()`, `HasPinnedConnection()`, `GetConnectionMutex()`) to `MSSQLTransaction` in `src/include/catalog/mssql_transaction.hpp`

### ConnectionProvider Utility Class (New)

- [ ] T008 [P] Create `ConnectionProvider` header with static methods `GetConnection()`, `ReleaseConnection()`, `IsInTransaction()` in `src/include/connection/mssql_connection_provider.hpp`
- [ ] T009 [P] Implement `ConnectionProvider::IsInTransaction()` - check if DuckDB transaction is active and has pinned connection in `src/connection/mssql_connection_provider.cpp`
- [ ] T010 Implement `ConnectionProvider::GetConnection()` - return pinned connection if in transaction, else acquire from pool in `src/connection/mssql_connection_provider.cpp`
- [ ] T011 Implement `ConnectionProvider::ReleaseConnection()` - no-op if pinned, return to pool otherwise in `src/connection/mssql_connection_provider.cpp`

### MSSQLTransactionManager Modification

- [ ] T012 Modify `MSSQLTransactionManager::StartTransaction()` to initialize transaction state (defer SQL Server transaction start) in `src/catalog/mssql_transaction.cpp`
- [ ] T013 Implement `MSSQLTransactionManager::CommitTransaction()` with SQL Server `COMMIT TRANSACTION` and pool return in `src/catalog/mssql_transaction.cpp`
- [ ] T014 Implement `MSSQLTransactionManager::RollbackTransaction()` with SQL Server `ROLLBACK TRANSACTION` and pool return in `src/catalog/mssql_transaction.cpp`
- [ ] T015 Add `VerifyCleanTransactionState()` helper to check `@@TRANCOUNT = 0` before pool return in `src/catalog/mssql_transaction.cpp`

**Checkpoint**: Foundation ready - user story implementation can now begin

---

## Phase 3: User Story 1 - DML Transaction Commit (Priority: P1) 🎯 MVP

**Goal**: Users can execute multi-statement DML (INSERT, UPDATE, DELETE) within a DuckDB transaction and commit atomically

**Independent Test**: `BEGIN; INSERT...; UPDATE...; DELETE...; COMMIT;` - all changes persist atomically

### Tests for User Story 1

- [ ] T016 [P] [US1] Create transaction commit test file `test/sql/transaction/transaction_commit.test` with basic INSERT commit scenario
- [ ] T017 [P] [US1] Add multi-statement INSERT commit test in `test/sql/transaction/transaction_commit.test`
- [ ] T018 [P] [US1] Add UPDATE commit test in `test/sql/transaction/transaction_commit.test`
- [ ] T019 [P] [US1] Add DELETE commit test in `test/sql/transaction/transaction_commit.test`
- [ ] T020 [P] [US1] Add combined INSERT/UPDATE/DELETE commit test in `test/sql/transaction/transaction_commit.test`

### Implementation for User Story 1

- [ ] T021 [US1] Modify `MSSQLInsertExecutor::GetConnectionPool()` to use `ConnectionProvider::GetConnection()` in `src/dml/insert/mssql_insert_executor.cpp`
- [ ] T022 [US1] Add `ConnectionProvider::ReleaseConnection()` call after INSERT batch execution in `src/dml/insert/mssql_insert_executor.cpp`
- [ ] T023 [P] [US1] Modify `MSSQLUpdateExecutor` to use `ConnectionProvider` for connection acquisition in `src/dml/update/mssql_update_executor.cpp`
- [ ] T024 [P] [US1] Modify `MSSQLDeleteExecutor` to use `ConnectionProvider` for connection acquisition in `src/dml/delete/mssql_delete_executor.cpp`
- [ ] T025 [US1] Implement lazy SQL Server transaction start in `ConnectionProvider::GetConnection()` - execute `BEGIN TRANSACTION` on first operation in `src/connection/mssql_connection_provider.cpp`

**Checkpoint**: User Story 1 complete - DML commit works atomically

---

## Phase 4: User Story 2 - DML Transaction Rollback (Priority: P1)

**Goal**: Users can undo all DML changes within a transaction via ROLLBACK

**Independent Test**: `BEGIN; INSERT...; ROLLBACK;` - inserted row does not exist after rollback

### Tests for User Story 2

- [ ] T026 [P] [US2] Create transaction rollback test file `test/sql/transaction/transaction_rollback.test` with basic INSERT rollback scenario
- [ ] T027 [P] [US2] Add UPDATE rollback test in `test/sql/transaction/transaction_rollback.test`
- [ ] T028 [P] [US2] Add DELETE rollback test in `test/sql/transaction/transaction_rollback.test`
- [ ] T029 [P] [US2] Add combined DML rollback test in `test/sql/transaction/transaction_rollback.test`
- [ ] T030 [P] [US2] Add transaction state verification test (@@TRANCOUNT = 0 after rollback) in `test/sql/transaction/transaction_rollback.test`

### Implementation for User Story 2

- [ ] T031 [US2] Verify `MSSQLTransactionManager::RollbackTransaction()` correctly executes `ROLLBACK TRANSACTION` in `src/catalog/mssql_transaction.cpp`
- [ ] T032 [US2] Add error logging when `@@TRANCOUNT != 0` after rollback attempt in `src/catalog/mssql_transaction.cpp`
- [ ] T033 [US2] Add destructor cleanup in `MSSQLTransaction` to handle abandoned transactions in `src/catalog/mssql_transaction.cpp`

**Checkpoint**: User Stories 1 AND 2 complete - commit and rollback both work

---

## Phase 5: User Story 3 - Read-Your-Writes via mssql_scan (Priority: P2)

**Goal**: Users can verify uncommitted DML changes using `mssql_scan()` within the same transaction

**Independent Test**: `BEGIN; INSERT...; SELECT FROM mssql_scan(...);` - inserted row visible in scan results

### Tests for User Story 3

- [ ] T034 [P] [US3] Create mssql_scan transaction test file `test/sql/transaction/transaction_mssql_scan.test` with read-your-writes INSERT scenario
- [ ] T035 [P] [US3] Add read-your-writes UPDATE scenario in `test/sql/transaction/transaction_mssql_scan.test`
- [ ] T036 [P] [US3] Add rollback verification via mssql_scan in `test/sql/transaction/transaction_mssql_scan.test`

### Implementation for User Story 3

- [ ] T037 [US3] Modify `MSSQLQueryExecutor::Execute()` to use `ConnectionProvider::GetConnection()` in `src/query/mssql_query_executor.cpp`
- [ ] T038 [US3] Modify `MSSQLResultStream` to use `ConnectionProvider::ReleaseConnection()` in destructor in `src/query/mssql_result_stream.cpp`
- [ ] T039 [US3] Ensure `MSSQLResultStream` holds shared_ptr to connection (already done, verify) in `src/query/mssql_result_stream.cpp`

**Checkpoint**: User Story 3 complete - read-your-writes via mssql_scan works

---

## Phase 6: User Story 4 - Execute via mssql_exec in Transaction (Priority: P2)

**Goal**: Users can execute arbitrary T-SQL via `mssql_exec()` within a DuckDB transaction

**Independent Test**: `BEGIN; SELECT mssql_exec('UPDATE...'); COMMIT;` - update persists

### Tests for User Story 4

- [ ] T040 [P] [US4] Create mssql_exec transaction test file `test/sql/transaction/transaction_mssql_exec.test` with basic UPDATE scenario
- [ ] T041 [P] [US4] Add temp table creation test (#temp) in `test/sql/transaction/transaction_mssql_exec.test`
- [ ] T042 [P] [US4] Add mssql_exec rollback test in `test/sql/transaction/transaction_mssql_exec.test`

### Implementation for User Story 4

- [ ] T043 [US4] Modify `mssql_exec` function to use `ConnectionProvider::GetConnection()` in `src/mssql_functions.cpp`
- [ ] T044 [US4] Modify `mssql_exec` function to use `ConnectionProvider::ReleaseConnection()` after execution in `src/mssql_functions.cpp`

**Checkpoint**: User Story 4 complete - mssql_exec works in transactions

---

## Phase 7: User Story 5 - Catalog Scan Restriction (Priority: P2)

**Goal**: System blocks catalog-based table scans inside DuckDB transactions with clear error message

**Independent Test**: `BEGIN; SELECT * FROM mssql.dbo.table;` - throws error

### Tests for User Story 5

- [ ] T045 [P] [US5] Create catalog restriction test file `test/sql/transaction/transaction_catalog_restriction.test` with table scan error scenario
- [ ] T046 [P] [US5] Add view scan error scenario in `test/sql/transaction/transaction_catalog_restriction.test`
- [ ] T047 [P] [US5] Add autocommit mode success scenario (no error outside transaction) in `test/sql/transaction/transaction_catalog_restriction.test`

### Implementation for User Story 5

- [ ] T048 [US5] Add transaction check in catalog scan bind phase - detect active transaction in `src/table_scan/catalog_scan_bind.cpp`
- [ ] T049 [US5] Throw BinderException with MVP error message when catalog scan attempted in transaction in `src/table_scan/catalog_scan_bind.cpp`

**Checkpoint**: User Story 5 complete - catalog scans properly restricted in transactions

---

## Phase 8: User Story 6 - Autocommit Mode Behavior (Priority: P3)

**Goal**: Existing autocommit behavior remains unchanged for non-transactional operations

**Independent Test**: `INSERT INTO mssql.dbo.table VALUES(...);` (no BEGIN) - row immediately committed

### Tests for User Story 6

- [ ] T050 [P] [US6] Create autocommit test file `test/sql/transaction/transaction_autocommit.test` with INSERT autocommit scenario
- [ ] T051 [P] [US6] Add catalog scan success scenario (outside transaction) in `test/sql/transaction/transaction_autocommit.test`
- [ ] T052 [P] [US6] Add multiple DML autocommit independence test in `test/sql/transaction/transaction_autocommit.test`

### Implementation for User Story 6

- [ ] T053 [US6] Verify `ConnectionProvider::GetConnection()` acquires from pool when no transaction active in `src/connection/mssql_connection_provider.cpp`
- [ ] T054 [US6] Verify `ConnectionProvider::ReleaseConnection()` returns to pool when no transaction active in `src/connection/mssql_connection_provider.cpp`

**Checkpoint**: User Story 6 complete - autocommit mode preserved

---

## Phase 9: Polish & Cross-Cutting Concerns

**Purpose**: Edge cases, error handling, and documentation

### Edge Case Handling

- [ ] T055 Implement connection cleanup in `MSSQLTransaction` destructor for abandoned transactions in `src/catalog/mssql_transaction.cpp`
- [ ] T056 Add MSSQL_DEBUG logging for transaction state changes in `src/catalog/mssql_transaction.cpp`
- [ ] T057 [P] Add commit failure handling - keep connection pinned on error in `src/catalog/mssql_transaction.cpp`

### Connection Pool Integration

- [ ] T058 Verify pool health after transaction tests (no leaked connections) in `test/sql/transaction/transaction_commit.test`

### Documentation

- [ ] T059 [P] Update `docs/TESTING.md` with transaction test instructions
- [ ] T060 [P] Add transaction usage examples to extension documentation

### Build & CI

- [ ] T061 Verify all transaction tests pass with `make integration-test`
- [ ] T062 Run lint check on all modified files

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup - BLOCKS all user stories
- **User Story 1 (Phase 3)**: Depends on Foundational
- **User Story 2 (Phase 4)**: Depends on Foundational (shares implementation with US1)
- **User Story 3 (Phase 5)**: Depends on Foundational
- **User Story 4 (Phase 6)**: Depends on Foundational
- **User Story 5 (Phase 7)**: Depends on Foundational
- **User Story 6 (Phase 8)**: Depends on Foundational
- **Polish (Phase 9)**: Depends on all user stories being complete

### User Story Dependencies

| Story | Depends On | Can Parallel With |
|-------|------------|-------------------|
| US1 (Commit) | Foundational | - |
| US2 (Rollback) | Foundational | US1 (shared infra) |
| US3 (mssql_scan) | Foundational | US1, US2 |
| US4 (mssql_exec) | Foundational | US1, US2, US3 |
| US5 (Catalog Restriction) | Foundational | US1, US2, US3, US4 |
| US6 (Autocommit) | Foundational | US1, US2, US3, US4, US5 |

### Within Each User Story

1. Tests FIRST (marked [P] within story)
2. Implementation tasks in dependency order
3. Verify tests pass before moving to next story

### Parallel Opportunities per Phase

**Phase 2 (Foundational)**:
- T008 + T009 can run in parallel (different files)
- T003-T007 must be sequential (same header file)

**Phase 3-8 (User Stories)**:
- All test tasks marked [P] can run in parallel
- After Foundational, stories can be worked on in parallel if team has multiple developers

---

## Parallel Example: Foundational Phase

```bash
# Run in parallel (different files):
Task T008: "Create ConnectionProvider header in src/include/connection/mssql_connection_provider.hpp"
Task T009: "Implement ConnectionProvider::IsInTransaction() in src/connection/mssql_connection_provider.cpp"

# Then sequential (dependencies):
Task T010: "Implement ConnectionProvider::GetConnection() in src/connection/mssql_connection_provider.cpp"
Task T011: "Implement ConnectionProvider::ReleaseConnection() in src/connection/mssql_connection_provider.cpp"
```

## Parallel Example: User Story Tests

```bash
# All tests for US1 can run in parallel:
Task T016: "Create transaction commit test file test/sql/transaction/transaction_commit.test"
Task T017: "Add multi-statement INSERT commit test"
Task T018: "Add UPDATE commit test"
Task T019: "Add DELETE commit test"
Task T020: "Add combined INSERT/UPDATE/DELETE commit test"
```

---

## Implementation Strategy

### MVP First (User Stories 1 + 2 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (CRITICAL)
3. Complete Phase 3: User Story 1 (Commit)
4. Complete Phase 4: User Story 2 (Rollback)
5. **STOP and VALIDATE**: Test commit/rollback independently
6. Deploy/demo if ready - this is a working MVP!

### Incremental Delivery

1. Setup + Foundational → Foundation ready
2. Add US1 + US2 → Test commit/rollback → Deploy (MVP!)
3. Add US3 → Test read-your-writes → Deploy
4. Add US4 → Test mssql_exec in transaction → Deploy
5. Add US5 → Test catalog restriction → Deploy
6. Add US6 → Verify autocommit → Deploy (Complete!)

### Recommended Execution Order (Single Developer)

1. T001-T002 (Setup)
2. T003-T015 (Foundational - sequential)
3. T016-T025 (US1: Commit)
4. T026-T033 (US2: Rollback)
5. T034-T039 (US3: mssql_scan)
6. T040-T044 (US4: mssql_exec)
7. T045-T049 (US5: Catalog Restriction)
8. T050-T054 (US6: Autocommit)
9. T055-T062 (Polish)

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story
- Each user story is independently testable after completion
- Tests use SQLLogicTest format per `docs/TESTING.md`
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently

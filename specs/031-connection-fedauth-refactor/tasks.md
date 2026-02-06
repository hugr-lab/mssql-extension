# Tasks: Connection & FEDAUTH Refactoring

**Input**: Design documents from `/specs/031-connection-fedauth-refactor/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Organization**: Tasks are grouped by user story to enable independent implementation and testing. Phase 0 bugs are mapped to their corresponding user stories.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, etc.)
- Include exact file paths in descriptions

---

## Phase 1: Setup

**Purpose**: No project initialization needed - this is an existing C++ extension. Verify build and tests pass before making changes.

- [ ] T001 Verify clean build with `make debug` passes
- [ ] T002 Verify existing unit tests pass with `make test`
- [ ] T003 Verify integration tests pass with `make integration-test`
- [ ] T004 Create feature branch from main if not already on `031-connection-fedauth-refactor`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Infrastructure changes that MUST be complete before fixing user-story-specific bugs.

**CRITICAL**: No user story work can begin until this phase is complete.

- [ ] T005 Add `MSSQL_POOL_DEBUG_LOG` macro to `src/tds/tds_connection_pool.cpp` (pattern from research.md)
- [ ] T006 Add `GetPoolDebugLevel()` function using `MSSQL_DEBUG` env var in `src/tds/tds_connection_pool.cpp`
- [ ] T007 [P] Add `IsFabricEndpoint()` declaration to `src/include/mssql_platform.hpp` if not already exported
- [ ] T008 [P] Add `is_fabric_endpoint` flag to `MSSQLConnectionInfo` struct in `src/include/connection/mssql_connection_info.hpp`

**Checkpoint**: Debug logging infrastructure ready, Fabric detection available

---

## Phase 3: User Story 1 - Azure Warehouse Operations Work Reliably (Priority: P1) MVP

**Goal**: Fix Bug 0.1 - BCP failures no longer corrupt connection pool state

**Independent Test**: Attach Azure Warehouse, trigger BCP failure, verify `SHOW ALL TABLES` succeeds afterward

### Implementation for User Story 1

- [ ] T009 [US1] Close connection before throwing in BCP error path in `src/copy/bcp_writer.cpp:257-261`
- [ ] T010 [US1] Add state validation in `ConnectionPool::Release()` in `src/tds/tds_connection_pool.cpp:130-167`
- [ ] T011 [US1] Add debug log when closing non-Idle connection in `src/tds/tds_connection_pool.cpp`
- [ ] T012 [US1] Verify with quickstart.md Bug 0.1 test scenario

**Checkpoint**: BCP failures no longer leave connections in corrupted state

---

## Phase 4: User Story 2 - INSERT in Transaction Works with FEDAUTH (Priority: P1)

**Goal**: Fix Bug 0.3 - Schema lookups use pinned transaction connection

**Independent Test**: Run `BEGIN; INSERT INTO azurewh.dbo.table ...; COMMIT;` on FEDAUTH database

### Implementation for User Story 2

- [ ] T013 [US2] Add `ClientContext*` parameter to `MSSQLCatalog::LookupSchema()` in `src/catalog/mssql_catalog.cpp:164`
- [ ] T014 [US2] Refactor `LookupSchema()` to use `ConnectionProvider::GetConnection()` instead of direct pool access
- [ ] T015 [US2] Add `ClientContext*` parameter to `MSSQLCatalog::ScanSchemas()` in `src/catalog/mssql_catalog.cpp:193`
- [ ] T016 [US2] Refactor `ScanSchemas()` to use `ConnectionProvider::GetConnection()`
- [ ] T017 [P] [US2] Add `ClientContext*` parameter to relevant `MSSQLTableSet` methods in `src/catalog/mssql_table_set.cpp`
- [ ] T018 [US2] Update all callers of modified methods to pass `ClientContext&`
- [ ] T019 [US2] Verify with quickstart.md Bug 0.3 test scenario

**Checkpoint**: INSERT in transaction succeeds on FEDAUTH connections

---

## Phase 5: User Story 3 - FEDAUTH Connections Survive Token Expiration (Priority: P1)

**Goal**: Fix Bug 0.4 - Automatic token refresh on expiration

**Independent Test**: Attach Azure database, wait for token expiration (~1 hour or mock), run query

### Implementation for User Story 3

- [ ] T020 [US3] Add `TokenCache::Invalidate(secret_name)` method to `src/azure/azure_token.cpp`
- [ ] T021 [US3] Modify pool factory in `src/connection/mssql_pool_manager.cpp` to support token refresh
- [ ] T022 [US3] Add retry-with-refresh logic on auth failure in `src/connection/mssql_pool_manager.cpp`
- [ ] T023 [US3] Call `TokenCache::Invalidate()` on DETACH in `src/mssql_storage.cpp` detach handler
- [ ] T024 [US3] Add debug logging for token refresh events
- [ ] T025 [US3] Verify with quickstart.md Bug 0.4 test scenario

**Checkpoint**: Long-running sessions survive token expiration

---

## Phase 6: User Story 4 - FEDAUTH ATTACH Fails Fast (Priority: P2)

**Goal**: Fix Bug 0.5 - Validate FEDAUTH connections at ATTACH time

**Independent Test**: ATTACH with invalid Azure secret should fail immediately

### Implementation for User Story 4

- [ ] T026 [US4] Create `ValidateAzureConnection()` function in `src/mssql_storage.cpp`
- [ ] T027 [US4] Call `ValidateAzureConnection()` in FEDAUTH branch at `src/mssql_storage.cpp:766-775`
- [ ] T028 [US4] Ensure validation executes SELECT 1 to verify connection
- [ ] T029 [US4] Verify with quickstart.md Bug 0.5 test scenario

**Checkpoint**: FEDAUTH ATTACH fails immediately on invalid credentials

---

## Phase 7: User Story 5 - Connection Pool Operations Are Silent (Priority: P2)

**Goal**: Fix Bug 0.6 - No debug output without MSSQL_DEBUG env var

**Independent Test**: Run query without MSSQL_DEBUG, verify no `[MSSQL POOL]` messages

### Implementation for User Story 5

- [ ] T030 [US5] Replace `fprintf` with `MSSQL_POOL_DEBUG_LOG` at `src/tds/tds_connection_pool.cpp:60`
- [ ] T031 [US5] Audit all `fprintf` calls in `src/tds/tds_connection_pool.cpp` and replace with debug macro
- [ ] T032 [P] [US5] Audit `src/connection/mssql_pool_manager.cpp` for unconditional debug output
- [ ] T033 [US5] Verify with quickstart.md Bug 0.6 test scenario

**Checkpoint**: Clean output in normal operation, debug info with MSSQL_DEBUG

---

## Phase 8: User Story 6 - Efficient Connection Usage for CTAS (Priority: P2)

**Goal**: Fix Bug 0.2 - Reduce connection acquires from 9+ to 3 or fewer

**Independent Test**: Run CTAS with MSSQL_DEBUG=1, count `[MSSQL POOL] Acquire` messages

### Implementation for User Story 6

- [ ] T034 [US6] Add `HasSchema()` and `IsSchemasLoaded()` methods to `src/catalog/mssql_metadata_cache.cpp`
- [ ] T035 [US6] Refactor `LookupSchema()` to check cache BEFORE acquiring connection in `src/catalog/mssql_catalog.cpp:164-171` *(depends on T014 - both modify same function)*
- [ ] T036 [US6] Add cache check pattern to `LookupTable()` and related methods
- [ ] T037 [US6] Change `CTASExecutionState::InvalidateCache()` to use point invalidation in `src/dml/ctas/mssql_ctas_executor.cpp:285`
- [ ] T038 [US6] Add cache debug logging with `MSSQL_CACHE` prefix at level 2
- [ ] T039 [US6] Verify with quickstart.md Bug 0.2 test scenario (count acquires)

**Checkpoint**: CTAS triggers 3 or fewer connection acquires for cached tables

---

## Phase 9: User Story 9 - Fabric Warehouse BCP Handling (Priority: P2)

**Goal**: Fix Bug 0.7 - Graceful handling of BCP on Fabric (which doesn't support INSERT BULK)

**Independent Test**: Attach Fabric Warehouse, run CTAS, verify graceful fallback or clear error

### Implementation for User Story 9

- [ ] T040 [US9] Set `is_fabric_endpoint` flag during ATTACH in `src/mssql_storage.cpp` using `IsFabricEndpoint()`
- [ ] T041 [US9] Store `is_fabric_endpoint` in catalog/connection info for later checks
- [ ] T042 [US9] Add Fabric check in BCP initialization in `src/copy/copy_function.cpp`
- [ ] T043 [P] [US9] Add Fabric check in CTAS BCP path in `src/dml/ctas/mssql_ctas_executor.cpp`
- [ ] T044 [US9] Implement auto-fallback to INSERT mode when Fabric detected
- [ ] T045 [US9] Add warning log: "Fabric detected, BCP disabled, using INSERT fallback"
- [ ] T046 [US9] Verify with quickstart.md Bug 0.7 test scenario

**Checkpoint**: CTAS on Fabric succeeds using INSERT fallback

---

## Phase 10: User Story 7 - Unified Authentication Architecture (Priority: P3)

**Goal**: Consolidate authentication code paths using strategy pattern

**Independent Test**: All existing auth tests pass after refactoring

### Implementation for User Story 7

- [ ] T047 [US7] Create `src/tds/auth/` directory for authentication strategies
- [ ] T048 [P] [US7] Define `AuthenticationStrategy` interface in `src/include/tds/auth/auth_strategy.hpp`
- [ ] T049 [P] [US7] Implement `SqlServerAuthStrategy` in `src/tds/auth/sql_auth_strategy.cpp`
- [ ] T050 [P] [US7] Implement `AzureServicePrincipalAuthStrategy` in `src/tds/auth/azure_sp_auth_strategy.cpp`
- [ ] T051 [P] [US7] Implement `AzureCliAuthStrategy` in `src/tds/auth/azure_cli_auth_strategy.cpp`
- [ ] T052 [P] [US7] Implement `AzureDeviceCodeAuthStrategy` in `src/tds/auth/azure_device_code_auth_strategy.cpp`
- [ ] T053 [US7] Create `AuthenticationStrategyFactory` in `src/tds/auth/auth_strategy_factory.cpp`
- [ ] T054 [US7] Refactor `TdsConnection` to use strategy pattern for authentication
- [ ] T054a [US7] Add state transition guards to `TdsConnection::TransitionState()` in `src/tds/tds_connection.cpp` (FR-011)
- [ ] T054b [US7] Add assertion/logging for invalid state transitions per data-model.md state machine
- [ ] T054c [US7] Audit transaction descriptor usage across `src/connection/` and `src/catalog/`
- [ ] T054d [US7] Consolidate transaction descriptor to `MSSQLTransaction` in `src/catalog/mssql_transaction.cpp` (FR-012)
- [ ] T055 [US7] Run all integration tests to verify no regressions

**Checkpoint**: Authentication code consolidated, all tests pass

---

## Phase 11: User Story 8 - Clear Error Messages with Context (Priority: P3)

**Goal**: Error messages include server, port, auth type, operation context

**Independent Test**: Trigger errors and verify messages include actionable context

### Implementation for User Story 8

- [ ] T056 [US8] Define `TdsError` struct with error codes in `src/include/tds/tds_error.hpp` (per data-model.md)
- [ ] T057 [US8] Add context fields (server, port, operation) to error construction
- [ ] T058 [US8] Update connection error paths to use `TdsError` with context
- [ ] T059 [US8] Update authentication error paths to include auth type in message
- [ ] T060 [US8] Update pool error messages to include pool name and state

**Checkpoint**: Errors include actionable context for diagnosis

---

## Phase 12: Polish & Cross-Cutting Concerns

**Purpose**: Final cleanup and validation

- [ ] T061 Run full integration test suite with `make integration-test`
- [ ] T062 [P] Update AZURE.md documentation with Fabric BCP limitation
- [ ] T063 [P] Run `clang-format` on all modified files
- [ ] T064 Manual validation using all quickstart.md test scenarios
- [ ] T065 Verify no performance regression in connection acquisition (< 5% increase)
- [ ] T066 Create PR with summary of all bugs fixed

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - verify build first
- **Foundational (Phase 2)**: Depends on Setup - BLOCKS all user stories
- **User Stories (Phases 3-11)**: All depend on Foundational phase completion
  - P1 stories (US1, US2, US3) should be completed first
  - P2 stories (US4, US5, US6, US9) can proceed in parallel
  - P3 stories (US7, US8) are optional refactoring
- **Polish (Phase 12)**: Depends on all desired user stories being complete

### User Story Dependencies

| Story | Priority | Can Start After | Blocks |
|-------|----------|-----------------|--------|
| US1 (BCP State) | P1 | Foundational | None - independent |
| US2 (Transaction INSERT) | P1 | Foundational | None - independent |
| US3 (Token Expiration) | P1 | Foundational | None - independent |
| US4 (ATTACH Validation) | P2 | Foundational | None - independent |
| US5 (Silent Pool) | P2 | Foundational | None - uses T005-T006 |
| US6 (Cache Efficiency) | P2 | Foundational | None - independent |
| US9 (Fabric BCP) | P2 | Foundational + US1 recommended | None |
| US7 (Auth Architecture) | P3 | US1-US6 recommended | None |
| US8 (Error Messages) | P3 | Foundational | None - independent |

### Parallel Opportunities

**Within Foundational Phase:**
```
T007 [IsFabricEndpoint export] + T008 [is_fabric_endpoint flag] - parallel (different files)
```

**Within User Stories:**
```
US1 + US2 + US3 - all P1 stories can run in parallel after Foundational
US4 + US5 + US6 + US9 - all P2 stories can run in parallel
US7 auth strategies (T049-T052) - all implementations can run in parallel
```

**Cross-Story Parallelism:**
```
With multiple developers:
- Developer A: US1 (BCP State)
- Developer B: US2 (Transaction INSERT)
- Developer C: US3 (Token Expiration)
```

---

## Implementation Strategy

### MVP First (P1 Stories Only)

1. Complete Phase 1: Setup (verify build)
2. Complete Phase 2: Foundational (debug macros)
3. Complete Phase 3: US1 - BCP State Corruption
4. Complete Phase 4: US2 - Transaction INSERT
5. Complete Phase 5: US3 - Token Expiration
6. **STOP and VALIDATE**: Test all P1 scenarios from quickstart.md
7. Create PR for Phase 0 bug fixes

### Incremental Delivery

| PR | Stories | Focus |
|----|---------|-------|
| PR #1 | US1, US5 | BCP + Debug Output (smallest, safest) |
| PR #2 | US2 | Transaction INSERT fix |
| PR #3 | US3, US4 | Token handling + ATTACH validation |
| PR #4 | US6 | Cache efficiency |
| PR #5 | US9 | Fabric BCP handling |
| PR #6 | US7, US8 | Refactoring (optional) |

### Per-Task Commit Strategy

- Each task = 1 commit (or logical group)
- Run `make test` after each task
- Run integration tests after each user story phase

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story
- Phase 0 bugs (0.1-0.7) are mapped to user stories US1-US9
- P3 stories (US7, US8) are optional refactoring and can be deferred
- All tests are manual via quickstart.md scenarios (no new test files required)
- Verify no regressions in SQL Server 2019+, Azure SQL, Fabric Warehouse

# Tasks: Extend Pool Stats and Connection Validation

**Input**: Design documents from `/specs/023-pool-stats-validation/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/

**Tests**: Integration tests included per existing project conventions (SQLLogicTest format).

**Organization**: Tasks grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1-US5)
- Include exact file paths in descriptions

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Header changes and constants that multiple user stories depend on

- [x] T001 [P] Add `query_timeout` field to `MSSQLPoolConfig` struct in src/include/connection/mssql_settings.hpp
- [x] T002 [P] Add `pinned_connections` field to `PoolStatistics` struct in src/include/tds/tds_connection_pool.hpp
- [x] T003 [P] Add `catalog_enabled` field to `MSSQLConnectionInfo` struct in src/include/mssql_storage.hpp
- [x] T004 [P] Add `MSSQL_SECRET_CATALOG` constant to src/include/mssql_secret.hpp
- [x] T005 [P] Add `IncrementPinnedCount`/`DecrementPinnedCount` declarations to src/include/connection/mssql_pool_manager.hpp

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure changes that MUST be complete before user stories

**CRITICAL**: No user story work can begin until this phase is complete

- [x] T006 Register `mssql_query_timeout` setting in src/connection/mssql_settings.cpp with default 30, type BIGINT
- [x] T007 [P] Implement `LoadQueryTimeout()` function in src/connection/mssql_settings.cpp to read setting value
- [x] T008 [P] Implement `IncrementPinnedCount`/`DecrementPinnedCount` in src/connection/mssql_pool_manager.cpp with atomic counter

**Checkpoint**: Foundation ready - user story implementation can now begin

---

## Phase 3: User Story 1 - TLS Validation at Attach Time (Priority: P1)

**Goal**: Validate TLS connectivity at ATTACH time with `SELECT 1` query

**Independent Test**: ATTACH with invalid TLS settings returns clear error within 5 seconds

### Implementation for User Story 1

- [x] T009 [US1] Add TLS validation query execution in `ValidateConnection()` after authentication in src/mssql_storage.cpp
- [x] T010 [US1] Add TLS-specific error patterns to `TranslateConnectionError()` in src/mssql_storage.cpp (certificate errors, handshake failures)
- [x] T011 [US1] Add error handling for `ENCRYPT_REQ` response when `use_encrypt=false` in src/mssql_storage.cpp

### Tests for User Story 1

- [ ] T012 [P] [US1] Create TLS validation integration test in test/sql/attach/attach_tls_validation.test

**Checkpoint**: TLS validation at ATTACH is functional; users get clear errors for TLS misconfigurations

---

## Phase 4: User Story 2 - Query Timeout Configuration (Priority: P1)

**Goal**: Allow users to configure query timeout via `mssql_query_timeout` setting

**Independent Test**: Set timeout to 10s, run long query, verify timeout error

### Implementation for User Story 2

- [x] T013 [US2] Update `MSSQLResultStream` constructor to accept timeout parameter in src/include/query/mssql_result_stream.hpp
- [x] T014 [US2] Replace hardcoded `read_timeout_ms_ = 30000` with configurable parameter in src/query/mssql_result_stream.cpp
- [x] T015 [US2] Load query timeout from settings in `ConnectionProvider` and pass to `MSSQLResultStream` in src/connection/mssql_connection_provider.cpp
- [x] T016 [US2] Handle timeout value 0 as infinite (use INT_MAX or similar large value) in src/query/mssql_result_stream.cpp
- [x] T017 [US2] Add timeout error message formatting ("MSSQL query timed out after N seconds") in src/query/mssql_result_stream.cpp

### Tests for User Story 2

- [ ] T018 [P] [US2] Create query timeout integration test in test/sql/integration/query_timeout.test

**Checkpoint**: Query timeout is configurable via SET mssql_query_timeout; users can run long queries

---

## Phase 5: User Story 3 - Catalog-Free Mode (Priority: P2)

**Goal**: Allow disabling catalog integration for serverless/restricted databases

**Independent Test**: ATTACH with `catalog=false`, use `mssql_scan()` successfully, verify catalog access fails with clear error

### Implementation for User Story 3

- [x] T019 [US3] Parse `catalog` field from secret in `CreateMSSQLSecretFromConfig()` in src/mssql_secret.cpp
- [x] T020 [US3] Parse `Catalog` parameter from connection string in `ParseConnectionString()` in src/mssql_storage.cpp
- [x] T021 [US3] Skip catalog initialization in `MSSQLAttach()` when `catalog_enabled=false` in src/mssql_storage.cpp
- [x] T022 [US3] Add `catalog_enabled_` flag to `MSSQLCatalog` class in src/include/catalog/mssql_catalog.hpp
- [x] T023 [US3] Guard `EnsureCacheLoaded()` with catalog_enabled check and throw clear error in src/catalog/mssql_catalog.cpp
- [x] T024 [US3] Verify `mssql_scan` continues to work when catalog disabled (no changes needed, add logging) in src/mssql_functions.cpp
- [x] T025 [US3] Verify `mssql_exec` continues to work when catalog disabled (no changes needed, add logging) in src/mssql_functions.cpp

### Tests for User Story 3

- [ ] T026 [P] [US3] Create catalog-disabled integration test in test/sql/attach/attach_catalog_disabled.test

**Checkpoint**: Users can ATTACH with catalog disabled and use mssql_scan/mssql_exec

---

## Phase 6: User Story 4 - Extended Pool Statistics (Priority: P2)

**Goal**: Add `pinned_connections` column to `mssql_pool_stats()` output

**Independent Test**: Start transaction, verify pinned_connections=1, commit, verify pinned_connections=0

### Implementation for User Story 4

- [x] T027 [US4] Call `IncrementPinnedCount()` in `MSSQLTransaction::SetPinnedConnection()` when pinning in src/catalog/mssql_transaction.cpp
- [x] T028 [US4] Call `DecrementPinnedCount()` in `MSSQLTransaction::SetPinnedConnection()` when unpinning (nullptr) in src/catalog/mssql_transaction.cpp
- [x] T029 [US4] Add `pinned_connections` column to `mssql_pool_stats()` output in src/connection/mssql_diagnostic.cpp
- [x] T030 [US4] Include pinned count from `MssqlPoolManager` in statistics retrieval in src/connection/mssql_diagnostic.cpp

### Tests for User Story 4

- [ ] T031 [P] [US4] Create pool stats integration test in test/sql/integration/pool_stats_extended.test
- [ ] T032 [P] [US4] Create pinned connection tracking test in test/sql/tds_connection/pool_stats_pinned.test

**Checkpoint**: Pool stats show active and pinned connection counts accurately

---

## Phase 7: User Story 5 - TLS by Default (Priority: P3)

**Goal**: Change default `use_encrypt` from `false` to `true`

**Independent Test**: ATTACH without Encrypt parameter to TLS server succeeds; ATTACH with `Encrypt=no` to TLS-required server fails with clear error

### Implementation for User Story 5

- [x] T033 [US5] Change default `use_encrypt = false` to `use_encrypt = true` in `MSSQLConnectionInfo` struct in src/include/mssql_storage.hpp
- [x] T034 [US5] Update `FromSecret()` to default `use_encrypt` to `true` when not specified in src/mssql_storage.cpp
- [x] T035 [US5] Update `FromConnectionString()` to default `encrypt_value` to `true` when not specified in src/mssql_storage.cpp
- [x] T036 [US5] Verify explicit `use_encrypt=false` still works (backward compatibility) in src/mssql_storage.cpp

### Tests for User Story 5

- [ ] T037 [P] [US5] Create TLS default behavior test in test/sql/attach/attach_tls_default.test

**Checkpoint**: TLS is enabled by default; explicit use_encrypt=false preserved for backward compatibility

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Documentation, code cleanup, and final validation

- [ ] T038 [P] Update README.md with new settings documentation (mssql_query_timeout, catalog parameter)
- [ ] T039 [P] Update docs/connection-management.md with TLS-by-default behavior
- [ ] T040 [P] Add C++ unit test for PoolStatistics struct invariants in test/cpp/test_pool_statistics.cpp
- [ ] T041 Run quickstart.md validation scenarios manually
- [ ] T042 Code cleanup: remove any debug logging added during development
- [ ] T043 Verify all error messages match contracts/settings-api.md and contracts/pool-stats-api.md

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - header changes only
- **Foundational (Phase 2)**: Depends on Setup - registers settings and implements core infrastructure
- **User Stories (Phase 3-7)**: All depend on Foundational phase completion
  - US1 (TLS Validation) and US2 (Query Timeout) can run in parallel
  - US3 (Catalog-Free) can run in parallel with US1/US2
  - US4 (Pool Stats) can run in parallel with others
  - US5 (TLS Default) depends on US1 for proper error handling
- **Polish (Phase 8)**: Depends on all user stories being complete

### User Story Dependencies

| Story | Depends On | Can Parallel With |
|-------|------------|-------------------|
| US1 - TLS Validation | Foundational | US2, US3, US4 |
| US2 - Query Timeout | Foundational | US1, US3, US4 |
| US3 - Catalog-Free | Foundational | US1, US2, US4 |
| US4 - Pool Stats | Foundational | US1, US2, US3, US5 |
| US5 - TLS Default | Foundational + US1 | US4 |

### Within Each User Story

- Implementation tasks before tests
- Header changes before implementation
- Core logic before error handling
- Story complete before moving to next priority

### Parallel Opportunities

**Phase 1 (All parallel)**:
```
T001, T002, T003, T004, T005 - all header changes in different files
```

**Phase 2 (Partial parallel)**:
```
T006 first (register setting)
T007, T008 in parallel after T006
```

**User Stories (Parallel across stories)**:
```
After Foundational complete:
- US1 (T009-T012) can run with US2 (T013-T018)
- US3 (T019-T026) can run with US4 (T027-T032)
```

---

## Parallel Example: Phase 1 Setup

```bash
# Launch all header changes together:
Task: "Add query_timeout field to MSSQLPoolConfig in src/include/connection/mssql_settings.hpp"
Task: "Add pinned_connections field to PoolStatistics in src/include/tds/tds_connection_pool.hpp"
Task: "Add catalog_enabled field to MSSQLConnectionInfo in src/include/mssql_storage.hpp"
Task: "Add MSSQL_SECRET_CATALOG constant in src/include/mssql_secret.hpp"
Task: "Add pinned count declarations in src/include/connection/mssql_pool_manager.hpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 + 2 Only)

1. Complete Phase 1: Setup (header changes)
2. Complete Phase 2: Foundational (settings registration)
3. Complete Phase 3: US1 - TLS Validation
4. Complete Phase 4: US2 - Query Timeout
5. **STOP and VALIDATE**: Test TLS validation and query timeout independently
6. Deploy/demo if ready

### Incremental Delivery

1. Setup + Foundational → Foundation ready
2. Add US1 (TLS Validation) → Test → users get fail-fast TLS errors
3. Add US2 (Query Timeout) → Test → users can configure timeouts
4. Add US3 (Catalog-Free) → Test → serverless database users unblocked
5. Add US4 (Pool Stats) → Test → admins can monitor connections
6. Add US5 (TLS Default) → Test → improved security posture

### Parallel Team Strategy

With 2+ developers after Foundational complete:
- Developer A: US1 (TLS Validation) → US5 (TLS Default)
- Developer B: US2 (Query Timeout) → US3 (Catalog-Free) → US4 (Pool Stats)

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story
- Each user story is independently completable and testable
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- US5 depends on US1 for proper error handling of TLS-required servers

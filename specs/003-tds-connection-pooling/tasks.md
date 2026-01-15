# Tasks: TDS Connection, Authentication, and Pooling

**Input**: Design documents from `/specs/003-tds-connection-pooling/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/

**Tests**: Tests included as this is infrastructure code requiring validation.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- Single project with: `src/`, `src/include/`, `test/` at repository root
- TDS layer: `src/tds/` and `src/include/tds/` (pure C++, no DuckDB deps)
- Connection layer: `src/connection/` and `src/include/connection/` (DuckDB integration)

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project initialization and header structure

- [x] T001 Create TDS header directory structure at `src/include/tds/`
- [x] T002 Create connection header directory structure at `src/include/connection/`
- [x] T003 [P] Create TDS source directory at `src/tds/`
- [x] T004 [P] Create connection source directory at `src/connection/`
- [x] T005 Update CMakeLists.txt to include new source directories

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core TDS protocol components that ALL user stories depend on

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

### TDS Types and Constants

- [x] T006 [P] Implement ConnectionState enum and TDS constants in `src/include/tds/tds_types.hpp`
- [x] T007 [P] Implement tds_types.cpp with enum/constant definitions in `src/tds/tds_types.cpp`

### TDS Packet Framing

- [x] T008 [P] Implement TdsPacket class declaration in `src/include/tds/tds_packet.hpp`
- [x] T009 [P] Implement TdsPacket construction/parsing in `src/tds/tds_packet.cpp`

### TCP Socket Wrapper

- [x] T010 [P] Implement TdsSocket class declaration in `src/include/tds/tds_socket.hpp`
- [x] T011 Implement TdsSocket TCP operations (connect/send/receive/close) in `src/tds/tds_socket.cpp`

### TDS Protocol Messages

- [x] T012 [P] Implement TdsProtocol class declaration (PRELOGIN, LOGIN7 builders) in `src/include/tds/tds_protocol.hpp`
- [x] T013 Implement PRELOGIN packet builder in `src/tds/tds_protocol.cpp`
- [x] T014 Implement LOGIN7 packet builder with password encoding (XOR 0xA5, rotate left 4) in `src/tds/tds_protocol.cpp`

**Checkpoint**: TDS protocol layer ready - user story implementation can now begin

---

## Phase 3: User Story 1 - Open and Authenticate Connection (Priority: P1) 🎯 MVP

**Goal**: Open TCP connection to SQL Server and authenticate using SQL Server authentication

**Independent Test**: Call `mssql_open` with valid credentials and verify connection reaches `idle` state, then call `mssql_ping` to confirm alive

### Tests for User Story 1

- [x] T015 [P] [US1] Create SQL test for successful connection open in `test/sql/tds_connection/open_close.test`
- [x] T016 [P] [US1] Create SQL test for authentication failure (invalid credentials) in `test/sql/tds_connection/open_close.test`

### Implementation for User Story 1

- [x] T017 [P] [US1] Implement TdsConnection class declaration with state machine in `src/include/tds/tds_connection.hpp`
- [x] T018 [US1] Implement TdsConnection::connect() TCP establishment in `src/tds/tds_connection.cpp`
- [x] T019 [US1] Implement TdsConnection::authenticate() PRELOGIN/LOGIN7 handshake in `src/tds/tds_connection.cpp`
- [x] T020 [US1] Implement atomic state transitions (disconnected→authenticating→idle) in `src/tds/tds_connection.cpp`
- [x] T021 [P] [US1] Implement MssqlDiagnostic header with mssql_open declaration in `src/include/connection/mssql_diagnostic.hpp`
- [x] T022 [US1] Implement mssql_open scalar function (resolve secret, create connection, return handle) in `src/connection/mssql_diagnostic.cpp`
- [x] T023 [US1] Register mssql_open function in `src/mssql_extension.cpp`

**Checkpoint**: mssql_open works - can establish authenticated TDS connection

---

## Phase 4: User Story 2 - Close Connection (Priority: P1)

**Goal**: Explicitly close a connection and release TCP resources

**Independent Test**: Open a connection, close it with `mssql_close`, verify handle is invalidated and socket released

### Tests for User Story 2

- [x] T024 [P] [US2] Add SQL test for successful connection close in `test/sql/tds_connection/open_close.test`
- [x] T025 [P] [US2] Add SQL test for idempotent close (closing already-closed handle) in `test/sql/tds_connection/open_close.test`

### Implementation for User Story 2

- [x] T026 [US2] Implement TdsConnection::close() with socket cleanup in `src/tds/tds_connection.cpp`
- [x] T027 [US2] Implement TdsConnection::send_attention() for cancelling executing queries in `src/tds/tds_connection.cpp`
- [x] T028 [US2] Add mssql_close declaration to `src/include/connection/mssql_diagnostic.hpp`
- [x] T029 [US2] Implement mssql_close scalar function (lookup handle, cancel if executing, close) in `src/connection/mssql_diagnostic.cpp`
- [x] T030 [US2] Register mssql_close function in `src/mssql_extension.cpp`

**Checkpoint**: mssql_open and mssql_close work - complete connection lifecycle

---

## Phase 5: User Story 3 - Ping Connection (Priority: P1)

**Goal**: Verify a connection is still alive using TDS-level ping

**Independent Test**: Ping an open connection (expect success), ping after server-side disconnect (expect failure)

### Tests for User Story 3

- [x] T031 [P] [US3] Create SQL test for successful ping on idle connection in `test/sql/tds_connection/ping.test`
- [x] T032 [P] [US3] Create SQL test for ping on invalid handle in `test/sql/tds_connection/ping.test`

### Implementation for User Story 3

- [x] T033 [US3] Implement TdsConnection::ping() sending empty SQLBATCH and awaiting DONE token in `src/tds/tds_connection.cpp`
- [x] T034 [US3] Implement TdsConnection::is_alive() quick state check (no I/O) in `src/tds/tds_connection.cpp`
- [x] T035 [US3] Implement TdsConnection::validate_with_ping() I/O-based health check in `src/tds/tds_connection.cpp`
- [x] T036 [US3] Add mssql_ping declaration to `src/include/connection/mssql_diagnostic.hpp`
- [x] T037 [US3] Implement mssql_ping scalar function (lookup handle, send TDS ping, return bool) in `src/connection/mssql_diagnostic.cpp`
- [x] T038 [US3] Register mssql_ping function in `src/mssql_extension.cpp`

**Checkpoint**: All P1 diagnostic functions work - mssql_open, mssql_close, mssql_ping

---

## Phase 6: User Story 4 - Configure Connection Pool Settings (Priority: P2)

**Goal**: Configure connection pool behavior through DuckDB variables

**Independent Test**: Set pool configuration variables and verify the pool respects those settings

### Tests for User Story 4

- [x] T039 [P] [US4] Create SQL test for setting mssql_connection_limit in `test/sql/tds_connection/settings.test`
- [x] T040 [P] [US4] Create SQL test for all pool settings (cache, timeout, idle_timeout, min_connections, acquire_timeout) in `test/sql/tds_connection/settings.test`

### Implementation for User Story 4

- [x] T041 [P] [US4] Implement PoolConfig struct in `src/include/tds/connection_pool.hpp`
- [x] T042 [P] [US4] Implement MssqlSettings header with RegisterMSSQLSettings declaration in `src/include/connection/mssql_settings.hpp`
- [x] T043 [US4] Implement RegisterMSSQLSettings with DBConfig::AddExtensionOption for all 6 settings in `src/connection/mssql_settings.cpp`
- [x] T044 [US4] Implement ValidatePositive and ValidateNonNegative callbacks in `src/connection/mssql_settings.cpp`
- [x] T045 [US4] Implement LoadPoolConfig to read settings from ClientContext in `src/connection/mssql_settings.cpp`
- [x] T046 [US4] Call RegisterMSSQLSettings from Load() in `src/mssql_extension.cpp`

**Checkpoint**: Pool settings configurable via SET commands

---

## Phase 7: User Story 5 - Automatic Connection Pooling (Priority: P2)

**Goal**: Connections are automatically pooled and reused

**Independent Test**: Execute multiple sequential queries and observe connections are reused via pool statistics

### Tests for User Story 5

- [x] T047 [P] [US5] Create SQL test for connection reuse (acquire, release, acquire same) in `test/sql/tds_connection/pool_stats.test`
- [x] T048 [P] [US5] Create SQL test for pool limit enforcement in `test/sql/tds_connection/pool_stats.test`

### Implementation for User Story 5

- [x] T049 [P] [US5] Implement ConnectionMetadata struct in `src/include/tds/connection_pool.hpp`
- [x] T050 [P] [US5] Implement PoolStatistics struct in `src/include/tds/connection_pool.hpp`
- [x] T051 [US5] Implement ConnectionPool class declaration (idle queue, active map, mutex, condition_variable) in `src/include/tds/connection_pool.hpp`
- [x] T052 [US5] Implement ConnectionPool::acquire() with wait-on-exhausted logic in `src/tds/connection_pool.cpp`
- [x] T053 [US5] Implement ConnectionPool::release() with idle queue return in `src/tds/connection_pool.cpp`
- [x] T054 [US5] Implement ConnectionPool::get_stats() in `src/tds/connection_pool.cpp`
- [x] T055 [US5] Implement background cleanup thread for idle timeout in `src/tds/connection_pool.cpp`
- [x] T056 [US5] Implement ConnectionPool::shutdown() with clean thread termination in `src/tds/connection_pool.cpp`
- [x] T057 [US5] Implement tiered validation (quick check vs TDS ping for long-idle) in `src/tds/connection_pool.cpp`
- [x] T058 [P] [US5] Implement MssqlPoolManager header (pool lifecycle per ATTACH context) in `src/include/connection/mssql_pool_manager.hpp`
- [x] T059 [US5] Implement MssqlPoolManager::GetOrCreatePool() in `src/connection/mssql_pool_manager.cpp`
- [x] T060 [US5] Implement MssqlPoolManager::RemovePool() in `src/connection/mssql_pool_manager.cpp`
- [x] T061 [US5] Integrate MssqlPoolManager with ATTACH mechanism in `src/mssql_storage.cpp`

**Checkpoint**: Connection pooling works - connections reused automatically

---

## Phase 8: User Story 6 - Connection Recovery After Cancellation (Priority: P2)

**Goal**: Connections are reusable after query cancellation

**Independent Test**: Start a query, cancel it, verify connection returns to `idle` state and can be reused

### Tests for User Story 6

- [x] T062 [P] [US6] Create SQL test for connection state after cancellation in `test/sql/tds_connection/open_close.test`

### Implementation for User Story 6

- [x] T063 [US6] Implement state transition executing→cancelling→idle in `src/tds/tds_connection.cpp`
- [x] T064 [US6] Implement ATTENTION_ACK detection in response parsing in `src/tds/tds_connection.cpp`
- [x] T065 [US6] Implement cancellation timeout (5 second) with fallback to disconnected in `src/tds/tds_connection.cpp`
- [x] T066 [US6] Update ConnectionPool::release() to validate and return cancelled connections to idle in `src/tds/connection_pool.cpp`

**Checkpoint**: Cancelled connections are recoverable and reusable

---

## Phase 9: User Story 7 - View Pool Statistics (Priority: P3)

**Goal**: View connection pool statistics for monitoring

**Independent Test**: Execute queries, call statistics function, verify counts match expected values

### Tests for User Story 7

- [x] T067 [P] [US7] Create SQL test for mssql_pool_stats table function in `test/sql/tds_connection/pool_stats.test`
- [x] T068 [P] [US7] Create SQL test for mssql_pool_stats on nonexistent context in `test/sql/tds_connection/pool_stats.test`

### Implementation for User Story 7

- [x] T069 [US7] Implement MssqlPoolStatsFunction table function class in `src/connection/mssql_diagnostic.cpp`
- [x] T070 [US7] Implement Bind(), Init(), Execute() for pool stats table function in `src/connection/mssql_diagnostic.cpp`
- [x] T071 [US7] Register mssql_pool_stats table function in `src/mssql_extension.cpp`

**Checkpoint**: All user stories complete - full TDS connection and pooling functionality

---

## Phase 10: Polish & Cross-Cutting Concerns

**Purpose**: Improvements that affect multiple user stories

- [x] T072 [P] Add error messages with SQL Server error codes where applicable
- [x] T073 [P] Add logging for connection state transitions
- [x] T074 Run quickstart.md validation to verify all examples work
- [x] T075 Code review for thread safety across all pool operations
- [x] T076 Verify all tests pass with SQL Server 2019 Docker container

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories 1-3 (P1)**: Sequential - US1 → US2 → US3 (build on each other)
- **User Stories 4-6 (P2)**: Depend on Foundational; US5 depends on US4 for settings
- **User Story 7 (P3)**: Depends on US5 (pool must exist for stats)
- **Polish (Phase 10)**: Depends on all user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational (Phase 2)
- **User Story 2 (P1)**: Depends on US1 (needs TdsConnection)
- **User Story 3 (P1)**: Depends on US1 (needs TdsConnection)
- **User Story 4 (P2)**: Can start after Foundational (Phase 2) - independent
- **User Story 5 (P2)**: Depends on US1-3 (needs TdsConnection), US4 (needs PoolConfig)
- **User Story 6 (P2)**: Depends on US5 (needs ConnectionPool)
- **User Story 7 (P3)**: Depends on US5 (needs ConnectionPool and stats)

### Within Each User Story

- Tests (if included) MUST be written and FAIL before implementation
- Headers before implementations
- TDS layer before connection layer
- Core implementation before DuckDB registration
- Story complete before moving to next priority

### Parallel Opportunities

- All Setup tasks marked [P] can run in parallel
- All Foundational tasks marked [P] can run in parallel (within Phase 2)
- Tests for a user story marked [P] can run in parallel
- Headers marked [P] can be created in parallel
- US4 (Settings) can run in parallel with US1-3 (they don't depend on each other initially)

---

## Parallel Example: Foundational Phase

```bash
# Launch all foundational headers together:
Task: "Implement ConnectionState enum and TDS constants in src/include/tds/tds_types.hpp"
Task: "Implement TdsPacket class declaration in src/include/tds/tds_packet.hpp"
Task: "Implement TdsSocket class declaration in src/include/tds/tds_socket.hpp"
Task: "Implement TdsProtocol class declaration in src/include/tds/tds_protocol.hpp"
```

---

## Parallel Example: User Story 4 (Settings) with User Story 1 (Open)

```bash
# These can proceed in parallel after Foundational:
# Developer A: US4 - Settings
Task: "Implement PoolConfig struct in src/include/tds/connection_pool.hpp"
Task: "Implement RegisterMSSQLSettings in src/connection/mssql_settings.cpp"

# Developer B: US1 - Open Connection
Task: "Implement TdsConnection class declaration in src/include/tds/tds_connection.hpp"
Task: "Implement TdsConnection::connect() in src/tds/tds_connection.cpp"
```

---

## Implementation Strategy

### MVP First (User Stories 1-3 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (CRITICAL - blocks all stories)
3. Complete Phase 3: User Story 1 (mssql_open)
4. Complete Phase 4: User Story 2 (mssql_close)
5. Complete Phase 5: User Story 3 (mssql_ping)
6. **STOP and VALIDATE**: Test diagnostic functions independently
7. Deploy/demo if ready - MVP complete!

### Incremental Delivery

1. Complete Setup + Foundational → Foundation ready
2. Add User Stories 1-3 → Test independently → Deploy/Demo (MVP!)
3. Add User Story 4 → Settings configurable
4. Add User Story 5 → Pool operational → Deploy/Demo
5. Add User Story 6 → Cancellation recovery
6. Add User Story 7 → Statistics observable → Deploy/Demo
7. Each story adds value without breaking previous stories

### Recommended Execution Order

1. T001-T005: Setup (directory structure)
2. T006-T014: Foundational (TDS protocol layer)
3. T015-T023: US1 - mssql_open (P1, MVP)
4. T024-T030: US2 - mssql_close (P1, MVP)
5. T031-T038: US3 - mssql_ping (P1, MVP)
6. T039-T046: US4 - Settings (P2)
7. T047-T061: US5 - Pooling (P2)
8. T062-T066: US6 - Cancellation recovery (P2)
9. T067-T071: US7 - Pool statistics (P3)
10. T072-T076: Polish

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each P1 story builds on previous: US1 (open) → US2 (close) → US3 (ping)
- US4 (Settings) can run in parallel with P1 stories
- Verify tests fail before implementing
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- Test with SQL Server 2019 Docker: `mcr.microsoft.com/mssql/server:2019-latest`

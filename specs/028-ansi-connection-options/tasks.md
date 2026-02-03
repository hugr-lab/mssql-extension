# Tasks: ANSI Connection Options Fix

**Input**: Design documents from `/specs/028-ansi-connection-options/`
**Prerequisites**: plan.md (required), spec.md (required), research.md

**Tests**: Integration tests are included per spec.md requirements (SC-003).

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2)
- Include exact file paths in descriptions

## Path Conventions

- **Single project**: C++ extension at repository root
- Source: `src/tds/`, `src/include/tds/`
- Tests: `test/sql/integration/`
- Docs: `docs/`

---

## Phase 1: Setup

**Purpose**: No setup required - this is a bug fix to existing codebase

**Note**: Existing project structure is already in place. Proceed directly to implementation.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Define ANSI initialization SQL constant used by both user stories

- [x] T001 Define ANSI_INIT_SQL constant with SET statements in src/tds/tds_connection.cpp

```cpp
// ANSI SET statements required for DDL compatibility (per FR-002)
static const char* ANSI_INIT_SQL =
    "SET CONCAT_NULL_YIELDS_NULL ON;"
    "SET ANSI_WARNINGS ON;"
    "SET ANSI_NULLS ON;"
    "SET ANSI_PADDING ON;"
    "SET QUOTED_IDENTIFIER ON;";
```

**Checkpoint**: Foundation ready - user story implementation can begin

---

## Phase 3: User Story 1 - Execute DDL Commands Successfully (Priority: P1) 🎯 MVP

**Goal**: Enable DDL commands (ALTER DATABASE, DBCC, etc.) to execute without SET options error

**Independent Test**: Execute `ALTER DATABASE CURRENT SET RECOVERY SIMPLE` via `mssql_exec()` and verify it completes without error

### Implementation for User Story 1

- [x] T002 [US1] Add `InitializeAnsiSettings()` method declaration in src/include/tds/tds_connection.hpp

Add to private section:
```cpp
bool InitializeAnsiSettings();
```

- [x] T003 [US1] Implement `InitializeAnsiSettings()` method in src/tds/tds_connection.cpp

Implementation must:
1. Build SQL_BATCH packet with ANSI_INIT_SQL using `TdsProtocol::BuildSqlBatch()`
2. Send packet via socket
3. Receive and validate response via `TdsProtocol::IsSuccessResponse()`
4. Set `last_error_` and return false on any failure
5. Return true on success

- [x] T004 [US1] Modify `DoLogin7()` to call `InitializeAnsiSettings()` after successful LOGIN7 in src/tds/tds_connection.cpp

After line 230 (after spid and packet size are set):
```cpp
// Initialize ANSI session options (required for DDL compatibility)
if (!InitializeAnsiSettings()) {
    return false;  // last_error_ already set
}
```

- [x] T005 [US1] Add debug logging for ANSI initialization in src/tds/tds_connection.cpp

Add logs in `InitializeAnsiSettings()`:
- Start: "InitializeAnsiSettings: starting"
- Success: "InitializeAnsiSettings: success"
- Failure: "InitializeAnsiSettings: failed - {error}"

### Tests for User Story 1

- [x] T006 [US1] Create integration test file test/sql/integration/ddl_ansi_settings.test

Test file must include:
1. Test group header: `# group: [integration]`
2. ATTACH to TestDB
3. Test: Verify @@OPTIONS bitmask includes ANSI bits
4. Test: Execute `ALTER DATABASE` DDL command via mssql_exec()
5. Test: Execute SELECT against the database to verify connection still works

**Checkpoint**: User Story 1 complete - DDL commands work on fresh connections

---

## Phase 4: User Story 2 - Connection Pool Maintains ANSI Settings (Priority: P2)

**Goal**: Ensure ANSI settings persist or are re-applied across connection pool reuse

**Independent Test**: Execute DDL command, return connection to pool, reacquire connection, execute another DDL command - both must succeed

### Implementation for User Story 2

- [x] T007 [US2] Add helper method `GetAnsiInitSql()` to return ANSI_INIT_SQL in src/tds/tds_connection.cpp

```cpp
static const std::string& GetAnsiInitSql() {
    static const std::string sql = ANSI_INIT_SQL;
    return sql;
}
```

- [x] T008 [US2] Modify `ExecuteBatch()` to prepend ANSI SET when `needs_reset_` is true in src/tds/tds_connection.cpp

Before building packets (around line 406):
```cpp
// If connection needs reset, prepend ANSI SET to SQL (RESET_CONNECTION clears session options)
std::string effective_sql = sql;
if (needs_reset_) {
    effective_sql = GetAnsiInitSql() + sql;
    MSSQL_CONN_DEBUG_LOG(1, "ExecuteBatch: prepending ANSI SET due to needs_reset_");
}

// Build SQL_BATCH packet(s) using effective_sql instead of sql
std::vector<TdsPacket> packets = TdsProtocol::BuildSqlBatchMultiPacket(effective_sql, negotiated_packet_size_, txn_desc);
```

### Tests for User Story 2

- [x] T009 [US2] Add pool reuse tests to test/sql/integration/ddl_ansi_settings.test

Add test scenarios:
1. Execute DDL command
2. Execute a normal SELECT (to potentially trigger pool return/reuse)
3. Execute another DDL command (verifies ANSI settings maintained)
4. Force pool reuse by running 10+ queries then DDL (stress test)

**Checkpoint**: User Story 2 complete - ANSI settings work across connection pool reuse

---

## Phase 5: Polish & Cross-Cutting Concerns

**Purpose**: Documentation updates and final validation

- [x] T010 [P] Update docs/architecture.md with ANSI connection initialization section

Add section describing:
- ANSI SET options applied on connection
- Why they are required (DDL, indexed views, etc.)
- Implementation location (DoLogin7 and ExecuteBatch for reset)

- [ ] T011 [P] Add note to README about ANSI-compliant connections

Brief mention that connections are initialized with SQL Server ANSI options for DDL compatibility.

- [x] T012 Run full test suite to verify no regressions (`make test-all`)

- [ ] T013 Run quickstart.md validation to verify fix works as documented

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: N/A - existing project
- **Foundational (Phase 2)**: T001 must complete first (defines constant)
- **User Story 1 (Phase 3)**: Depends on T001
- **User Story 2 (Phase 4)**: Depends on T001; can run in parallel with US1 for separate files
- **Polish (Phase 5)**: Depends on both user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Independent - fixes fresh connections
- **User Story 2 (P2)**: Independent - fixes pooled connections (uses same constant)

### Within Each User Story

- Header declaration (T002) before implementation (T003)
- Implementation before test (T003 before T006)
- T003 before T004 (method must exist before being called)

### Parallel Opportunities

**Phase 3 (US1)**:
- T002 and T003 can be done together (different files)

**Phase 4 (US2)**:
- T007 and T008 are in same file, must be sequential
- T009 (test) depends on T007, T008

**Phase 5 (Polish)**:
- T010 and T011 can run in parallel (different files)
- T012 and T013 must wait for all implementation

---

## Parallel Example: Phase 3 and Phase 4

```bash
# After T001 completes, User Stories can proceed in parallel:

# User Story 1 (Developer A):
Task: "Add InitializeAnsiSettings() declaration in src/include/tds/tds_connection.hpp"
Task: "Implement InitializeAnsiSettings() in src/tds/tds_connection.cpp"

# User Story 2 (Developer B) - can start in parallel if modifying different sections:
Task: "Add GetAnsiInitSql() helper in src/tds/tds_connection.cpp"
# (Wait for T003 if editing same function area)
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete T001: Define ANSI_INIT_SQL constant
2. Complete T002-T005: Implement InitializeAnsiSettings and integrate into DoLogin7
3. Complete T006: Write and run integration test
4. **STOP and VALIDATE**: Test DDL commands on fresh connections
5. If MVP works, proceed to US2

### Incremental Delivery

1. T001 (Foundational) → Constant defined
2. T002-T006 (US1) → Fresh connections work → Test independently
3. T007-T009 (US2) → Pooled connections work → Test independently
4. T010-T013 (Polish) → Documentation complete, full regression test

### Single Developer Strategy

Execute tasks in order: T001 → T002 → T003 → T004 → T005 → T006 → T007 → T008 → T009 → T010 → T011 → T012 → T013

---

## Notes

- Total tasks: 13
- User Story 1 (P1/MVP): 5 tasks (T002-T006)
- User Story 2 (P2): 3 tasks (T007-T009)
- Foundational: 1 task (T001)
- Polish: 4 tasks (T010-T013)
- Parallel opportunities: T002/T003 (different files), T010/T011 (different files)
- Suggested MVP scope: Complete through T006 (User Story 1)
- All tasks affect 3 files: tds_connection.cpp, tds_connection.hpp, ddl_ansi_settings.test

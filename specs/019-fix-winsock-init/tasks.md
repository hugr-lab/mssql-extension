# Tasks: Fix Windows Winsock Initialization

**Input**: Design documents from `/specs/019-fix-winsock-init/`
**Prerequisites**: plan.md, spec.md, research.md, quickstart.md

**Tests**: Not explicitly requested in spec. No test tasks generated.

**Organization**: Tasks grouped by user story. This is a minimal bug fix — most stories map to a single implementation change.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup

**Purpose**: No setup needed — existing project, single file change.

- [x] T001 Verify current branch is `019-fix-winsock-init` and clean working tree

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Add Winsock initialization infrastructure that all connection stories depend on.

**CRITICAL**: No user story work can begin until this phase is complete.

- [x] T002 Add `EnsureWinsockInitialized()` static function in `src/tds/tds_socket.cpp` — include `<mutex>` for `std::call_once`, define `static std::once_flag winsock_init_flag` and `static bool winsock_initialized`, implement `InitializeWinsock()` calling `WSAStartup(MAKEWORD(2, 2), &wsaData)` with `atexit(WSACleanup)` on success. Entire block guarded by `#ifdef _WIN32`. Place after existing `#ifdef _WIN32` / `#else` platform block (after line 44).
- [x] T003 Add `EnsureWinsockInitialized()` call at top of `TdsSocket::Connect()` in `src/tds/tds_socket.cpp` — before the `getaddrinfo` call (around line 99), add `#ifdef _WIN32` block calling `std::call_once(winsock_init_flag, InitializeWinsock)` and returning false with error message if `!winsock_initialized`.

**Checkpoint**: Foundation ready — Winsock initialization code is in place.

---

## Phase 3: User Story 1 — Connect to SQL Server from Windows (Priority: P1) MVP

**Goal**: Windows users can connect to SQL Server using TCP connection strings (IP:port or hostname:port).

**Independent Test**: Build on Windows (MSVC and MinGW), run `ATTACH 'Server=host,port;...' AS db (TYPE mssql)` and execute a query.

### Implementation for User Story 1

- [x] T004 [US1] Build release on local machine to verify no compilation errors on macOS/Linux: `GEN=ninja make release`
- [x] T005 [US1] Run existing integration tests on Linux/macOS to verify no regression: `make test && make integration-test`

**Checkpoint**: US1 code is complete. Needs Windows machine or CI to verify actual Windows connectivity.

---

## Phase 4: User Story 2 — TLS Connections from Windows (Priority: P1)

**Goal**: Windows users can connect with TLS encryption enabled.

**Independent Test**: On Windows, attach with `encrypt=yes;trustServerCertificate=yes` and run queries.

### Implementation for User Story 2

No additional code changes needed — TLS uses the same `TdsSocket::Connect()` for the underlying TCP connection. Once Winsock is initialized (Phase 2), TLS connections work automatically.

- [x] T006 [US2] Verify TLS tests still pass on Linux/macOS with `MSSQL_TEST_DSN_TLS` exported

**Checkpoint**: US2 is covered by the same fix as US1.

---

## Phase 5: User Story 3 — Multiple Concurrent Connections (Priority: P2)

**Goal**: Thread-safe initialization supports concurrent connection creation.

**Independent Test**: Attach multiple MSSQL databases and run parallel queries on Windows.

### Implementation for User Story 3

No additional code changes needed — `std::call_once` guarantees thread-safe one-time execution. The design in Phase 2 already handles this.

- [x] T007 [US3] Verify parallel query tests pass on Linux/macOS (no regression in connection pool behavior)

**Checkpoint**: US3 is inherently covered by the `std::call_once` pattern.

---

## Phase 6: User Story 4 — No Regression on Linux/macOS (Priority: P1)

**Goal**: Existing Linux/macOS behavior is completely unchanged.

**Independent Test**: Full test suite passes with zero failures.

### Implementation for User Story 4

- [x] T008 [US4] Run full test suite on Linux/macOS: `make test && make integration-test` with `MSSQL_TEST_DSN_TLS` exported — all 108 test cases, 2741 assertions must pass

**Checkpoint**: No regression confirmed.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: CI verification and commit.

- [ ] T009 Trigger Windows CI builds (MSVC and MinGW): `gh workflow run ci.yml --ref 019-fix-winsock-init -f run_windows_build=true`
- [ ] T010 Verify both Windows builds pass in CI
- [ ] T011 Commit changes with descriptive message and create PR

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies
- **Foundational (Phase 2)**: Depends on Phase 1 — T002 then T003 (sequential, same file)
- **User Stories (Phases 3-6)**: All depend on Phase 2 completion
  - US1, US2, US3, US4 can run in parallel (they are verification tasks, not code changes)
- **Polish (Phase 7)**: Depends on all user stories being verified

### User Story Dependencies

- **US1 (P1)**: Depends on Phase 2 only
- **US2 (P1)**: Depends on Phase 2 only (same fix covers TLS)
- **US3 (P2)**: Depends on Phase 2 only (std::call_once handles concurrency)
- **US4 (P1)**: Depends on Phase 2 only (regression check)

### Parallel Opportunities

- T004, T005, T006, T007, T008 can all run in parallel after Phase 2 is complete (they test different aspects on the same build)

---

## Parallel Example: Verification

```bash
# After Phase 2 is complete, run all verification in parallel:
Task: "Build release to verify compilation" (T004)
Task: "Run integration tests" (T005)
Task: "Verify TLS tests" (T006)
Task: "Verify parallel query tests" (T007)
Task: "Run full test suite" (T008)
```

---

## Implementation Strategy

### MVP First (Phase 2 Only)

1. Complete T001 (verify branch)
2. Complete T002 (add initialization function)
3. Complete T003 (add call in Connect)
4. **STOP and VALIDATE**: Build and test locally (T004, T005)
5. This is the entire fix — 2 code tasks in 1 file

### Incremental Delivery

1. Phase 2: Add WSAStartup code (~15 lines in 1 file)
2. Phase 3-6: Verify on all platforms
3. Phase 7: CI and PR

---

## Notes

- This is a minimal bug fix: 2 implementation tasks (T002, T003) in a single file
- All user stories are satisfied by the same code change
- Verification tasks (T004-T008) confirm the fix works without regression
- The actual Windows connectivity test requires either a Windows machine or Windows CI

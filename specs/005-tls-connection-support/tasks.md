# Tasks: TLS Connection Support

**Input**: Design documents from `/specs/005-tls-connection-support/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Integration test requested in spec (In Scope: "Integration test with TLS-enabled SQL Server")

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## User Story Mapping

| ID | Title | Priority |
|----|-------|----------|
| US1 | Encrypted Connection to SQL Server | P1 |
| US2 | Plaintext Connection Remains Default | P1 |
| US3 | Clear TLS Error Messages | P1 |
| US4 | Trust Server Certificate by Default | P2 |
| US5 | Cross-Platform TLS Support | P2 |

---

## Phase 1: Setup (Build System Configuration) ✅

**Purpose**: Configure vcpkg and CMake for mbedTLS integration

- [x] T001 Verify mbedtls dependency in vcpkg.json
- [x] T002 Add mbedTLS find_package and target_link_libraries to CMakeLists.txt
- [x] T003 Add src/tds/tds_tls_context.cpp to EXTENSION_SOURCES in CMakeLists.txt
- [x] T004 Verify build succeeds with mbedTLS on current platform

---

## Phase 2: Foundational (TLS Context Infrastructure) ✅

**Purpose**: Core TLS wrapper that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [x] T005 Create TlsTdsContext header in src/include/tds/tds_tls_context.hpp with mbedTLS members
- [x] T006 Implement TlsTdsContext::Initialize() in src/tds/tds_tls_context.cpp (entropy, RNG, config)
- [x] T007 Implement TlsTdsContext::WrapSocket() in src/tds/tds_tls_context.cpp (attach to existing fd)
- [x] T008 Implement TlsTdsContext::Handshake() in src/tds/tds_tls_context.cpp (TLS handshake loop)
- [x] T009 Implement TlsTdsContext::Send() in src/tds/tds_tls_context.cpp (mbedtls_ssl_write wrapper)
- [x] T010 Implement TlsTdsContext::Receive() in src/tds/tds_tls_context.cpp (mbedtls_ssl_read wrapper)
- [x] T011 Implement TlsTdsContext::Close() in src/tds/tds_tls_context.cpp (proper cleanup order)
- [x] T012 Implement TlsTdsContext::GetLastError() in src/tds/tds_tls_context.cpp (mbedtls_strerror)

**Checkpoint**: TlsTdsContext compiles and can be instantiated - ready for socket integration ✅

---

## Phase 3: User Story 1 - Encrypted Connection to SQL Server (Priority: P1) 🎯 MVP ✅

**Goal**: Enable TLS-encrypted connections via `use_encrypt=true` secret option

**Independent Test**: Create secret with `use_encrypt=true`, attach to TLS-enabled SQL Server, execute query

### Implementation for User Story 1

- [x] T013 [US1] Rename MSSQL_SECRET_USE_SSL to MSSQL_SECRET_USE_ENCRYPT in src/include/mssql_secret.hpp
- [x] T014 [US1] Update CreateMSSQLSecretFromConfig() to parse use_encrypt in src/mssql_secret.cpp
- [x] T015 [US1] Add use_encrypt parameter to TdsProtocol::BuildPrelogin() in src/include/tds/tds_protocol.hpp
- [x] T016 [US1] Modify BuildPrelogin() to send ENCRYPT_ON when use_encrypt=true in src/tds/tds_protocol.cpp
- [x] T017 [US1] Add optional TlsTdsContext member to TdsSocket in src/include/tds/tds_socket.hpp
- [x] T018 [US1] Add TdsSocket::EnableTls() method declaration in src/include/tds/tds_socket.hpp
- [x] T019 [US1] Implement TdsSocket::EnableTls() to create and handshake TlsTdsContext in src/tds/tds_socket.cpp
- [x] T020 [US1] Modify TdsSocket::Send() to route through TLS when enabled in src/tds/tds_socket.cpp
- [x] T021 [US1] Modify TdsSocket::Receive() to route through TLS when enabled in src/tds/tds_socket.cpp
- [x] T022 [US1] Add use_encrypt parameter to TdsConnection::Authenticate() in src/include/tds/tds_connection.hpp
- [x] T023 [US1] Modify DoPrelogin() to pass use_encrypt and check server response in src/tds/tds_connection.cpp
- [x] T024 [US1] Call socket_->EnableTls() after PRELOGIN when encryption negotiated in src/tds/tds_connection.cpp
- [x] T025 [US1] Pass use_encrypt from secret to TdsConnection in pool acquire flow in src/connection/mssql_pool_manager.cpp

**Checkpoint**: TLS connection can be established with `use_encrypt=true` ✅

---

## Phase 4: User Story 2 - Plaintext Connection Remains Default (Priority: P1) ✅

**Goal**: Ensure existing connections without `use_encrypt` continue to work unchanged

**Independent Test**: Create secret without `use_encrypt`, attach to SQL Server, verify plaintext connection

### Implementation for User Story 2

- [x] T026 [US2] Verify use_encrypt defaults to false in CreateMSSQLSecretFromConfig() in src/mssql_secret.cpp
- [x] T027 [US2] Ensure BuildPrelogin() sends ENCRYPT_NOT_SUP when use_encrypt=false in src/tds/tds_protocol.cpp
- [x] T028 [US2] Verify TdsSocket skips TLS when use_encrypt=false in src/tds/tds_socket.cpp
- [x] T029 [US2] Add backward compatibility test case to test environment configuration

**Checkpoint**: Existing plaintext connections work without modification ✅

---

## Phase 5: User Story 3 - Clear TLS Error Messages (Priority: P1) ✅

**Goal**: Provide distinct, actionable error messages for all TLS failure scenarios

**Independent Test**: Cause TLS failures and verify each produces unique error message

### Implementation for User Story 3

- [x] T030 [US3] Define TLS error codes enum in src/include/tds/tds_tls_context.hpp
- [x] T031 [US3] Implement FormatTlsError() helper using mbedtls_strerror in src/tds/tds_tls_context.cpp
- [x] T032 [US3] Add error for server ENCRYPT_NOT_SUP response in src/tds/tds_connection.cpp
- [x] T033 [US3] Add error for server ENCRYPT_OFF response in src/tds/tds_connection.cpp
- [x] T034 [US3] Add error for TLS handshake timeout in src/tds/tds_tls_context.cpp
- [x] T035 [US3] Add error for TLS handshake failure with mbedTLS details in src/tds/tds_tls_context.cpp
- [x] T036 [US3] Add error for TLS initialization failure in src/tds/tds_tls_context.cpp
- [x] T037 [US3] Propagate TLS errors to TdsConnection::last_error_ in src/tds/tds_connection.cpp

**Checkpoint**: All TLS error scenarios produce distinct, descriptive error messages ✅

---

## Phase 6: User Story 4 - Trust Server Certificate by Default (Priority: P2) ✅

**Goal**: Accept any server certificate without verification (self-signed, expired, hostname mismatch)

**Independent Test**: Connect to SQL Server with self-signed certificate using `use_encrypt=true`

### Implementation for User Story 4

- [x] T038 [US4] Configure MBEDTLS_SSL_VERIFY_NONE in TlsTdsContext::Initialize() in src/tds/tds_tls_context.cpp
- [x] T039 [US4] Set max TLS version to 1.2 for VERIFY_NONE compatibility in src/tds/tds_tls_context.cpp
- [x] T040 [US4] Add debug log message for trust-server-certificate mode in src/tds/tds_tls_context.cpp

**Checkpoint**: TLS works with self-signed and expired certificates ✅

---

## Phase 7: User Story 5 - Cross-Platform TLS Support (Priority: P2) 🔄

**Goal**: Ensure TLS works on Linux, macOS, and Windows

**Independent Test**: Run TLS connection test on each platform

### Implementation for User Story 5

- [ ] T041 [P] [US5] Verify mbedTLS vcpkg builds on Linux (x64-linux triplet)
- [x] T042 [P] [US5] Verify mbedTLS vcpkg builds on macOS (arm64-osx triplet)
- [ ] T043 [P] [US5] Verify mbedTLS vcpkg builds on Windows (x64-windows-static triplet)
- [ ] T044 [US5] Add platform-specific socket handling for Windows in src/tds/tds_tls_context.cpp if needed
- [x] T045 [US5] Test TLS handshake on macOS with test SQL Server

**Checkpoint**: TLS functionality verified on macOS; Linux/Windows pending CI verification

---

## Phase 8: Connection Pool Integration ✅

**Purpose**: Ensure TLS connections work correctly with the existing connection pool

- [x] T046 Add use_encrypt to connection pool key in src/include/tds/connection_pool.hpp
- [x] T047 Modify pool key hash/equality to include use_encrypt in src/tds/connection_pool.cpp
- [x] T048 Verify pooled TLS connections retain TLS state on reuse in src/tds/connection_pool.cpp
- [x] T049 Verify pool ping works over TLS connections in src/tds/connection_pool.cpp
- [x] T050 Add debug logging for TLS pool operations in src/tds/connection_pool.cpp

**Checkpoint**: TLS and non-TLS connections are properly pooled separately ✅

---

## Phase 9: Observability & Logging ✅

**Purpose**: Add debug-level logging for TLS operations per FR-024

- [x] T051 Add debug log for TLS handshake start in src/tds/tds_tls_context.cpp
- [x] T052 Add debug log for TLS handshake success with cipher info in src/tds/tds_tls_context.cpp
- [x] T053 Add debug log for TLS handshake failure with error details in src/tds/tds_tls_context.cpp
- [x] T054 Add debug log for TLS connection close in src/tds/tds_tls_context.cpp

**Checkpoint**: TLS operations are observable via debug logging ✅

---

## Phase 10: Integration Testing ✅

**Purpose**: End-to-end validation with TLS-enabled SQL Server

- [x] T055 Use existing Docker Compose config for TLS-enabled SQL Server in docker/docker-compose.yml
- [x] T056 Create TLS integration test file in test/sql/integration/tls_connection.test
- [x] T057 Add test: create secret with use_encrypt=true and attach database
- [x] T058 Add test: execute SELECT query over TLS connection
- [x] T059 Add test: create secret without use_encrypt (backward compatibility)
- [x] T060 Add test: TLS with various connection string formats (URI and ADO.NET)
- [x] T061 Update Makefile with MSSQL_TEST_DSN_TLS environment variable

**Checkpoint**: All TLS scenarios covered by integration tests ✅

---

## Phase 11: Polish & Documentation ✅

**Purpose**: Final cleanup and documentation updates

- [x] T062 [P] Update quickstart.md with verified TLS usage examples in specs/005-tls-connection-support/quickstart.md
- [x] T063 [P] Add TLS section to extension README
- [x] T064 Review and clean up any TODO comments in TLS code
- [x] T065 Verify all error messages are user-friendly and actionable
- [x] T066 Run full test suite to verify no regressions (10 integration tests, 191 assertions passed)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3-7)**: All depend on Foundational phase completion
  - US1, US2, US3 are P1 (core functionality) - implement first
  - US4, US5 are P2 (polish) - implement after P1 stories
- **Pool Integration (Phase 8)**: Depends on US1 completion
- **Observability (Phase 9)**: Can be done in parallel with later phases
- **Integration Testing (Phase 10)**: Depends on all user stories
- **Polish (Phase 11)**: Depends on all phases complete

### User Story Dependencies

- **User Story 1 (P1)**: Requires Foundational (Phase 2) - Core TLS functionality
- **User Story 2 (P1)**: Requires US1 - Validates backward compatibility
- **User Story 3 (P1)**: Requires US1 - Error handling layer on top of TLS
- **User Story 4 (P2)**: Requires US1 - Certificate trust configuration
- **User Story 5 (P2)**: Requires US1, US4 - Platform verification

### Within Each User Story

- Header changes before implementation changes
- Protocol changes before socket changes
- Socket changes before connection changes
- Connection changes before pool manager changes

### Parallel Opportunities

- T041, T042, T043 (platform builds) can run in parallel
- T051-T054 (logging) can run in parallel with Phase 8
- T062, T063 (documentation) can run in parallel

---

## Parallel Example: Foundational Phase

```bash
# After T005 (header), these can run in parallel:
Task T006: "Implement TlsTdsContext::Initialize()"
Task T009: "Implement TlsTdsContext::Send()"
Task T010: "Implement TlsTdsContext::Receive()"
Task T012: "Implement TlsTdsContext::GetLastError()"

# T007, T008, T011 depend on earlier tasks
```

---

## Implementation Strategy

### MVP First (User Stories 1-3)

1. Complete Phase 1: Setup (build configuration)
2. Complete Phase 2: Foundational (TlsTdsContext)
3. Complete Phase 3: US1 - Encrypted Connection
4. Complete Phase 4: US2 - Backward Compatibility
5. Complete Phase 5: US3 - Error Messages
6. **STOP and VALIDATE**: Test TLS with local SQL Server
7. Deploy/demo if ready

### Full Feature

1. MVP (above)
2. Add Phase 6: US4 - Trust Server Certificate
3. Add Phase 7: US5 - Cross-Platform Verification
4. Add Phase 8: Connection Pool Integration
5. Add Phase 9: Observability
6. Add Phase 10: Integration Tests
7. Add Phase 11: Polish

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- mbedTLS cleanup order is critical - see research.md
- TLS 1.2 max version required for VERIFY_NONE mode
- Socket ownership: mbedTLS does NOT close fd on cleanup
- Test with Docker SQL Server that has TLS enabled

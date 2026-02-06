# Tasks: FEDAUTH Token Provider Enhancements

**Input**: Design documents from `/specs/032-fedauth-token-provider/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Unit tests for JWT parsing included (no external dependencies). Integration tests marked as requiring Azure.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create foundational JWT parsing module used by all user stories

- [ ] T001 [P] Create JWT claims struct and parser header in src/include/azure/jwt_parser.hpp
- [ ] T002 [P] Implement base64url decoding function in src/azure/jwt_parser.cpp
- [ ] T003 Implement ParseJwtClaims() function with JSON extraction in src/azure/jwt_parser.cpp
- [ ] T004 Add JWT parser to CMakeLists.txt build configuration

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [ ] T005 [P] Create ManualTokenAuthStrategy header in src/include/tds/auth/manual_token_strategy.hpp
- [ ] T006 [P] Add access_token field to MSSQLConnectionInfo struct in src/include/mssql_storage.hpp
- [ ] T007 Implement ManualTokenAuthStrategy class in src/tds/auth/manual_token_strategy.cpp
- [ ] T008 Register ManualTokenAuthStrategy source file in CMakeLists.txt

**Checkpoint**: Foundation ready - user story implementation can now begin

---

## Phase 3: User Story 1 - Manual Token Authentication (Priority: P1) 🎯 MVP

**Goal**: Enable users to attach using a pre-provided ACCESS_TOKEN

**Independent Test**: Obtain token via `az account get-access-token --resource https://database.windows.net/` and use in ATTACH statement

### Unit Tests for User Story 1

- [ ] T009 [P] [US1] Create JWT parser unit tests in test/cpp/test_jwt_parser.cpp
- [ ] T010 [P] [US1] Add test cases for valid JWT parsing (exp, aud extraction)
- [ ] T011 [P] [US1] Add test cases for malformed JWT error handling
- [ ] T012 [P] [US1] Add test cases for expired token detection

### Implementation for User Story 1

- [ ] T013 [US1] Parse ACCESS_TOKEN ATTACH option in src/mssql_storage.cpp MSSQLAttach()
- [ ] T014 [US1] Add ACCESS_TOKEN option support to MSSQL secret in src/mssql_secret.cpp
- [ ] T015 [US1] Read ACCESS_TOKEN from secret in src/azure/azure_secret_reader.cpp
- [ ] T016 [US1] Extend AuthStrategyFactory::Create() to route to ManualTokenAuthStrategy in src/tds/auth/auth_strategy_factory.cpp
- [ ] T017 [US1] Implement token validation (audience check) in ManualTokenAuthStrategy
- [ ] T018 [US1] Implement GetFedAuthToken() with UTF-16LE encoding in ManualTokenAuthStrategy
- [ ] T019 [US1] Add actionable error message for invalid JWT format
- [ ] T020 [US1] Add actionable error message for wrong audience

### Integration Test for User Story 1 (Requires Azure)

- [ ] T021 [US1] Create integration test file test/sql/azure/azure_access_token.test
- [ ] T022 [US1] Add test case for valid token via ATTACH ACCESS_TOKEN option
- [ ] T023 [US1] Add test case for valid token via MSSQL secret with ACCESS_TOKEN

**Checkpoint**: User Story 1 complete - manual token authentication works independently

---

## Phase 4: User Story 2 - Environment-Based Service Principal (Priority: P2)

**Goal**: Enable users to authenticate via Azure SDK environment variables

**Independent Test**: Set AZURE_TENANT_ID, AZURE_CLIENT_ID, AZURE_CLIENT_SECRET env vars and use credential_chain with 'env' chain

### Implementation for User Story 2

- [ ] T024 [P] [US2] Add ChainContainsEnv() helper function in src/azure/azure_token.cpp
- [ ] T025 [US2] Implement AcquireTokenFromEnv() function in src/azure/azure_token.cpp
- [ ] T026 [US2] Add env var validation with specific missing variable error messages
- [ ] T027 [US2] Add env chain routing in AcquireToken() dispatch logic in src/azure/azure_token.cpp
- [ ] T028 [US2] Implement auto-refresh for env-based tokens using stored credentials

### Integration Test for User Story 2 (Requires Azure)

- [ ] T029 [US2] Create integration test file test/sql/azure/azure_env_provider.test
- [ ] T030 [US2] Add test case for valid env-based authentication
- [ ] T031 [US2] Add test case for missing AZURE_CLIENT_ID error
- [ ] T032 [US2] Add test case for partial env vars error message

**Checkpoint**: User Story 2 complete - environment-based authentication works independently

---

## Phase 5: User Story 3 - Token Expiration Awareness (Priority: P2)

**Goal**: Provide clear error messages with timestamps when tokens expire

**Independent Test**: Use an expired token and verify error message includes expiration timestamp

### Implementation for User Story 3

- [ ] T033 [US3] Implement FormatTimestamp() utility for human-readable UTC timestamps in src/azure/jwt_parser.cpp
- [ ] T034 [US3] Implement IsTokenExpired() with 5-minute margin in ManualTokenAuthStrategy
- [ ] T035 [US3] Add expiration check at connection time in ManualTokenAuthStrategy::GetFedAuthToken()
- [ ] T036 [US3] Format actionable expiration error with exact timestamp

### Unit Tests for User Story 3

- [ ] T037 [P] [US3] Add test case for expired token error message format in test/cpp/test_jwt_parser.cpp
- [ ] T038 [P] [US3] Add test case for 5-minute margin expiration detection

**Checkpoint**: User Story 3 complete - clear expiration messages work

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Documentation and final validation

- [ ] T039 [P] Update AZURE.md with ACCESS_TOKEN documentation
- [ ] T040 [P] Update AZURE.md with environment provider documentation
- [ ] T041 [P] Add ACCESS_TOKEN and env provider examples to docs/architecture.md
- [ ] T042 Run quickstart.md validation with real Azure SQL Database
- [ ] T043 Verify all error messages match spec (SC-004: 100% actionable)
- [ ] T044 Code review for C++11 compatibility (no C++17-only features)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3+)**: All depend on Foundational phase completion
  - User stories can then proceed in parallel (if staffed)
  - Or sequentially in priority order (P1 → P2 → P3)
- **Polish (Final Phase)**: Depends on all desired user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational (Phase 2) - No dependencies on other stories
- **User Story 2 (P2)**: Can start after Foundational (Phase 2) - Independent of US1
- **User Story 3 (P2)**: Can start after Foundational (Phase 2) - Uses JWT parsing from US1 but independently testable

### Within Each User Story

- Unit tests can run without Azure (JWT parsing)
- Integration tests require Azure SQL/Fabric
- Implementation follows: Parse option → Validate → Integrate → Error handling

### Parallel Opportunities

**Phase 1 (Setup)**:
- T001 and T002 can run in parallel (header vs implementation)

**Phase 2 (Foundational)**:
- T005 and T006 can run in parallel (different headers)

**User Story 1**:
- T009, T010, T011, T012 (unit tests) can all run in parallel
- T013, T014, T015 can run in parallel (different files)

**User Story 2**:
- T024 can run in parallel with any other US2 task (isolated helper)

**User Story 3**:
- T037, T038 (unit tests) can run in parallel

**Polish**:
- T039, T040, T041 (docs) can all run in parallel

---

## Parallel Example: User Story 1

```bash
# Launch unit tests in parallel (no Azure required):
Task: "Create JWT parser unit tests in test/cpp/test_jwt_parser.cpp"
Task: "Add test cases for valid JWT parsing"
Task: "Add test cases for malformed JWT error handling"
Task: "Add test cases for expired token detection"

# Launch option parsing in parallel (different files):
Task: "Parse ACCESS_TOKEN ATTACH option in src/mssql_storage.cpp"
Task: "Add ACCESS_TOKEN option support to MSSQL secret in src/mssql_secret.cpp"
Task: "Read ACCESS_TOKEN from secret in src/azure/azure_secret_reader.cpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (JWT parsing module)
2. Complete Phase 2: Foundational (ManualTokenAuthStrategy)
3. Complete Phase 3: User Story 1 (ACCESS_TOKEN option)
4. **STOP and VALIDATE**: Test with Azure CLI token
5. Deploy/PR if ready - manual token auth is the primary use case

### Incremental Delivery

1. Setup + Foundational → JWT parsing and strategy infrastructure ready
2. Add User Story 1 → Manual token auth works → Deploy/PR (MVP!)
3. Add User Story 2 → Environment auth works → Deploy/PR
4. Add User Story 3 → Expiration messages → Deploy/PR
5. Each story adds value without breaking previous stories

### Parallel Team Strategy

With multiple developers:

1. Team completes Setup + Foundational together
2. Once Foundational is done:
   - Developer A: User Story 1 (manual token)
   - Developer B: User Story 2 (env provider)
   - Developer C: User Story 3 (expiration messages)
3. Stories complete and integrate independently

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story should be independently completable and testable
- Unit tests (JWT parsing) can run without Azure
- Integration tests require Azure SQL/Fabric with valid credentials
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- C++11 compatibility required (no std::optional, no structured bindings)

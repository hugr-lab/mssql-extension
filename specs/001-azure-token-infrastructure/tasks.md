# Tasks: Azure Token Infrastructure (Phase 1)

**Input**: Design documents from `/specs/001-azure-token-infrastructure/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md
**Branch**: `001-azure-token-infrastructure`

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3, US4)
- Include exact file paths in descriptions

## Test Environment Variables

Tests use environment variables from `.env`:

| Variable | Purpose |
|----------|---------|
| `AZURE_DIRECTORY_ID` | Azure tenant ID |
| `AZURE_APP_ID` | Azure client/application ID |
| `AZURE_APP_SECRET` | Azure client secret |
| `AZURE_SQL_DB_HOST` | Azure SQL Database host |
| `AZURE_SQL_DB` | Azure SQL Database name |

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create directory structure, CMake integration, and vcpkg dependencies for Azure module

- [ ] T001 Create `src/azure/` directory for Azure authentication module
- [ ] T002 Create `src/include/azure/` directory for Azure module headers
- [ ] T003 Update `vcpkg.json` to add `azure-identity-cpp` dependency (matching DuckDB Azure)
- [ ] T004 Update `CMakeLists.txt` to include new `src/azure/*.cpp` source files and link Azure SDK
- [ ] T005 Create `test/sql/azure/` directory for Azure-specific SQL tests

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core entities and utilities that ALL user stories depend on

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

### Core Entities

- [ ] T006 [P] Create `AzureSecretInfo` struct in `src/include/azure/azure_secret_reader.hpp`
- [ ] T007 [P] Create `TokenResult` struct in `src/include/azure/azure_token.hpp`
- [ ] T008 [P] Create `CachedToken` struct in `src/include/azure/azure_token.hpp`

### Azure Secret Reader

- [ ] T009 Implement `ReadAzureSecret()` function in `src/azure/azure_secret_reader.cpp`
- [ ] T010 Add Azure extension detection with clear error message in `src/azure/azure_secret_reader.cpp`

### Azure SDK Credential Factory

- [ ] T011 Create `CreateCredential()` factory function using Azure SDK in `src/azure/azure_token.cpp`
- [ ] T012 Implement `ClientSecretCredential` creation for service_principal in `src/azure/azure_token.cpp`
- [ ] T013 Implement `ChainedTokenCredential` creation for credential_chain in `src/azure/azure_token.cpp`
- [ ] T014 Implement `ManagedIdentityCredential` creation for managed_identity in `src/azure/azure_token.cpp`

### Token Cache

- [ ] T015 Implement thread-safe `TokenCache` class in `src/azure/azure_token.cpp`
- [ ] T016 Add `IsValid()` method with 5-minute expiration margin in `src/azure/azure_token.cpp`

### Token Acquisition

- [ ] T017 Implement `AcquireTokenWithSDK()` using Azure SDK credentials in `src/azure/azure_token.cpp`
- [ ] T018 Implement main `AcquireToken()` dispatcher in `src/azure/azure_token.cpp`

### Constants

- [ ] T019 Add `MSSQL_SECRET_AZURE_SECRET` constant in `src/include/mssql_secret.hpp`

**Checkpoint**: Foundation ready - Azure SDK token acquisition infrastructure complete

---

## Phase 3: User Story 1 - Test Azure Credentials (Priority: P1) 🎯 MVP

**Goal**: Users can verify Azure credentials work before attempting database connections via `mssql_azure_auth_test()` function

**Independent Test**: Call `SELECT mssql_azure_auth_test('azure_secret_name')` and verify truncated token is returned for valid credentials

### Tests for User Story 1

- [ ] T020 [P] [US1] Create unit test for token cache thread-safety in `test/cpp/test_azure_token_cache.cpp`
- [ ] T021 [P] [US1] Create SQL test for valid service principal token in `test/sql/azure/azure_auth_test_function.test`
- [ ] T022 [P] [US1] Create SQL test for invalid credentials error handling in `test/sql/azure/azure_auth_test_function.test`
- [ ] T023 [P] [US1] Create SQL test for non-existent secret error in `test/sql/azure/azure_auth_test_function.test`

### Implementation for User Story 1

- [ ] T024 [US1] Create `azure_test_function.hpp` header in `src/include/azure/azure_test_function.hpp`
- [ ] T025 [US1] Implement `AzureAuthTestFunction()` scalar function in `src/azure/azure_test_function.cpp`
- [ ] T026 [US1] Implement token truncation logic (first 10 + "..." + last 3 + length) in `src/azure/azure_test_function.cpp`
- [ ] T027 [US1] Implement error message formatting with Azure AD error codes in `src/azure/azure_test_function.cpp`
- [ ] T028 [US1] Register `mssql_azure_auth_test` function in `src/mssql_extension.cpp`

**Checkpoint**: `mssql_azure_auth_test()` function works independently - users can validate Azure credentials (via Azure SDK)

---

## Phase 4: User Story 2 - Create MSSQL Secret with Azure Auth (Priority: P1)

**Goal**: Users can create MSSQL secrets with `azure_secret` parameter, eliminating need for user/password

**Independent Test**: Create MSSQL secret with `AZURE_SECRET 'my_azure'` and verify it succeeds without user/password

### Tests for User Story 2

- [ ] T029 [P] [US2] Create SQL test for MSSQL secret with valid azure_secret in `test/sql/azure/azure_secret_validation.test`
- [ ] T030 [P] [US2] Create SQL test for MSSQL secret with non-existent azure_secret in `test/sql/azure/azure_secret_validation.test`
- [ ] T031 [P] [US2] Create SQL test for MSSQL secret with wrong secret type in `test/sql/azure/azure_secret_validation.test`
- [ ] T032 [P] [US2] Create SQL test for MSSQL secret with both azure_secret and user/password in `test/sql/azure/azure_secret_validation.test`

### Implementation for User Story 2

- [ ] T033 [US2] Add `azure_secret` named parameter to `CreateSecretFunction` in `src/mssql_secret.cpp`
- [ ] T034 [US2] Modify `ValidateMSSQLSecretFields()` to accept `ClientContext` parameter in `src/mssql_secret.cpp`
- [ ] T035 [US2] Implement Azure secret lookup and validation in `src/mssql_secret.cpp`
- [ ] T036 [US2] Implement conditional user/password requirement logic in `src/mssql_secret.cpp`
- [ ] T037 [US2] Store `azure_secret` value in MSSQL secret via `TrySetValue()` in `src/mssql_secret.cpp`
- [ ] T038 [US2] Add error messages for Azure secret validation failures in `src/mssql_secret.cpp`

**Checkpoint**: MSSQL secrets can be created with Azure auth reference - Phase 2 integration point ready

---

## Phase 5: User Story 3 - Backward Compatibility (Priority: P1)

**Goal**: Existing MSSQL secrets with SQL authentication continue to work unchanged

**Independent Test**: Run all existing MSSQL secret tests and verify zero regressions

### Tests for User Story 3

- [ ] T039 [P] [US3] Verify existing SQL auth tests pass unchanged in `test/sql/attach/` directory
- [ ] T040 [P] [US3] Create SQL test for MSSQL secret without azure_secret requires user/password in `test/sql/azure/azure_secret_validation.test`
- [ ] T041 [P] [US3] Create SQL test for "Either user/password or azure_secret required" error in `test/sql/azure/azure_secret_validation.test`

### Implementation for User Story 3

- [ ] T042 [US3] Ensure existing validation path for user/password unchanged in `src/mssql_secret.cpp`
- [ ] T043 [US3] Add "Either user/password or azure_secret required" error case in `src/mssql_secret.cpp`
- [ ] T044 [US3] Verify `RegisterMSSQLSecretType()` signature unchanged in `src/mssql_secret.cpp`

**Checkpoint**: All existing MSSQL secret tests pass - backward compatibility verified

---

## Phase 5b: User Story 4 - Interactive Device Code Authentication (Priority: P1)

**Goal**: Users with MFA-enforced Entra ID accounts can authenticate via Device Code Flow without needing Azure CLI

**Note**: Azure SDK for C++ does NOT support interactive auth, so we implement Device Code Flow (RFC 8628) - same approach as MotherDuck

**Independent Test**: Create Azure secret with `CHAIN 'interactive'`, call `mssql_azure_auth_test()`, visit displayed URL, enter code, complete MFA, verify token returned

### Device Code Infrastructure

- [ ] T051 [P] [US4] Create `DeviceCodeResponse` struct in `src/include/azure/azure_device_code.hpp`
- [ ] T052 [P] [US4] Create `DeviceCodePollingState` struct in `src/include/azure/azure_device_code.hpp`

### Device Code Flow Implementation

- [ ] T053 [US4] Implement `RequestDeviceCode()` - POST to `/devicecode` endpoint in `src/azure/azure_device_code.cpp`
- [ ] T054 [US4] Implement `PollForToken()` - polling loop with interval sleep in `src/azure/azure_device_code.cpp`
- [ ] T055 [US4] Implement device code response JSON parsing in `src/azure/azure_device_code.cpp`
- [ ] T056 [US4] Implement token polling response handling (pending, declined, expired) in `src/azure/azure_device_code.cpp`

### User Interaction

- [ ] T057 [US4] Implement message display (verification_uri + user_code) in `src/azure/azure_device_code.cpp`
- [ ] T058 [US4] Implement progress indicator during polling ("waiting for authentication...") in `src/azure/azure_device_code.cpp`

### Token Acquisition Integration

- [ ] T059 [US4] Implement `AcquireInteractiveToken()` main entry point in `src/azure/azure_device_code.cpp`
- [ ] T060 [US4] Create custom `DeviceCodeCredential` class implementing Azure SDK `TokenCredential` interface in `src/azure/azure_device_code.cpp`
- [ ] T061 [US4] Integrate device code credential into `ChainedTokenCredential` in `src/azure/azure_token.cpp`

### Tests for User Story 4

- [ ] T062 [P] [US4] Create unit test for device code response parsing in `test/cpp/test_azure_device_code.cpp`
- [ ] T063 [P] [US4] Create unit test for polling state handling in `test/cpp/test_azure_device_code.cpp`
- [ ] T064 [P] [US4] Create SQL test for device code error handling in `test/sql/azure/azure_device_code.test`
- [ ] T065 [P] [US4] Create manual test instructions for device code flow in `test/manual/azure_device_code_test.md`

### Error Handling

- [ ] T066 [US4] Implement "Device code expired" error handling in `src/azure/azure_device_code.cpp`
- [ ] T067 [US4] Implement "Authorization was declined" error handling in `src/azure/azure_device_code.cpp`
- [ ] T068 [US4] Implement network error handling with retry in `src/azure/azure_device_code.cpp`

**Checkpoint**: Device Code authentication works for MFA users - `mssql_azure_auth_test()` with `CHAIN 'interactive'` displays URL/code and returns token after user completes auth

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Improvements that affect multiple user stories

- [ ] T045 [P] Update `src/include/mssql_secret.hpp` with updated function signatures
- [ ] T046 [P] Add documentation comments to all new public functions
- [ ] T047 Verify build on all platforms (Linux, macOS, Windows) via `make` and CI
- [ ] T048 Run full test suite including existing tests to confirm no regressions
- [ ] T049 [P] Update CLAUDE.md with Azure authentication section in Active Technologies
- [ ] T050 Validate quickstart.md scenarios work end-to-end

---

## Dependencies & Execution Order

### Phase Dependencies

```
Phase 1: Setup
    │
    ▼
Phase 2: Foundational (BLOCKS all user stories)
    │
    ├──────────────────┬──────────────────┬──────────────────┐
    ▼                  ▼                  ▼                  ▼
Phase 3: US1       Phase 4: US2       Phase 5: US3       Phase 5b: US4
(Test Function)    (Azure Secret)     (Backward Compat)  (Interactive)
    │                  │                  │                  │
    └──────────────────┴──────────────────┴──────────────────┘
                                │
                                ▼
                       Phase 6: Polish
```

### User Story Dependencies

- **User Story 1 (P1)**: Depends only on Foundational - can start after Phase 2
- **User Story 2 (P1)**: Depends only on Foundational - can start after Phase 2
- **User Story 3 (P1)**: Depends only on Foundational - can start after Phase 2
- **User Story 4 (P1)**: Depends only on Foundational - can start after Phase 2

All user stories are **independent** and can be implemented in parallel.

### Within Each Phase

**Foundational (Phase 2)**:
- T006, T007, T008 (structs) can run in parallel
- T009-T010 (secret reader) depend on T006
- T011-T014 (Azure SDK credentials) depend on T006
- T015-T016 (cache) depend on T007, T008
- T017-T018 (token acquisition) depend on T011-T016
- T019 (constant) can run anytime

**User Stories**:
- Tests marked [P] can run in parallel within each story
- Implementation tasks are sequential within each story
- US4 (interactive) is independent of Azure SDK setup (custom OAuth2)

---

## Parallel Examples

### Phase 2: Foundational - Entity Creation

```bash
# Launch in parallel:
Task: T006 "Create AzureSecretInfo struct in src/include/azure/azure_secret_reader.hpp"
Task: T007 "Create TokenResult struct in src/include/azure/azure_token.hpp"
Task: T008 "Create CachedToken struct in src/include/azure/azure_token.hpp"
```

### User Story 1: Tests

```bash
# Launch in parallel after Phase 2 complete:
Task: T020 "Create unit test for token cache in test/cpp/test_azure_token_cache.cpp"
Task: T021 "Create SQL test for valid token in test/sql/azure/azure_auth_test_function.test"
Task: T022 "Create SQL test for invalid credentials in test/sql/azure/azure_auth_test_function.test"
Task: T023 "Create SQL test for non-existent secret in test/sql/azure/azure_auth_test_function.test"
```

### Cross-Story Parallel

```bash
# After Phase 2, launch all story test tasks in parallel:
# US1 Tests:
Task: T020, T021, T022, T023
# US2 Tests:
Task: T029, T030, T031, T032
# US3 Tests:
Task: T039, T040, T041
# US4 Tests:
Task: T062, T063, T064, T065
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001-T005)
2. Complete Phase 2: Foundational (T006-T019)
3. Complete Phase 3: User Story 1 (T020-T028)
4. **STOP and VALIDATE**: Test `mssql_azure_auth_test()` with real Azure credentials
5. Deploy/demo if ready - users can validate credentials!

### Incremental Delivery

1. Setup + Foundational → Token infrastructure ready
2. Add User Story 1 → `mssql_azure_auth_test()` works → **MVP!**
3. Add User Story 2 → MSSQL secrets accept `azure_secret` → Phase 2 integration ready
4. Add User Story 3 → Backward compatibility verified → Core complete
5. Add User Story 4 → Interactive browser auth for MFA users → Full auth coverage
6. Polish → Documentation, cleanup → Release

### Test Environment Setup

```bash
# Set environment variables before running Azure tests
export AZURE_DIRECTORY_ID='your-tenant-id'
export AZURE_APP_ID='your-client-id'
export AZURE_APP_SECRET='your-client-secret'
export AZURE_SQL_DB_HOST='your-server.database.windows.net'
export AZURE_SQL_DB='your-database'

# Run Azure-specific tests
make test ARGS="--test-dir test/sql/azure"
```

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability (US1, US2, US3, US4)
- Each user story is independently completable and testable
- Use `require-env` directive in SQL tests to skip when Azure credentials unavailable
- Commit after each task or logical group
- Test functions return VARCHAR to support both success tokens and error messages
- Token cache is global (static) - thread-safe via mutex
- Interactive auth (US4) requires manual testing in non-headless environment
- US4 automated tests focus on unit tests and headless error handling

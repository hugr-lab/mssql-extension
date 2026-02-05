# Tasks: TDS FEDAUTH Integration (Phase 2)

**Input**: Design documents from `/specs/001-tds-fedauth-integration/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

**Tests**: Unit tests and integration tests are included as this is a protocol integration with clear testable boundaries per spec.md FR-019 through FR-022.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1-US7)
- Include exact file paths in descriptions

## Path Conventions

- **Source**: `src/` at repository root
- **Headers**: `src/include/` mirroring source layout
- **Tests**: `test/cpp/` for unit tests, `test/sql/azure/` for integration tests

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create new files and headers for FEDAUTH support

- [x] T001 [P] Create endpoint detection helpers in src/include/mssql_platform.hpp with IsAzureEndpoint(), IsFabricEndpoint(), GetEndpointType(), RequiresHostnameVerification()
- [x] T002 [P] Create FedAuthData struct and FedAuthLibrary enum in src/include/azure/azure_fedauth.hpp per data-model.md
- [x] T003 [P] Create EndpointType enum in src/include/mssql_platform.hpp (AzureSQL, Fabric, OnPremises, Synapse)
- [x] T004 Add azure_fedauth.cpp to CMakeLists.txt EXTENSION_SOURCES list

**Checkpoint**: New header files exist, project compiles

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core protocol changes that ALL user stories depend on

**CRITICAL**: No user story work can begin until this phase is complete

- [x] T005 Add FEDAUTHREQUIRED constant (0x06) and FEDAUTH FeatureExtId (0x02) to src/include/tds/tds_types.hpp
- [x] T006 Extend MSSQLConnectionInfo in src/include/mssql_storage.hpp with use_azure_auth and azure_secret_name fields
- [x] T007 Add IsAzureEndpoint() and IsFabricEndpoint() helper methods to MSSQLConnectionInfo
- [x] T008 Implement endpoint detection functions in src/azure/azure_fedauth.cpp (IsAzureEndpoint, IsFabricEndpoint, GetEndpointType)
- [x] T009 Implement FedAuthData::GetDataSize() and FedAuthData::IsValid() in src/include/azure/azure_fedauth.hpp (inline)
- [x] T010 Implement EncodeFedAuthToken() UTF-16LE encoding in src/azure/azure_fedauth.cpp using existing tds::encoding::Utf16LEEncode()
- [x] T011 Implement BuildFedAuthExtension() in src/azure/azure_fedauth.cpp that acquires token and builds FedAuthData

**Checkpoint**: Foundation ready - FEDAUTH data structures and encoding complete, user story implementation can begin

---

## Phase 3: User Story 1 - Connect with Azure Authentication (Priority: P1)

**Goal**: Enable ATTACH to Azure SQL Database using azure_secret with FEDAUTH protocol

**Independent Test**: `ATTACH '' AS db (TYPE mssql, SECRET my_sql);` where secret uses azure_secret, then `SELECT 1` succeeds

### Unit Tests for User Story 1

- [x] T012 [P] [US1] Create test/cpp/test_fedauth_encoding.cpp with tests for UTF-16LE token encoding
- [x] T013 [P] [US1] Add tests for FedAuthData::GetDataSize() and IsValid() in test/cpp/test_fedauth_encoding.cpp
- [x] T014 [P] [US1] Add tests for endpoint detection (IsAzureEndpoint, IsFabricEndpoint, GetEndpointType) in test/cpp/test_fedauth_encoding.cpp

### Implementation for User Story 1

- [x] T015 [US1] Modify mssql_storage.cpp FromSecret() to read azure_secret field from MSSQL secret and set use_azure_auth
- [x] T016 [US1] Add BuildPreloginWithFedAuth() to include FEDAUTHREQUIRED option (0x06) when use_azure_auth is true
- [x] T017 [US1] Add BuildLogin7WithFedAuth() feature extension builder with FEDAUTH token support
- [x] T018 [US1] Add AuthenticateWithFedAuth() method in tds_connection.cpp for Azure authentication flow
- [x] T019 [US1] Add FEDAUTHINFO (0xEE) token parsing to tds_token_parser.cpp (skip token)
- [x] T020 [US1] Add DoPreloginWithFedAuth() and DoLogin7WithFedAuth() helpers in tds_connection.cpp

### Integration Test for User Story 1

- [x] T021 [US1] Create test/sql/azure/azure_service_principal.test with require-env for Azure credentials and basic ATTACH/SELECT 1/DETACH

**Checkpoint**: Azure SQL connection with service principal works, SELECT 1 returns result

---

## Phase 4: User Story 2 - Backward Compatibility with SQL Auth (Priority: P1)

**Goal**: Ensure all existing SQL authentication continues to work unchanged

**Independent Test**: All existing tests in test/sql/ pass without modification

### Verification for User Story 2

- [x] T022 [US2] Verify PRELOGIN does NOT include FEDAUTHREQUIRED when azure_secret is absent - add test case in test/cpp/test_fedauth_encoding.cpp
- [x] T023 [US2] Verify LOGIN7 does NOT include FEDAUTH extension when azure_secret is absent - add test case in test/cpp/test_fedauth_encoding.cpp
- [x] T024 [US2] Run full existing test suite (`make test-all`) and verify zero regressions

**Checkpoint**: All 105+ existing tests pass, SQL auth unchanged

---

## Phase 5: User Story 3 - Handle Authentication Failures Gracefully (Priority: P1)

**Goal**: Provide clear, actionable error messages for Azure auth failures

**Independent Test**: Provide invalid credentials, verify error message contains Azure AD error code

### Unit Tests for User Story 3

- [ ] T025 [P] [US3] Create test/cpp/test_mssql_secret_azure.cpp with tests for azure_secret validation
- [ ] T026 [P] [US3] Add tests for missing azure_secret error message in test/cpp/test_mssql_secret_azure.cpp
- [ ] T027 [P] [US3] Add tests for Azure AD error code extraction in test/cpp/test_mssql_secret_azure.cpp

### Implementation for User Story 3

- [ ] T028 [US3] Add azure_secret existence validation in mssql_connection_provider.cpp with clear "Azure secret 'name' not found" error
- [ ] T029 [US3] Enhance token acquisition error handling in src/azure/azure_token.cpp to extract AADSTS error codes
- [ ] T030 [US3] Add user-friendly error messages for common Azure AD errors (AADSTS7000215, AADSTS700016, etc.) in azure_token.cpp
- [ ] T031 [US3] Add TDS login failure translation in tds_connection.cpp to convert server errors to actionable messages
- [ ] T032 [US3] Implement automatic token refresh on connection reuse in tds_connection.cpp when token is near expiration

### Integration Test for User Story 3

- [ ] T033 [US3] Create test/sql/azure/azure_error_handling.test with invalid credentials scenarios (requires manual verification of error messages)

**Checkpoint**: Error messages include Azure AD error codes and actionable guidance

---

## Phase 6: User Story 4 - Catalog Operations with Azure Auth (Priority: P2)

**Goal**: Catalog browsing (schemas, tables, columns) works on Azure SQL with Azure auth

**Independent Test**: `SELECT * FROM duckdb_schemas() WHERE database_name = 'azuredb'` returns schemas

### Implementation for User Story 4

- [ ] T034 [US4] Verify catalog queries work through Azure-authenticated connection (no code change expected, verification only)
- [ ] T035 [US4] Add Fabric endpoint detection in mssql_catalog.cpp for graceful degradation of catalog features

### Integration Test for User Story 4

- [ ] T036 [US4] Create test/sql/azure/azure_catalog.test with duckdb_schemas(), duckdb_tables(), duckdb_columns() queries

**Checkpoint**: Catalog discovery works on Azure SQL

---

## Phase 7: User Story 5 - Data Operations with Azure Auth (Priority: P2)

**Goal**: All DML operations (SELECT, INSERT, UPDATE, DELETE) work on Azure SQL with Azure auth

**Independent Test**: Full CRUD cycle on test table via Azure-authenticated connection

### Implementation for User Story 5

- [ ] T037 [US5] Verify SELECT works through Azure-authenticated connection (no code change expected)
- [ ] T038 [US5] Verify INSERT with OUTPUT INSERTED works through Azure-authenticated connection
- [ ] T039 [US5] Verify UPDATE/DELETE work through Azure-authenticated connection
- [ ] T040 [US5] Verify transaction semantics (BEGIN/COMMIT/ROLLBACK) work with Azure auth

### Integration Test for User Story 5

- [ ] T041 [US5] Create test/sql/azure/azure_dml.test with CREATE TABLE, INSERT, SELECT, UPDATE, DELETE, transaction tests

**Checkpoint**: All DML operations work on Azure SQL

---

## Phase 8: User Story 6 - Microsoft Fabric Warehouse Support (Priority: P2)

**Goal**: Connect to Fabric Warehouse, execute SELECT, provide clear errors for unsupported operations

**Independent Test**: Connect to Fabric, execute SELECT, verify catalog works

### Unit Tests for User Story 6

- [ ] T042 [P] [US6] Add Fabric endpoint detection tests to test/cpp/test_fedauth_encoding.cpp

### Implementation for User Story 6

- [ ] T043 [US6] Add Fabric-specific TLS handling in tds_tls_context.cpp (same as Azure SQL - hostname verification)
- [ ] T044 [US6] Add Fabric limitation detection in mssql_catalog.cpp for DBCC statistics (graceful degradation)
- [ ] T045 [US6] Add clear error messages for unsupported Fabric operations (UPDATE/DELETE may fail)

### Integration Test for User Story 6

- [ ] T046 [US6] Create test/sql/azure/fabric_warehouse.test with require-env for Fabric credentials and SELECT/catalog tests

**Checkpoint**: Fabric connection works for basic operations with documented limitations

---

## Phase 9: User Story 7 - COPY/BCP with Azure Auth (Priority: P3)

**Goal**: Bulk load data to Azure SQL using COPY command with BCP protocol via Azure auth

**Independent Test**: `COPY (SELECT * FROM local_data) TO 'azuredb.dbo.staging'` succeeds

### Implementation for User Story 7

- [ ] T047 [US7] Verify BCP protocol works through Azure-authenticated connection (no code change expected)
- [ ] T048 [US7] Verify temp table creation via COPY works with Azure auth
- [ ] T049 [US7] Add Fabric COPY/BCP limitation handling with clear error if unsupported

### Integration Test for User Story 7

- [ ] T050 [US7] Create test/sql/azure/azure_copy.test with COPY TO MSSQL tests via Azure auth

**Checkpoint**: COPY/BCP works on Azure SQL

---

## Phase 10: TLS Hostname Verification (Cross-cutting)

**Goal**: Proper TLS certificate validation for Azure endpoints

**Independent Test**: Connection to Azure SQL succeeds with valid cert, fails with wrong hostname

### Unit Tests for TLS

- [ ] T051 [P] Create test/cpp/test_hostname_verification.cpp with MatchHostname() tests
- [ ] T052 [P] Add wildcard matching tests (*.database.windows.net) to test/cpp/test_hostname_verification.cpp
- [ ] T053 [P] Add negative tests (wrong domain, multiple wildcards) to test/cpp/test_hostname_verification.cpp

### Implementation for TLS

- [ ] T054 Implement MatchHostname() wildcard matching in src/tds/tls/tds_tls_context.cpp
- [ ] T055 Add ConfigureHostnameVerification() in src/tds/tls/tds_tls_context.cpp using OpenSSL SSL_set1_host()
- [ ] T056 Modify TLS handshake in tds_tls_context.cpp to call ConfigureHostnameVerification() based on endpoint type
- [ ] T057 Ensure on-premises SQL Server continues to accept self-signed certificates (SSL_VERIFY_NONE for OnPremises)

**Checkpoint**: Azure endpoints verified, on-prem unchanged

---

## Phase 11: Polish & Cross-Cutting Concerns

**Purpose**: Documentation, cleanup, final validation

- [ ] T058 [P] Update AZURE.md with working connection examples after Phase 2
- [ ] T059 [P] Update CLAUDE.md Active Technologies section with Phase 2 info
- [ ] T060 [P] Add Azure test environment variables to CI workflow (.github/workflows/) with conditional execution
- [ ] T061 Run quickstart.md validation - verify all steps work end-to-end
- [ ] T062 Final regression test - run full test suite including Azure tests if credentials available
- [ ] T063 Code review: verify namespace conventions (no MSSQL prefix in duckdb::mssql namespace)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Phase 1 - BLOCKS all user stories
- **User Stories (Phase 3-9)**: All depend on Phase 2 (Foundational) completion
  - US1 (Connect): Must complete first - enables all other Azure features
  - US2 (Backward Compat): Can run in parallel with US1
  - US3 (Error Handling): Depends on US1 being functional
  - US4-US7: Depend on US1, can proceed in parallel or sequentially by priority
- **TLS (Phase 10)**: Can run in parallel with US1 after Phase 2
- **Polish (Phase 11)**: Depends on all user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Foundational - No dependencies on other stories
- **User Story 2 (P1)**: No dependencies - verification only
- **User Story 3 (P1)**: Soft dependency on US1 for error path testing
- **User Story 4 (P2)**: Depends on US1 for Azure connection
- **User Story 5 (P2)**: Depends on US1 for Azure connection
- **User Story 6 (P2)**: Depends on US1 for Azure connection
- **User Story 7 (P3)**: Depends on US1 for Azure connection

### Critical Path

```
Phase 1 → Phase 2 → US1 → US3 → US4/US5/US6/US7 → Phase 11
                  ↘ US2 (parallel) ↗
          Phase 10 (TLS, parallel with US1)
```

### Parallel Opportunities

**Within Phase 1 (Setup)**:

```
T001 (mssql_platform.hpp) | T002 (azure_fedauth.hpp) | T003 (EndpointType)
```

**Within Phase 2 (Foundational)**: Sequential (files depend on each other)

**Within US1 (Connect)**:

```
T012 (encoding tests) | T013 (FedAuthData tests) | T014 (endpoint tests)
```

**After Phase 2 completes**:

```
US1 implementation | US2 verification | Phase 10 TLS
```

**Within US3 (Error Handling)**:

```
T025 (secret validation tests) | T026 (missing secret tests) | T027 (error code tests)
```

---

## Parallel Example: Phase 1 Setup

```bash
# Launch all setup tasks together:
Task: "Create endpoint detection helpers in src/include/mssql_platform.hpp"
Task: "Create FedAuthData struct in src/include/azure/azure_fedauth.hpp"
Task: "Create EndpointType enum in src/include/mssql_platform.hpp"
```

## Parallel Example: US1 Unit Tests

```bash
# Launch all US1 unit tests together:
Task: "Create test/cpp/test_fedauth_encoding.cpp with UTF-16LE encoding tests"
Task: "Add FedAuthData tests in test/cpp/test_fedauth_encoding.cpp"
Task: "Add endpoint detection tests in test/cpp/test_fedauth_encoding.cpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational
3. Complete Phase 3: User Story 1 (Connect with Azure Auth)
4. **STOP and VALIDATE**: Test connection to Azure SQL with service principal
5. Deploy/demo if ready - users can now connect to Azure SQL!

### Incremental Delivery

1. Setup + Foundational → Infrastructure ready
2. Add US1 (Connect) → Test independently → **MVP Complete!**
3. Add US2 (Backward Compat) → Verify no regressions
4. Add US3 (Error Handling) → Better UX for failures
5. Add US4-US5 (Catalog + DML) → Full data operations
6. Add US6 (Fabric) → Fabric support
7. Add US7 (COPY/BCP) → Bulk loading
8. Polish → Documentation, CI updates

### Suggested MVP Scope

**MVP = Phase 1 + Phase 2 + Phase 3 (User Story 1)**

This delivers:
- Azure SQL connection with service principal
- Basic query execution (SELECT 1)
- Token encoding and FEDAUTH protocol

Total MVP tasks: **21 tasks** (T001-T021)

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story should be independently completable and testable
- Unit tests use DuckDB unittest framework (Catch2-based)
- Integration tests use SQLLogicTest format with `require-env` for Azure credentials
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- Existing tests (105+) MUST pass - zero regressions is a hard requirement

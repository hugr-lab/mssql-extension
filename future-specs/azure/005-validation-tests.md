# Feature Specification: Validation & Tests

**Feature Branch**: `azure-005-validation-tests`
**Created**: 2026-02-03
**Status**: Future / Planning
**Dependencies**: 001-azure-secret-reuse, 002-extend-mssql-secret, 003-azure-auth-test-function, 004-auth-flow-integration

## Problem Statement

Azure AD authentication introduces new code paths that must be thoroughly tested. Both unit tests (no Azure required) and integration tests (Azure credentials required) are needed to ensure reliability and prevent regressions.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Verify SQL Auth Unchanged (Priority: P1)

Existing SQL authentication functionality must continue to work without any changes.

**Acceptance Scenarios**:

1. **Given** all existing SQL auth integration tests, **When** test suite runs, **Then** all tests pass (zero regressions)
2. **Given** MSSQL secret with user/password, **When** user connects to on-prem SQL Server, **Then** authentication works as before
3. **Given** connection pool with SQL auth connections, **When** connections reused, **Then** behavior unchanged

---

### User Story 2 - Verify Azure Auth Works (Priority: P1)

Azure AD authentication must work for all supported credential types.

**Acceptance Scenarios**:

1. **Given** Azure secret with service principal, **When** user connects to Azure SQL, **Then** authentication succeeds
2. **Given** Azure secret with `CHAIN 'cli'`, **When** user has run `az login`, **Then** authentication succeeds
3. **Given** Azure secret with managed identity, **When** running on Azure VM, **Then** authentication succeeds

---

### User Story 3 - Verify Error Handling (Priority: P1)

Error scenarios must produce clear, actionable messages.

**Acceptance Scenarios**:

1. **Given** invalid Azure credentials, **When** connection attempted, **Then** error includes Azure AD error code
2. **Given** missing Azure secret, **When** MSSQL secret created, **Then** error: "Azure secret 'name' not found"
3. **Given** Azure account without DB access, **When** connection attempted, **Then** clear permission error

---

### User Story 4 - Verify Mixed Configurations (Priority: P2)

Mixed environments with both SQL and Azure auth must work correctly.

**Acceptance Scenarios**:

1. **Given** two databases (one SQL auth, one Azure auth), **When** both attached, **Then** both work correctly
2. **Given** MSSQL secret with both azure_secret AND user/password, **When** created, **Then** azure_secret takes precedence
3. **Given** cross-database query between SQL and Azure auth sources, **When** executed, **Then** results are correct

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: All existing integration tests MUST pass unchanged
- **FR-002**: New unit tests MUST cover Azure secret reading
- **FR-003**: New unit tests MUST cover MSSQL secret validation with azure_secret
- **FR-004**: New unit tests MUST cover FEDAUTH packet encoding
- **FR-005**: New integration tests MUST cover Azure SQL connection
- **FR-006**: Integration tests MUST be skippable when no Azure credentials available
- **FR-007**: Tests MUST verify error messages are clear and actionable

### Test Categories

| Category | Azure Required | Description |
|----------|----------------|-------------|
| Unit - Secret Reading | No | Mock SecretManager, test Azure secret field extraction |
| Unit - MSSQL Secret Validation | No | Test azure_secret validation logic |
| Unit - FEDAUTH Encoding | No | Test token UTF-16LE encoding |
| Unit - Test Function | No | Mock token acquisition, test return format |
| Integration - SQL Auth | No* | Verify existing SQL auth works |
| Integration - Azure SQL | Yes | Full connection with service principal |
| Integration - Azure CLI | Yes | Full connection with CLI credentials |
| Integration - Fabric | Yes | Full connection to Fabric endpoint |

*Requires SQL Server but not Azure

---

## Success Criteria *(mandatory)*

- **SC-001**: All existing tests pass (zero regressions)
- **SC-002**: New unit tests achieve >90% coverage of new code
- **SC-003**: Integration tests validate all supported Azure auth methods
- **SC-004**: Test suite gracefully skips Azure tests when credentials unavailable
- **SC-005**: CI pipeline updated to run unit tests (always) and integration tests (when credentials available)

---

## Technical Context (For Planning Reference)

### Test File Structure

```
test/
├── cpp/                              # Unit tests (no Azure required)
│   ├── test_azure_secret_reader.cpp  # Azure secret field extraction
│   ├── test_mssql_secret_azure.cpp   # MSSQL secret validation
│   └── test_fedauth_encoding.cpp     # FEDAUTH packet encoding
└── sql/
    └── azure/                        # Integration tests
        ├── sql_auth_regression.test  # Verify SQL auth unchanged
        ├── azure_service_principal.test
        ├── azure_cli_auth.test
        ├── azure_error_handling.test
        └── fabric_warehouse.test
```

### Skip Conditions for Azure Tests

```sql
# name: test/sql/azure/azure_service_principal.test
# description: Test Azure SQL connection with service principal
# group: [azure]

require mssql

# Skip if no Azure credentials configured
require-env AZURE_TEST_TENANT_ID
require-env AZURE_TEST_CLIENT_ID
require-env AZURE_TEST_CLIENT_SECRET
require-env AZURE_SQL_TEST_HOST
```

### Unit Test Example

```cpp
// test/cpp/test_mssql_secret_azure.cpp

TEST_CASE("MSSQL secret with azure_secret validates Azure secret exists", "[mssql][azure]") {
    // Mock SecretManager to return/not return Azure secret
    MockSecretManager mock_manager;

    SECTION("Valid Azure secret reference") {
        mock_manager.AddSecret("my_azure", "azure", {...});
        auto result = ValidateMSSQLSecretFields(context, input_with_azure_secret);
        REQUIRE(result.empty());  // No error
    }

    SECTION("Non-existent Azure secret") {
        auto result = ValidateMSSQLSecretFields(context, input_with_azure_secret);
        REQUIRE(result == "Azure secret 'my_azure' not found");
    }

    SECTION("Wrong secret type") {
        mock_manager.AddSecret("my_azure", "s3", {...});  // Not azure type
        auto result = ValidateMSSQLSecretFields(context, input_with_azure_secret);
        REQUIRE(result == "Secret 'my_azure' is not an Azure secret (type: s3)");
    }
}
```

### Integration Test Example

```sql
# name: test/sql/azure/azure_service_principal.test

require mssql
require-env AZURE_TEST_TENANT_ID
require-env AZURE_TEST_CLIENT_ID
require-env AZURE_TEST_CLIENT_SECRET
require-env AZURE_SQL_TEST_HOST

# Create Azure secret
statement ok
CREATE SECRET test_azure (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID '${AZURE_TEST_TENANT_ID}',
    CLIENT_ID '${AZURE_TEST_CLIENT_ID}',
    CLIENT_SECRET '${AZURE_TEST_CLIENT_SECRET}'
);

# Create MSSQL secret with Azure auth
statement ok
CREATE SECRET test_mssql (
    TYPE mssql,
    HOST '${AZURE_SQL_TEST_HOST}',
    PORT 1433,
    DATABASE 'testdb',
    AZURE_SECRET 'test_azure'
);

# Attach and query
statement ok
ATTACH '' AS azuredb (TYPE mssql, SECRET test_mssql);

query I
SELECT 1 AS test_value;
----
1

statement ok
DETACH azuredb;
```

---

## CI/CD Updates

### GitHub Actions

```yaml
# Add to CI workflow
- name: Run Unit Tests
  run: make test

- name: Run Azure Integration Tests
  if: env.AZURE_TEST_TENANT_ID != ''
  env:
    AZURE_TEST_TENANT_ID: ${{ secrets.AZURE_TEST_TENANT_ID }}
    AZURE_TEST_CLIENT_ID: ${{ secrets.AZURE_TEST_CLIENT_ID }}
    AZURE_TEST_CLIENT_SECRET: ${{ secrets.AZURE_TEST_CLIENT_SECRET }}
    AZURE_SQL_TEST_HOST: ${{ secrets.AZURE_SQL_TEST_HOST }}
  run: make azure-integration-test
```

---

## Out of Scope

- Performance testing / benchmarking
- Load testing / stress testing
- Security penetration testing

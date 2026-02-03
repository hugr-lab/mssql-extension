# Feature Specification: Azure Auth Test Function

**Feature Branch**: `azure-003-auth-test-function`
**Created**: 2026-02-03
**Status**: Future / Planning
**Dependencies**: 001-azure-secret-reuse

## Problem Statement

Users need a way to validate Azure authentication independently of the full database connection flow. This helps debug authentication issues without the complexity of TDS protocol errors.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Test Valid Azure Credentials (Priority: P1)

A user wants to verify their Azure secret credentials work before attempting a database connection.

**Acceptance Scenarios**:

1. **Given** valid Azure secret with service principal, **When** user calls `mssql_azure_auth_test('my_azure')`, **Then** function returns truncated token (e.g., "eyJ0eXAi...xyz [1847 chars]")
2. **Given** valid Azure secret with CLI credentials, **When** user has run `az login`, **Then** function returns truncated token
3. **Given** valid credentials, **When** function called multiple times, **Then** cached token returned (no repeated auth requests)

---

### User Story 2 - Test Invalid Credentials (Priority: P1)

A user needs clear error messages when Azure authentication fails.

**Acceptance Scenarios**:

1. **Given** Azure secret with invalid client_secret, **When** test function called, **Then** error: "Azure AD error AADSTS7000215: Invalid client secret"
2. **Given** Azure secret with invalid tenant_id, **When** test function called, **Then** error: "Azure AD error AADSTS90002: Tenant not found"
3. **Given** Azure secret with expired CLI session, **When** test function called, **Then** error: "Azure CLI credentials expired. Run 'az login' to refresh."

---

### User Story 3 - Test Missing/Invalid Secret (Priority: P1)

A user needs clear errors for configuration issues.

**Acceptance Scenarios**:

1. **Given** non-existent secret name, **When** test function called, **Then** error: "Azure secret 'name' not found"
2. **Given** secret that is not TYPE azure, **When** test function called, **Then** error: "Secret 'name' is not an Azure secret (type: mssql)"
3. **Given** NULL or empty secret name, **When** test function called, **Then** error: "Secret name required"

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Function `mssql_azure_auth_test(secret_name VARCHAR) -> VARCHAR`
- **FR-002**: On success, return truncated token with length indicator
- **FR-003**: On failure, return clear error message with Azure AD error code if available
- **FR-004**: Function MUST validate secret exists and is TYPE azure
- **FR-005**: Function MUST use token cache (don't acquire new token if cached valid)
- **FR-006**: Function is for testing/debugging only (not for production use)

### Function Signature

```sql
mssql_azure_auth_test(azure_secret_name VARCHAR) -> VARCHAR
```

### Return Values

| Scenario | Return Value |
|----------|--------------|
| Success | `"eyJ0eXAi...xyz [1847 chars]"` (first 10 + last 3 chars + length) |
| Azure AD error | `"Azure AD error AADSTS7000215: Invalid client secret provided"` |
| Secret not found | `"Error: Azure secret 'name' not found"` |
| Wrong secret type | `"Error: Secret 'name' is not an Azure secret (type: mssql)"` |
| CLI not logged in | `"Error: Azure CLI credentials expired. Run 'az login' to refresh."` |

---

## Success Criteria *(mandatory)*

- **SC-001**: Function returns token for valid service principal credentials
- **SC-002**: Function returns token for valid CLI credentials (after `az login`)
- **SC-003**: Function returns clear Azure AD error codes on auth failure
- **SC-004**: Function returns clear errors for missing/invalid secrets
- **SC-005**: Token caching prevents repeated auth requests

---

## Technical Context (For Planning Reference)

### Implementation

```cpp
// src/azure/azure_test_function.cpp

namespace duckdb {
namespace mssql {
namespace azure {

static void AzureAuthTestFunction(DataChunk &args, ExpressionState &state,
                                  Vector &result) {
    auto &secret_name_vector = args.data[0];

    UnaryExecutor::Execute<string_t, string_t>(
        secret_name_vector, result, args.size(),
        [&](string_t secret_name) {
            auto name = secret_name.GetString();
            if (name.empty()) {
                return StringVector::AddString(result, "Error: Secret name required");
            }

            // Lookup Azure secret and acquire token
            auto &context = state.GetContext();
            auto token_result = TokenAcquisition::AcquireToken(context, name);

            if (token_result.success) {
                // Return truncated token
                auto &token = token_result.access_token;
                auto truncated = token.substr(0, 10) + "..." +
                                token.substr(token.size() - 3) +
                                " [" + std::to_string(token.size()) + " chars]";
                return StringVector::AddString(result, truncated);
            } else {
                return StringVector::AddString(result, token_result.error_message);
            }
        });
}

void RegisterAzureAuthTestFunction(DatabaseInstance &db) {
    ScalarFunction func("mssql_azure_auth_test",
                        {LogicalType::VARCHAR},
                        LogicalType::VARCHAR,
                        AzureAuthTestFunction);
    ExtensionUtil::RegisterFunction(db, func);
}

} // namespace azure
} // namespace mssql
} // namespace duckdb
```

### Registration

```cpp
// In mssql_extension.cpp
#include "azure/azure_test_function.hpp"

void MSSQLExtension::Load(DuckDB &db) {
    // ... existing registrations ...

    // Register Azure auth test function
    mssql::azure::RegisterAzureAuthTestFunction(*db.instance);
}
```

### File Structure

```
src/
├── azure/
│   ├── azure_test_function.cpp   # Test function (namespace duckdb::mssql::azure)
│   └── azure_token.cpp           # Token acquisition (from phase 1)
├── include/azure/
│   ├── azure_test_function.hpp   # Function registration header
│   └── azure_token.hpp           # Token types and interfaces
├── mssql_extension.cpp           # Load function registrations
```

### Namespace

All Azure code uses `duckdb::mssql::azure` namespace (no prefix needed per project conventions).

---

## Example Usage

```sql
-- Load extensions
LOAD azure;
LOAD mssql;

-- Create Azure secret
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID '...',
    CLIENT_ID '...',
    CLIENT_SECRET '...'
);

-- Test authentication
SELECT mssql_azure_auth_test('my_azure');
-- Returns: "eyJ0eXAi...xyz [1847 chars]"

-- Test with bad credentials
SELECT mssql_azure_auth_test('bad_secret');
-- Returns: "Azure AD error AADSTS7000215: Invalid client secret provided"
```

---

## Out of Scope

- TDS authentication flow integration (phase 4)
- Database connection testing
- Token refresh testing (implicit via cache)

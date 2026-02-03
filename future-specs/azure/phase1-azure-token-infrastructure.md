# Feature Specification: Azure Token Infrastructure (Phase 1)

**Feature Branch**: `azure-phase1-token-infrastructure`
**Created**: 2026-02-03
**Status**: Future / Planning
**Dependencies**: None (first phase)

## Problem Statement

The MSSQL extension needs Azure AD token acquisition infrastructure before implementing TDS FEDAUTH authentication. This phase delivers:

1. **Token Acquisition** - Read Azure secrets and acquire tokens from Azure AD
2. **MSSQL Secret Extension** - Add `azure_secret` field to reference Azure credentials
3. **Test Function** - `mssql_azure_auth_test()` for validating credentials without connection

**Deliverable**: Users can validate Azure credentials work before attempting database connections.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Create MSSQL Secret with Azure Auth (Priority: P1)

A user wants to create an MSSQL secret that uses Azure AD authentication.

**Acceptance Scenarios**:

1. **Given** an existing Azure secret `my_azure`, **When** user creates MSSQL secret with `AZURE_SECRET 'my_azure'`, **Then** secret is created successfully without requiring user/password
2. **Given** MSSQL secret with `azure_secret`, **When** user also provides `user` and `password`, **Then** secret is created (fields ignored for Azure auth)
3. **Given** non-existent Azure secret name, **When** user creates MSSQL secret, **Then** clear error: "Azure secret 'name' not found"

---

### User Story 2 - Test Azure Credentials (Priority: P1)

A user wants to verify their Azure credentials work before attempting a database connection.

**Acceptance Scenarios**:

1. **Given** valid Azure secret with service principal, **When** user calls `mssql_azure_auth_test('my_azure')`, **Then** function returns truncated token (e.g., "eyJ0eXAi...xyz [1847 chars]")
2. **Given** valid Azure secret with `CHAIN 'cli'`, **When** user has run `az login`, **Then** function returns truncated token
3. **Given** Azure secret with invalid client_secret, **When** test function called, **Then** error: "Azure AD error AADSTS7000215: Invalid client secret"
4. **Given** Azure secret with expired CLI session, **When** test function called, **Then** error: "Azure CLI credentials expired. Run 'az login' to refresh."

---

### User Story 3 - Backward Compatibility (Priority: P1)

Existing MSSQL secrets with SQL authentication must continue to work unchanged.

**Acceptance Scenarios**:

1. **Given** existing MSSQL secret with `user` and `password`, **When** user attaches database, **Then** SQL authentication works as before
2. **Given** MSSQL secret without `azure_secret`, **When** `user` or `password` is missing, **Then** validation error (existing behavior)
3. **Given** MSSQL secret with neither SQL auth nor Azure auth, **When** created, **Then** error: "Either user/password or azure_secret required"

---

### Edge Cases

- Azure extension not loaded → Clear error: "Azure extension required. Run INSTALL azure; LOAD azure;"
- Azure secret doesn't exist → Clear error: "Azure secret 'name' not found"
- Secret is wrong type → Clear error: "Secret 'name' is not an Azure secret (type: mssql)"
- Service principal missing required fields → Clear error listing missing fields

---

## Requirements *(mandatory)*

### Functional Requirements

**Token Acquisition**:
- **FR-001**: System MUST NOT introduce a new Azure secret type
- **FR-002**: System MUST use DuckDB's existing `TYPE azure` secret infrastructure
- **FR-003**: System MUST support all Azure secret providers: `service_principal`, `credential_chain`, `managed_identity`
- **FR-004**: System MUST acquire tokens with scope `https://database.windows.net/.default`
- **FR-005**: System MUST cache tokens and refresh before expiration

**MSSQL Secret Extension**:
- **FR-006**: MSSQL secret MUST accept optional `azure_secret` parameter
- **FR-007**: When `azure_secret` is present, `user` and `password` MUST be optional
- **FR-008**: When `azure_secret` is absent, `user` and `password` MUST be required (existing behavior)
- **FR-009**: System MUST validate Azure secret exists at MSSQL secret creation time
- **FR-010**: Full backward compatibility with existing MSSQL secrets

**Test Function**:
- **FR-011**: Function `mssql_azure_auth_test(secret_name VARCHAR) -> VARCHAR`
- **FR-012**: On success, return truncated token with length indicator
- **FR-013**: On failure, return clear error message with Azure AD error code if available

### Supported Azure Secret Configurations

| Provider | Required Fields | Optional Fields |
|----------|-----------------|-----------------|
| `service_principal` | `tenant_id`, `client_id`, `client_secret` | - |
| `credential_chain` | - | `chain` (e.g., 'cli;env;managed_identity') |
| `managed_identity` | - | `client_id` (for user-assigned) |

### Extended MSSQL Secret Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `host` | VARCHAR | Yes | SQL Server hostname |
| `port` | INTEGER | Yes | TCP port (default 1433) |
| `database` | VARCHAR | Yes | Database name |
| `user` | VARCHAR | Conditional | Required if no `azure_secret` |
| `password` | VARCHAR | Conditional | Required if no `azure_secret` |
| `use_encrypt` | BOOLEAN | No | TLS encryption (default: true) |
| `catalog` | BOOLEAN | No | Enable catalog (default: true) |
| **`azure_secret`** | VARCHAR | No | **NEW**: Name of Azure secret for Entra ID auth |

### Test Function Return Values

| Scenario | Return Value |
|----------|--------------|
| Success | `"eyJ0eXAi...xyz [1847 chars]"` (first 10 + last 3 chars + length) |
| Azure AD error | `"Azure AD error AADSTS7000215: Invalid client secret provided"` |
| Secret not found | `"Error: Azure secret 'name' not found"` |
| Wrong secret type | `"Error: Secret 'name' is not an Azure secret (type: mssql)"` |
| CLI not logged in | `"Error: Azure CLI credentials expired. Run 'az login' to refresh."` |

---

## Success Criteria *(mandatory)*

- **SC-001**: Azure secrets created by DuckDB Azure extension are readable by MSSQL extension
- **SC-002**: Tokens acquired successfully for service principal, CLI, and managed identity providers
- **SC-003**: MSSQL secret with `azure_secret` creates successfully
- **SC-004**: `mssql_azure_auth_test()` returns token for valid credentials
- **SC-005**: `mssql_azure_auth_test()` returns clear Azure AD error codes on auth failure
- **SC-006**: All existing MSSQL secret tests pass (no regression)
- **SC-007**: Token caching prevents repeated auth requests

---

## Technical Context (For Planning Reference)

### Namespace

All Azure authentication code uses `duckdb::mssql::azure` namespace per project conventions (no prefix needed within namespace).

### File Structure

```
src/
├── azure/
│   ├── azure_token.cpp           # Token acquisition implementation
│   ├── azure_secret_reader.cpp   # Read Azure secrets from SecretManager
│   └── azure_test_function.cpp   # mssql_azure_auth_test() function
├── include/azure/
│   ├── azure_token.hpp           # Token result struct, acquisition interface
│   ├── azure_secret_reader.hpp   # Secret reader interface
│   └── azure_test_function.hpp   # Function registration header
├── mssql_secret.cpp              # Add azure_secret handling
├── include/mssql_secret.hpp      # Add MSSQL_SECRET_AZURE_SECRET constant
└── mssql_extension.cpp           # Register test function
```

### Reading Azure Secrets

```cpp
// src/azure/azure_secret_reader.cpp
namespace duckdb {
namespace mssql {
namespace azure {

struct AzureSecretInfo {
    string provider;
    string tenant_id;
    string client_id;
    string client_secret;
    string chain;
};

AzureSecretInfo ReadAzureSecret(ClientContext &context, const string &secret_name) {
    auto &secret_manager = SecretManager::Get(context);
    auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
    auto secret_match = secret_manager.LookupSecret(transaction, secret_name, "azure");

    if (!secret_match.HasMatch()) {
        throw InvalidInputException("Azure secret '%s' not found", secret_name);
    }

    auto &secret = secret_match.GetSecret();
    auto &kv_secret = secret.Cast<KeyValueSecret>();

    AzureSecretInfo info;
    info.provider = kv_secret.TryGetValue("provider").ToString();
    info.tenant_id = kv_secret.TryGetValue("tenant_id").ToString();
    info.client_id = kv_secret.TryGetValue("client_id").ToString();
    info.client_secret = kv_secret.TryGetValue("client_secret").ToString();
    info.chain = kv_secret.TryGetValue("chain").ToString();
    return info;
}

} // namespace azure
} // namespace mssql
} // namespace duckdb
```

### Token Acquisition

```cpp
// src/azure/azure_token.cpp
namespace duckdb {
namespace mssql {
namespace azure {

struct TokenResult {
    bool success;
    string access_token;
    string error_message;
    timestamp_t expires_at;
};

//! Acquire token for Azure SQL Database
TokenResult AcquireToken(ClientContext &context, const string &azure_secret_name) {
    auto secret_info = ReadAzureSecret(context, azure_secret_name);

    // Check cache first
    auto cached = TokenCache::Get(azure_secret_name);
    if (cached.has_value() && !cached->IsExpired()) {
        return cached.value();
    }

    // Acquire new token based on provider
    TokenResult result;
    if (secret_info.provider == "service_principal") {
        result = AcquireServicePrincipalToken(secret_info);
    } else if (secret_info.provider == "credential_chain") {
        result = AcquireCredentialChainToken(secret_info);
    } else if (secret_info.provider == "managed_identity") {
        result = AcquireManagedIdentityToken(secret_info);
    }

    if (result.success) {
        TokenCache::Put(azure_secret_name, result);
    }
    return result;
}

} // namespace azure
} // namespace mssql
} // namespace duckdb
```

### MSSQL Secret Validation

```cpp
// src/mssql_secret.cpp
string ValidateMSSQLSecretFields(ClientContext &context, const CreateSecretInput &input) {
    auto azure_secret_it = input.options.find(MSSQL_SECRET_AZURE_SECRET);
    bool has_azure_secret = azure_secret_it != input.options.end() &&
                            !azure_secret_it->second.ToString().empty();

    if (has_azure_secret) {
        // Validate Azure secret exists and is correct type
        auto azure_secret_name = azure_secret_it->second.ToString();
        auto &secret_manager = SecretManager::Get(context);
        auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
        auto match = secret_manager.LookupSecret(transaction, azure_secret_name, "azure");
        if (!match.HasMatch()) {
            return StringUtil::Format("Azure secret '%s' not found", azure_secret_name);
        }
        // user/password not required
    } else {
        // Existing validation: require user and password
        // ... existing code ...
    }
    return "";  // Valid
}
```

### Test Function

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

            auto &context = state.GetContext();
            auto token_result = AcquireToken(context, name);

            if (token_result.success) {
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

---

## Example Usage

```sql
-- Load extensions
LOAD azure;
LOAD mssql;

-- Create Azure secret (service principal)
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
    CLIENT_ID 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
    CLIENT_SECRET 'your-client-secret'
);

-- Or use Azure CLI credentials (easiest for analysts)
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'cli'
);

-- Test authentication before connecting
SELECT mssql_azure_auth_test('my_azure');
-- Returns: "eyJ0eXAi...xyz [1847 chars]"

-- Create MSSQL secret with Azure auth
CREATE SECRET my_sql (
    TYPE mssql,
    HOST 'myserver.database.windows.net',
    PORT 1433,
    DATABASE 'mydb',
    AZURE_SECRET 'my_azure'
);

-- Note: Actual connection (ATTACH) requires Phase 2 TDS FEDAUTH integration
```

---

## Out of Scope

- TDS FEDAUTH protocol integration (Phase 2)
- Actual database connections with Azure auth (Phase 2)
- Catalog/DML operations verification (Phase 2)

---

## Assumptions

- DuckDB Azure extension is installed and loaded
- Azure SDK for C++ available via vcpkg (or OAuth2 implemented directly)

# Feature Specification: Azure Secret Reuse

**Feature Branch**: `azure-001-secret-reuse`
**Created**: 2026-02-03
**Status**: Future / Planning
**Dependencies**: None (first phase)

## Problem Statement

The DuckDB ecosystem already has an Azure extension with mature secret infrastructure supporting multiple authentication providers (service principal, credential chain, managed identity). Rather than duplicating this functionality, the MSSQL extension should reuse the existing Azure secret type for token acquisition.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Reuse Existing Azure Secret (Priority: P1)

A user who already has Azure secrets configured for DuckDB's Azure extension (e.g., for blob storage) wants to use the same credentials for MSSQL connections.

**Acceptance Scenarios**:

1. **Given** an existing Azure secret created via DuckDB Azure extension, **When** MSSQL extension reads that secret, **Then** it can extract credentials for token acquisition
2. **Given** Azure secret with `PROVIDER service_principal`, **When** MSSQL reads it, **Then** tenant_id, client_id, client_secret are accessible
3. **Given** Azure secret with `PROVIDER credential_chain`, **When** MSSQL reads it, **Then** the chain configuration is accessible

---

### User Story 2 - Token Acquisition from Azure Secret (Priority: P1)

The system needs to acquire Azure AD tokens using credentials from an Azure secret.

**Acceptance Scenarios**:

1. **Given** Azure secret with service principal credentials, **When** token is requested with scope `https://database.windows.net/.default`, **Then** valid JWT access token is returned
2. **Given** Azure secret with `CHAIN 'cli'`, **When** user has run `az login`, **Then** token is acquired using CLI credentials
3. **Given** Azure secret with invalid credentials, **When** token is requested, **Then** clear Azure AD error is returned (with AADSTS code)

---

### Edge Cases

- Azure extension not loaded → Clear error: "Azure extension required. Run INSTALL azure; LOAD azure;"
- Azure secret doesn't exist → Clear error: "Azure secret 'name' not found"
- Service principal missing required fields → Clear error listing missing fields

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST NOT introduce a new Azure secret type
- **FR-002**: System MUST use DuckDB's existing `TYPE azure` secret infrastructure
- **FR-003**: System MUST support reading Azure secrets via SecretManager API
- **FR-004**: System MUST support all Azure secret providers: `service_principal`, `credential_chain`, `managed_identity`
- **FR-005**: System MUST acquire tokens with scope `https://database.windows.net/.default`

### Supported Azure Secret Configurations

| Provider | Required Fields | Optional Fields |
|----------|-----------------|-----------------|
| `service_principal` | `tenant_id`, `client_id`, `client_secret` | - |
| `credential_chain` | - | `chain` (e.g., 'cli;env;managed_identity') |
| `managed_identity` | - | `client_id` (for user-assigned) |

### Key Entities

- **Azure Secret**: DuckDB's standard secret (TYPE azure) - no changes needed
- **Token Acquisition Module**: New internal module to acquire tokens using Azure secret credentials
- **Token Cache**: Cache tokens with expiry tracking

---

## Success Criteria *(mandatory)*

- **SC-001**: Azure secrets created by DuckDB Azure extension are readable by MSSQL extension
- **SC-002**: Tokens acquired successfully for all supported providers
- **SC-003**: No modifications required to DuckDB Azure extension
- **SC-004**: Clear error messages for all failure scenarios

---

## Technical Context (For Planning Reference)

### Reading Azure Secrets

```cpp
// src/azure/azure_secret_reader.cpp
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {
namespace mssql {
namespace azure {

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

### Token Acquisition Options

1. **Use Azure SDK for C++** - `azure-identity` library
2. **Implement OAuth2 directly** - HTTPS POST to Azure AD token endpoint
3. **Leverage DuckDB Azure extension** - if it exposes token acquisition API

### File Structure

```
src/
├── azure/
│   ├── azure_token.hpp          # Token result struct, acquisition interface
│   ├── azure_token.cpp          # Token acquisition implementation
│   ├── azure_secret_reader.hpp  # Read Azure secrets from SecretManager
│   └── azure_secret_reader.cpp
├── include/azure/
│   ├── azure_token.hpp          # namespace duckdb::mssql::azure
│   └── azure_secret_reader.hpp  # namespace duckdb::mssql::azure
```

### Namespace

All Azure authentication code uses `duckdb::mssql::azure` namespace per project conventions (no prefix needed within namespace).

---

## Out of Scope

- Modifying DuckDB Azure extension
- Creating new secret types
- TDS protocol changes (handled in later phase)

---

## Assumptions

- DuckDB Azure extension is installed and loaded
- Azure SDK for C++ available via vcpkg (or OAuth2 implemented directly)

# Research: Azure Token-Only Secret (Issue #57)

## 1. Approach: New `access_token` Provider for Azure Secrets

**Decision**: Add an `access_token` provider for `TYPE azure` secrets. The MSSQL extension already reads Azure secrets and dispatches by provider (`service_principal`, `credential_chain`). Adding `access_token` follows the same pattern.

**Rationale**: The user's request (issue #57) is to reuse a single token across multiple connections. Instead of modifying the MSSQL secret validation (which requires HOST/DATABASE/PORT), we add a new Azure secret provider that holds the token. The MSSQL secret's `AZURE_SECRET` parameter already supports referencing Azure secrets — this just adds a new provider type.

**User flow**:
```sql
-- Create Azure secret with pre-provided token
CREATE SECRET azure_token (
    TYPE azure,
    PROVIDER access_token,
    ACCESS_TOKEN 'eyJ0eXAiOi...your-jwt-token'
);

-- Reuse across multiple connections via AZURE_SECRET
ATTACH 'Server=server1.database.windows.net;Database=db1' AS db1 (
    TYPE mssql, AZURE_SECRET 'azure_token'
);
ATTACH 'Server=server2.database.windows.net;Database=db2' AS db2 (
    TYPE mssql, AZURE_SECRET 'azure_token'
);
```

## 2. Implementation Points

### 2a. Azure Secret Reader (`azure_secret_reader.cpp` / `.hpp`)

- Add `access_token` field to `AzureSecretInfo` struct
- In `ReadAzureSecret()`, read `access_token` value from the secret when provider is `access_token`
- Skip validation that requires tenant_id/client_id/client_secret for this provider

### 2b. Token Acquisition (`azure_token.cpp`)

- In `AcquireToken()`, add `access_token` provider branch
- When provider is `access_token`, return the token directly (no HTTP/CLI call needed)
- Still use token cache for consistency

### 2c. Azure Secret Registration

The DuckDB Azure extension creates `TYPE azure` secrets. We need to register our own `access_token` provider for the `azure` type. This is done in the extension's registration code.

**Key question**: Can we register a provider for an existing type from another extension? Yes — DuckDB's `SecretManager` allows multiple extensions to register providers for the same type via `CreateSecretFunction`.

### 2d. Files to modify:
- `src/include/azure/azure_secret_reader.hpp` — Add `access_token` field to `AzureSecretInfo`
- `src/azure/azure_secret_reader.cpp` — Read `access_token` from secret
- `src/azure/azure_token.cpp` — Handle `access_token` provider in `AcquireToken()`
- `src/mssql_extension.cpp` — Register `access_token` provider for `TYPE azure`
- `AZURE.md` — Document new provider with examples
- `README.md` — Add PK update limitation (issue #53)
- `test/sql/azure/azure_secret_token_only.test` — Unit test

## 3. No MSSQL Secret Changes Needed

HOST/DATABASE/PORT remain required in the MSSQL secret. The change is entirely in the Azure secret layer — adding a new provider that holds a pre-provided token.

## 4. Documentation Updates

- **AZURE.md**: Add section for `access_token` provider under Authentication Methods
- **README.md**: Add "Updating primary key columns is not supported" to Limitations section (issue #53)

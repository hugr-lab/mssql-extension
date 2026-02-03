# Research: Azure AD Authentication for MSSQL Extension

**Created**: 2026-02-03
**Status**: Research Notes
**Purpose**: Technical research for Azure AD (Entra ID) authentication implementation

---

## DuckDB Azure Extension

The DuckDB Azure extension provides a mature secret infrastructure that we should leverage for token acquisition.

### Secret Providers

| Provider | Use Case | Required Fields |
|----------|----------|-----------------|
| `service_principal` | Automated pipelines, CI/CD | `TENANT_ID`, `CLIENT_ID`, `CLIENT_SECRET` |
| `credential_chain` | Auto-detect (managed identity, CLI, env) | `CHAIN` (optional) |
| `managed_identity` | Azure VM/AKS with specific identity | `CLIENT_ID` (optional) |
| `config` | Connection string / anonymous | `CONNECTION_STRING` or `ACCOUNT_NAME` |

### Service Principal Secret

```sql
CREATE SECRET my_azure_sp (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
    CLIENT_ID 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
    CLIENT_SECRET 'your-client-secret'
);
```

### Credential Chain Secret

```sql
CREATE SECRET my_azure_chain (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'managed_identity;cli;env'
);
```

Chain values: `cli`, `managed_identity`, `workload_identity`, `env`, `default`

### Managed Identity Secret

```sql
CREATE SECRET my_azure_mi (
    TYPE azure,
    PROVIDER managed_identity,
    CLIENT_ID 'user-assigned-identity-client-id'  -- Optional
);
```

### Accessing Azure Secrets Programmatically

```cpp
#include "duckdb/main/secret/secret_manager.hpp"

auto &secret_manager = SecretManager::Get(context);
auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
auto secret_match = secret_manager.LookupSecret(transaction, "my_azure_secret", "azure");

if (secret_match.HasMatch()) {
    auto &secret = secret_match.GetSecret();
    auto &kv_secret = secret.Cast<KeyValueSecret>();

    auto provider = kv_secret.TryGetValue("provider");
    auto tenant_id = kv_secret.TryGetValue("tenant_id");
    auto client_id = kv_secret.TryGetValue("client_id");
    auto client_secret = kv_secret.TryGetValue("client_secret");
}
```

### Azure SDK Integration

The DuckDB Azure extension uses the Azure SDK for C++. For token acquisition, we need:

```cpp
// Using Azure Identity SDK
#include <azure/identity/client_secret_credential.hpp>

auto credential = std::make_shared<Azure::Identity::ClientSecretCredential>(
    tenant_id, client_id, client_secret);

Azure::Core::Credentials::TokenRequestContext token_context;
token_context.Scopes.push_back("https://database.windows.net/.default");

auto token = credential->GetToken(token_context, Azure::Core::Context());
std::string access_token = token.Token;
std::chrono::system_clock::time_point expires_on = token.ExpiresOn;
```

### Debug Environment Variable

```bash
export AZURE_LOG_LEVEL=verbose
```

---

## TDS Federated Authentication (FEDAUTH)

### Protocol Overview

TDS v7.4+ supports Federated Authentication for Azure AD token-based login. This allows passing pre-acquired tokens instead of username/password.

### PRELOGIN Changes

Add FEDAUTHREQUIRED option (0x06) to advertise FEDAUTH support:

```
PRELOGIN Options:
├── VERSION (0x00)
├── ENCRYPTION (0x01)
├── INSTOPT (0x02)
├── THREADID (0x03)
├── MARS (0x04)
├── FEDAUTHREQUIRED (0x06) ← NEW: 1 byte, value 0x01
└── TERMINATOR (0xFF)
```

### LOGIN7 FEDAUTH Extension

When using FEDAUTH, add feature extension to LOGIN7:

```
LOGIN7 Packet:
├── Fixed Header (94 bytes)
├── Variable Data
├── FeatureExt Offset → Feature Extensions
└── Feature Extensions:
    ├── FEATUREEXT_FEDAUTH (0x02)
    │   ├── FeatureId: 0x02
    │   ├── FeatureDataLen: 4 + token_len_bytes
    │   ├── Options: 4 bytes
    │   │   └── FedAuthLibrary: 0x02 (ADAL/MSAL)
    │   │   └── Workflow: Azure AD token
    │   └── Token: UTF-16LE encoded JWT
    └── FEATUREEXT_TERMINATOR (0xFF)
```

### FedAuth Library Values

| Value | Library | Description |
|-------|---------|-------------|
| 0x01 | SSPI | Windows integrated |
| **0x02** | ADAL/MSAL | Azure AD (our target) |
| 0x03 | Security Token | Pre-acquired token |

### Token Encoding

Access token must be UTF-16LE encoded:

```cpp
std::string token_utf8 = "eyJ0eXAiOiJKV1Q...";
std::vector<uint8_t> token_utf16le = Utf8ToUtf16LE(token_utf8);

// In LOGIN7 FEDAUTH extension:
// 4 bytes: length of token in bytes
// N bytes: UTF-16LE encoded token
```

### FEDAUTHINFO Response Token

Server may return FEDAUTHINFO (0xEE) with guidance:

```
FEDAUTHINFO Token (0xEE):
├── TokenType: 0xEE
├── Length: 4 bytes (total data length)
├── CountOfInfoIDs: DWORD
└── InfoData[]:
    ├── InfoID: DWORD (1=SPN, 2=STSURL)
    ├── InfoDataLen: DWORD
    └── InfoData: variable
```

---

## Azure AD Token Acquisition

### OAuth2 Token Endpoint

```
POST https://login.microsoftonline.com/{tenant_id}/oauth2/v2.0/token
Content-Type: application/x-www-form-urlencoded

grant_type=client_credentials
&client_id={client_id}
&client_secret={client_secret}
&scope=https://database.windows.net/.default
```

### Response

```json
{
  "token_type": "Bearer",
  "expires_in": 3599,
  "access_token": "eyJ0eXAiOiJKV1QiLCJhbGciOiJSUzI1NiIs..."
}
```

### Required Scope for SQL

```
https://database.windows.net/.default
```

This scope works for:
- Azure SQL Database
- Azure SQL Managed Instance
- Microsoft Fabric SQL Endpoint
- Azure Synapse Analytics

### Token Lifetime

- Default: ~1 hour (3600 seconds)
- Refresh: 5 minutes before expiration
- No refresh token in client credentials flow; acquire new token

---

## Microsoft Fabric Considerations

### Endpoint Format

```
{workspace-name}.datawarehouse.fabric.microsoft.com
```

### Differences from Azure SQL

| Aspect | Azure SQL | Fabric |
|--------|-----------|--------|
| Port | 1433 | 1433 |
| TLS | Required | Required |
| Auth | Azure AD or SQL | Azure AD only |
| DBCC | Supported | Limited |
| Statistics | Full | Limited |
| DDL | Full | Limited |

### Catalog Query Differences

Fabric may have different system view schemas. Test:
- `sys.schemas`
- `sys.tables`
- `sys.columns`
- `sys.indexes`

---

## Implementation Options

### Option 1: Use Azure SDK Directly

Pros:
- Well-tested, maintained by Microsoft
- Handles all credential types
- Automatic token refresh

Cons:
- Large dependency
- Complex build integration

### Option 2: Leverage DuckDB Azure Extension

Pros:
- Already integrated with DuckDB
- Consistent secret management
- Smaller footprint

Cons:
- Need to access internal token acquisition
- May require extension API additions

### Option 3: Implement OAuth2 Client Credentials

Pros:
- Minimal dependencies
- Full control
- Uses existing TLS/HTTP code

Cons:
- Only supports service principal initially
- Must implement managed identity separately

### Recommendation

**Option 2 + Option 3 fallback**:
1. Try to use DuckDB Azure extension's token acquisition if accessible
2. Fall back to direct OAuth2 for service principal
3. Add managed identity support via IMDS endpoint

---

## Token Caching Strategy

```cpp
struct CachedToken {
    std::string access_token;
    std::chrono::system_clock::time_point expires_at;

    bool IsValid() const {
        auto margin = std::chrono::minutes(5);
        return std::chrono::system_clock::now() < (expires_at - margin);
    }
};

class TokenCache {
    std::mutex mutex_;
    std::unordered_map<std::string, CachedToken> cache_;  // keyed by secret name

public:
    std::optional<std::string> GetToken(const std::string &secret_name);
    void SetToken(const std::string &secret_name, const std::string &token, int expires_in);
    void Invalidate(const std::string &secret_name);
};
```

---

## Error Handling

### Azure AD Error Codes

| Code | Description |
|------|-------------|
| AADSTS7000215 | Invalid client secret |
| AADSTS700016 | Application not found in tenant |
| AADSTS50034 | User account doesn't exist |
| AADSTS65001 | User hasn't consented |
| AADSTS70011 | Invalid scope |
| AADSTS90002 | Tenant not found |

### Error Message Format

```
Azure AD authentication failed: AADSTS7000215 - Invalid client secret provided.
Verify the client_secret in your Azure secret 'my_azure_secret'.
```

---

## References

- [DuckDB Azure Extension](https://duckdb.org/docs/stable/core_extensions/azure)
- [GitHub - duckdb/duckdb-azure](https://github.com/duckdb/duckdb-azure)
- [TDS Protocol Specification (MS-TDS)](https://docs.microsoft.com/en-us/openspecs/windows_protocols/ms-tds/)
- [Azure AD Authentication for Azure SQL](https://docs.microsoft.com/en-us/azure/azure-sql/database/authentication-aad-overview)
- [OAuth2 Client Credentials Flow](https://docs.microsoft.com/en-us/azure/active-directory/develop/v2-oauth2-client-creds-grant-flow)
- [Azure SDK for C++](https://github.com/Azure/azure-sdk-for-cpp)
- [Microsoft Fabric Documentation](https://docs.microsoft.com/en-us/fabric/)

---

## Open Questions

1. **Q**: Can we access DuckDB Azure extension's token acquisition API?
   **A**: Needs investigation - may need to use Azure SDK directly

2. **Q**: What Azure SDK components are needed for token acquisition?
   **A**: `azure-identity` for credentials, `azure-core` for HTTP

3. **Q**: How does Fabric handle DBCC and statistics?
   **A**: Needs testing - likely limited support

4. **Q**: Should we add vcpkg dependency for Azure SDK?
   **A**: Evaluate size impact vs implementing OAuth2 directly

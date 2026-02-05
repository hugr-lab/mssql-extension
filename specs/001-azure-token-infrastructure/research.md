# Research: Azure Token Infrastructure (Phase 1)

**Feature**: 001-azure-token-infrastructure
**Created**: 2026-02-05
**Status**: Complete

## Research Questions

### RQ-001: How to read DuckDB Azure secrets programmatically?

**Decision**: Use `SecretManager::Get(context).LookupSecret()` with type filter "azure"

**Rationale**: DuckDB provides a unified secret management API. The Azure extension registers secrets with `TYPE azure`, and we can access them by name and type. The `KeyValueSecret` class provides `TryGetValue()` for field access.

**Implementation Pattern**:
```cpp
auto &secret_manager = SecretManager::Get(context);
auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
auto secret_match = secret_manager.LookupSecret(transaction, secret_name, "azure");

if (!secret_match.HasMatch()) {
    throw InvalidInputException("Azure secret '%s' not found", secret_name);
}

auto &secret = secret_match.GetSecret();
auto &kv_secret = secret.Cast<KeyValueSecret>();

auto provider = kv_secret.TryGetValue("provider").ToString();
auto tenant_id = kv_secret.TryGetValue("tenant_id").ToString();
auto client_id = kv_secret.TryGetValue("client_id").ToString();
auto client_secret = kv_secret.TryGetValue("client_secret").ToString();
auto chain = kv_secret.TryGetValue("chain").ToString();
```

**Alternatives Considered**:
- Direct file access to secrets storage: Rejected (bypasses DuckDB API, not portable)
- Creating our own Azure secret type: Rejected (duplicates DuckDB Azure extension functionality)

---

### RQ-002: How to acquire Azure AD tokens - Azure SDK vs Manual OAuth2?

**Decision**: Use Azure SDK for C++ (`azure-identity-cpp`) for non-interactive flows; implement OAuth2+PKCE manually for interactive browser auth

**Rationale**: After researching both the Azure SDK for C++ and DuckDB's Azure extension implementation:

1. **DuckDB Azure extension uses `azure-identity-cpp`** - They use `AzureCliCredential`, `ManagedIdentityCredential`, `ClientSecretCredential`, and `ChainedTokenCredential` from the Azure SDK
2. **Azure SDK for C++ does NOT support `InteractiveBrowserCredential`** - Unlike .NET/Python/JavaScript SDKs, the C++ SDK lacks interactive browser authentication
3. **Consistency with DuckDB ecosystem** - Using the same SDK as DuckDB Azure provides consistent behavior and easier maintenance

**Credential Support Matrix**:

| Credential Type | Azure SDK C++ | Our Need | Approach |
|-----------------|---------------|----------|----------|
| Service Principal | ✅ `ClientSecretCredential` | ✅ | Use SDK |
| Azure CLI | ✅ `AzureCliCredential` | ✅ | Use SDK |
| Managed Identity | ✅ `ManagedIdentityCredential` | ✅ | Use SDK |
| Environment | ✅ `EnvironmentCredential` | ✅ | Use SDK |
| Credential Chain | ✅ `ChainedTokenCredential` | ✅ | Use SDK |
| Interactive Browser | ❌ Not available | ✅ | Manual OAuth2+PKCE |

**DuckDB Azure Implementation Reference** (from `azure_storage_account_client.cpp`):

```cpp
// Chained credentials
if (item == "cli") {
    sources.push_back(std::make_shared<Azure::Identity::AzureCliCredential>(credential_options));
} else if (item == "managed_identity") {
    sources.push_back(std::make_shared<Azure::Identity::ManagedIdentityCredential>(credential_options));
}
auto credential = std::make_shared<Azure::Identity::ChainedTokenCredential>(sources);

// Service principal
auto credential = std::make_shared<Azure::Identity::ClientSecretCredential>(
    tenant_id, client_id, client_secret, credential_options);
```

**Dependencies** (via vcpkg):

```json
{
  "dependencies": [
    "azure-identity-cpp"
  ],
  "overrides": [
    { "name": "openssl", "version": "3.6.0" }
  ]
}
```

The `azure-identity-cpp` package depends on:
- `azure-core-cpp` (>= 1.16.2) - HTTP transport, credentials base
- `openssl` - Already in our project

**Alternatives Considered**:
- Full manual OAuth2 implementation: Rejected (reinvents well-tested code for non-interactive flows)
- Azure SDK only (no interactive): Rejected (MFA users without CLI need interactive auth)
- Different SDK (MSAL): Not available for C++ in a usable form

**Azure SDK Token Acquisition** (for non-interactive):

```cpp
#include <azure/identity/azure_cli_credential.hpp>
#include <azure/identity/client_secret_credential.hpp>
#include <azure/identity/managed_identity_credential.hpp>
#include <azure/identity/chained_token_credential.hpp>

// Get token using Azure SDK
Azure::Core::Credentials::TokenRequestContext context;
context.Scopes = {"https://database.windows.net/.default"};
auto token = credential->GetToken(context, Azure::Core::Context());
// token.Token contains the access token string
// token.ExpiresOn contains expiration time
```

**Manual OAuth2 Token Endpoint** (for interactive browser only):

Token Endpoint: `https://login.microsoftonline.com/{tenant_id}/oauth2/v2.0/token`

**Error Response** (both SDK and manual):

```json
{
  "error": "invalid_client",
  "error_description": "AADSTS7000215: Invalid client secret provided...",
  "error_codes": [7000215],
  "timestamp": "2026-02-05T10:00:00Z"
}
```

**Namespace Convention**:

All Azure authentication code uses `duckdb::mssql::azure` namespace per project conventions:

```cpp
namespace duckdb {
namespace mssql {
namespace azure {

// No prefix needed within namespace
class TokenCache { ... };
struct TokenResult { ... };
TokenResult AcquireToken(ClientContext &context, const string &secret_name);

} // namespace azure
} // namespace mssql
} // namespace duckdb
```

---

### RQ-003: How to handle credential_chain and managed_identity providers?

**Decision**: Use Azure SDK credential classes directly, matching DuckDB Azure extension

**Provider Implementations** (via Azure SDK):

| Provider | Azure SDK Class | Notes |
|----------|-----------------|-------|
| `service_principal` | `ClientSecretCredential` | Requires tenant_id, client_id, client_secret |
| `credential_chain` with `cli` | `AzureCliCredential` | Uses Azure CLI under the hood |
| `credential_chain` with `env` | `EnvironmentCredential` | Reads AZURE_* env vars |
| `credential_chain` with `managed_identity` | `ManagedIdentityCredential` | IMDS endpoint (handled by SDK) |
| `credential_chain` with `interactive` | **Manual OAuth2+PKCE** | SDK doesn't support this |

**Implementation Pattern** (matching DuckDB Azure):

```cpp
#include <azure/identity.hpp>

namespace duckdb {
namespace mssql {
namespace azure {

std::shared_ptr<Azure::Core::Credentials::TokenCredential>
CreateCredential(const AzureSecretInfo &info) {
    if (info.provider == "service_principal") {
        return std::make_shared<Azure::Identity::ClientSecretCredential>(
            info.tenant_id, info.client_id, info.client_secret);
    }

    if (info.provider == "credential_chain") {
        std::vector<std::shared_ptr<Azure::Core::Credentials::TokenCredential>> sources;

        for (const auto &item : ParseChain(info.chain)) {
            if (item == "cli") {
                sources.push_back(std::make_shared<Azure::Identity::AzureCliCredential>());
            } else if (item == "managed_identity") {
                sources.push_back(std::make_shared<Azure::Identity::ManagedIdentityCredential>());
            } else if (item == "env") {
                sources.push_back(std::make_shared<Azure::Identity::EnvironmentCredential>());
            } else if (item == "interactive") {
                // Special case: use our custom interactive credential
                sources.push_back(CreateInteractiveCredential(info));
            }
        }

        return std::make_shared<Azure::Identity::ChainedTokenCredential>(sources);
    }

    if (info.provider == "managed_identity") {
        if (!info.client_id.empty()) {
            return std::make_shared<Azure::Identity::ManagedIdentityCredential>(info.client_id);
        }
        return std::make_shared<Azure::Identity::ManagedIdentityCredential>();
    }

    throw InvalidInputException("Unknown provider: %s", info.provider);
}

} // namespace azure
} // namespace mssql
} // namespace duckdb
```

**Alternatives Considered**:
- Manual OAuth2 for all flows: Rejected (reinvents well-tested SDK code)
- SDK only (skip interactive): Rejected (MFA users need browser auth)

---

### RQ-004: How to implement thread-safe token caching?

**Decision**: Use `std::mutex`-protected `std::unordered_map` with 5-minute expiration margin

**Rationale**: DuckDB can execute queries from multiple threads. Token cache must be thread-safe. Standard library primitives are sufficient; no external cache library needed.

**Cache Structure**:
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
    std::unordered_map<std::string, CachedToken> cache_;

public:
    std::optional<std::string> GetToken(const std::string &secret_name);
    void SetToken(const std::string &secret_name, const std::string &token, int64_t expires_in);
    void Invalidate(const std::string &secret_name);
    void Clear();
};
```

**Cache Key**: Azure secret name (not MSSQL secret name)

**Expiration Strategy**:
- Store `expires_at = now + expires_in seconds`
- Return cached token if `now < (expires_at - 5 minutes)`
- Refresh proactively before expiration

**Alternatives Considered**:
- No caching: Rejected (unnecessary network requests, rate limiting risk)
- LRU cache with max size: Over-engineering for expected usage (~100 secrets max)
- Atomic operations only: Rejected (need to update multiple fields atomically)

---

### RQ-005: How to detect if Azure extension is loaded?

**Decision**: Attempt secret lookup with type "azure" and check error type

**Rationale**: There's no direct API to check if an extension is loaded. The cleanest approach is to attempt the operation and provide a clear error message if it fails.

**Detection Pattern**:
```cpp
try {
    auto &secret_manager = SecretManager::Get(context);
    auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
    auto match = secret_manager.LookupSecret(transaction, azure_secret_name, "azure");

    if (!match.HasMatch()) {
        // Azure extension loaded but secret not found
        throw InvalidInputException("Azure secret '%s' not found", azure_secret_name);
    }
} catch (const CatalogException &e) {
    // Azure extension not loaded - secret type "azure" not registered
    throw InvalidInputException(
        "Azure extension required for Azure authentication. "
        "Run: INSTALL azure; LOAD azure;");
}
```

**Alternatives Considered**:
- Check extension registry: DuckDB doesn't expose this cleanly
- Try loading azure extension automatically: Rejected (user should explicitly load)

---

### RQ-006: How to handle HTTP requests for OAuth2?

**Decision**: Azure SDK handles HTTP for non-interactive; manual HTTP only for interactive browser auth

**Rationale**: With Azure SDK adoption, most HTTP communication is handled internally by the SDK. We only need manual HTTP for:
1. Interactive browser OAuth2 callback server (localhost)
2. Token exchange after interactive auth

**Azure SDK HTTP Transport** (automatic):

The Azure SDK uses its own HTTP transport (`azure-core-cpp`), which:
- Handles HTTPS to Azure AD endpoints
- Manages retries and timeouts
- Supports proxy configuration

**Manual HTTP for Interactive Auth Only**:

```cpp
namespace duckdb {
namespace mssql {
namespace azure {

// Minimal HTTP server for OAuth callback
class OAuthCallbackServer {
    int server_fd_;
    int port_;

public:
    OAuthCallbackServer();  // Bind to random port
    int GetPort() const { return port_; }

    // Serve success/error HTML and capture callback
    OAuthCallbackResult WaitForCallback(int timeout_seconds);
};

// Token exchange POST request (only for interactive flow)
TokenResult ExchangeCodeForToken(const string &code, const string &code_verifier,
                                  const string &redirect_uri, const string &tenant_id);

} // namespace azure
} // namespace mssql
} // namespace duckdb
```

**Alternatives Considered**:
- Full manual HTTP for all flows: Rejected (SDK handles this better)
- External HTTP library: Rejected (SDK already includes HTTP transport)

---

### RQ-007: How to validate MSSQL secret with conditional fields?

**Decision**: Modify `ValidateMSSQLSecretFields()` to check for `azure_secret` before requiring user/password

**Rationale**: The existing validation enforces user/password as required. With Azure auth, these become optional when `azure_secret` is present.

**Validation Logic**:
```cpp
string ValidateMSSQLSecretFields(ClientContext &context, const CreateSecretInput &input) {
    auto azure_it = input.options.find(MSSQL_SECRET_AZURE_SECRET);
    bool has_azure = azure_it != input.options.end() &&
                     !azure_it->second.ToString().empty();

    if (has_azure) {
        // Validate Azure secret exists and is correct type
        auto azure_name = azure_it->second.ToString();
        // ... lookup and validate Azure secret ...

        // user/password not required for Azure auth
        // Still check required fields: host, port, database
    } else {
        // Existing validation: require user and password
        // ... existing code ...
    }

    // Always required: host, port, database
    // ...

    return "";  // Valid
}
```

**Error Messages**:
- Azure secret not found: `"Azure secret 'name' not found"`
- Wrong secret type: `"Secret 'name' is not an Azure secret (type: mssql)"`
- Neither auth method: `"Either user/password or azure_secret required"`

**Alternatives Considered**:
- Separate creation functions for SQL vs Azure: Over-complicated, harder to maintain
- New MSSQL secret type for Azure: Rejected (breaks existing patterns)

---

### RQ-008: How to implement interactive authentication?

**Decision**: Implement OAuth2 Device Authorization Grant (RFC 8628) - same approach as MotherDuck

**Rationale**: Device Code Flow is simpler and more robust than Authorization Code + PKCE:

| Aspect | Device Code Flow | Authorization Code + PKCE |
|--------|------------------|---------------------------|
| Browser opening | ❌ Not needed | ✅ Required |
| Localhost HTTP server | ❌ Not needed | ✅ Required |
| Works in SSH/headless | ✅ Yes | ❌ No |
| Works in containers | ✅ Yes | ❌ No |
| Firewall issues | ❌ None | ✅ Port binding |
| Implementation | Simple polling | Complex callbacks |

MotherDuck uses Device Code Flow for their DuckDB extension authentication, validating this approach for CLI tools.

**Flow Overview**:
1. Request device code from `/devicecode` endpoint
2. Display verification URL and user code to user
3. Poll `/token` endpoint at specified interval
4. User visits URL on any device, enters code, completes MFA
5. Polling returns access token when user completes authentication

**Device Code Request**:
```
POST https://login.microsoftonline.com/{tenant}/oauth2/v2.0/devicecode
Content-Type: application/x-www-form-urlencoded

client_id={client_id}
&scope=https://database.windows.net/.default
```

**Device Code Response**:
```json
{
    "device_code": "GMMhmHCXhWEzkobqIHGG_EnNYYsAkukHspeYUk9E8...",
    "user_code": "ABC123XYZ",
    "verification_uri": "https://microsoft.com/devicelogin",
    "expires_in": 900,
    "interval": 5,
    "message": "To sign in, visit https://microsoft.com/devicelogin and enter code ABC123XYZ"
}
```

**Token Polling Request**:
```
POST https://login.microsoftonline.com/{tenant}/oauth2/v2.0/token
Content-Type: application/x-www-form-urlencoded

grant_type=urn:ietf:params:oauth:grant-type:device_code
&client_id={client_id}
&device_code={device_code}
```

**Polling Response Codes**:

| Error | Meaning | Action |
|-------|---------|--------|
| `authorization_pending` | User hasn't completed login | Continue polling |
| `authorization_declined` | User denied request | Stop, return error |
| `expired_token` | 15 min timeout exceeded | Stop, return error |
| `bad_verification_code` | Invalid device_code | Stop, return error |
| (success) | Token returned | Return token |

**Implementation** (in `duckdb::mssql::azure` namespace):
```cpp
namespace duckdb {
namespace mssql {
namespace azure {

struct DeviceCodeResponse {
    string device_code;
    string user_code;
    string verification_uri;
    string message;
    int expires_in;
    int interval;
};

// Request device code from Azure AD
DeviceCodeResponse RequestDeviceCode(const string &tenant_id, const string &client_id);

// Poll for token (blocking, with interval sleep)
TokenResult PollForToken(const string &tenant_id, const string &client_id,
                          const string &device_code, int interval, int timeout);

// Main entry point for interactive auth
TokenResult AcquireInteractiveToken(const AzureSecretInfo &info);

} // namespace azure
} // namespace mssql
} // namespace duckdb
```

**Client ID for Interactive Auth**:
- Use Azure Data Studio public client ID: `e32693cc-fce3-49e4-8b0f-d8a66c4fb1a9`
- Or require user to provide `client_id` in the Azure secret

**User Experience**:
```
D SELECT mssql_azure_auth_test('my_azure');
To sign in, visit https://microsoft.com/devicelogin and enter code ABC123XYZ
(waiting for authentication...)
┌─────────────────────────────────────┐
│ mssql_azure_auth_test('my_azure')   │
├─────────────────────────────────────┤
│ eyJ0eXAi...xyz [1847 chars]         │
└─────────────────────────────────────┘
```

**Alternatives Considered**:
- Authorization Code + PKCE: Rejected (requires browser opening, localhost server, doesn't work in SSH)
- Embedded webview: Rejected (complex, platform-specific, security concerns)
- MSAL library: Rejected (not available for C++ in usable form)

---

## Dependencies Summary

| Dependency | Type | Purpose | Notes |
|------------|------|---------|-------|
| DuckDB | Build + Runtime | Extension framework, SecretManager | main branch |
| OpenSSL | Build | TLS, crypto (PKCE SHA256) | vcpkg, already present |
| **azure-identity-cpp** | **Build** | **Token acquisition (non-interactive)** | **NEW via vcpkg** |
| **azure-core-cpp** | **Build** | **HTTP transport, credentials base** | **Transitive dep** |
| DuckDB Azure extension | Runtime | Provides `TYPE azure` secrets | User must `LOAD azure` |

**New vcpkg dependencies** (matching DuckDB Azure extension):

```json
{
  "dependencies": [
    "azure-identity-cpp"
  ],
  "overrides": [
    { "name": "openssl", "version": "3.6.0" }
  ]
}
```

**Rationale for Azure SDK**:
1. **Consistency** - Same libraries as DuckDB Azure extension
2. **Well-tested** - Production-quality credential handling
3. **Maintained** - Active development by Microsoft
4. **License** - MIT license, compatible with our project

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Azure AD rate limiting | Low | Medium | Token caching, 5-minute margin |
| CLI auth not available on all platforms | Medium | Low | Service principal or interactive fallback |
| IMDS endpoint unreachable (non-Azure env) | Medium | Low | Clear error message, timeout handling |
| JSON parsing edge cases | Low | Low | Only parse well-known Azure AD response format |
| Device code expires (15 min timeout) | Medium | Low | Clear error, user can retry immediately |
| User doesn't complete device code auth | Medium | Low | Polling stops gracefully, clear error |
| Network issues during polling | Low | Medium | Exponential backoff, clear error after max retries |

---

## Open Questions Resolved

1. **Q**: Should we use Azure SDK for C++?
   **A**: No - implement OAuth2 directly to minimize dependencies

2. **Q**: What scope for Azure SQL Database tokens?
   **A**: `https://database.windows.net/.default` (works for all Azure SQL services)

3. **Q**: How long are tokens valid?
   **A**: ~1 hour (3600 seconds), refresh at 55 minutes (5-minute margin)

4. **Q**: Should we support certificate-based auth?
   **A**: Out of scope for Phase 1 (see spec Out of Scope)

5. **Q**: What if Azure extension provides token acquisition API in future?
   **A**: Designed for easy refactoring - token acquisition isolated in `azure_token.cpp`

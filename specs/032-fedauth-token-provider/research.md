# Research: FEDAUTH Token Provider Enhancements

**Feature**: 032-fedauth-token-provider
**Date**: 2026-02-06
**Status**: Complete

## Research Questions

### RQ1: JWT Parsing Without External Library

**Decision**: Implement minimal JWT parser using base64 decoding + JSON parsing

**Rationale**:
- Azure AD JWTs are standard format: `header.payload.signature` (base64url encoded)
- Only need to parse payload for `exp`, `aud`, `oid` claims
- DuckDB already includes `yyjson` for JSON parsing
- Base64url decoding is simple (~30 lines of code)
- No signature verification needed (token already validated by Azure AD)

**Alternatives Considered**:
- jwt-cpp library: Header-only but adds complexity, not needed for claim extraction
- OpenSSL JWT functions: Requires signature verification, overkill for our needs

**Implementation**:
```cpp
struct JwtClaims {
    int64_t exp;           // Expiration timestamp (Unix seconds)
    std::string aud;       // Audience (resource URL)
    std::string oid;       // Object ID (for logging)
    bool valid;            // Parse success flag
    std::string error;     // Parse error message
};

JwtClaims ParseJwtClaims(const std::string &token);
```

### RQ2: ACCESS_TOKEN Option Placement

**Decision**: Support ACCESS_TOKEN in both ATTACH options and MSSQL secrets

**Rationale**:
- ATTACH option: Simple one-off usage for Fabric notebooks
- MSSQL secret: Reusable configuration for scripts
- Follows existing pattern where `AZURE_SECRET` works in both places

**Alternatives Considered**:
- ATTACH option only: Simpler but forces repeated token entry
- Secret only: More complex for simple use cases

**ATTACH Syntax**:
```sql
-- Direct in ATTACH
ATTACH 'Server=...;Database=...' AS db (TYPE mssql, ACCESS_TOKEN 'eyJ...');

-- Via MSSQL secret
CREATE SECRET my_token (TYPE mssql, ACCESS_TOKEN 'eyJ...', HOST '...', DATABASE '...');
ATTACH '' AS db (TYPE mssql, SECRET my_token);
```

### RQ3: Environment Variable Authentication Flow

**Decision**: Integrate with existing credential_chain using `env` chain

**Rationale**:
- Follows DuckDB Azure extension pattern
- Users already familiar with `CHAIN 'cli'` syntax
- Environment variables are standard Azure SDK names
- Token refresh possible (have full credentials)

**Alternatives Considered**:
- New secret type: More explicit but inconsistent with DuckDB Azure extension
- Direct env var reading without secret: Less secure, no caching

**Environment Variables**:
| Variable | Purpose | Required |
|----------|---------|----------|
| `AZURE_TENANT_ID` | Azure AD tenant | Yes |
| `AZURE_CLIENT_ID` | Service principal app ID | Yes |
| `AZURE_CLIENT_SECRET` | Service principal secret | Yes |

**Usage**:
```sql
CREATE SECRET azure_env (TYPE azure, PROVIDER credential_chain, CHAIN 'env');
ATTACH '...' AS db (TYPE mssql, AZURE_SECRET 'azure_env');
```

### RQ4: Token Expiration Handling Strategy

**Decision**: Different strategies for refreshable vs manual tokens

**Rationale**:
- Manual tokens: Cannot refresh (no credentials), must fail with clear message
- Env-based tokens: Can refresh using stored credentials
- 5-minute margin before expiration prevents mid-query failures

**Implementation**:
```cpp
// In FedAuthStrategy
bool IsTokenExpired() const override {
    if (!cached_claims_.valid) return true;
    auto now = std::chrono::system_clock::now();
    auto exp_time = std::chrono::system_clock::from_time_t(cached_claims_.exp);
    auto margin = std::chrono::minutes(5);
    return now >= (exp_time - margin);
}

// Token refresh decision
if (IsTokenExpired()) {
    if (can_refresh_) {
        // Re-acquire using stored credentials
        RefreshToken();
    } else {
        // Manual token - fail with clear message
        throw InvalidInputException(
            "Access token expired at %s. Please provide a new token.",
            FormatTimestamp(cached_claims_.exp)
        );
    }
}
```

### RQ5: Error Message Design

**Decision**: Actionable error messages with specific guidance

**Rationale**:
- SC-004 requires 100% actionable errors
- Users need to know exactly what went wrong and how to fix it
- Include timestamps for expiration errors

**Error Messages**:
```
# Malformed token
"Invalid access token format: unable to parse JWT. Ensure token is a valid Azure AD access token."

# Expired token
"Access token expired at 2026-02-06 14:30:00 UTC. Please provide a new token."

# Wrong audience
"Access token audience 'https://graph.microsoft.com' does not match expected 'https://database.windows.net/'. Ensure token was requested for the correct resource."

# Missing env var
"Environment variable AZURE_CLIENT_ID not set. Required for credential_chain with 'env' provider."

# Partial env vars
"Environment variables AZURE_TENANT_ID and AZURE_CLIENT_ID are set but AZURE_CLIENT_SECRET is missing."
```

### RQ6: Integration with Existing TokenCache

**Decision**: Extend TokenCache to store JwtClaims alongside raw token

**Rationale**:
- Avoids re-parsing JWT on every use
- Enables expiration checking without HTTP calls
- Thread-safe singleton pattern already in place

**Changes to TokenCache**:
```cpp
struct CachedToken {
    std::string access_token;
    JwtClaims claims;
    bool can_refresh;
    std::chrono::system_clock::time_point cached_at;
};

// New method
CachedToken GetTokenWithClaims(const std::string &key);
```

## Architecture Decisions

### AD1: Manual Token Authentication Strategy

**New class: ManualTokenAuthStrategy**

Separate from existing FedAuthStrategy to clearly distinguish:
- No token acquirer function needed
- Cannot refresh
- Stores pre-provided token directly

```cpp
class ManualTokenAuthStrategy : public AuthenticationStrategy {
    std::string raw_token_;
    JwtClaims claims_;

public:
    ManualTokenAuthStrategy(const std::string &token);
    bool RequiresFedAuth() const override { return true; }
    std::vector<uint8_t> GetFedAuthToken(const FedAuthInfo &info) override;
    bool IsTokenExpired() const override;
    // InvalidateToken() does nothing - can't refresh
};
```

### AD2: AuthStrategyFactory Extension

```cpp
AuthStrategyPtr AuthStrategyFactory::Create(const MSSQLConnectionInfo &conn_info, ClientContext *context) {
    if (!conn_info.access_token.empty()) {
        // P1: Manual token authentication
        return CreateManualToken(conn_info.access_token);
    } else if (conn_info.use_azure_auth) {
        // Existing: Azure secret-based auth
        return CreateFedAuth(*context, conn_info.azure_secret_name, ...);
    } else {
        // Existing: SQL Server auth
        return CreateSqlAuth(conn_info.user, conn_info.password, ...);
    }
}
```

### AD3: Environment Provider Implementation

Add to `azure_token.cpp`:

```cpp
TokenResult AcquireTokenFromEnv(const AzureSecretInfo &info) {
    // Read environment variables
    const char *tenant = std::getenv("AZURE_TENANT_ID");
    const char *client_id = std::getenv("AZURE_CLIENT_ID");
    const char *client_secret = std::getenv("AZURE_CLIENT_SECRET");

    // Validate all required vars present
    if (!tenant || !client_id || !client_secret) {
        return TokenResult::Failure(BuildMissingEnvError(tenant, client_id, client_secret));
    }

    // Use existing service principal flow
    AzureSecretInfo env_info;
    env_info.tenant_id = tenant;
    env_info.client_id = client_id;
    env_info.client_secret = client_secret;

    return AcquireTokenForServicePrincipal(env_info);
}
```

## File Changes Summary

| File | Change Type | Description |
|------|-------------|-------------|
| `src/azure/jwt_parser.cpp` | NEW | Base64url decode + JSON claim extraction |
| `src/include/azure/jwt_parser.hpp` | NEW | JwtClaims struct and ParseJwtClaims() |
| `src/azure/azure_token.cpp` | MODIFY | Add AcquireTokenFromEnv(), integrate JWT parsing |
| `src/azure/azure_secret_reader.cpp` | MODIFY | Read ACCESS_TOKEN from MSSQL secrets |
| `src/mssql_secret.cpp` | MODIFY | Add ACCESS_TOKEN option validation |
| `src/mssql_storage.cpp` | MODIFY | Parse ACCESS_TOKEN ATTACH option |
| `src/tds/auth/manual_token_strategy.cpp` | NEW | ManualTokenAuthStrategy implementation |
| `src/include/tds/auth/manual_token_strategy.hpp` | NEW | ManualTokenAuthStrategy header |
| `src/tds/auth/auth_strategy_factory.cpp` | MODIFY | Route to ManualTokenAuthStrategy |
| `test/cpp/test_jwt_parser.cpp` | NEW | JWT parsing unit tests |
| `test/sql/azure/azure_access_token.test` | NEW | Manual token integration tests |
| `test/sql/azure/azure_env_provider.test` | NEW | Env provider integration tests |

## Dependencies

- No new external dependencies
- Uses existing: libcurl, OpenSSL, yyjson (via DuckDB)
- C++11 compatible (no std::optional, no structured bindings)

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| JWT format variations | Low | Medium | Test with multiple token sources (CLI, SP, Fabric) |
| Base64url edge cases | Low | Low | Comprehensive unit tests with known tokens |
| Token refresh race conditions | Medium | Low | Mutex already in TokenCache |
| Cross-platform env var reading | Low | Low | std::getenv is portable |

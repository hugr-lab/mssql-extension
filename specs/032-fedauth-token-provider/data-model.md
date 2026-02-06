# Data Model: FEDAUTH Token Provider Enhancements

**Feature**: 032-fedauth-token-provider
**Date**: 2026-02-06

## Entities

### JwtClaims

Represents parsed claims from an Azure AD JWT access token.

| Field | Type | Description | Source |
|-------|------|-------------|--------|
| `exp` | `int64_t` | Expiration timestamp (Unix seconds) | JWT `exp` claim |
| `aud` | `std::string` | Audience (resource URL) | JWT `aud` claim |
| `oid` | `std::string` | Object ID (user/service principal) | JWT `oid` claim |
| `tid` | `std::string` | Tenant ID | JWT `tid` claim |
| `valid` | `bool` | Parse success flag | Parser result |
| `error` | `std::string` | Parse error message (if invalid) | Parser result |

**Validation Rules**:
- `exp` must be positive integer
- `aud` must be non-empty string
- Token must be valid base64url encoded JWT format

**State Transitions**: N/A (immutable after parsing)

### CachedToken

Extended token cache entry with parsed claims.

| Field | Type | Description |
|-------|------|-------------|
| `access_token` | `std::string` | Raw JWT string |
| `claims` | `JwtClaims` | Parsed JWT claims |
| `can_refresh` | `bool` | Whether token can be refreshed |
| `cached_at` | `time_point` | When token was cached |

**Validation Rules**:
- `access_token` must be non-empty
- `claims.valid` should be true for usable tokens

### MSSQLConnectionInfo (Extended)

Extended connection info to support ACCESS_TOKEN option.

| Field | Type | Description | New? |
|-------|------|-------------|------|
| `host` | `std::string` | Server hostname | Existing |
| `port` | `int` | Server port | Existing |
| `database` | `std::string` | Database name | Existing |
| `user` | `std::string` | SQL auth username | Existing |
| `password` | `std::string` | SQL auth password | Existing |
| `use_azure_auth` | `bool` | Use Azure AD auth | Existing |
| `azure_secret_name` | `std::string` | Azure secret name | Existing |
| `access_token` | `std::string` | Direct access token | **NEW** |

**Validation Rules**:
- If `access_token` is set, `use_azure_auth` and SQL auth are ignored
- `access_token` must be valid JWT format if provided

### ManualTokenAuthStrategy

New authentication strategy for pre-provided tokens.

| Field | Type | Description |
|-------|------|-------------|
| `raw_token_` | `std::string` | UTF-8 access token |
| `token_utf16le_` | `std::vector<uint8_t>` | Pre-encoded UTF-16LE |
| `claims_` | `JwtClaims` | Parsed claims |

**State Transitions**:
```
Created → Valid (if JWT parses successfully)
Created → Invalid (if JWT parse fails)
Valid → Expired (when current time >= exp - 5 minutes)
```

## Relationships

```
┌─────────────────────┐
│ MSSQLConnectionInfo │
└─────────────────────┘
          │
          │ access_token set?
          ▼
┌─────────────────────────┐     ┌─────────────────────┐
│ ManualTokenAuthStrategy │     │ FedAuthStrategy     │
│ (can_refresh = false)   │     │ (can_refresh = true)│
└─────────────────────────┘     └─────────────────────┘
          │                              │
          │ parses                       │ uses
          ▼                              ▼
┌─────────────────────┐         ┌─────────────────────┐
│ JwtClaims           │         │ TokenCache          │
│ (exp, aud, oid)     │         │ (CachedToken)       │
└─────────────────────┘         └─────────────────────┘
```

## Secret Configuration

### MSSQL Secret with ACCESS_TOKEN

```sql
CREATE SECRET my_token (
    TYPE mssql,
    HOST 'myserver.database.windows.net',
    DATABASE 'mydb',
    ACCESS_TOKEN 'eyJ0eXAiOiJKV1QiLCJhbGciOiJSUzI1NiIsIng1dCI6Ik1uQ19...'
);
```

| Option | Type | Required | Description |
|--------|------|----------|-------------|
| `HOST` | VARCHAR | Yes | Server hostname |
| `DATABASE` | VARCHAR | Yes | Database name |
| `ACCESS_TOKEN` | VARCHAR | Yes | Azure AD JWT token |

### Azure Secret with env Chain

```sql
CREATE SECRET azure_env (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'env'
);
```

Reads from environment:
- `AZURE_TENANT_ID`
- `AZURE_CLIENT_ID`
- `AZURE_CLIENT_SECRET`

## ATTACH Options

| Option | Type | Mutual Exclusion | Description |
|--------|------|------------------|-------------|
| `SECRET` | VARCHAR | With ACCESS_TOKEN | MSSQL secret name |
| `AZURE_SECRET` | VARCHAR | With ACCESS_TOKEN | Azure secret name |
| `ACCESS_TOKEN` | VARCHAR | With SECRET, AZURE_SECRET | Direct JWT token |

**Priority**: `ACCESS_TOKEN` > `SECRET` > Connection string auth

## Error Codes

| Code | Message Template | Trigger |
|------|------------------|---------|
| `INVALID_JWT` | "Invalid access token format: unable to parse JWT" | Malformed token |
| `EXPIRED_TOKEN` | "Access token expired at {timestamp}. Please provide a new token." | exp claim in past |
| `INVALID_AUDIENCE` | "Access token audience '{aud}' does not match expected 'https://database.windows.net/'" | Wrong aud claim |
| `MISSING_ENV_VAR` | "Environment variable {name} not set" | Missing env var |
| `PARTIAL_ENV_VARS` | "Environment variables {set} are set but {missing} is missing" | Incomplete env vars |

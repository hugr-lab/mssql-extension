# Data Model: Azure Token Infrastructure (Phase 1)

**Feature**: 001-azure-token-infrastructure
**Created**: 2026-02-05
**Status**: Complete

## Entities

### AzureSecretInfo

Represents parsed information from a DuckDB Azure secret.

| Field           | Type   | Required    | Description                                |
| --------------- | ------ | ----------- | ------------------------------------------ |
| `secret_name`   | string | Yes         | Name of the Azure secret                   |
| `provider`      | string | Yes         | Authentication provider type               |
| `tenant_id`     | string | Conditional | Azure AD tenant ID (service_principal)     |
| `client_id`     | string | Conditional | Application/client ID                      |
| `client_secret` | string | Conditional | Client secret (service_principal)          |
| `chain`         | string | Optional    | Credential chain order (credential_chain)  |

**Provider Values**:

- `service_principal` - Service principal with client credentials
- `credential_chain` - Chained credentials (cli, env, managed_identity, interactive)
- `managed_identity` - Azure managed identity

**Chain Values** (for credential_chain provider):

- `cli` - Azure CLI credentials (`az login`)
- `env` - Environment variables (AZURE_TENANT_ID, AZURE_CLIENT_ID, AZURE_CLIENT_SECRET)
- `managed_identity` - Azure managed identity (IMDS endpoint)
- `interactive` - Browser-based OAuth2 with PKCE (supports MFA)

**Validation Rules**:

- For `service_principal`: `tenant_id`, `client_id`, `client_secret` MUST be non-empty
- For `credential_chain`: `chain` MAY be empty (defaults to `default`)
- For `managed_identity`: `client_id` MAY be provided (for user-assigned)

---

### TokenResult

Represents the result of a token acquisition attempt.

| Field           | Type      | Required    | Description                               |
| --------------- | --------- | ----------- | ----------------------------------------- |
| `success`       | bool      | Yes         | Whether token was acquired successfully   |
| `access_token`  | string    | Conditional | JWT access token (only if success)        |
| `error_message` | string    | Conditional | Error description (only if !success)      |
| `expires_at`    | timestamp | Conditional | Token expiration time (only if success)   |

**State Transitions**:

```text
┌─────────────────┐
│  Acquire Token  │
└────────┬────────┘
         │
         ▼
    ┌─────────┐
    │ Success │──────────► success=true, access_token set
    │    ?    │
    └────┬────┘
         │ No
         ▼
    ┌─────────┐
    │ Failure │──────────► success=false, error_message set
    └─────────┘
```

---

### CachedToken

Represents a cached token with expiration tracking.

| Field          | Type       | Required | Description              |
| -------------- | ---------- | -------- | ------------------------ |
| `access_token` | string     | Yes      | JWT access token         |
| `expires_at`   | time_point | Yes      | Absolute expiration time |

**Validation Rules**:
- Token is valid if `now < (expires_at - 5 minutes)`
- 5-minute margin ensures refresh before actual expiration

**Cache Key**: Azure secret name (string)

---

### DeviceCodeResponse

Represents the response from Azure AD device code endpoint.

| Field              | Type   | Required | Description                                       |
| ------------------ | ------ | -------- | ------------------------------------------------- |
| `device_code`      | string | Yes      | Long code for token polling                       |
| `user_code`        | string | Yes      | Short code displayed to user (e.g., "ABC123")     |
| `verification_uri` | string | Yes      | URL user visits (https://microsoft.com/devicelogin) |
| `message`          | string | Yes      | Human-readable instructions for user              |
| `expires_in`       | int    | Yes      | Seconds until device code expires (default: 900)  |
| `interval`         | int    | Yes      | Seconds to wait between polling requests          |

**Lifecycle**:

1. Created when device code is requested
2. User code displayed to user
3. Polling starts with device_code
4. Destroyed after token received or timeout

---

### DeviceCodePollingState

Represents the state of device code polling.

| Field              | Type   | Required | Description                             |
| ------------------ | ------ | -------- | --------------------------------------- |
| `pending`          | bool   | Yes      | User hasn't completed login yet         |
| `success`          | bool   | Yes      | Token was successfully acquired         |
| `error`            | string | No       | Error code if failed                    |
| `error_description`| string | No       | Error description if failed             |

**Polling Error Codes**:

- `authorization_pending` - Continue polling
- `authorization_declined` - User denied, stop polling
- `expired_token` - Timeout exceeded, stop polling
- `bad_verification_code` - Invalid device code, stop polling

---

### Extended MSSQLSecretFields

Extends existing MSSQL secret with Azure authentication reference.

| Field              | Type    | Required    | Description                                     |
| ------------------ | ------- | ----------- | ----------------------------------------------- |
| `host`             | VARCHAR | Yes         | SQL Server hostname                             |
| `port`             | INTEGER | Yes         | TCP port (default 1433)                         |
| `database`         | VARCHAR | Yes         | Database name                                   |
| `user`             | VARCHAR | Conditional | Required if no `azure_secret`                   |
| `password`         | VARCHAR | Conditional | Required if no `azure_secret`                   |
| `use_encrypt`      | BOOLEAN | No          | TLS encryption (default: true)                  |
| `catalog`          | BOOLEAN | No          | Enable catalog (default: true)                  |
| **`azure_secret`** | VARCHAR | No          | Name of Azure secret for Entra ID auth          |

**Validation Rules**:
- Either (`user` AND `password`) OR `azure_secret` MUST be provided
- `host`, `port`, `database` always required
- When `azure_secret` present, referenced Azure secret MUST exist and be TYPE azure

---

## Namespace

All Azure authentication code uses `duckdb::mssql::azure` namespace:

```cpp
namespace duckdb {
namespace mssql {
namespace azure {

// No prefix needed within namespace (per CLAUDE.md conventions)
struct AzureSecretInfo { ... };
struct TokenResult { ... };
struct CachedToken { ... };
class TokenCache { ... };

TokenResult AcquireToken(ClientContext &context, const string &secret_name);

} // namespace azure
} // namespace mssql
} // namespace duckdb
```

---

## Relationships

```text
┌─────────────────────┐         ┌─────────────────────┐
│    MSSQL Secret     │────────►│    Azure Secret     │
│                     │ refs    │                     │
│  azure_secret: str  │         │  TYPE: azure        │
└─────────────────────┘         │  provider: str      │
                                │  tenant_id: str     │
                                │  client_id: str     │
                                │  client_secret: str │
                                │  chain: str         │
                                └─────────────────────┘
                                         │
                                         │ provides credentials for
                                         ▼
                                ┌─────────────────────┐
                                │    Token Cache      │
                                │  (duckdb::mssql::   │
                                │   azure namespace)  │
                                │  key: secret_name   │
                                │  value: CachedToken │
                                └─────────────────────┘
```

---

## Constants

### Secret Field Names

```cpp
// Existing
constexpr const char *MSSQL_SECRET_HOST = "host";
constexpr const char *MSSQL_SECRET_PORT = "port";
constexpr const char *MSSQL_SECRET_DATABASE = "database";
constexpr const char *MSSQL_SECRET_USER = "user";
constexpr const char *MSSQL_SECRET_PASSWORD = "password";
constexpr const char *MSSQL_SECRET_USE_ENCRYPT = "use_encrypt";
constexpr const char *MSSQL_SECRET_CATALOG = "catalog";

// NEW
constexpr const char *MSSQL_SECRET_AZURE_SECRET = "azure_secret";
```

### Azure AD Constants

```cpp
// Token endpoint base URL
constexpr const char *AZURE_AD_BASE_URL = "login.microsoftonline.com";

// Resource scope for Azure SQL Database
constexpr const char *AZURE_SQL_SCOPE = "https://database.windows.net/.default";

// Token refresh margin (seconds before expiration)
constexpr int64_t TOKEN_REFRESH_MARGIN_SECONDS = 300;  // 5 minutes

// Default token lifetime if not specified (seconds)
constexpr int64_t DEFAULT_TOKEN_LIFETIME_SECONDS = 3600;  // 1 hour

// Device code flow constants
constexpr int64_t DEVICE_CODE_DEFAULT_TIMEOUT_SECONDS = 900;  // 15 minutes (Azure AD default)
constexpr int64_t DEVICE_CODE_DEFAULT_INTERVAL_SECONDS = 5;   // Default polling interval

// Device code grant type
constexpr const char *DEVICE_CODE_GRANT_TYPE = "urn:ietf:params:oauth:grant-type:device_code";

// Default client ID for interactive auth (Azure Data Studio public client)
constexpr const char *AZURE_INTERACTIVE_CLIENT_ID = "e32693cc-fce3-49e4-8b0f-d8a66c4fb1a9";

// Default tenant for interactive auth
constexpr const char *AZURE_DEFAULT_TENANT = "common";
```

---

## Error Messages

| Scenario                       | Error Message                                                        |
| ------------------------------ | -------------------------------------------------------------------- |
| Azure secret not found         | `"Azure secret 'name' not found"`                                    |
| Secret is wrong type           | `"Secret 'name' is not an Azure secret (type: actual_type)"`         |
| Neither auth method            | `"Either user/password or azure_secret required"`                    |
| Azure extension missing        | `"Azure extension required. Run: INSTALL azure; LOAD azure;"`        |
| Service principal missing      | `"Service principal requires tenant_id, client_id, client_secret"`   |
| Token acquisition failed       | `"Azure AD error AADSTS{code}: {description}"`                       |
| CLI not logged in              | `"Azure CLI credentials expired. Run 'az login' to refresh."`        |
| Empty secret name              | `"Secret name required"`                                             |
| HTTP connection failed         | `"Failed to connect to Azure AD: {details}"`                         |
| Timeout                        | `"Token acquisition timed out after {seconds} seconds"`              |
| Device code expired            | `"Device code expired. Please try again."`                           |
| Authorization declined         | `"Authorization was declined by user"`                               |
| Bad verification code          | `"Invalid device code. Please try again."`                           |
| Polling error                  | `"Error during authentication: {error_description}"`                 |

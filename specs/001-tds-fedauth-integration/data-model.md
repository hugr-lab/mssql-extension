# Data Model: TDS FEDAUTH Integration (Phase 2)

**Created**: 2026-02-05
**Status**: Complete
**Purpose**: Define data structures and interfaces for TDS FEDAUTH integration

---

## Key Entities

### FedAuthData

Holds FEDAUTH extension data for LOGIN7 packet.

```cpp
namespace duckdb {
namespace mssql {
namespace azure {

//! FedAuth library identifier
enum class FedAuthLibrary : uint8_t {
    SSPI = 0x01,          // Windows integrated (not supported)
    MSAL = 0x02,          // Azure AD via MSAL/ADAL (our target)
    SECURITY_TOKEN = 0x03 // Pre-acquired token (not used)
};

//! FEDAUTH feature extension data for LOGIN7 packet
struct FedAuthData {
    FedAuthLibrary library = FedAuthLibrary::MSAL;
    std::vector<uint8_t> token_utf16le;  // UTF-16LE encoded access token

    //! Returns total size of FEDAUTH extension data (4 + token bytes)
    size_t GetDataSize() const {
        return 4 + token_utf16le.size();  // 4 bytes options + token
    }

    //! Returns true if this is a valid FEDAUTH extension
    bool IsValid() const {
        return !token_utf16le.empty();
    }
};

} // namespace azure
} // namespace mssql
} // namespace duckdb
```

### ConnectionParams (Extended)

Extended `ConnectionParams` structure with Azure authentication fields.

```cpp
namespace duckdb {
namespace tds {

struct ConnectionParams {
    // Existing fields
    std::string host;
    int port = 1433;
    std::string database;
    std::string username;       // Optional when using Azure auth
    std::string password;       // Optional when using Azure auth
    bool use_ssl = true;
    std::string application_name = "DuckDB MSSQL Extension";

    // Azure authentication (NEW)
    bool use_azure_auth = false;
    std::string azure_secret_name;

    //! Returns true if connecting to Azure endpoint
    bool IsAzureEndpoint() const;

    //! Returns true if connecting to Fabric endpoint
    bool IsFabricEndpoint() const;

    //! Returns endpoint type for TLS and feature handling
    EndpointType GetEndpointType() const;
};

} // namespace tds
} // namespace duckdb
```

### EndpointType

Classification of target endpoint for TLS and feature handling.

```cpp
namespace duckdb {
namespace mssql {

//! Endpoint type for connection behavior
enum class EndpointType {
    OnPremises,    // Traditional SQL Server (self-signed certs OK)
    AzureSQL,      // Azure SQL Database (hostname verification required)
    Fabric,        // Microsoft Fabric Warehouse (limited features)
    Synapse        // Azure Synapse Analytics (similar to AzureSQL)
};

//! Determine endpoint type from hostname
EndpointType GetEndpointType(const std::string &host);

//! Check if endpoint requires hostname verification
bool RequiresHostnameVerification(EndpointType type);

//! Check if endpoint is Azure-based (any cloud endpoint)
bool IsAzureEndpoint(const std::string &host);

//! Check if endpoint is Microsoft Fabric
bool IsFabricEndpoint(const std::string &host);

} // namespace mssql
} // namespace duckdb
```

---

## Protocol Structures

### PreloginOptions (Extended)

Extended PRELOGIN options with FEDAUTHREQUIRED.

```cpp
namespace duckdb {
namespace tds {

//! PRELOGIN option identifiers
enum class PreloginOptionId : uint8_t {
    VERSION = 0x00,
    ENCRYPTION = 0x01,
    INSTOPT = 0x02,
    THREADID = 0x03,
    MARS = 0x04,
    FEDAUTHREQUIRED = 0x06,  // NEW
    TERMINATOR = 0xFF
};

//! PRELOGIN options builder
struct PreloginOptions {
    uint32_t version = TDS_VERSION_7_4;
    uint8_t encryption = ENCRYPT_ON;
    uint32_t thread_id = 0;
    bool mars = false;
    bool fedauth_required = false;  // NEW: Set to true for Azure auth

    //! Build PRELOGIN packet data
    std::vector<uint8_t> Build() const;
};

} // namespace tds
} // namespace duckdb
```

### Login7FeatureExt

Feature extension structure for LOGIN7 packet.

```cpp
namespace duckdb {
namespace tds {

//! Feature extension IDs
enum class FeatureExtId : uint8_t {
    SESSION_RECOVERY = 0x01,
    FEDAUTH = 0x02,          // Azure AD authentication
    COLUMN_ENCRYPTION = 0x04,
    GLOBAL_TRANSACTIONS = 0x05,
    UTF8_SUPPORT = 0x0A,
    TERMINATOR = 0xFF
};

//! Feature extension for LOGIN7 packet
struct FeatureExtension {
    FeatureExtId feature_id;
    std::vector<uint8_t> data;

    //! Create FEDAUTH feature extension
    static FeatureExtension CreateFedAuth(const mssql::azure::FedAuthData &fedauth);
};

} // namespace tds
} // namespace duckdb
```

---

## Token Response Structures

### FedAuthInfoToken

Structure for FEDAUTHINFO (0xEE) server response.

```cpp
namespace duckdb {
namespace tds {

//! FEDAUTHINFO info IDs
enum class FedAuthInfoId : uint32_t {
    SPN = 1,       // Service Principal Name
    STSURL = 2     // Security Token Service URL
};

//! FEDAUTHINFO entry
struct FedAuthInfoEntry {
    FedAuthInfoId info_id;
    std::string data;
};

//! FEDAUTHINFO token response
struct FedAuthInfoToken {
    std::vector<FedAuthInfoEntry> entries;

    //! Get SPN if present
    std::optional<std::string> GetSPN() const;

    //! Get STS URL if present
    std::optional<std::string> GetSTSURL() const;
};

} // namespace tds
} // namespace duckdb
```

---

## Interface Contracts

### IAzureTokenProvider

Interface for Azure token acquisition (implemented in Phase 1).

```cpp
namespace duckdb {
namespace mssql {
namespace azure {

//! Token acquisition result
struct TokenResult {
    bool success = false;
    std::string access_token;
    int64_t expires_in_seconds = 0;
    std::string error_code;
    std::string error_message;
};

//! Azure token acquisition interface
class IAzureTokenProvider {
public:
    virtual ~IAzureTokenProvider() = default;

    //! Acquire token for the given Azure secret
    virtual TokenResult AcquireToken(ClientContext &context,
                                     const std::string &azure_secret_name) = 0;

    //! Check if token is valid (not expired)
    virtual bool IsTokenValid(const std::string &azure_secret_name) const = 0;

    //! Invalidate cached token
    virtual void InvalidateToken(const std::string &azure_secret_name) = 0;
};

} // namespace azure
} // namespace mssql
} // namespace duckdb
```

### IFedAuthEncoder

Interface for FEDAUTH packet encoding.

```cpp
namespace duckdb {
namespace mssql {
namespace azure {

//! FEDAUTH encoding interface
class IFedAuthEncoder {
public:
    virtual ~IFedAuthEncoder() = default;

    //! Encode access token to UTF-16LE for TDS
    virtual std::vector<uint8_t> EncodeToken(const std::string &token_utf8) = 0;

    //! Build complete FEDAUTH extension data
    virtual FedAuthData BuildFedAuthExtension(ClientContext &context,
                                               const std::string &azure_secret_name) = 0;
};

} // namespace azure
} // namespace mssql
} // namespace duckdb
```

---

## State Diagrams

### Authentication Flow State

```text
                    ┌─────────────────┐
                    │     START       │
                    └────────┬────────┘
                             │
                             ▼
                    ┌─────────────────┐
                    │ Check MSSQL     │
                    │ Secret for      │
                    │ azure_secret    │
                    └────────┬────────┘
                             │
            ┌────────────────┴────────────────┐
            │                                 │
            ▼                                 ▼
   ┌─────────────────┐               ┌─────────────────┐
   │ AZURE_AUTH      │               │ SQL_AUTH        │
   │ azure_secret    │               │ No azure_secret │
   │ present         │               │                 │
   └────────┬────────┘               └────────┬────────┘
            │                                 │
            ▼                                 │
   ┌─────────────────┐                        │
   │ ACQUIRING_TOKEN │                        │
   └────────┬────────┘                        │
            │                                 │
      ┌─────┴─────┐                           │
      │           │                           │
      ▼           ▼                           │
   ┌──────┐  ┌──────────┐                     │
   │ OK   │  │ FAILED   │                     │
   └──┬───┘  └────┬─────┘                     │
      │           │                           │
      │           ▼                           │
      │   ┌─────────────────┐                 │
      │   │ AUTH_ERROR      │                 │
      │   │ (with Azure AD  │                 │
      │   │  error code)    │                 │
      │   └─────────────────┘                 │
      │                                       │
      ▼                                       ▼
┌─────────────────┐               ┌─────────────────┐
│ BUILD_LOGIN7    │               │ BUILD_LOGIN7    │
│ with FEDAUTH    │               │ with user/pass  │
│ extension       │               │                 │
└────────┬────────┘               └────────┬────────┘
         │                                 │
         └────────────────┬────────────────┘
                          │
                          ▼
                 ┌─────────────────┐
                 │ SEND_LOGIN7     │
                 └────────┬────────┘
                          │
                          ▼
                 ┌─────────────────┐
                 │ WAIT_LOGINACK   │
                 └────────┬────────┘
                          │
                    ┌─────┴─────┐
                    │           │
                    ▼           ▼
               ┌────────┐  ┌────────────┐
               │ DONE   │  │ LOGIN_FAIL │
               └────────┘  └────────────┘
```

### Token Lifecycle State

```text
                    ┌─────────────────┐
                    │  NO_TOKEN       │
                    └────────┬────────┘
                             │ AcquireToken()
                             ▼
                    ┌─────────────────┐
                    │  VALID          │
                    │  (expires_at    │
                    │   in future)    │
                    └────────┬────────┘
                             │
            ┌────────────────┼────────────────┐
            │                │                │
            │ Connection     │ 5 min before   │ Token used
            │ returned to    │ expiration     │ for LOGIN7
            │ pool           │                │
            ▼                ▼                ▼
   ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
   │ Check on        │ │ EXPIRING_SOON   │ │ VALID           │
   │ next acquire    │ │                 │ │                 │
   └────────┬────────┘ └────────┬────────┘ └─────────────────┘
            │                   │
            │                   │ Background refresh
            │                   ▼
            │          ┌─────────────────┐
            │          │ REFRESHING      │
            │          └────────┬────────┘
            │                   │
            │          ┌───────┴───────┐
            │          │               │
            │          ▼               ▼
            │    ┌──────────┐   ┌───────────┐
            │    │ VALID    │   │ EXPIRED   │
            │    │ (new     │   │ (refresh  │
            │    │  token)  │   │  failed)  │
            │    └──────────┘   └─────┬─────┘
            │                         │
            └─────────────────────────┘
                                      │ Force acquire
                                      ▼
                             ┌─────────────────┐
                             │ NO_TOKEN        │
                             │ (start over)    │
                             └─────────────────┘
```

---

## Error Types

### AzureAuthError

Error structure for Azure authentication failures.

```cpp
namespace duckdb {
namespace mssql {
namespace azure {

//! Azure AD error categories
enum class AzureErrorCategory {
    InvalidCredentials,    // Wrong client_secret, etc.
    NotFound,              // Tenant/app not found
    PermissionDenied,      // No access to resource
    NetworkError,          // Cannot reach Azure AD
    TokenExpired,          // Token expired during operation
    Unknown                // Unexpected error
};

//! Azure authentication error
struct AzureAuthError {
    AzureErrorCategory category;
    std::string aad_error_code;      // e.g., "AADSTS7000215"
    std::string aad_error_message;   // Azure AD error description
    std::string user_message;        // Human-readable suggestion

    //! Create error from Azure AD response
    static AzureAuthError FromAADResponse(const std::string &error_code,
                                          const std::string &error_description);

    //! Get user-friendly error message
    std::string GetUserMessage() const;
};

} // namespace azure
} // namespace mssql
} // namespace duckdb
```

---

## Configuration

### AzureAuthConfig

Configuration for Azure authentication behavior.

```cpp
namespace duckdb {
namespace mssql {

//! Azure authentication configuration
struct AzureAuthConfig {
    //! Token refresh margin (seconds before expiration to refresh)
    int64_t token_refresh_margin_seconds = 300;  // 5 minutes

    //! Maximum retries for token acquisition
    int max_token_retries = 3;

    //! Token acquisition timeout (seconds)
    int token_timeout_seconds = 30;

    //! Whether to enable hostname verification for Azure endpoints
    bool enable_hostname_verification = true;
};

} // namespace mssql
} // namespace duckdb
```

---

## Relationships

```text
┌─────────────────────────────────────────────────────────────────┐
│                       ConnectionProvider                         │
│   - Reads MSSQL secret                                          │
│   - Determines auth method (SQL vs Azure)                       │
│   - Creates ConnectionParams with azure_secret_name             │
└───────────────────────────┬─────────────────────────────────────┘
                            │ uses
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                       ConnectionParams                           │
│   - host, port, database                                        │
│   - use_azure_auth, azure_secret_name                           │
│   - GetEndpointType()                                           │
└───────────────────────────┬─────────────────────────────────────┘
                            │ passed to
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                       TdsConnection                              │
│   - Creates PRELOGIN (with FEDAUTHREQUIRED if Azure)            │
│   - Creates LOGIN7 (with FEDAUTH extension if Azure)            │
│   - Handles TLS with/without hostname verification              │
└───────────────────────────┬─────────────────────────────────────┘
                            │ uses
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                       FedAuthData                                │
│   - library (MSAL)                                              │
│   - token_utf16le (encoded token)                               │
│   - Built by FedAuthEncoder                                     │
└───────────────────────────┬─────────────────────────────────────┘
                            │ encoded by
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                       AzureTokenProvider                         │
│   - Acquires tokens via OAuth2 (Phase 1)                        │
│   - Caches tokens with expiration                               │
│   - Returns TokenResult                                         │
└─────────────────────────────────────────────────────────────────┘
```

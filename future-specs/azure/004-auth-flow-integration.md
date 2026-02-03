# Feature Specification: Auth Flow Integration

**Feature Branch**: `azure-004-auth-flow-integration`
**Created**: 2026-02-03
**Status**: Future / Planning
**Dependencies**: 001-azure-secret-reuse, 002-extend-mssql-secret

## Problem Statement

The MSSQL extension's TDS authentication flow currently only supports SQL Server authentication (user/password). This phase integrates Azure AD token-based authentication into the existing login flow using TDS FEDAUTH (Federated Authentication) protocol.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Connect with Azure Authentication (Priority: P1)

A user wants to connect to Azure SQL Database using Azure AD credentials.

**Acceptance Scenarios**:

1. **Given** MSSQL secret with `azure_secret` configured, **When** user executes `ATTACH`, **Then** connection authenticates via Azure AD token
2. **Given** valid Azure credentials, **When** connection is established, **Then** user can execute queries
3. **Given** Azure auth connection, **When** connection is returned to pool and reused, **Then** token is refreshed if needed

---

### User Story 2 - Backward Compatibility with SQL Auth (Priority: P1)

Existing SQL authentication must continue to work unchanged.

**Acceptance Scenarios**:

1. **Given** MSSQL secret with `user` and `password` (no `azure_secret`), **When** user executes `ATTACH`, **Then** SQL authentication is used (existing behavior)
2. **Given** on-premises SQL Server, **When** user connects with SQL auth, **Then** connection works as before
3. **Given** mixed environment with both Azure and SQL auth databases, **When** user attaches both, **Then** each uses appropriate auth method

---

### User Story 3 - Handle Authentication Failures (Priority: P1)

Users need clear error messages when Azure authentication fails at connection time.

**Acceptance Scenarios**:

1. **Given** expired Azure token, **When** connection attempted, **Then** token is refreshed automatically
2. **Given** invalid Azure credentials, **When** connection attempted, **Then** error includes Azure AD error code
3. **Given** Azure account without database access, **When** connection attempted, **Then** clear error: "Login failed: Azure AD principal does not have access to database"

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST detect `azure_secret` in MSSQL secret and use Azure auth flow
- **FR-002**: System MUST acquire token BEFORE sending LOGIN7 packet
- **FR-003**: System MUST implement TDS FEDAUTH extension in PRELOGIN packet
- **FR-004**: System MUST implement TDS FEDAUTH extension in LOGIN7 packet
- **FR-005**: System MUST encode access token as UTF-16LE for TDS protocol
- **FR-006**: System MUST handle FEDAUTHINFO server response
- **FR-007**: System MUST fall back to SQL auth when no `azure_secret` present
- **FR-008**: Changes MUST be localized to login/authentication path only

### TDS Protocol Changes

#### PRELOGIN Packet

Add FEDAUTHREQUIRED option when using Azure auth:

```
Option 0x06 (FEDAUTHREQUIRED): 1 byte
  Value: 0x01 (client requires federated auth)
```

#### LOGIN7 Packet

Add FEDAUTH feature extension:

```
Feature Extension (when azure_secret present):
├── FeatureId: 0x02 (FEDAUTH)
├── FeatureDataLen: 4 + token_length_bytes
├── Options: 4 bytes
│   └── FedAuthLibrary: 0x02 (ADAL/MSAL)
└── Token: UTF-16LE encoded access token
```

### Authentication Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                    Connection Request                            │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
                    ┌─────────────────┐
                    │ Check MSSQL     │
                    │ secret for      │
                    │ azure_secret    │
                    └─────────────────┘
                              │
              ┌───────────────┴───────────────┐
              │                               │
              ▼                               ▼
    ┌─────────────────┐             ┌─────────────────┐
    │ azure_secret    │             │ No azure_secret │
    │ PRESENT         │             │ (SQL Auth)      │
    └─────────────────┘             └─────────────────┘
              │                               │
              ▼                               │
    ┌─────────────────┐                       │
    │ Acquire Azure   │                       │
    │ AD Token        │                       │
    └─────────────────┘                       │
              │                               │
              ▼                               │
    ┌─────────────────┐                       │
    │ PRELOGIN with   │                       │
    │ FEDAUTHREQUIRED │                       │
    └─────────────────┘                       │
              │                               │
              ▼                               ▼
    ┌─────────────────┐             ┌─────────────────┐
    │ LOGIN7 with     │             │ LOGIN7 with     │
    │ FEDAUTH token   │             │ user/password   │
    └─────────────────┘             └─────────────────┘
              │                               │
              └───────────────┬───────────────┘
                              │
                              ▼
                    ┌─────────────────┐
                    │ Server validates│
                    │ & returns       │
                    │ LOGINACK        │
                    └─────────────────┘
```

---

## Success Criteria *(mandatory)*

- **SC-001**: Azure SQL Database connection succeeds with service principal
- **SC-002**: Azure SQL Database connection succeeds with CLI credentials
- **SC-003**: Microsoft Fabric Warehouse connection succeeds
- **SC-004**: SQL Server (on-prem) connection with SQL auth unchanged
- **SC-005**: Token refresh works for long-lived connections
- **SC-006**: Clear error messages for all auth failure scenarios

---

## Technical Context (For Planning Reference)

### Modified Files

```
src/
├── azure/
│   └── azure_fedauth.cpp     # FEDAUTH token encoding (namespace duckdb::mssql::azure)
├── tds/
│   ├── tds_protocol.cpp      # PRELOGIN & LOGIN7 FEDAUTH extensions (namespace duckdb::tds)
│   ├── tds_connection.cpp    # Auth flow branching (namespace duckdb::tds)
│   └── tds_token_parser.cpp  # FEDAUTHINFO response handling (namespace duckdb::tds)
├── connection/
│   └── mssql_connection_provider.cpp  # Read azure_secret from MSSQL secret
├── include/azure/
│   └── azure_fedauth.hpp     # FEDAUTH encoding interface
├── include/tds/
│   ├── tds_types.hpp         # FEDAUTH constants
│   └── tds_connection.hpp    # ConnectionParams update
```

### Connection Parameters Update

```cpp
// src/include/tds/tds_connection.hpp
namespace duckdb {
namespace tds {

struct ConnectionParams {
    std::string host;
    int port;
    std::string database;
    std::string username;       // Optional for Azure auth
    std::string password;       // Optional for Azure auth
    bool use_ssl;

    // NEW: Azure authentication
    bool use_azure_auth = false;
    std::string azure_secret_name;
};

} // namespace tds
} // namespace duckdb
```

### FEDAUTH Token Encoding

```cpp
// src/azure/azure_fedauth.cpp
namespace duckdb {
namespace mssql {
namespace azure {

//! Encode access token as UTF-16LE for TDS FEDAUTH packet
std::vector<uint8_t> EncodeFedAuthToken(const std::string &token_utf8) {
    // Convert UTF-8 token to UTF-16LE using existing encoding utilities
    std::vector<uint8_t> token_utf16le;
    encoding::Utf8ToUtf16LE(token_utf8, token_utf16le);
    return token_utf16le;
}

//! Build FEDAUTH feature extension data for LOGIN7 packet
FedAuthData BuildFedAuthExtension(ClientContext &context, const std::string &azure_secret_name) {
    // Acquire token from Azure secret
    auto token_result = TokenAcquisition::AcquireToken(context, azure_secret_name);
    if (!token_result.success) {
        throw ConnectionException("Azure AD authentication failed: %s", token_result.error_message);
    }

    FedAuthData data;
    data.library = FedAuthLibrary::MSAL;  // 0x02
    data.token_utf16le = EncodeFedAuthToken(token_result.access_token);
    return data;
}

} // namespace azure
} // namespace mssql
} // namespace duckdb
```

### Namespace Structure

| Component | Namespace | Notes |
|-----------|-----------|-------|
| Token acquisition | `duckdb::mssql::azure` | Azure-specific code |
| FEDAUTH encoding | `duckdb::mssql::azure` | Azure-specific code |
| TDS protocol | `duckdb::tds` | Existing TDS layer |
| Connection provider | `duckdb::mssql` | Existing MSSQL layer |

---

## Out of Scope

- Token caching improvements (basic caching from phase 1 is sufficient)
- Connection pool token management (use existing pool with fresh tokens)
- Catalog/data operation verification (handled in phase 6)

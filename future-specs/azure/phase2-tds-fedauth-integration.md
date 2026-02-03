# Feature Specification: TDS FEDAUTH Integration (Phase 2)

**Feature Branch**: `azure-phase2-tds-fedauth-integration`
**Created**: 2026-02-03
**Status**: Future / Planning
**Dependencies**: phase1-azure-token-infrastructure

## Problem Statement

Phase 1 established Azure token infrastructure. This phase integrates tokens into the TDS authentication flow using FEDAUTH protocol, enabling actual database connections. This phase delivers:

1. **TDS FEDAUTH Protocol** - PRELOGIN and LOGIN7 packet extensions for token-based auth
2. **Full Connectivity** - Azure SQL Database, Fabric Warehouse connections
3. **Validation Tests** - Unit and integration tests for all code paths
4. **Catalog/Data Verification** - Ensure all extension features work with Azure auth

**Deliverable**: Users can connect to Azure SQL and Fabric with Azure AD authentication.

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

### User Story 4 - Catalog Operations with Azure Auth (Priority: P1)

A user needs to browse schemas, tables, and columns on Azure SQL Database.

**Acceptance Scenarios**:

1. **Given** Azure-authenticated connection, **When** user queries `duckdb_schemas()`, **Then** all accessible schemas listed
2. **Given** Azure-authenticated connection, **When** user queries `duckdb_tables()`, **Then** all accessible tables listed
3. **Given** Azure SQL with limited permissions, **When** catalog queried, **Then** only accessible objects shown (no permission errors)

---

### User Story 5 - Data Operations with Azure Auth (Priority: P1)

A user needs to perform all standard data operations on Azure SQL.

**Acceptance Scenarios**:

1. **Given** Azure-authenticated connection, **When** user executes SELECT, **Then** results returned correctly
2. **Given** Azure-authenticated connection, **When** user executes INSERT with RETURNING, **Then** inserted values returned
3. **Given** Azure-authenticated connection, **When** user executes UPDATE/DELETE, **Then** rows modified correctly
4. **Given** Azure-authenticated connection, **When** user uses transactions, **Then** transaction semantics preserved

---

### User Story 6 - Microsoft Fabric Warehouse (Priority: P1)

A user needs to work with Microsoft Fabric SQL endpoints.

**Acceptance Scenarios**:

1. **Given** Fabric Warehouse connection, **When** user queries catalog, **Then** schemas/tables listed
2. **Given** Fabric Warehouse connection, **When** user executes SELECT, **Then** results returned correctly
3. **Given** Fabric Warehouse connection, **When** user attempts unsupported DDL, **Then** clear error message (not cryptic TDS error)

---

### User Story 7 - COPY/BCP with Azure Auth (Priority: P1)

A user needs to bulk load data into Azure SQL using COPY command.

**Acceptance Scenarios**:

1. **Given** Azure-authenticated connection, **When** user executes COPY TO, **Then** data transferred via BCP protocol
2. **Given** Azure-authenticated connection, **When** user creates temp table via COPY, **Then** temp table accessible
3. **Given** Fabric Warehouse connection, **When** user attempts COPY/BCP, **Then** operation succeeds or clear "not supported" error

---

## Requirements *(mandatory)*

### Functional Requirements

**TDS Protocol**:
- **FR-001**: System MUST detect `azure_secret` in MSSQL secret and use Azure auth flow
- **FR-002**: System MUST acquire token BEFORE sending LOGIN7 packet
- **FR-003**: PRELOGIN MUST always be sent (both SQL and Azure auth paths)
- **FR-004**: PRELOGIN MUST include FEDAUTHREQUIRED=0x01 option when using Azure auth
- **FR-005**: System MUST implement TDS FEDAUTH extension in LOGIN7 packet
- **FR-006**: System MUST encode access token as UTF-16LE for TDS protocol
- **FR-007**: System MUST handle FEDAUTHINFO server response
- **FR-008**: System MUST fall back to SQL auth when no `azure_secret` present

**TLS Requirements**:
- **FR-009**: TLS MUST be enabled for Azure endpoints (Azure SQL, Fabric)
- **FR-010**: For Azure endpoints, hostname verification SHOULD be performed using OpenSSL
- **FR-011**: For on-premises SQL Server, self-signed certificates MUST still be accepted (existing behavior)
- **FR-012**: Hostname verification uses OpenSSL APIs only (no additional dependencies)

**Data Operations**:
- **FR-008**: Catalog operations MUST work with Azure-authenticated connections
- **FR-009**: All DML operations (SELECT, INSERT, UPDATE, DELETE) MUST work
- **FR-010**: COPY/BCP operations MUST work with Azure auth
- **FR-011**: Transactions MUST work correctly with Azure auth
- **FR-012**: Statistics retrieval MUST work (with graceful degradation)
- **FR-013**: Microsoft Fabric Warehouse MUST be supported with documented limitations

**Testing**:
- **FR-014**: All existing integration tests MUST pass unchanged
- **FR-015**: New unit tests MUST cover FEDAUTH packet encoding
- **FR-016**: New integration tests MUST cover Azure SQL connection
- **FR-017**: Integration tests MUST be skippable when no Azure credentials available

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

```text
┌─────────────────────────────────────────────────────────────────┐
│                    Connection Request                           │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
                    ┌─────────────────┐
                    │ TCP Connect     │
                    └─────────────────┘
                              │
                              ▼
                    ┌─────────────────┐
                    │ PRELOGIN        │◄── Always sent (both auth paths)
                    │ (negotiate TLS) │    Azure: + FEDAUTHREQUIRED=0x01
                    └─────────────────┘
                              │
                              ▼
                    ┌─────────────────┐
                    │ TLS Handshake   │◄── Azure: verify hostname in cert
                    │ (if encryption) │    On-prem: accept self-signed
                    └─────────────────┘
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

### Azure SQL vs Fabric Comparison

| Feature | Azure SQL | Fabric Warehouse |
|---------|-----------|------------------|
| Catalog queries | Full support | May differ |
| SELECT | Full support | Full support |
| INSERT | Full support | Limited |
| UPDATE/DELETE | Full support | Limited |
| DDL | Full support | Limited |
| COPY/BCP | Full support | TBD |
| DBCC statistics | With permissions | Not available |
| Transactions | Full support | Limited |

### Test Strategy

**Existing tests remain unchanged** - all tests in `test/sql/` continue to work with SQL Server authentication. No modifications to existing test files.

**New Azure tests are separate** - all Azure-specific tests live in `test/sql/azure/` and are skipped when Azure credentials are not available.

### Test Categories

| Category | Location | Azure Required | Description |
|----------|----------|----------------|-------------|
| Unit - FEDAUTH Encoding | `test/cpp/` | No | Test token UTF-16LE encoding |
| Unit - Secret Validation | `test/cpp/` | No | Test azure_secret validation logic |
| Unit - Hostname Matching | `test/cpp/` | No | Test TLS hostname verification |
| **Existing SQL Auth** | `test/sql/*` | No* | **Unchanged** - all existing tests |
| Azure - Service Principal | `test/sql/azure/` | Yes | Connection with service principal |
| Azure - CLI Auth | `test/sql/azure/` | Yes | Connection with CLI credentials |
| Azure - Fabric | `test/sql/azure/` | Yes | Fabric Warehouse connection |
| Azure - Catalog | `test/sql/azure/` | Yes | Schema/table discovery |
| Azure - DML | `test/sql/azure/` | Yes | SELECT/INSERT/UPDATE/DELETE |
| Azure - COPY | `test/sql/azure/` | Yes | Bulk load operations |

*Requires SQL Server but not Azure

---

## Success Criteria *(mandatory)*

**Connectivity**:
- **SC-001**: Azure SQL Database connection succeeds with service principal
- **SC-002**: Azure SQL Database connection succeeds with CLI credentials
- **SC-003**: Microsoft Fabric Warehouse connection succeeds
- **SC-004**: SQL Server (on-prem) connection with SQL auth unchanged
- **SC-005**: Token refresh works for long-lived connections
- **SC-006**: Clear error messages for all auth failure scenarios

**Data Operations**:
- **SC-007**: Schema discovery works on Azure SQL and Fabric
- **SC-008**: Table/column metadata retrieval works
- **SC-009**: All DML operations work on Azure SQL
- **SC-010**: COPY/BCP operations work on Azure SQL
- **SC-011**: Fabric Warehouse basic operations documented and working
- **SC-012**: Fabric limitations clearly documented

**Testing**:
- **SC-013**: All existing tests pass (zero regressions)
- **SC-014**: New unit tests achieve >90% coverage of new code
- **SC-015**: Integration tests validate all supported Azure auth methods
- **SC-016**: Test suite gracefully skips Azure tests when credentials unavailable

---

## Technical Context (For Planning Reference)

### Namespace

- `duckdb::mssql::azure` - Token acquisition, FEDAUTH encoding
- `duckdb::tds` - TDS protocol extensions
- `duckdb::mssql` - Connection provider

### File Structure

```text
src/
├── azure/
│   ├── azure_fedauth.cpp         # FEDAUTH token encoding
│   └── azure_fedauth.hpp
├── tds/
│   ├── tds_protocol.cpp          # PRELOGIN & LOGIN7 FEDAUTH extensions
│   ├── tds_connection.cpp        # Auth flow branching
│   ├── tds_token_parser.cpp      # FEDAUTHINFO response handling
│   └── tls/
│       └── tls_context.cpp       # Add hostname verification for Azure
├── connection/
│   └── mssql_connection_provider.cpp  # Read azure_secret from MSSQL secret
├── catalog/
│   └── mssql_catalog.cpp         # Add Fabric detection/handling
├── include/
│   ├── azure/azure_fedauth.hpp
│   ├── tds/tds_types.hpp         # FEDAUTH constants
│   ├── tds/tds_connection.hpp    # ConnectionParams update
│   └── mssql_platform.hpp        # IsAzureEndpoint(), IsFabricEndpoint()
test/
├── cpp/                              # Unit tests (no Azure required)
│   ├── test_fedauth_encoding.cpp     # FEDAUTH packet encoding
│   ├── test_mssql_secret_azure.cpp   # azure_secret validation
│   └── test_hostname_verification.cpp # TLS hostname matching
└── sql/
    ├── attach/                       # UNCHANGED - existing SQL auth tests
    ├── catalog/                      # UNCHANGED - existing tests
    ├── dml/                          # UNCHANGED - existing tests
    ├── insert/                       # UNCHANGED - existing tests
    ├── integration/                  # UNCHANGED - existing tests
    ├── query/                        # UNCHANGED - existing tests
    ├── ...                           # All other existing test dirs unchanged
    └── azure/                        # NEW - Azure-specific tests (skipped if no creds)
        ├── azure_service_principal.test
        ├── azure_cli_auth.test
        ├── azure_error_handling.test
        ├── azure_catalog.test
        ├── azure_dml.test
        ├── azure_copy.test
        └── fabric_warehouse.test
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

    // Azure authentication
    bool use_azure_auth = false;
    std::string azure_secret_name;

    //! Returns true if connecting to Azure endpoint (for TLS hostname verification)
    bool IsAzureEndpoint() const {
        return mssql::IsAzureEndpoint(host);
    }
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
    std::vector<uint8_t> token_utf16le;
    encoding::Utf8ToUtf16LE(token_utf8, token_utf16le);
    return token_utf16le;
}

//! Build FEDAUTH feature extension data for LOGIN7 packet
FedAuthData BuildFedAuthExtension(ClientContext &context, const std::string &azure_secret_name) {
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

### Fabric Detection

```cpp
// src/include/mssql_platform.hpp
namespace duckdb {
namespace mssql {

bool IsFabricEndpoint(const std::string &host) {
    return host.find(".datawarehouse.fabric.microsoft.com") != std::string::npos ||
           host.find(".pbidedicated.windows.net") != std::string::npos;
}

bool IsAzureEndpoint(const std::string &host) {
    return host.find(".database.windows.net") != std::string::npos ||
           IsFabricEndpoint(host);
}

} // namespace mssql
} // namespace duckdb
```

### TLS Hostname Verification (OpenSSL Only)

For Azure endpoints, verify the server certificate hostname. Uses OpenSSL APIs only - no additional dependencies.

```cpp
// src/tds/tls/tls_context.cpp
namespace duckdb {
namespace tds {
namespace tls {

//! Configure hostname verification for Azure endpoints
void ConfigureHostnameVerification(SSL *ssl, const std::string &host, bool is_azure) {
    if (is_azure) {
        // Enable hostname verification for Azure endpoints
        // OpenSSL 1.1.0+ provides built-in hostname checking
        SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        SSL_set1_host(ssl, host.c_str());
        SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);
    } else {
        // On-premises: accept self-signed certificates (existing behavior)
        SSL_set_verify(ssl, SSL_VERIFY_NONE, nullptr);
    }
}

//! Manual hostname verification for older OpenSSL versions
bool VerifyHostname(X509 *cert, const std::string &host) {
    // Check Subject Alternative Names (SAN) first
    GENERAL_NAMES *san = static_cast<GENERAL_NAMES*>(
        X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));

    if (san) {
        for (int i = 0; i < sk_GENERAL_NAME_num(san); i++) {
            GENERAL_NAME *name = sk_GENERAL_NAME_value(san, i);
            if (name->type == GEN_DNS) {
                const char *dns_name = reinterpret_cast<const char*>(
                    ASN1_STRING_get0_data(name->d.dNSName));
                if (MatchHostname(host, dns_name)) {
                    GENERAL_NAMES_free(san);
                    return true;
                }
            }
        }
        GENERAL_NAMES_free(san);
    }

    // Fallback to Common Name (CN) - deprecated but some certs still use it
    X509_NAME *subject = X509_get_subject_name(cert);
    char cn[256];
    if (X509_NAME_get_text_by_NID(subject, NID_commonName, cn, sizeof(cn)) > 0) {
        return MatchHostname(host, cn);
    }

    return false;
}

//! Match hostname with wildcard support (e.g., *.database.windows.net)
bool MatchHostname(const std::string &host, const std::string &pattern) {
    if (pattern.empty() || host.empty()) {
        return false;
    }

    // Exact match
    if (pattern == host) {
        return true;
    }

    // Wildcard match (*.example.com matches foo.example.com)
    if (pattern.size() > 2 && pattern[0] == '*' && pattern[1] == '.') {
        auto suffix = pattern.substr(1);  // .example.com
        if (host.size() > suffix.size() &&
            host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0) {
            // Ensure wildcard only matches single label (no dots before suffix)
            auto prefix = host.substr(0, host.size() - suffix.size());
            return prefix.find('.') == std::string::npos;
        }
    }

    return false;
}

} // namespace tls
} // namespace tds
} // namespace duckdb
```

**Key Points**:
- Azure endpoints (*.database.windows.net, *.fabric.microsoft.com): Verify hostname
- On-premises SQL Server: Continue accepting self-signed certificates
- Uses only OpenSSL APIs - no additional dependencies (azure-sdk, curl, etc.)
- Supports wildcard certificates (*.database.windows.net)

### Skip Conditions for Azure Tests

```sql
# name: test/sql/azure/azure_service_principal.test
# group: [azure]

require mssql
require-env AZURE_TEST_TENANT_ID
require-env AZURE_TEST_CLIENT_ID
require-env AZURE_TEST_CLIENT_SECRET
require-env AZURE_SQL_TEST_HOST
```

---

## Example Usage

```sql
-- Load extensions
LOAD azure;
LOAD mssql;

-- Create Azure secret (service principal for production)
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

-- Create MSSQL secret with Azure auth
CREATE SECRET my_sql (
    TYPE mssql,
    HOST 'myserver.database.windows.net',
    PORT 1433,
    DATABASE 'mydb',
    AZURE_SECRET 'my_azure'
);

-- Connect to Azure SQL
ATTACH '' AS azuredb (TYPE mssql, SECRET my_sql);

-- Query data
SELECT * FROM azuredb.dbo.customers LIMIT 10;

-- Browse catalog
SELECT * FROM duckdb_schemas() WHERE database_name = 'azuredb';
SELECT * FROM duckdb_tables() WHERE database_name = 'azuredb';

-- DML operations
INSERT INTO azuredb.dbo.orders (customer_id, total) VALUES (1, 100.00);

-- Bulk load
COPY (SELECT * FROM local_data) TO 'azuredb.dbo.staging_table';

-- Disconnect
DETACH azuredb;
```

---

## CI/CD Updates

### GitHub Actions

```yaml
# Add to CI workflow
- name: Run Unit Tests
  run: make test

- name: Run Azure Integration Tests
  if: env.AZURE_TEST_TENANT_ID != ''
  env:
    AZURE_TEST_TENANT_ID: ${{ secrets.AZURE_TEST_TENANT_ID }}
    AZURE_TEST_CLIENT_ID: ${{ secrets.AZURE_TEST_CLIENT_ID }}
    AZURE_TEST_CLIENT_SECRET: ${{ secrets.AZURE_TEST_CLIENT_SECRET }}
    AZURE_SQL_TEST_HOST: ${{ secrets.AZURE_SQL_TEST_HOST }}
    FABRIC_TEST_HOST: ${{ secrets.FABRIC_TEST_HOST }}
    FABRIC_TEST_DATABASE: ${{ secrets.FABRIC_TEST_DATABASE }}
  run: make azure-integration-test
```

---

## Documentation Updates

- README: Azure SQL section with feature compatibility
- README: Microsoft Fabric section with known limitations
- CLAUDE.md: Fabric endpoint detection and handling
- Troubleshooting: Common Azure/Fabric errors and solutions

---

## Out of Scope

- Performance optimization for Azure/Fabric
- Azure Synapse Analytics dedicated pools (different from Fabric)
- Azure SQL Managed Instance (should work but not explicitly tested)
- Performance testing / benchmarking
- Load testing / stress testing

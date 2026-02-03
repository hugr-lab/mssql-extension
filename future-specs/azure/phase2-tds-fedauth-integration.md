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
- **FR-003**: System MUST implement TDS FEDAUTH extension in PRELOGIN packet
- **FR-004**: System MUST implement TDS FEDAUTH extension in LOGIN7 packet
- **FR-005**: System MUST encode access token as UTF-16LE for TDS protocol
- **FR-006**: System MUST handle FEDAUTHINFO server response
- **FR-007**: System MUST fall back to SQL auth when no `azure_secret` present

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

### Test Categories

| Category | Azure Required | Description |
|----------|----------------|-------------|
| Unit - FEDAUTH Encoding | No | Test token UTF-16LE encoding |
| Unit - Secret Validation | No | Test azure_secret validation logic |
| Integration - SQL Auth | No* | Verify existing SQL auth works |
| Integration - Azure SQL | Yes | Full connection with service principal |
| Integration - Azure CLI | Yes | Full connection with CLI credentials |
| Integration - Fabric | Yes | Full connection to Fabric endpoint |
| Integration - Catalog | Yes | Schema/table discovery |
| Integration - DML | Yes | SELECT/INSERT/UPDATE/DELETE |
| Integration - COPY | Yes | Bulk load operations |

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

```
src/
├── azure/
│   ├── azure_fedauth.cpp         # FEDAUTH token encoding
│   └── azure_fedauth.hpp
├── tds/
│   ├── tds_protocol.cpp          # PRELOGIN & LOGIN7 FEDAUTH extensions
│   ├── tds_connection.cpp        # Auth flow branching
│   └── tds_token_parser.cpp      # FEDAUTHINFO response handling
├── connection/
│   └── mssql_connection_provider.cpp  # Read azure_secret from MSSQL secret
├── catalog/
│   └── mssql_catalog.cpp         # Add Fabric detection/handling
├── include/
│   ├── azure/azure_fedauth.hpp
│   ├── tds/tds_types.hpp         # FEDAUTH constants
│   ├── tds/tds_connection.hpp    # ConnectionParams update
│   └── mssql_platform.hpp        # IsFabricEndpoint() utility
test/
├── cpp/
│   ├── test_fedauth_encoding.cpp
│   └── test_mssql_secret_azure.cpp
└── sql/azure/
    ├── sql_auth_regression.test
    ├── azure_service_principal.test
    ├── azure_cli_auth.test
    ├── azure_error_handling.test
    ├── azure_catalog.test
    ├── azure_dml.test
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

} // namespace mssql
} // namespace duckdb
```

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

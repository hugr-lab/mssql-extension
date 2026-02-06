# Feature Specification: TDS FEDAUTH Integration (Phase 2)

**Feature Branch**: `001-tds-fedauth-integration`
**Created**: 2026-02-05
**Status**: Draft
**Dependencies**: phase1-azure-token-infrastructure (completed)

## Problem Statement

Phase 1 established Azure token infrastructure (token acquisition, MSSQL secret `azure_secret` field, `mssql_azure_auth_test()` function). This phase integrates those tokens into the TDS authentication flow using the FEDAUTH protocol, enabling actual database connections to Azure SQL Database and Microsoft Fabric Warehouse.

**User Pain Point**: Users have validated their Azure credentials with `mssql_azure_auth_test()` but cannot yet connect to Azure SQL or Fabric databases. This phase delivers the actual connectivity.

**Deliverable**: Users can connect to Azure SQL and Fabric with Azure AD authentication, using all existing extension features (catalog browsing, DML operations, COPY/BCP).

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Connect with Azure Authentication (Priority: P1)

A user wants to connect to Azure SQL Database using Azure AD credentials (service principal or CLI).

**Why this priority**: This is the core deliverable - without FEDAUTH integration, Azure AD authentication cannot complete the TDS handshake. This unblocks all other Azure scenarios.

**Independent Test**: Can be tested by executing `ATTACH '' AS db (TYPE mssql, SECRET my_sql);` where the secret uses `azure_secret`. A successful connection that allows `SELECT 1` proves the integration works.

**Acceptance Scenarios**:

1. **Given** MSSQL secret with `azure_secret` configured, **When** user executes `ATTACH`, **Then** connection authenticates via Azure AD token and LOGINACK is received
2. **Given** valid Azure credentials, **When** connection is established, **Then** user can execute `SELECT 1` and receive result
3. **Given** Azure auth connection, **When** connection is returned to pool and reused, **Then** token is refreshed automatically if near expiration

---

### User Story 2 - Backward Compatibility with SQL Auth (Priority: P1)

Existing SQL authentication must continue to work unchanged for on-premises SQL Server and Azure SQL with SQL auth enabled.

**Why this priority**: Must not break existing users. Zero regressions is a hard requirement.

**Independent Test**: All existing integration tests in `test/sql/` pass without modification.

**Acceptance Scenarios**:

1. **Given** MSSQL secret with `user` and `password` (no `azure_secret`), **When** user executes `ATTACH`, **Then** SQL authentication is used (existing LOGIN7 behavior)
2. **Given** on-premises SQL Server, **When** user connects with SQL auth, **Then** connection works exactly as before
3. **Given** mixed environment with both Azure and SQL auth databases, **When** user attaches both simultaneously, **Then** each uses its appropriate auth method

---

### User Story 3 - Handle Authentication Failures Gracefully (Priority: P1)

Users need clear, actionable error messages when Azure authentication fails at connection time.

**Why this priority**: Poor error messages cause support burden and user frustration. Clear errors enable self-service resolution.

**Independent Test**: Can be tested by providing invalid credentials and verifying error message contains Azure AD error code and suggested fix.

**Acceptance Scenarios**:

1. **Given** expired Azure token, **When** connection attempted, **Then** token is refreshed automatically (transparent to user)
2. **Given** invalid Azure credentials (wrong client_secret), **When** connection attempted, **Then** error includes Azure AD error code: "Azure AD error AADSTS7000215: Invalid client secret"
3. **Given** Azure account without database access, **When** connection attempted, **Then** clear error: "Login failed: Azure AD principal does not have access to database"
4. **Given** Azure secret that doesn't exist, **When** ATTACH attempted, **Then** clear error: "Azure secret 'name' not found"

---

### User Story 4 - Catalog Operations with Azure Auth (Priority: P2)

A user needs to browse schemas, tables, and columns on Azure SQL Database after connecting with Azure AD.

**Why this priority**: Catalog browsing is essential for data discovery but is secondary to basic connectivity.

**Independent Test**: Can be tested by connecting with Azure auth and running `SELECT * FROM duckdb_schemas() WHERE database_name = 'azuredb'`.

**Acceptance Scenarios**:

1. **Given** Azure-authenticated connection, **When** user queries `duckdb_schemas()`, **Then** all accessible schemas are listed
2. **Given** Azure-authenticated connection, **When** user queries `duckdb_tables()`, **Then** all accessible tables are listed
3. **Given** Azure SQL with limited permissions, **When** catalog queried, **Then** only accessible objects shown (no permission errors for hidden objects)

---

### User Story 5 - Data Operations with Azure Auth (Priority: P2)

A user needs to perform all standard data operations (SELECT, INSERT, UPDATE, DELETE) on Azure SQL.

**Why this priority**: DML operations are the primary use case after connectivity is established.

**Independent Test**: Can be tested by running a full CRUD cycle on a test table via Azure-authenticated connection.

**Acceptance Scenarios**:

1. **Given** Azure-authenticated connection, **When** user executes SELECT, **Then** results returned correctly
2. **Given** Azure-authenticated connection, **When** user executes INSERT with RETURNING, **Then** inserted values returned via OUTPUT INSERTED
3. **Given** Azure-authenticated connection, **When** user executes UPDATE/DELETE, **Then** affected row count is correct
4. **Given** Azure-authenticated connection, **When** user uses transactions (BEGIN/COMMIT/ROLLBACK), **Then** transaction semantics are preserved

---

### User Story 6 - Microsoft Fabric Warehouse Support (Priority: P2)

A user needs to work with Microsoft Fabric SQL endpoints, which require Azure AD and have different capabilities than Azure SQL.

**Why this priority**: Fabric is a growing platform with different limitations. Users need working connectivity with documented limitations.

**Independent Test**: Can be tested by connecting to a Fabric Warehouse endpoint and executing SELECT queries.

**Acceptance Scenarios**:

1. **Given** Fabric Warehouse connection, **When** user queries catalog, **Then** schemas/tables are listed
2. **Given** Fabric Warehouse connection, **When** user executes SELECT, **Then** results returned correctly
3. **Given** Fabric Warehouse connection, **When** user attempts unsupported operation (e.g., certain DDL), **Then** clear error message explaining limitation (not cryptic TDS error)

---

### User Story 7 - COPY/BCP with Azure Auth (Priority: P3)

A user needs to bulk load data into Azure SQL using the COPY command with BCP protocol.

**Why this priority**: Bulk loading is important for ETL but is an advanced use case after basic operations work.

**Independent Test**: Can be tested by executing `COPY (SELECT * FROM local_data) TO 'azuredb.dbo.staging_table'` via Azure-authenticated connection.

**Acceptance Scenarios**:

1. **Given** Azure-authenticated connection, **When** user executes COPY TO, **Then** data transferred via BCP protocol
2. **Given** Azure-authenticated connection, **When** user creates temp table via COPY, **Then** temp table accessible within session
3. **Given** Fabric Warehouse connection, **When** user attempts COPY/BCP, **Then** operation succeeds or clear "not supported" error is returned

---

### Edge Cases

- Token expiration during long-running query: Should refresh token on next connection acquisition, not mid-query
- Connection pool with mixed auth types: Pools are keyed by connection string, so Azure and SQL auth get separate pools
- Azure extension not loaded: Clear error: "Azure extension required. Run INSTALL azure; LOAD azure;"
- Network timeout during token acquisition: Propagate timeout error with suggestion to retry
- Hostname verification failure: Clear TLS error with hostname mismatch details

---

## Requirements *(mandatory)*

### Functional Requirements

**TDS Protocol - FEDAUTH**:
- **FR-001**: System MUST detect `azure_secret` in MSSQL secret and use Azure auth flow instead of SQL auth
- **FR-002**: System MUST acquire token BEFORE sending LOGIN7 packet (not during)
- **FR-003**: PRELOGIN packet MUST always be sent (both SQL and Azure auth paths) per TDS spec
- **FR-004**: PRELOGIN MUST include FEDAUTHREQUIRED=0x01 option (option 0x06) when using Azure auth
- **FR-005**: LOGIN7 MUST include FEDAUTH feature extension (FeatureId 0x02) with UTF-16LE encoded token
- **FR-006**: System MUST encode access token as UTF-16LE for TDS FEDAUTH packet
- **FR-007**: System MUST handle FEDAUTHINFO (token 0xEE) server response if returned
- **FR-008**: System MUST fall back to SQL auth (user/password in LOGIN7) when no `azure_secret` present

**TLS Requirements**:
- **FR-009**: TLS MUST be enabled for Azure endpoints (Azure SQL, Fabric) - this is Azure's requirement
- **FR-010**: For Azure endpoints (*.database.windows.net, *.fabric.microsoft.com), hostname verification MUST be performed
- **FR-011**: For on-premises SQL Server, self-signed certificates MUST still be accepted (existing behavior)
- **FR-012**: Hostname verification MUST use OpenSSL APIs only (no additional dependencies)

**Data Operations**:
- **FR-013**: Catalog operations MUST work identically with Azure-authenticated connections
- **FR-014**: All DML operations (SELECT, INSERT, UPDATE, DELETE) MUST work with Azure auth
- **FR-015**: COPY/BCP operations MUST work with Azure auth on Azure SQL
- **FR-016**: Transactions MUST work correctly with Azure auth (connection pinning preserved)
- **FR-017**: Statistics retrieval MUST work with graceful degradation (Fabric may not support DBCC)
- **FR-018**: Microsoft Fabric Warehouse MUST be supported with documented limitations

**Testing**:
- **FR-019**: All existing integration tests MUST pass unchanged (zero regressions)
- **FR-020**: New unit tests MUST cover FEDAUTH packet encoding and hostname matching
- **FR-021**: New integration tests MUST cover Azure SQL connection scenarios
- **FR-022**: Integration tests MUST be skippable when Azure credentials unavailable (require-env)

### Key Entities

- **FedAuthData**: Holds FEDAUTH extension data (library type, UTF-16LE token bytes)
- **ConnectionParams**: Extended with `use_azure_auth` flag and `azure_secret_name` field
- **EndpointType**: Classification for Azure SQL, Fabric, and On-Premises to determine TLS behavior

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

**Connectivity**:
- **SC-001**: Azure SQL Database connection succeeds with service principal credentials
- **SC-002**: Azure SQL Database connection succeeds with Azure CLI credentials
- **SC-003**: Microsoft Fabric Warehouse connection succeeds with Azure AD
- **SC-004**: SQL Server (on-prem) connection with SQL auth remains unchanged
- **SC-005**: Token refresh works for connections held longer than token lifetime (typically 1 hour)
- **SC-006**: All auth failure scenarios produce error messages containing actionable guidance

**Data Operations**:
- **SC-007**: Schema discovery returns results within 5 seconds on Azure SQL and Fabric
- **SC-008**: Table/column metadata retrieval works correctly
- **SC-009**: All DML operations (SELECT, INSERT, UPDATE, DELETE) work on Azure SQL
- **SC-010**: COPY/BCP operations complete successfully on Azure SQL
- **SC-011**: Fabric Warehouse basic operations (SELECT, catalog) documented and working
- **SC-012**: Fabric limitations clearly documented in user-facing documentation

**Testing**:
- **SC-013**: All existing tests pass (105 tests, 89 test cases currently)
- **SC-014**: New unit tests cover FEDAUTH encoding, hostname verification, secret validation
- **SC-015**: Integration tests validate service principal and CLI auth methods
- **SC-016**: Test suite gracefully skips Azure tests when credentials unavailable

---

## Assumptions

- Phase 1 Azure token infrastructure is complete and working (`mssql_azure_auth_test()` validates tokens successfully)
- DuckDB Azure extension is installed and loaded for Azure secret management
- libcurl is available for OAuth2 token acquisition (already added in Phase 1)
- OpenSSL is available for TLS and hostname verification (already a dependency)
- Azure SQL Database accepts TDS FEDAUTH authentication per MS-TDS specification
- Fabric Warehouse uses standard TDS FEDAUTH like Azure SQL (to be validated during implementation)

---

## Out of Scope

- Performance optimization for Azure/Fabric (separate effort after correctness)
- Azure Synapse Analytics dedicated pools (different architecture)
- Azure SQL Managed Instance (should work but not explicitly tested)
- Performance/load testing and benchmarking
- Device code flow for headless environments (future enhancement)
- Certificate-based service principal authentication (future enhancement)

---

## User Experience *(see AZURE.md for full documentation)*

### Quick Start Flow

The target user experience mirrors the patterns documented in `AZURE.md`. After this feature, the full connection flow works:

```sql
-- 1. Install and load extensions
INSTALL azure;
LOAD azure;
INSTALL mssql FROM community;
LOAD mssql;

-- 2. Create Azure secret (service principal for production)
CREATE SECRET my_azure_secret (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID 'your-tenant-id',
    CLIENT_ID 'your-client-id',
    CLIENT_SECRET 'your-client-secret'
);

-- 3. Test credentials (Phase 1 - already works)
SELECT mssql_azure_auth_test('my_azure_secret');
-- Returns: eyJ0eXAiOi...xyz [1634 chars]

-- 4. Connect to Azure SQL Database (Phase 2 - THIS FEATURE)
ATTACH 'Server=myserver.database.windows.net;Database=mydb' AS azuredb (
    TYPE mssql,
    AZURE_SECRET 'my_azure_secret'
);

-- 5. Query data
SELECT * FROM azuredb.dbo.customers LIMIT 10;
```

### Supported Connection Patterns

All patterns from AZURE.md must work after this feature:

| Pattern | Connection String |
| ------- | ----------------- |
| Azure SQL Database | `Server=name.database.windows.net;Database=dbname` |
| Microsoft Fabric DW | `Server=xyz.datawarehouse.fabric.microsoft.com;Database=warehouse` |
| Azure Synapse Serverless | `Server=name-ondemand.sql.azuresynapse.net;Database=dbname` |

### Error Message UX

Error messages must be actionable and consistent with AZURE.md troubleshooting patterns:

| Scenario | Error Message |
| -------- | ------------- |
| Azure extension not loaded | "Azure extension required for Azure authentication" |
| Azure secret not found | "Azure secret 'xyz' not found" |
| Invalid client secret | "Azure AD error AADSTS7000215: Invalid client secret provided" |
| Application not found | "Azure AD error AADSTS700016: Application not found" |
| CLI credentials expired | "Azure CLI credentials expired. Run 'az login' to refresh." |
| No database access | "Login failed: Azure AD principal does not have access to database" |

---

## Technical Context (For Planning Reference)

### Namespace Structure

Per project conventions (no prefix needed within namespace):
- `duckdb::mssql::azure` - Token acquisition, FEDAUTH encoding
- `duckdb::tds` - TDS protocol extensions
- `duckdb::tds::tls` - TLS hostname verification
- `duckdb::mssql` - Connection provider updates

### TDS Protocol Changes Summary

**PRELOGIN** (when using Azure auth):
```
Option 0x06 (FEDAUTHREQUIRED): 1 byte
  Value: 0x01 (client requires federated auth)
```

**LOGIN7** (when using Azure auth):
```
Feature Extension:
  FeatureId: 0x02 (FEDAUTH)
  FeatureDataLen: 4 + token_length_bytes
  Options: 4 bytes (FedAuthLibrary: 0x02 = MSAL)
  Token: UTF-16LE encoded access token
```

### Endpoint Detection

```cpp
// Azure SQL: *.database.windows.net
// Fabric: *.datawarehouse.fabric.microsoft.com, *.pbidedicated.windows.net
bool IsAzureEndpoint(const std::string &host);
bool IsFabricEndpoint(const std::string &host);
```

### Test Environment Variables

Azure tests require:
- `AZURE_TEST_TENANT_ID` - Azure tenant ID
- `AZURE_TEST_CLIENT_ID` - Service principal client ID
- `AZURE_TEST_CLIENT_SECRET` - Service principal secret
- `AZURE_SQL_TEST_HOST` - Azure SQL Database host
- `FABRIC_TEST_HOST` - Fabric Warehouse host (optional)
- `FABRIC_TEST_DATABASE` - Fabric database name (optional)

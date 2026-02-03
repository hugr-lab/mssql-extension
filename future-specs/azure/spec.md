# Feature Specification: Azure AD (Entra ID) Authentication Support

**Feature Branch**: `azure-entra-id-auth`
**Created**: 2026-02-03
**Status**: Future / Planning
**Input**: User vision for Azure AD authentication with Azure SQL and Microsoft Fabric Warehouse support

## Problem Statement

The DuckDB MSSQL extension currently supports only SQL Server authentication (user/password) over TDS. Many modern Azure deployments use Azure AD (Entra ID) authentication exclusively:

- **Azure SQL Database** with Azure AD-only authentication enabled
- **Microsoft Fabric SQL Analytics Endpoint** (Fabric Warehouse)
- **Azure Synapse Analytics** serverless SQL pools

**Key User Pain Point**: Most analysts cannot manage Azure resources and don't have access to tenant IDs or service principals. They need an authentication flow as simple as "run `az login`, then connect."

---

## Authentication Scenarios (Priority Order)

### Scenario 1: Azure CLI Credentials (Easiest for Analysts) ⭐

**Target Users**: Data analysts using DuckDB CLI, DBeaver, Python notebooks

**Why Easiest**:
- User just runs `az login` once (they may already be logged in)
- No tenant ID needed - uses their home tenant automatically
- No secrets to manage
- Works with MFA

**Usage**:
```sql
-- User has already run: az login

-- Option A: Simplest - just set authentication method
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'cli'
);

CREATE SECRET my_sql (
    TYPE mssql,
    HOST 'myserver.database.windows.net',
    PORT 1433,
    DATABASE 'mydb',
    AZURE_SECRET 'my_azure'
);

ATTACH '' AS db (TYPE mssql, SECRET my_sql);
```

---

### Scenario 2: Interactive Browser Login (For MFA Users)

**Target Users**: Analysts who can't use Azure CLI, need MFA authentication

**Why Important**: Opens browser, user logs in with their corporate account - no credentials stored

**Usage**:
```sql
-- Opens browser for login when first connection is made
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'interactive'   -- or 'default' which includes interactive as fallback
);

CREATE SECRET my_sql (
    TYPE mssql,
    HOST 'myserver.database.windows.net',
    PORT 1433,
    DATABASE 'mydb',
    AZURE_SECRET 'my_azure'
);

ATTACH '' AS db (TYPE mssql, SECRET my_sql);
-- Browser opens for authentication
```

---

### Scenario 3: Service Principal (For Automation/CI/CD) ⭐

**Target Users**: DevOps, data engineers, automated pipelines, server-to-server integrations

**Why Important**: **Main use case for production** - service-to-service authentication without user interaction. DuckDB running as a service authenticating to Azure SQL.

**Usage**:
```sql
-- Service principal credentials (typically from environment/secrets manager)
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
    CLIENT_ID 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
    CLIENT_SECRET 'your-client-secret'
);

CREATE SECRET my_sql (
    TYPE mssql,
    HOST 'myserver.database.windows.net',
    PORT 1433,
    DATABASE 'mydb',
    AZURE_SECRET 'my_azure'
);

ATTACH '' AS db (TYPE mssql, SECRET my_sql);
```

---

### Scenario 4: Managed Identity (For Azure-Hosted Apps)

**Target Users**: Applications running on Azure VMs, AKS, App Service

**Why Important**: No credentials needed - Azure provides identity automatically

**Usage**:
```sql
-- System-assigned managed identity (no client_id needed)
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'managed_identity'
);

-- Or user-assigned managed identity
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER managed_identity,
    CLIENT_ID 'user-assigned-identity-client-id'
);

CREATE SECRET my_sql (..., AZURE_SECRET 'my_azure');
```

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Analyst with Azure CLI (Priority: P1) ⭐

A data analyst wants to query Azure SQL from DuckDB. They have Azure CLI installed and have logged in with `az login`.

**Why this priority**: **Easiest path for most users** - no Azure admin knowledge required.

**Acceptance Scenarios**:

1. **Given** user has run `az login`, **When** they create Azure secret with `CHAIN 'cli'` and MSSQL secret with `azure_secret`, **Then** connection authenticates automatically
2. **Given** user is logged into multiple Azure tenants, **When** they connect, **Then** their default tenant is used (or they can specify `AZURE_TENANT_ID` env var)
3. **Given** `az login` session has expired, **When** they try to connect, **Then** clear error: "Azure CLI credentials expired. Run 'az login' to refresh."

---

### User Story 2 - Interactive Browser for MFA Users (Priority: P1)

An analyst cannot use Azure CLI but needs to authenticate with their corporate account that has MFA enabled.

**Acceptance Scenarios**:

1. **Given** Azure secret with `CHAIN 'interactive'`, **When** user attaches database, **Then** browser opens for Azure AD login
2. **Given** user completes browser login, **Then** connection succeeds and token is cached for session
3. **Given** user cancels browser login, **Then** clear error message is returned

---

### User Story 3 - Service Principal for Server-to-Server (Priority: P1) ⭐

A data engineer runs DuckDB as a service (ETL pipeline, data platform, backend service) and needs service-to-service authentication to Azure SQL. This is the **main production use case**.

**Acceptance Scenarios**:

1. **Given** Azure secret with `tenant_id`, `client_id`, `client_secret`, **When** connection is made, **Then** authentication succeeds without user interaction
2. **Given** environment variables `AZURE_TENANT_ID`, `AZURE_CLIENT_ID`, `AZURE_CLIENT_SECRET`, **When** Azure secret uses `CHAIN 'env'`, **Then** authentication succeeds
3. **Given** invalid client secret, **Then** clear error: "Azure AD authentication failed: AADSTS7000215 - Invalid client secret"
4. **Given** service principal without database access, **Then** clear error: "Access denied for service principal. Grant database access in Azure portal."

---

### User Story 4 - Microsoft Fabric Warehouse (Priority: P1)

An analyst needs to query Fabric Warehouse endpoint.

**Acceptance Scenarios**:

1. **Given** Fabric endpoint and Azure CLI credentials, **When** user connects, **Then** connection succeeds
2. **Given** Fabric connection, **When** user queries tables, **Then** data returns correctly
3. **Given** Fabric-specific limitations, **Then** catalog operations degrade gracefully with clear messages

---

### Edge Cases

- Azure CLI not installed → "Azure CLI not found. Install Azure CLI or use a different authentication method."
- No `az login` session → "Not logged into Azure CLI. Run 'az login' first."
- Token expired → Automatic refresh or clear retry message
- Wrong tenant for database → "Access denied. Verify your Azure account has access to this database."
- Interactive browser blocked (headless environment) → "Interactive browser authentication not available. Use 'cli' or 'service_principal' instead."

---

## Requirements *(mandatory)*

### Functional Requirements

**Credential Chain Support**:
- **FR-001**: Support `CHAIN 'cli'` for Azure CLI credentials (easiest for analysts)
- **FR-002**: Support `CHAIN 'interactive'` for browser-based login
- **FR-003**: Support `CHAIN 'managed_identity'` for Azure-hosted apps
- **FR-004**: Support `CHAIN 'default'` which tries: env → managed_identity → cli → interactive
- **FR-005**: Support `PROVIDER service_principal` for automated pipelines

**MSSQL Secret Extension**:
- **FR-006**: MSSQL secret MUST accept `azure_secret` parameter
- **FR-007**: When `azure_secret` present, `user` and `password` MUST be optional
- **FR-008**: Full backward compatibility with SQL authentication

**Token Handling**:
- **FR-009**: Request tokens with scope `https://database.windows.net/.default`
- **FR-010**: Cache tokens and refresh before expiration
- **FR-011**: **No tenant ID required for CLI/interactive** - uses user's home tenant

**TDS Authentication**:
- **FR-012**: Implement TDS FEDAUTH for token-based login
- **FR-013**: Handle authentication errors with clear, actionable messages

### Authentication Method Comparison

| Method | Tenant ID | Credentials | User Interaction | Best For |
|--------|-----------|-------------|------------------|----------|
| **Service Principal** | Yes | Yes | No | Production services ⭐ |
| **CLI** (`credential_chain`) | No | No | No* | Analysts (easiest) |
| **Interactive** | No | No | Yes (browser) | MFA users |
| **Managed Identity** | No | No | No | Azure-hosted apps |

*After initial `az login`

### Extended MSSQL Secret

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `host` | VARCHAR | Yes | Azure SQL/Fabric endpoint |
| `port` | INTEGER | Yes | TCP port (default 1433) |
| `database` | VARCHAR | Yes | Database name |
| `user` | VARCHAR | Conditional | Required for SQL auth only |
| `password` | VARCHAR | Conditional | Required for SQL auth only |
| `use_encrypt` | BOOLEAN | No | TLS (default: true, required for Azure) |
| `azure_secret` | VARCHAR | No | Name of Azure secret for Entra ID auth |

---

## Success Criteria *(mandatory)*

- **SC-001**: Service principal works for server-to-server (main production use case)
- **SC-002**: Analyst can connect with just `az login` + Azure secret (no tenant ID)
- **SC-003**: Interactive browser login works for MFA users
- **SC-004**: Managed identity works on Azure VMs/containers
- **SC-005**: Microsoft Fabric Warehouse connections work
- **SC-006**: All existing SQL auth functionality unchanged
- **SC-007**: Error messages clearly indicate what action user should take

---

## Assumptions

- DuckDB Azure extension provides credential chain infrastructure
- Azure SDK for C++ can be used for token acquisition if needed
- Azure CLI (`az`) is commonly installed by analysts
- Interactive browser can be launched from CLI environment (not headless)

---

## Out of Scope

- Device code flow (for headless interactive auth) - may add later
- Certificate-based service principal auth - may add later
- Azure AD B2C / ADFS

---

## Technical Context (For Planning Reference)

### Namespace Structure

All Azure authentication code uses `duckdb::mssql::azure` namespace:

```cpp
namespace duckdb {
namespace mssql {
namespace azure {

// Token acquisition, FEDAUTH encoding, test functions
// No prefix needed per project naming conventions

} // namespace azure
} // namespace mssql
} // namespace duckdb
```

### DuckDB Azure Extension Credential Chain

The DuckDB Azure extension already supports credential chains:

```sql
CREATE SECRET (TYPE azure, PROVIDER credential_chain, CHAIN 'cli;env;managed_identity');
```

Chain values: `cli`, `managed_identity`, `workload_identity`, `env`, `interactive`, `default`

### Token Scope

All Azure SQL services use: `https://database.windows.net/.default`

### No Tenant ID Needed for Most Users

`InteractiveBrowserCredential` and Azure CLI credentials automatically use the user's home tenant. Only service principals require explicit tenant ID.

---

## Documentation Updates Required

- README: "Azure Authentication" section with examples for each scenario
- Quick start guide: "Connecting to Azure SQL" (focus on CLI method)
- Troubleshooting: Common Azure auth errors and solutions

---

## Sources

- [DuckDB Azure Extension](https://duckdb.org/docs/stable/core_extensions/azure)
- [DBeaver Microsoft Entra ID Authentication](https://dbeaver.com/docs/dbeaver/Authentication-Microsoft-Entra-ID/)
- [Azure Identity DefaultAzureCredential](https://learn.microsoft.com/en-us/python/api/azure-identity/azure.identity.defaultazurecredential)
- [Connect to Azure SQL with Python (pyodbc)](https://learn.microsoft.com/en-us/azure/azure-sql/database/azure-sql-python-quickstart)
- [Azure Identity Credential Chains](https://learn.microsoft.com/en-us/azure/developer/python/sdk/authentication/credential-chains)

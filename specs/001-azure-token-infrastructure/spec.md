# Feature Specification: Azure Token Infrastructure (Phase 1)

**Feature Branch**: `001-azure-token-infrastructure`
**Created**: 2026-02-05
**Status**: Draft
**Input**: Azure AD token acquisition infrastructure for MSSQL extension - Phase 1 of Azure authentication support

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Test Azure Credentials (Priority: P1)

A user wants to verify their Azure credentials work before attempting a database connection to Azure SQL or Fabric Warehouse.

**Why this priority**: Users need immediate feedback on credential validity without the complexity of full TDS FEDAUTH. This builds confidence and reduces debugging time.

**Independent Test**: Can be fully tested by calling `mssql_azure_auth_test('secret_name')` and verifying token acquisition works, delivering immediate value for credential validation.

**Acceptance Scenarios**:

1. **Given** valid Azure secret with service principal (tenant_id, client_id, client_secret), **When** user calls `mssql_azure_auth_test('my_azure')`, **Then** function returns truncated token (e.g., "eyJ0eXAi...xyz [1847 chars]")
2. **Given** valid Azure secret with `CHAIN 'cli'`, **When** user has run `az login`, **Then** function returns truncated token showing CLI credentials work
3. **Given** Azure secret with invalid client_secret, **When** test function called, **Then** error includes Azure AD error code: "Azure AD error AADSTS7000215: Invalid client secret"
4. **Given** Azure secret with expired CLI session, **When** test function called, **Then** error with actionable guidance: "Azure CLI credentials expired. Run 'az login' to refresh."

---

### User Story 2 - Create MSSQL Secret with Azure Auth (Priority: P1)

A user wants to create an MSSQL secret that uses Azure AD authentication instead of SQL Server username/password.

**Why this priority**: Foundation for Azure authentication - users need to configure MSSQL secrets before any Azure auth connection can be attempted (Phase 2).

**Independent Test**: Can be tested by creating MSSQL secrets with `azure_secret` parameter and verifying validation rules work correctly.

**Acceptance Scenarios**:

1. **Given** an existing Azure secret `my_azure`, **When** user creates MSSQL secret with `AZURE_SECRET 'my_azure'`, **Then** secret is created successfully without requiring user/password
2. **Given** MSSQL secret with `azure_secret`, **When** user also provides `user` and `password`, **Then** secret is created (SQL auth fields ignored for Azure auth)
3. **Given** non-existent Azure secret name, **When** user creates MSSQL secret, **Then** clear error: "Azure secret 'name' not found"
4. **Given** secret name that exists but is not Azure type, **When** user creates MSSQL secret, **Then** clear error: "Secret 'name' is not an Azure secret (type: mssql)"

---

### User Story 3 - Backward Compatibility (Priority: P1)

Existing MSSQL secrets with SQL Server authentication must continue to work unchanged.

**Why this priority**: Non-negotiable - cannot break existing functionality for current users.

**Independent Test**: Can be verified by running all existing MSSQL secret tests and confirming zero regressions.

**Acceptance Scenarios**:

1. **Given** existing MSSQL secret with `user` and `password`, **When** user attaches database, **Then** SQL authentication works exactly as before
2. **Given** MSSQL secret without `azure_secret`, **When** `user` or `password` is missing, **Then** validation error occurs (existing behavior preserved)
3. **Given** MSSQL secret with neither SQL auth nor Azure auth, **When** created, **Then** error: "Either user/password or azure_secret required"

---

### User Story 4 - Interactive Device Code Authentication (Priority: P1)

A normal user (analyst) wants to connect to Azure SQL/Fabric using their Entra ID credentials with MFA, without needing Azure CLI installed.

**Why this priority**: Many enterprise users have MFA-enforced accounts and may not have Azure CLI. Device Code Flow provides a simple "visit URL, enter code" experience that works everywhere including SSH sessions and containers.

**Independent Test**: Can be tested by creating Azure secret with `CHAIN 'interactive'`, calling `mssql_azure_auth_test()`, visiting the displayed URL, entering the code, completing MFA, and verifying token is returned.

**Acceptance Scenarios**:

1. **Given** Azure secret with `CHAIN 'interactive'`, **When** user calls `mssql_azure_auth_test()`, **Then** displays message: "To sign in, visit https://microsoft.com/devicelogin and enter code ABC123"
2. **Given** user visits URL and enters code, **When** authentication succeeds (including MFA), **Then** function returns truncated token
3. **Given** user doesn't complete login within 15 minutes, **When** timeout expires, **Then** clear error: "Device code expired. Please try again."
4. **Given** user declines authorization, **When** polling detects decline, **Then** clear error: "Authorization was declined by user"

---

### Edge Cases

- Azure extension not loaded: Clear error "Azure extension required. Run INSTALL azure; LOAD azure;"
- Azure secret doesn't exist: Clear error "Azure secret 'name' not found"
- Secret is wrong type: Clear error "Secret 'name' is not an Azure secret (type: mssql)"
- Service principal missing required fields: Clear error listing which fields are missing
- Empty secret name passed to test function: Error "Secret name required"
- Token acquisition timeout: Appropriate timeout error with retry guidance
- Device code expired (15 min timeout): Clear error "Device code expired. Please try again."
- User declines authorization: Clear error "Authorization was declined by user"
- Network error during polling: Retry with exponential backoff, then error with guidance

---

## Requirements *(mandatory)*

### Functional Requirements

**Token Acquisition**:

- **FR-001**: System MUST NOT introduce a new Azure secret type; MUST use DuckDB's existing `TYPE azure` secret infrastructure
- **FR-002**: System MUST support all Azure secret providers: `service_principal`, `credential_chain`, `managed_identity`
- **FR-003**: System MUST acquire tokens with scope `https://database.windows.net/.default` for Azure SQL compatibility
- **FR-004**: System MUST cache tokens and refresh before expiration (5 minute margin)
- **FR-005**: Token cache MUST be thread-safe for concurrent access

**MSSQL Secret Extension**:

- **FR-006**: MSSQL secret MUST accept optional `azure_secret` parameter referencing an Azure secret by name
- **FR-007**: When `azure_secret` is present, `user` and `password` MUST be optional (not required)
- **FR-008**: When `azure_secret` is absent, `user` and `password` MUST be required (existing behavior)
- **FR-009**: System MUST validate Azure secret exists and is correct type at MSSQL secret creation time
- **FR-010**: Full backward compatibility with existing MSSQL secrets using SQL authentication

**Test Function**:

- **FR-011**: System MUST provide function `mssql_azure_auth_test(secret_name VARCHAR) -> VARCHAR`
- **FR-012**: On success, function returns truncated token: first 10 chars + "..." + last 3 chars + " [N chars]"
- **FR-013**: On failure, function returns clear error message with Azure AD error code when available

**Interactive Device Code Authentication**:

- **FR-014**: System MUST support `CHAIN 'interactive'` for device code OAuth2 authentication
- **FR-015**: Interactive flow MUST use OAuth2 Device Authorization Grant (RFC 8628)
- **FR-016**: System MUST display verification URL and user code to console/output
- **FR-017**: System MUST poll token endpoint with appropriate interval (typically 5 seconds)
- **FR-018**: Interactive auth MUST support MFA-enforced Entra ID accounts
- **FR-019**: System MUST handle all device code errors: `authorization_pending`, `authorization_declined`, `expired_token`
- **FR-020**: Device code timeout is 15 minutes (Azure AD default, not configurable)

### Supported Azure Secret Configurations

| Provider             | Required Fields                            | Optional Fields                            |
| -------------------- | ------------------------------------------ | ------------------------------------------ |
| `service_principal`  | `tenant_id`, `client_id`, `client_secret`  | -                                          |
| `credential_chain`   | -                                          | `chain` (e.g., 'cli;env;managed_identity;interactive') |
| `managed_identity`   | -                                          | `client_id` (for user-assigned)            |

**Credential Chain Options**:

| Chain Value        | Description                                           | Requirements                  |
| ------------------ | ----------------------------------------------------- | ----------------------------- |
| `cli`              | Azure CLI credentials (`az login`)                    | Azure CLI installed, logged in |
| `env`              | Environment variables                                 | `AZURE_*` env vars set        |
| `managed_identity` | Azure managed identity (system or user-assigned)      | Running on Azure compute      |
| `interactive`      | Device Code Flow (visit URL, enter code, supports MFA) | User can open browser anywhere |

### Extended MSSQL Secret Fields

| Field              | Type    | Required    | Description                                     |
| ------------------ | ------- | ----------- | ----------------------------------------------- |
| `host`             | VARCHAR | Yes         | SQL Server hostname                             |
| `port`             | INTEGER | Yes         | TCP port (default 1433)                         |
| `database`         | VARCHAR | Yes         | Database name                                   |
| `user`             | VARCHAR | Conditional | Required if no `azure_secret`                   |
| `password`         | VARCHAR | Conditional | Required if no `azure_secret`                   |
| `use_encrypt`      | BOOLEAN | No          | TLS encryption (default: true)                  |
| `catalog`          | BOOLEAN | No          | Enable catalog (default: true)                  |
| **`azure_secret`** | VARCHAR | No          | **NEW**: Name of Azure secret for Entra ID auth |

### Test Function Return Values

| Scenario                   | Return Value                                                              |
| -------------------------- | ------------------------------------------------------------------------- |
| Success                    | `"eyJ0eXAi...xyz [1847 chars]"` (first 10 + "..." + last 3 + length)      |
| Azure AD error             | `"Azure AD error AADSTS7000215: Invalid client secret provided"`          |
| Secret not found           | `"Error: Azure secret 'name' not found"`                                  |
| Wrong secret type          | `"Error: Secret 'name' is not an Azure secret (type: mssql)"`             |
| CLI not logged in          | `"Error: Azure CLI credentials expired. Run 'az login' to refresh."`      |
| Empty secret name          | `"Error: Secret name required"`                                           |
| Device code expired        | `"Error: Device code expired. Please try again."`                         |
| Authorization declined     | `"Error: Authorization was declined by user"`                             |
| Device code pending        | (internal - continues polling, not returned to user)                      |

### Key Entities

- **Azure Secret**: DuckDB `TYPE azure` secret containing credentials (service principal, credential chain, or managed identity)
- **MSSQL Secret**: Extension-managed secret with optional `azure_secret` reference for authentication delegation
- **Token Result**: Acquired access token with expiration time and success/error status
- **Token Cache**: Thread-safe in-memory cache keyed by secret name with automatic expiration tracking

---

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Azure secrets created by DuckDB Azure extension are readable by MSSQL extension (100% compatibility with existing Azure secret types)
- **SC-002**: Tokens acquired successfully for all four providers: service_principal, credential_chain (cli), managed_identity, and interactive
- **SC-003**: MSSQL secret with valid `azure_secret` reference creates without error and without requiring user/password
- **SC-004**: `mssql_azure_auth_test()` returns truncated token (with length) for valid credentials within 5 seconds (60 seconds for interactive with user action)
- **SC-005**: `mssql_azure_auth_test()` returns clear Azure AD error codes (AADSTS*) on authentication failure
- **SC-006**: All existing MSSQL secret tests pass with zero regressions (run full test suite)
- **SC-007**: Token caching prevents repeated auth requests - second call within 5 minutes uses cached token (no network request)
- **SC-008**: Interactive Device Code authentication displays URL and code, user completes login on any device, polling returns valid token when user completes MFA

---

## Out of Scope

- TDS FEDAUTH protocol integration (deferred to Phase 2)
- Actual database connections with Azure auth (deferred to Phase 2)
- Catalog/DML operations verification with Azure auth (deferred to Phase 2)
- Certificate-based service principal authentication

---

## Assumptions

- DuckDB Azure extension is installed and provides `TYPE azure` secret infrastructure
- User has valid Azure credentials configured (service principal, Azure CLI session, managed identity environment, or can complete device code flow)
- Azure SDK for C++ (`azure-identity-cpp`) is used for non-interactive flows (matching DuckDB Azure extension)
- Azure SDK for C++ does NOT support interactive authentication - Device Code Flow implemented manually
- Token scope `https://database.windows.net/.default` works for Azure SQL Database, Azure SQL Managed Instance, and Microsoft Fabric SQL endpoints
- For interactive authentication: user can access a web browser on any device (not necessarily the same machine)
- Device Code Flow uses Azure AD endpoints: `/devicecode` for code request, `/token` for polling

---

## Technical Context (For Planning Reference)

### Namespace

All Azure authentication code uses `duckdb::mssql::azure` namespace per project conventions (no prefix needed within namespace).

### Proposed File Structure

```text
src/
├── azure/
│   ├── azure_token.cpp           # Token acquisition implementation (uses Azure SDK)
│   ├── azure_secret_reader.cpp   # Read Azure secrets from SecretManager
│   ├── azure_test_function.cpp   # mssql_azure_auth_test() function
│   └── azure_device_code.cpp     # Device Code Flow (RFC 8628) implementation
├── include/azure/
│   ├── azure_token.hpp           # Token result struct, acquisition interface
│   ├── azure_secret_reader.hpp   # Secret reader interface
│   ├── azure_test_function.hpp   # Function registration header
│   └── azure_device_code.hpp     # Device Code Flow structs
├── mssql_secret.cpp              # Add azure_secret handling
├── include/mssql_secret.hpp      # Add MSSQL_SECRET_AZURE_SECRET constant
└── mssql_extension.cpp           # Register test function
```

### Example Usage

```sql
-- Load extensions
LOAD azure;
LOAD mssql;

-- Option 1: Service principal (for production/CI)
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
    CLIENT_ID 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
    CLIENT_SECRET 'your-client-secret'
);

-- Option 2: Azure CLI credentials (for developers with az login)
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'cli'
);

-- Option 3: Interactive device code login (for analysts with MFA - no CLI needed)
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'interactive',
    TENANT_ID 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx'  -- Optional: defaults to 'common'
);

-- Test authentication before connecting
SELECT mssql_azure_auth_test('my_azure');
-- For interactive: Displays "To sign in, visit https://microsoft.com/devicelogin and enter code ABC123"
-- User visits URL on any device, enters code, completes MFA
-- Returns: "eyJ0eXAi...xyz [1847 chars]"

-- Create MSSQL secret with Azure auth
CREATE SECRET my_sql (
    TYPE mssql,
    HOST 'myserver.database.windows.net',
    PORT 1433,
    DATABASE 'mydb',
    AZURE_SECRET 'my_azure'
);

-- Note: Actual connection (ATTACH) requires Phase 2 TDS FEDAUTH integration
```

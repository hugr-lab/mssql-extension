# Feature Specification: FEDAUTH Token Provider Enhancements

**Feature Branch**: `032-fedauth-token-provider`
**Created**: 2026-02-06
**Status**: Draft
**Input**: User description: "Support user-provided tokens and environment-based service principal credentials for Azure AD authentication"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Manual Token Authentication (Priority: P1)

As a data analyst working in Microsoft Fabric notebooks, I want to attach to Azure SQL/Fabric using a manually-obtained access token, so that I can use tokens from my notebook environment without configuring additional secrets.

**Why this priority**: This is the primary use case - Fabric notebooks already provide tokens via `mssparkutils.credentials.getToken()`, and users need a simple way to use these tokens directly without complex secret configuration.

**Independent Test**: Can be fully tested by obtaining a token via Azure CLI (`az account get-access-token --resource https://database.windows.net/`) and using it in an ATTACH statement.

**Acceptance Scenarios**:

1. **Given** a valid Azure AD access token, **When** user executes `ATTACH '...' AS db (TYPE mssql, ACCESS_TOKEN '<token>')`, **Then** connection is established successfully using FEDAUTH authentication.

2. **Given** a valid token provided via MSSQL secret with ACCESS_TOKEN option, **When** user executes `ATTACH '' AS db (TYPE mssql, SECRET my_token)`, **Then** connection is established using the token from the secret.

3. **Given** an expired access token, **When** user attempts to connect, **Then** system returns clear error: "Access token expired at {timestamp}. Please provide a new token."

4. **Given** a token with invalid audience, **When** user attempts to connect, **Then** system returns error: "Access token audience does not match expected 'https://database.windows.net/'"

---

### User Story 2 - Environment-Based Service Principal (Priority: P2)

As a DevOps engineer, I want to configure Azure service principal credentials via environment variables, so that I can use standard Azure SDK environment variables without hardcoding secrets in my scripts or configuration.

**Why this priority**: This enables CI/CD pipelines and containerized deployments where credentials are injected via environment variables, following Azure SDK conventions used by other tools.

**Independent Test**: Can be tested by setting `AZURE_TENANT_ID`, `AZURE_CLIENT_ID`, `AZURE_CLIENT_SECRET` environment variables and creating an Azure secret with `credential_chain` provider using `env` chain.

**Acceptance Scenarios**:

1. **Given** environment variables `AZURE_TENANT_ID`, `AZURE_CLIENT_ID`, `AZURE_CLIENT_SECRET` are set, **When** user creates a secret with `PROVIDER credential_chain, CHAIN 'env'` and attaches using it, **Then** connection is established using service principal authentication.

2. **Given** `AZURE_CLIENT_ID` environment variable is not set, **When** user attempts to use env-based authentication, **Then** system returns error: "Environment variable AZURE_CLIENT_ID not set"

3. **Given** env-based service principal credentials, **When** token expires during a long session, **Then** system automatically refreshes the token using the stored credentials.

---

### User Story 3 - Token Expiration Awareness (Priority: P2)

As a user with a manually-provided token, I want to see clear error messages when my token expires, so that I know exactly what went wrong and how to fix it.

**Why this priority**: Manual tokens cannot be refreshed automatically, so clear error messaging is critical for user experience.

**Independent Test**: Can be tested by using an expired token and verifying the error message includes the expiration timestamp.

**Acceptance Scenarios**:

1. **Given** a token that expired at 14:30 UTC, **When** user attempts to use it at 14:35 UTC, **Then** error message shows: "Access token expired at 2026-02-06 14:30:00 UTC. Please provide a new token."

2. **Given** a token expiring in 2 minutes, **When** user attempts a long-running query, **Then** query fails with clear expiration error if token expires mid-query.

---

### Edge Cases

- What happens when token is malformed (not valid JWT)? → Return "Invalid access token format: unable to parse JWT"
- What happens when token is valid but for wrong resource? → Return audience mismatch error
- What happens when env variables are partially set? → Return specific error for missing variable
- What happens when token refresh fails for env-based auth? → Return error with refresh failure reason

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST accept `ACCESS_TOKEN` option in ATTACH statement for direct token authentication
- **FR-002**: System MUST accept `ACCESS_TOKEN` option in MSSQL secrets for token-based authentication
- **FR-003**: System MUST parse JWT tokens to extract expiration timestamp (`exp` claim)
- **FR-004**: System MUST validate token audience (`aud` claim) against `https://database.windows.net/`
- **FR-005**: System MUST return user-friendly error message with expiration timestamp when token is expired
- **FR-006**: System MUST support Azure SDK standard environment variables (`AZURE_TENANT_ID`, `AZURE_CLIENT_ID`, `AZURE_CLIENT_SECRET`) via `credential_chain` with `env` chain
- **FR-007**: System MUST automatically refresh tokens for authentication methods that support refresh (env-based service principal)
- **FR-008**: System MUST NOT attempt token refresh for manually-provided tokens (no credentials available)
- **FR-009**: System MUST return specific error messages for missing environment variables

### Key Entities

- **AccessToken**: Represents an Azure AD JWT token with raw value, expiration timestamp, audience, and refresh capability flag
- **TokenProvider**: Abstraction for different token acquisition methods (manual, env-based, existing Azure secret methods)

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can successfully connect using a manually-provided token in under 5 seconds
- **SC-002**: Token expiration errors include the exact expiration timestamp in a human-readable format
- **SC-003**: Environment-based authentication works with standard Azure SDK environment variable names
- **SC-004**: 100% of token-related errors provide actionable guidance (what went wrong, how to fix)
- **SC-005**: Token refresh for env-based auth happens automatically before expiration (5-minute margin)

## Assumptions

- Azure AD tokens are standard JWT format with `exp` and `aud` claims
- DuckDB Azure extension's `credential_chain` provider with `env` chain is available and functional
- Token refresh margin of 5 minutes before expiration is sufficient for most use cases
- Fabric notebook tokens obtained via `mssparkutils.credentials.getToken()` follow standard Azure AD JWT format

## Out of Scope

- Managed Identity authentication (separate feature)
- Token persistence/caching across DuckDB sessions
- Interactive browser-based authentication for manual tokens
- Custom token refresh callbacks

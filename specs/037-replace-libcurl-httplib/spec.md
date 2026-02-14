# Feature Specification: Replace libcurl with DuckDB Built-in httplib

**Feature Branch**: `037-replace-libcurl-httplib`
**Created**: 2026-02-14
**Status**: Draft
**Input**: Replace libcurl with DuckDB's built-in httplib for Azure OAuth2 HTTP requests, fixing Windows MSVC linker errors on community-extensions CI.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Windows Build Compatibility (Priority: P1)

As an extension maintainer, I need the extension to build successfully on the DuckDB community-extensions CI Windows pipeline so that Windows users can install the extension from the DuckDB community repository.

**Why this priority**: Without this, the extension cannot be distributed to Windows users through the official community-extensions channel. This is the primary motivation for the entire change.

**Independent Test**: Build the extension with the `x64-windows-static-md-release-vs2019comp` vcpkg triplet (dynamic CRT /MD + static libraries) and verify zero linker errors.

**Acceptance Scenarios**:

1. **Given** the extension source code, **When** built with the community-extensions MSVC triplet (static libs + dynamic CRT /MD), **Then** the build completes with zero unresolved external symbol errors
2. **Given** the extension source code, **When** built on Linux (GCC) and macOS (Clang), **Then** the build succeeds without regressions

---

### User Story 2 - Azure Service Principal Authentication (Priority: P1)

As a user connecting to Azure SQL Database with a service principal, I need the extension to acquire OAuth2 tokens via HTTPS so that I can authenticate without manual token management.

**Why this priority**: Service principal is the most common Azure authentication method for automated/production workloads. Must work identically after the HTTP library change.

**Independent Test**: Configure an Azure secret with service_principal provider and verify `mssql_azure_auth_test()` returns a valid token.

**Acceptance Scenarios**:

1. **Given** a valid Azure secret with service_principal provider (tenant_id, client_id, client_secret), **When** `mssql_azure_auth_test('secret_name')` is called, **Then** a valid access token is returned
2. **Given** invalid client credentials, **When** token acquisition is attempted, **Then** a clear error message is returned describing the authentication failure
3. **Given** the Azure AD endpoint is unreachable (network timeout), **When** token acquisition is attempted, **Then** a clear error message is returned within 30 seconds

---

### User Story 3 - Azure Device Code Flow (Priority: P2)

As a developer connecting to Azure SQL interactively, I need the device code authentication flow to work so that I can authenticate with MFA-enabled accounts.

**Why this priority**: Device code flow is used for interactive/development scenarios. Less critical than service principal but important for developer experience.

**Independent Test**: Initiate device code flow and verify the user code and verification URL are displayed, then poll for token completion.

**Acceptance Scenarios**:

1. **Given** a credential_chain secret with "interactive" provider, **When** authentication is initiated, **Then** the user sees a device code and verification URL
2. **Given** a device code has been issued, **When** the user completes authentication in the browser, **Then** the extension receives a valid access token
3. **Given** a device code has been issued, **When** the timeout expires without user action, **Then** a clear expiration error message is displayed

---

### User Story 4 - Reduced Build Dependencies (Priority: P2)

As an extension maintainer, I need to reduce the number of external binary dependencies so that the build is simpler, faster, and less prone to platform-specific linker issues.

**Why this priority**: Removing the curl dependency simplifies the build matrix and eliminates an entire class of Windows linker compatibility issues.

**Independent Test**: Verify that curl is no longer in the dependency manifest and the extension builds without it on all platforms.

**Acceptance Scenarios**:

1. **Given** the extension dependency manifest, **When** inspected, **Then** curl is not listed as a dependency
2. **Given** the extension source code, **When** searched for curl-specific includes or API calls, **Then** no references are found

---

### Edge Cases

- What happens when the Azure AD endpoint returns an unexpected HTTP status (e.g., 500, 503)?
- How does the system handle SSL certificate validation failures (e.g., corporate proxy with custom CA)?
- What happens when the HTTP response body is malformed (not valid JSON)?
- How does the system behave when the network connection drops mid-request?
- What happens with very large response bodies (unexpected server behavior)?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The extension MUST make HTTPS POST requests to Azure AD endpoints for OAuth2 token acquisition without using libcurl
- **FR-002**: The extension MUST support URL-encoding of OAuth2 parameters (client_id, client_secret, scope, device_code)
- **FR-003**: The extension MUST enforce a 30-second timeout on HTTP requests to Azure AD
- **FR-004**: The extension MUST return clear error messages for network failures, HTTP errors, and malformed responses
- **FR-005**: The extension MUST perform SSL/TLS certificate validation on HTTPS connections to Azure AD
- **FR-006**: The extension MUST NOT include libcurl as a build dependency
- **FR-007**: The extension MUST continue to use OpenSSL for TDS protocol TLS connections (no change)
- **FR-008**: The extension MUST NOT introduce any new external binary dependencies for HTTP functionality
- **FR-009**: The extension MUST build successfully on all supported platforms: Linux (GCC), macOS (Clang), Windows (MSVC with /MD dynamic CRT), Windows (MinGW)
- **FR-010**: All existing Azure authentication flows (service_principal, credential_chain with cli/env/interactive, access_token) MUST continue to work identically

## Assumptions

- DuckDB will continue to bundle cpp-httplib in `third_party/httplib/` in future versions
- The httplib header supports OpenSSL for HTTPS via a compile-time flag (confirmed: `CPPHTTPLIB_OPENSSL_SUPPORT`)
- The httplib OpenSSL mode uses a separate namespace from the non-SSL mode, preventing symbol conflicts with DuckDB core
- Azure AD OAuth2 endpoints only require HTTP POST with form-encoded body and standard headers — no advanced HTTP features (cookies, redirects, multipart, etc.) are needed
- OpenSSL static libraries alone (without libcurl) produce fewer or no unresolved `__imp_` symbols on MSVC with dynamic CRT

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The extension builds with zero errors on Windows MSVC using the community-extensions triplet (static libs + dynamic CRT /MD)
- **SC-002**: All Azure AD authentication methods produce identical results before and after the change (verified via `mssql_azure_auth_test()`)
- **SC-003**: The build dependency count is reduced by 1 (curl removed, no new binary dependencies added)
- **SC-004**: The extension builds successfully on all 4 platform targets (Linux GCC, macOS Clang, Windows MSVC, Windows MinGW)
- **SC-005**: No POSIX compatibility wrapper code is needed for curl-related linker symbols on Windows

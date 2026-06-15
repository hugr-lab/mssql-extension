# Feature Specification: Lazy GSSAPI/Kerberos Linking on Linux

**Feature Branch**: `task/161-271a9d`

**Created**: 2026-06-15

**Status**: Draft

**Input**: User description: "Make GSSAPI/Kerberos a lazy runtime dependency on Linux so the mssql extension loads without libgssapi_krb5.so.2 present (issue #161). Replace the link-time dependency on libgssapi_krb5 and libkrb5 with a dlopen/dlsym shim that is only triggered when authenticator=krb5 is actually selected. macOS keeps -framework GSS (system framework, always present). When Kerberos is requested but libgssapi_krb5.so.2 is absent at runtime, fail with a clear actionable error ("install libgssapi-krb5-2"). The extension must LOAD successfully on a clean Linux image with no Kerberos libraries installed."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Load the extension without Kerberos libraries (Priority: P1)

A user runs a minimal Linux image (e.g. a slim Python/Debian container) that does not have the MIT Kerberos runtime libraries installed. They install and load the community `mssql` extension to connect to SQL Server using SQL authentication or Azure AD — they never intend to use integrated/Kerberos authentication. The extension loads and works without requiring any additional system packages.

**Why this priority**: This is the exact failure reported in issue #161 and the entire reason for the feature. Every Linux community-binary user who does not use Kerberos is currently blocked from loading the extension at all. Fixing this unblocks the most common usage path.

**Independent Test**: On a clean Linux image with no `libgssapi-krb5-2` / `libkrb5-3` packages installed, `INSTALL mssql FROM community; LOAD mssql;` succeeds and a SQL-auth connection works end to end. No GSSAPI shared object is required to be present.

**Acceptance Scenarios**:

1. **Given** a Linux image with no MIT Kerberos runtime libraries installed, **When** the user runs `LOAD mssql`, **Then** the extension loads successfully with no "cannot open shared object file" error.
2. **Given** the extension is loaded on such an image, **When** the user opens a connection using SQL authentication or Azure AD, **Then** the connection succeeds with no reference to GSSAPI/Kerberos.
3. **Given** the extension is loaded on such an image, **When** the user inspects the binary's required shared libraries, **Then** `libgssapi_krb5.so.2` and `libkrb5.so.3` are NOT listed as hard load-time dependencies.

---

### User Story 2 - Use Kerberos when the runtime libraries are present (Priority: P1)

A user on a Linux host that has the MIT Kerberos runtime installed (and a valid ticket via `kinit`) connects to SQL Server using integrated authentication (`authenticator=krb5`, `Trusted_Connection=yes`, or `Integrated Security=SSPI`). Kerberos authentication works exactly as it does today.

**Why this priority**: The fix must not regress the shipped Kerberos feature (spec 042). Lazy loading is only acceptable if Kerberos still works fully when the libraries are available.

**Independent Test**: In the existing `test/kerberos/` docker-compose stack (KDC + SQL Server + test-client with Kerberos libraries installed), the Kerberos authentication tests pass unchanged.

**Acceptance Scenarios**:

1. **Given** a Linux host with the MIT Kerberos runtime installed and a valid ticket, **When** the user connects with `authenticator=krb5`, **Then** authentication completes successfully (CredCache mode).
2. **Given** the same host, **When** the user connects in keytab mode (`krb5-keytabfile=...`), **Then** authentication completes successfully.
3. **Given** the same host, **When** `mssql_kerberos_auth_test(host)` is called, **Then** it returns the SPN / principal / token size exactly as before.

---

### User Story 3 - Clear, actionable error when Kerberos is requested but unavailable (Priority: P2)

A user on a Linux image without the Kerberos runtime explicitly requests integrated authentication. Instead of a cryptic loader failure (which would no longer occur, since the extension loads fine), they receive a clear runtime error at connection time telling them which system package to install.

**Why this priority**: Once loading no longer fails up front, the failure mode for an actual Kerberos user moves to connection time. That error must be self-explanatory so users can fix it without filing a bug. Important, but secondary to making the extension load and keeping Kerberos working where supported.

**Independent Test**: On a clean Linux image with no Kerberos runtime, attempting a connection with `authenticator=krb5` produces an error message that names the missing library and the package to install.

**Acceptance Scenarios**:

1. **Given** a Linux image with no Kerberos runtime, **When** the user attempts a connection with `authenticator=krb5`, **Then** the connection fails with an error that names the missing shared object (`libgssapi_krb5.so.2`) and recommends installing the system Kerberos package (e.g. `libgssapi-krb5-2`).
2. **Given** the same image, **When** the user calls `mssql_kerberos_auth_test(host)`, **Then** it returns the same clear "library not found — install ..." message rather than crashing or returning a generic error.

---

### Edge Cases

- **Partial install**: The base GSSAPI library is present but a Kerberos mode requiring MIT-specific extensions (keytab/raw) is requested and the symbol is missing. The system must report which capability is unavailable rather than crash.
- **macOS unchanged**: On macOS the GSS system framework is always present; behavior (including the existing rejection of keytab/raw modes) is unchanged.
- **Concurrent first use**: Two connections request Kerberos for the first time simultaneously on worker threads. The runtime library is loaded exactly once without a data race.
- **Windows unaffected**: Windows uses SSPI (secur32) and has no GSSAPI/Kerberos shared-object dependency; this feature does not change Windows behavior.
- **Library present but unloadable** (corrupt/ABI-mismatched): The system reports a clear error rather than crashing.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The Linux community binary MUST load successfully (`LOAD mssql`) on a system where the MIT Kerberos runtime libraries are not installed.
- **FR-002**: The Linux binary MUST NOT declare the MIT Kerberos runtime libraries as hard load-time dependencies (they MUST NOT appear as required shared objects that block loading).
- **FR-003**: The system MUST locate and load the Kerberos runtime library only when integrated/Kerberos authentication is actually selected for a connection (or when the Kerberos test function is invoked) — never as a side effect of loading the extension or of non-Kerberos connections.
- **FR-004**: When the Kerberos runtime is available, the system MUST provide the full Kerberos authentication behavior shipped in spec 042 (CredCache, keytab, and raw modes on Linux per the existing platform matrix), with no functional regression.
- **FR-005**: When Kerberos authentication is requested but the runtime library cannot be loaded, the system MUST fail with an error that (a) states Kerberos support could not be loaded, (b) names the missing shared object, and (c) recommends the system package to install.
- **FR-006**: The Kerberos diagnostic functions (`mssql_kerberos_auth_test`, `mssql_kerberos_auth_test_secret`) MUST remain registered and return the same clear library-unavailable message when the runtime is absent, without crashing.
- **FR-007**: macOS behavior MUST remain unchanged: the system GSS framework is always present and continues to be used as today, including the existing rejection of keytab/raw modes.
- **FR-008**: Windows behavior MUST remain unchanged (SSPI path; no GSSAPI dependency).
- **FR-009**: First-time loading of the Kerberos runtime MUST be safe under concurrent connection attempts (no data race, loaded at most once).
- **FR-010**: All connection paths that do not use Kerberos (SQL auth, Azure AD/FEDAUTH) MUST NOT trigger any attempt to load the Kerberos runtime.

### Key Entities

- **Kerberos runtime library**: The platform Kerberos/GSSAPI shared library (`libgssapi_krb5.so.2` and its krb5 companion on Linux; the GSS system framework on macOS) providing the authentication primitives. On Linux it becomes a runtime-discovered dependency rather than a load-time one.
- **Integrated authentication request**: A connection whose selected authentication method is Kerberos/integrated (via `authenticator=krb5`, `Trusted_Connection=yes`, or `Integrated Security=SSPI`). This is the sole trigger for loading the Kerberos runtime.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: On a clean Linux image with zero Kerberos runtime packages installed, the extension loads and a non-Kerberos connection succeeds — reproducing issue #161's setup with a passing result (0 load failures).
- **SC-002**: The shipped Linux binary lists no hard load-time dependency on the Kerberos runtime libraries (verifiable by inspecting the binary's required shared objects).
- **SC-003**: 100% of the existing Kerberos authentication tests in the `test/kerberos/` stack pass after the change (no regression).
- **SC-004**: When Kerberos is requested without the runtime present, the error message names both the missing library and the package to install, in a single message — confirmed by a test asserting on the message text.
- **SC-005**: A user following the reported reproduction steps in issue #161 can install and load the extension with no additional system packages.

## Assumptions

- The MIT Kerberos runtime, when present on a Linux host, exposes the same GSSAPI/krb5 symbol names and ABI the extension already targets via spec 042; runtime discovery uses those same names.
- The standard library name `libgssapi_krb5.so.2` is the correct discovery target on supported Linux distributions (Debian/Ubuntu, RHEL-family). The krb5 companion library is discovered under its standard runtime name as well.
- macOS always ships the GSS system framework; no runtime discovery is needed there and the existing link arrangement is retained.
- The recommended install package referenced in the error message is the Debian/Ubuntu name (`libgssapi-krb5-2`), as that matches the environment in the issue report; equivalent package names for other distributions may be mentioned in documentation.
- This change is build/packaging and authentication-plumbing only; it introduces no new user-facing connection-string keys or settings.

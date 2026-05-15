# Feature Specification: Integrated Authentication (Kerberos / SSPI)

**Feature Branch**: `042-integrated-authentication`
**Created**: 2026-05-12
**Status**: Draft
**Input**: User description: "Add Integrated Authentication (Kerberos on POSIX, SSPI on Windows) to the mssql-extension"

## Problem Statement

Users with on-premises Active-Directory-joined SQL Server cannot connect from DuckDB today. The extension supports only two authentication paths:

1. **SQL Server authentication** — username and password sent in LOGIN7.
2. **Azure AD (FEDAUTH)** — JWT access token negotiated via PRELOGIN FEDAUTHREQUIRED.

Neither covers the standard enterprise on-prem case where the user runs `kinit user@REALM` (or simply has a Windows logon session) and expects the client to negotiate a Kerberos ticket with the SQL Server SPN. With pyodbc the same flow is one line:

```python
pyodbc.connect("DRIVER={ODBC Driver 18 for SQL Server};SERVER=sqlhost;DATABASE=db;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes")
```

The README currently states *"Windows Authentication: Only SQL Server authentication is supported"* — this feature removes that limitation by introducing **Integrated Authentication**: SPNEGO/Kerberos through GSSAPI on POSIX, and SPNEGO/Kerberos through SSPI on Windows. The user-visible surface and connection-string key names follow `microsoft/go-mssqldb`'s `integratedauth/` package verbatim, so users already familiar with that driver, with `mssql-jdbc`, or with pyodbc see the same names.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Connect from Linux/macOS with an existing Kerberos ticket (Priority: P1)

A developer on a Linux or macOS workstation has run `kinit user@REALM` and verified `klist` shows a valid TGT. They attach an on-prem SQL Server in DuckDB using `Trusted_Connection=yes`. The extension obtains a service ticket for `MSSQLSvc/<fqdn>:<port>`, drives the SSPI negotiation via TDS `0xED` tokens, and the connection succeeds. The credential cache mode is the v1 primary target — it is what the overwhelming majority of interactive users hit.

**Why this priority**: This is the single most common Integrated Auth scenario. The other two credential modes (keytab, raw) build on the same code path, but credential cache is the one most users will ever touch. Without it, users with on-prem AD-joined SQL Server still cannot connect.

**Independent Test**: After `kinit user@REALM`, run `ATTACH 'Server=sqlhost.example.com;Database=db;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes' AS db (TYPE mssql)` and a basic `SELECT TOP 1 name FROM db.sys.tables`. Delivers end-to-end Kerberos authentication.

**Acceptance Scenarios**:

1. **Given** a valid Kerberos ticket in the user's credential cache, **When** the user attaches with `Trusted_Connection=yes`, **Then** the connection succeeds and catalog queries return the expected rows.
2. **Given** no Kerberos ticket (the user has never run `kinit`), **When** the user attaches with `Trusted_Connection=yes`, **Then** ATTACH fails fast with an error containing `"no credentials cache"` or `"no Kerberos credentials available"`.
3. **Given** an expired Kerberos ticket, **When** the user attaches, **Then** ATTACH fails with an error containing `"ticket expired"` and suggesting `kinit`.
4. **Given** the user supplies both `Trusted_Connection=yes` and `User Id=...`, **When** they attach, **Then** ATTACH fails at validation time with an error explaining the two are mutually exclusive.
5. **Given** an attached database using Kerberos, **When** the user runs many concurrent queries, **Then** the connection pool reuses authenticated connections identically to SQL-auth — no per-query re-authentication.

---

### User Story 2 - Connect from Windows with the current logon session (Priority: P1)

A user on a Windows workstation logged into a domain account attaches an on-prem SQL Server. The extension uses SSPI (`secur32.dll`, `InitializeSecurityContext`) to acquire a service ticket as the current Windows principal and negotiates via TDS `0xED` tokens. No `kinit` step is needed — the OS already holds the ticket.

**Why this priority**: This is the corresponding case for Windows users and is, in practice, the most common deployment surface for SQL Server (Windows clients, Windows-domain-joined SQL Server). Same priority as US1.

**Independent Test**: On a domain-joined Windows host, run `ATTACH 'Server=sqlhost.example.com;Database=db;Trusted_Connection=yes' AS db (TYPE mssql)` and verify a basic query.

**Acceptance Scenarios**:

1. **Given** the user is logged into a domain account on Windows, **When** they attach with `Trusted_Connection=yes`, **Then** the connection succeeds without any extra configuration.
2. **Given** an attached Kerberos-backed connection, **When** `mssql_pool_stats()` is queried, **Then** the same statistics (active, idle, total) are reported as for SQL-auth connections.
3. **Given** the user is logged into a *local* (non-domain) account on Windows, **When** they attach with `Trusted_Connection=yes` to a remote domain server, **Then** the connection fails with an SSPI error explaining no Kerberos credentials are available for the target.

---

### User Story 3 - Service-account / CI scenarios using a keytab (Priority: P2)

A batch job or CI pipeline cannot run `kinit` interactively. It points the extension at a keytab file containing a service principal's long-term keys, and the extension acquires its own initial credentials before talking to SQL Server.

**Why this priority**: This is the standard automation pattern (Airflow workers, container pipelines, scheduled ETL). Less common than interactive use, but critical for production deployments.

**Independent Test**: With a valid keytab for `svc_etl@REALM`, run `ATTACH 'Server=...;authenticator=krb5;krb5-keytabfile=/etc/keytabs/svc_etl.keytab;krb5-realm=REALM;User Id=svc_etl' AS db (TYPE mssql)` and verify a query.

**Acceptance Scenarios**:

1. **Given** a valid keytab and matching realm, **When** the user attaches with `krb5-keytabfile`, `krb5-realm`, and `User Id`, **Then** the connection succeeds without depending on any pre-existing credential cache.
2. **Given** a keytab that does not contain a key for the supplied user@realm, **When** the user attaches, **Then** ATTACH fails with an error naming the principal and the keytab path.
3. **Given** the keytab file is unreadable (permissions), **When** the user attaches, **Then** ATTACH fails with a clear filesystem error.

---

### User Story 4 - Raw credentials passed in the connection string (Priority: P3)

A user supplies a domain username, password, and realm directly in the connection string (no system Kerberos config required). The extension performs the AS-REQ/AS-REP itself via GSSAPI and then proceeds with the service ticket exchange.

**Why this priority**: Rare in practice — most users have either a ticket or a keytab. Kept for symmetry with `go-mssqldb` and for the case where `/etc/krb5.conf` is awkward to configure (containers without persistent config).

**Independent Test**: With known-good AD credentials and a reachable KDC, run `ATTACH 'Server=...;authenticator=krb5;User Id=alice;Password=...;krb5-realm=EXAMPLE.COM' AS db (TYPE mssql)`.

**Acceptance Scenarios**:

1. **Given** valid domain credentials and a reachable KDC, **When** the user attaches with raw credentials and `authenticator=krb5`, **Then** the connection succeeds.
2. **Given** wrong password, **When** the user attaches, **Then** ATTACH fails with a clear `"preauthentication failed"` or `"bad password"` error sourced from the GSSAPI minor status code.
3. **Given** unreachable KDC, **When** the user attaches, **Then** ATTACH fails with a `"KDC unreachable"` error.

---

### User Story 5 - Existing SQL auth and Azure AD paths are untouched (Priority: P1, regression)

Existing users — SQL auth and Azure AD — must see no observable change. The new `IAuthenticator` abstraction refactors the current code paths first as a no-behavior-change step; Kerberos is layered on top afterwards.

**Why this priority**: Regression-blocking. If existing flows break, the feature is not shippable regardless of how well Kerberos works.

**Independent Test**: All existing integration tests in `test/sql/attach/`, `test/sql/azure/`, `test/sql/integration/`, `test/sql/transaction/`, `test/sql/insert/`, `test/sql/dml/` pass unchanged.

**Acceptance Scenarios**:

1. **Given** existing SQL-auth secrets, **When** the user attaches with the same syntax, **Then** behavior is identical to before.
2. **Given** existing Azure AD secrets (service principal, CLI, device code, manual token), **When** the user attaches, **Then** authentication proceeds identically — including FEDAUTHREQUIRED handshake, token caching, and routing.
3. **Given** the `mssql_pool_stats()` view, **When** queried, **Then** new fields (if any) are documented; existing fields keep their semantics.

---

### Edge Cases

- **Clock skew > 5 minutes between client and KDC.** Kerberos rejects with `"clock skew too great"`. The error must surface verbatim — users need to recognize and fix it.
- **SPN not registered in AD.** The KDC returns `S_PRINCIPAL_UNKNOWN`. Error must include the SPN that was attempted (`MSSQLSvc/<fqdn>:<port>`) so the AD admin can `setspn -L`.
- **Server requires encryption AND Kerberos auth fails.** The TLS handshake succeeds but LOGIN7 fails. Error must distinguish "TLS failed" from "auth failed" so users don't chase the wrong root cause.
- **Connection-string conflicts.** `Trusted_Connection=yes` together with `User Id`, `Password`, `authenticator=ActiveDirectoryServicePrincipal`, or an Azure secret must fail at ATTACH-time validation with a single clear error, not at LOGIN7 time.
- **Custom port versus instance name.** SPN form depends on the dispatcher: `MSSQLSvc/host:1433` for fixed port, `MSSQLSvc/host:INSTANCE` for named instance.
- **Multi-homed hosts / canonical names.** GSSAPI may rewrite the hostname via DNS canonicalization (`rdns = true` in `krb5.conf`). Document this; do not work around it in code.
- **macOS Heimdal vs MIT Kerberos.** macOS ships Heimdal-flavored `GSS.framework`. It is API-compatible at the GSSAPI level used here. No special-casing required.
- **Connection pool reuse.** A pooled connection authenticated via Kerberos must NOT be re-authenticated when reused — the TDS session state remains valid until idle eviction. Identical to SQL-auth.
- **`TrustServerCertificate=yes` combined with Kerberos.** Kerberos provides server identity verification at the auth layer; `TrustServerCertificate=yes` only relaxes TLS certificate validation. Both apply independently.
- **Channel binding (EPA).** Out of scope for v1 — see Out of Scope below.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST support `authenticator=krb5` (POSIX) and `authenticator=winsspi` (Windows) as authentication selectors in connection strings, ADO.NET style, URI style, and MSSQL secrets.
- **FR-002**: System MUST treat `Trusted_Connection=yes`, `Integrated Security=SSPI`, and `Integrated Security=true` as aliases that resolve to `authenticator=krb5` on POSIX and `authenticator=winsspi` on Windows.
- **FR-003**: System MUST parse the following connection-string keys verbatim from `microsoft/go-mssqldb`: `authenticator`, `krb5-configfile`, `krb5-keytabfile`, `krb5-credcachefile`, `krb5-realm`, `krb5-dnslookupkdc`, `krb5-udppreferencelimit`. The same keys MUST be accepted on MSSQL secrets.
- **FR-004**: On POSIX, system MUST acquire credentials in three modes: credential cache (default, no extra keys), keytab (`krb5-keytabfile` + principal), and raw credentials (`User Id` + `Password` + `krb5-realm`).
- **FR-005**: System MUST construct the service principal name as `MSSQLSvc/<fqdn>:<port>` by default, derived from the `host` and `port` fields. An override key `service_principal_name` MUST be honored when supplied.
- **FR-006**: System MUST set the `fIntSecurity` bit (bit 7, `0x80`) of `OptionFlags2` in LOGIN7 when Integrated Auth is selected, MUST place the initial SPNEGO blob in the LOGIN7 `SSPI` field at offset 36 (using `cbSSPILong` at offset 86 when the blob exceeds 65535 bytes), and MUST drive the continuation loop by consuming TDS `0xED` (SSPI) tokens until `gss_init_sec_context` / `InitializeSecurityContext` returns "complete" and the server sends `DONE`.
- **FR-007**: System MUST NOT include username or password in LOGIN7 when Integrated Auth is selected — user identity is conveyed entirely inside the SSPI blob.
- **FR-008**: System MUST reject conflicting connection-string combinations at ATTACH-time validation, before any TDS traffic. Conflicts include: `Trusted_Connection=yes` with any of `User Id`, `Password`, `authenticator=ActiveDirectory*`, or an Azure secret; `authenticator=krb5` on Windows together with `authenticator=winsspi`.
- **FR-009**: System MUST expose Kerberos errors with the GSSAPI/SSPI minor status code rendered into human-readable text via `gss_display_status` / `FormatMessage`, prefixed with `MSSQL Kerberos auth failed:`.
- **FR-010**: System MUST refactor existing SQL-auth and Azure-AD code behind a new `IAuthenticator` interface as a precursor step. The refactor MUST be a no-behavior-change commit (all existing tests pass unchanged) before any Kerberos code is added.
- **FR-011**: System MUST treat Kerberos-authenticated connections identically to SQL-auth connections at the pool layer — same lifecycle, same idle timeout, same `mssql_pool_stats()` semantics, no per-query re-authentication.
- **FR-012**: System MUST compile the Kerberos authenticator only when `ENABLE_KRB5=ON` (default `ON` for POSIX targets, ignored on Windows). The SSPI authenticator MUST compile only on `_WIN32`. The TDS layer MUST link without the Kerberos authenticator when `ENABLE_KRB5=OFF`, with `Trusted_Connection=yes` producing a clear "extension built without Kerberos support" error.
- **FR-013**: No header in `src/tds/auth/` may include any DuckDB headers — the TDS layer (including auth providers) is reusable outside DuckDB.

### Key Entities

- **`IAuthenticator`**: TDS-layer interface modeled on `go-mssqldb`'s `integratedauth.IntegratedAuthenticator`. Three methods: `InitialBytes() -> vector<uint8_t>`, `NextBytes(server_blob) -> vector<uint8_t>`, `Free()`. Optional `SetChannelBinding(ChannelBindings)` for future EPA support. The existing `AuthenticationStrategy` (`PreloginOptions`, `Login7Options`, FEDAUTH hooks) wraps this new interface for the SQL-auth and Azure-AD paths; the Kerberos path uses the new interface directly.
- **`Krb5Authenticator`**: POSIX implementation of `IAuthenticator` via system GSSAPI (`libgssapi_krb5` on Linux, `GSS.framework` on macOS). Uses SPNEGO mechanism (`1.3.6.1.5.5.2`), name type `GSS_C_NT_HOSTBASED_SERVICE` (value 2).
- **`WinSspiAuthenticator`**: Windows implementation of `IAuthenticator` via `secur32.dll` (`AcquireCredentialsHandle` + `InitializeSecurityContext` with `Negotiate` package).
- **Service Principal Name (SPN)**: `MSSQLSvc/<fqdn>:<port>` (default port instance) or `MSSQLSvc/<fqdn>:<instance_name>` (named instance). Derivable from `host` and `port`; override via `service_principal_name` secret/connection-string key.
- **Credential acquisition mode**: One of `CredCache` (default), `Keytab` (with file path + principal), `Raw` (user + password + realm).

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A POSIX user with a valid TGT (`kinit user@REALM` succeeded, `klist` shows a TGT) can `ATTACH 'Server=...;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes' AS db (TYPE mssql)` and run `SELECT TOP 1 name FROM db.sys.tables` end-to-end.
- **SC-002**: A Windows user logged into a domain account can `ATTACH 'Server=...;Trusted_Connection=yes' AS db (TYPE mssql)` and run the same query.
- **SC-003**: A keytab-based service principal can ATTACH and query without depending on any pre-existing credential cache.
- **SC-004**: All existing SQL-auth and Azure-AD integration tests pass unchanged — no regressions in `test/sql/attach/`, `test/sql/azure/`, `test/sql/integration/`, `test/sql/transaction/`, `test/sql/insert/`, `test/sql/dml/`.
- **SC-005**: Authentication failure cases (no ticket, expired ticket, wrong SPN, clock skew, KDC unreachable, conflicting connection-string keys) each produce a distinct, actionable error message that names the underlying GSSAPI/SSPI status.
- **SC-006**: Kerberos-authenticated connections reuse identically to SQL-auth — `mssql_pool_stats()` reports the same numbers and no per-query re-authentication is observable in trace logs.
- **SC-007**: All four target platforms (Linux x86_64, Linux ARM64, macOS ARM64, Windows x64) build in CI with Kerberos support enabled.

## Assumptions

- The user has already done the Kerberos setup outside DuckDB: `/etc/krb5.conf` (or `~/.config/krb5.conf`) points at the correct KDC, the SPN `MSSQLSvc/<fqdn>:<port>` is registered in AD, the user has run `kinit` (or the equivalent on Windows is implicit via logon).
- DuckDB itself does NOT distribute a Kerberos client. POSIX users install `krb5-user` (Debian/Ubuntu), `krb5-workstation` (RHEL/Fedora), or use the macOS built-in `GSS.framework`. Windows ships SSPI as part of the OS.
- The TDS protocol layer's existing PRELOGIN / LOGIN7 / packet-fragmentation infrastructure (used by the FEDAUTH path) is reused as-is. The SSPI continuation token loop is structurally identical to the existing FEDAUTHINFO loop — handle `0xED` instead of `0xEE`, write back into the next outbound packet instead of the FEDAUTH extension.
- `go-mssqldb`'s `integratedauth/` package is the structural reference. Connection-string key names are copied verbatim. The `IAuthenticator` interface shape (`InitialBytes` / `NextBytes` / `Free`) is the same three methods.
- Channel binding / Extended Protection for Authentication (EPA) is documented in code as a no-op stub for v1 (`SetChannelBinding` returns without effect). Servers that *require* EPA will reject Kerberos auth with a specific error — those users get a v2 escalation, not silent failure.

## Out of Scope (v1)

- **NTLM authentication.** Servers without Kerberos SPNs that fall back to NTLM are not supported. Workaround: register an SPN, then Kerberos works.
- **Channel binding / EPA.** Stubbed; servers with `Extended Protection = Required` will reject the connection.
- **Cross-realm trust corner cases.** Cross-realm referrals are supported transparently by GSSAPI/SSPI, but explicit configuration (`krb5-realm` differing from the principal's realm) for transit policies is not tested.
- **Constrained delegation (S4U2Self / S4U2Proxy).** Not exposed.
- **MIT Kerberos GSSAPI vs Heimdal subtle differences.** Tested with MIT (Linux container) and the system framework on macOS. No special-casing.
- **Vendoring/statically linking Kerberos.** POSIX dynamic-link to system `libgssapi_krb5`. The vcpkg `krb5` port is a documented fallback if a future hardened build requires static linkage.

## Open Questions

1. **Default for `ENABLE_KRB5` in the published community-extension build.** ON gives users Kerberos out of the box but adds a runtime dependency on `libgssapi_krb5` (present on virtually all Linux distros and built into macOS, but adds a CI matrix dimension). OFF keeps the published build hermetic but means `Trusted_Connection=yes` errors out until users rebuild. Recommend ON for source builds; default for the published artifact pending DuckDB community-extension policy review.
2. **Windows SSPI in v1 or v2?** The POSIX path is the dominant user case for this extension (DuckDB CLI is heavily used on Linux/macOS). Shipping POSIX-only in v1 and Windows SSPI in v2 is a viable scope cut if the SSPI path adds material risk. Decision should be made before T009 of `tasks.md`.
3. **Channel binding (EPA) — v1 or v2?** Some hardened SQL Server deployments enforce EPA. Stubbing in v1 with a clear error is the proposed default. Confirm no enterprise pilot user needs EPA on day one.
4. **`service_principal_name` override location.** Either ATTACH option, secret field, or both. Recommend both, parallel to `schema_filter` / `table_filter`. Confirm.
5. **Error message wording for the `ENABLE_KRB5=OFF` build path.** Proposal: `"MSSQL Error: This build of the mssql extension was compiled without Kerberos support. Rebuild with -DENABLE_KRB5=ON or use SQL authentication."` — confirm tone/phrasing.

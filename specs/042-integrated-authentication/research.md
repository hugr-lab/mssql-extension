# Research: Integrated Authentication (Kerberos / SSPI)

**Feature**: 042-integrated-authentication
**Date**: 2026-05-12

## R1: TDS LOGIN7 protocol mechanics for Integrated Auth

**Decision**: Set `OptionFlags2.fIntSecurity` (bit 7, mask `0x80`) on LOGIN7. Write the initial SPNEGO blob into the LOGIN7 `SSPI` field (offset variables at bytes 36/38, data appended to the variable-length section after `ServerName`). For blobs larger than 65 535 bytes, set the standard `cbSSPI` to `0xFFFF` and write the true length into `cbSSPILong` at LOGIN7 offset 86. Drive the continuation loop by reading TDS `SSPI` tokens (token type `0xED`, per `[MS-TDS]` §2.2.7.21) from the server and writing the GSSAPI-output blob into the next outbound packet (packet type `0x11`, "SSPI Message"). Loop terminates when `gss_init_sec_context` / `InitializeSecurityContext` returns `GSS_S_COMPLETE` / `SEC_E_OK` and the server emits `LOGINACK` followed by `DONE`.

**Rationale**: This is the path defined in `[MS-TDS]` §2.2.6.4 (LOGIN7) and §2.2.6.16 (SSPI Message). `go-mssqldb`'s implementation in `msdsn/conn_str.go` + `tds.go` does exactly this, and it's the same loop FreeTDS uses in `src/tds/login.c`. Our existing FEDAUTH continuation loop is structurally identical (handling `0xEE` FEDAUTHINFO tokens); we add a sibling branch for `0xED`.

**Key spec references**:
- `[MS-TDS]` §2.2.6.4 LOGIN7 — `OptionFlags2` and `ibSSPI` / `cbSSPI` / `cbSSPILong` fields
- `[MS-TDS]` §2.2.6.16 SSPI Message — outbound continuation packet
- `[MS-TDS]` §2.2.7.21 SSPI Token Stream — `0xED` token format

**Code paths to extend**:
- `src/tds/tds_protocol.cpp` LOGIN7 builder — add `fIntSecurity` flag setter and SSPI-field writer
- `src/tds/tds_token_parser.cpp` — add `0xED` token recognition
- `src/tds/tds_connection.cpp` `Login()` — add SSPI continuation loop (mirror of existing FEDAUTH loop near line 466)

**Alternatives considered**:
- Skip LOGIN7 SSPI field and use SSPI Message packet immediately: rejected — `[MS-TDS]` requires the initial blob in LOGIN7.
- Reuse the FEDAUTH path with a different token type: rejected — FEDAUTH carries a complete pre-acquired token; SPNEGO requires a multi-round exchange.

## R2: SPN derivation for SQL Server

**Decision**: Default SPN is `MSSQLSvc/<fqdn>:<port>` for fixed-port instances, `MSSQLSvc/<fqdn>:<instance_name>` for named instances. The `<fqdn>` is taken from the user's `host` field after DNS canonicalization (`getaddrinfo` `AI_CANONNAME`). Override the entire SPN via the `service_principal_name` secret/connection-string key when defaults are wrong.

When passing the SPN to GSSAPI, use name type `GSS_C_NT_HOSTBASED_SERVICE` (OID `1.2.840.113554.1.2.1.4`, numeric value 2). The string form passed to `gss_import_name` is `"MSSQLSvc@<fqdn>"` (note: `@` separator, NOT `/`). GSSAPI translates this internally into the slash form when querying the KDC.

**Rationale**: This is the form SQL Server registers in AD by default — see Microsoft documentation "Register a Service Principal Name for Kerberos Connections". The `@`-vs-`/` distinction is a well-documented FreeTDS pitfall (see `src/tds/gssapi.c` comments and FreeTDS commit `b4f7e8a` from 2016). Using name type 1 (`GSS_C_NT_USER_NAME`) silently produces tickets the server cannot decrypt.

**Code paths**:
- New helper: `MakeSpn(host, port, instance_name) -> string` in `src/tds/auth/krb5_authenticator.cpp`
- Accept `service_principal_name` in `ParseConnectionString` / `ParseUri` / secret parser

**Alternatives considered**:
- Always use port form even for named instances: incorrect; named instances register with the instance name, not port.
- Require the user to always supply the SPN: rejected — defeats the "just works" goal of `Trusted_Connection=yes`.

## R3: Library choice and CMake wiring

**Decision (POSIX)**: Dynamic link to system `libgssapi_krb5`. Linux runtime dependency: `libgssapi-krb5-2` (already installed almost everywhere). Linux build dependency: `libkrb5-dev`. macOS: built-in `GSS.framework`, link with `-framework GSS`. Discover via `pkg-config --cflags --libs krb5-gssapi` on Linux and `find_library(GSS GSS)` on macOS.

**Decision (Windows)**: Link `secur32.lib` (in the Windows SDK). No third-party dependency.

**Rationale**: Kerberos configuration (`/etc/krb5.conf`, KDC discovery, ticket cache location) is system-wide. Statically linking GSSAPI provides no portability benefit because the *config* is system-wide regardless. The vcpkg `krb5` port exists (port version 1.21.x at time of writing) and is the documented fallback if a future hardened build requires the static path, but it is NOT the default.

**CMake skeleton** (sketched here; full diff lives in `tasks.md` T006):
```cmake
option(ENABLE_KRB5 "Enable Kerberos integrated authentication" ON)

if(ENABLE_KRB5)
    if(WIN32)
        target_link_libraries(${EXTENSION_NAME} PRIVATE secur32)
        target_compile_definitions(${EXTENSION_NAME} PRIVATE MSSQL_ENABLE_SSPI=1)
    elseif(APPLE)
        find_library(GSS_FRAMEWORK GSS)
        target_link_libraries(${EXTENSION_NAME} PRIVATE ${GSS_FRAMEWORK})
        target_compile_definitions(${EXTENSION_NAME} PRIVATE MSSQL_ENABLE_KRB5=1)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GSSAPI REQUIRED krb5-gssapi)
        target_include_directories(${EXTENSION_NAME} PRIVATE ${GSSAPI_INCLUDE_DIRS})
        target_link_libraries(${EXTENSION_NAME} PRIVATE ${GSSAPI_LIBRARIES})
        target_compile_definitions(${EXTENSION_NAME} PRIVATE MSSQL_ENABLE_KRB5=1)
    endif()
endif()
```

**Alternatives considered**:
- vcpkg `krb5` static port as default: rejected — adds ~3 MB to the artifact, complicates build, provides no functional benefit because config is still system-wide.
- Dlopen GSSAPI at runtime: rejected for v1 — adds complexity, lets a missing `libgssapi_krb5` produce a runtime error instead of a load-time error. Reconsider if community-extension publishing policy requires zero runtime deps.

## R4: Mechanism — SPNEGO, not raw Kerberos

**Decision**: Negotiate via SPNEGO (`1.3.6.1.5.5.2`), not raw Kerberos (`1.2.840.113554.1.2.2`).

**Rationale**: SQL Server's SSPI implementation expects SPNEGO blobs — it uses the `Negotiate` security package, not raw `Kerberos`. Sending raw Kerberos blobs causes the server to drop the connection. `go-mssqldb`, `mssql-jdbc`, and FreeTDS all use SPNEGO. This is also what Windows clients send when going through `secur32.dll` with the `Negotiate` package.

**GSSAPI call**: `gss_init_sec_context` with `desired_mech = SPNEGO`. The SPNEGO mechanism is built into MIT Kerberos since ~1.6 and macOS GSS.framework natively.

**Code paths**: `Krb5Authenticator::InitialBytes()` passes `gss_OID_desc spnego = { 6, "\x2b\x06\x01\x05\x05\x02" }` (DER-encoded form of OID `1.3.6.1.5.5.2`).

## R5: `IAuthenticator` interface shape

**Decision**: Three methods, modeled verbatim on `go-mssqldb`'s `integratedauth.IntegratedAuthenticator`:

```cpp
// src/include/tds/auth/iauthenticator.hpp - NO duckdb headers permitted
namespace duckdb { namespace tds {
class IAuthenticator {
public:
    virtual ~IAuthenticator() = default;
    virtual std::vector<uint8_t> InitialBytes() = 0;
    virtual std::vector<uint8_t> NextBytes(const std::vector<uint8_t>& server_blob) = 0;
    virtual void Free() = 0;
    // Optional - default no-op for v1
    virtual void SetChannelBinding(const ChannelBindings& /*cb*/) {}
};
}}
```

**Rationale**: Mirrors `go-mssqldb` to give users (and code reviewers) one-to-one structural correspondence. The three-method shape is exactly what SPNEGO needs: an initial blob, zero or more continuations, and explicit resource release (GSSAPI's `gss_delete_sec_context` and SSPI's `DeleteSecurityContext` are not RAII-friendly across the underlying handle types).

**Interaction with existing `AuthenticationStrategy`**: The existing strategy interface (`PreloginOptions`, `Login7Options`, `GetFedAuthToken`) is FEDAUTH-shaped — it does NOT model a multi-round handshake. We layer the new `IAuthenticator` *underneath* the strategy:

- `SqlAuthStrategy` / `FedAuthStrategy` continue to exist and return `Login7Options` as before; they hold a null `IAuthenticator` (no SSPI loop needed).
- New `IntegratedAuthStrategy` returns `Login7Options{ include_sspi = true }` and supplies an `IAuthenticator` instance. `TdsConnection::Login()` checks for the authenticator and runs the `0xED` loop.

This keeps the existing two strategies' diff to roughly zero while adding the new path cleanly.

**Alternatives considered**:
- Replace `AuthenticationStrategy` with `IAuthenticator` everywhere: rejected — would require rewriting the FEDAUTH path's pre-acquired-token logic into a single-shot `InitialBytes()` and break test coverage. Net no benefit.
- Make `IAuthenticator` a peer of `AuthenticationStrategy`: this is what we do — clean separation, small refactor.

## R6: Reference implementations to read (and which not to copy)

| Project | License | Relevance | Action |
|---------|---------|-----------|--------|
| `microsoft/go-mssqldb` — `integratedauth/`, `integratedauth/krb5/`, `integratedauth/winsspi/` | BSD-3 | **Primary structural reference.** Three-method interface, connection-string keys, error mapping, both GSSAPI and SSPI implementations. | Read; copy structure and key names; do not copy code (different language). |
| FreeTDS — `src/tds/gssapi.c` | LGPL-2 | Secondary reference. Older; ships with documented caveats in its own comments (SPN name-type pitfall, channel binding stub). | Read for "what to avoid"; do not vendor. |
| `maharmstone/tdscpp` — `-DENABLE_KRB5=ON` path | LGPL-3.0 | C++20 implementation; closest existing C++ design. | Read for design ideas only — license is incompatible with extension; do not copy code. |
| `microsoft/mssql-jdbc` — Kerberos path | MIT | Java reference for connection-string semantics, error wording, EPA. | Read for error wording; do not copy code. |
| Microsoft Learn: "SSPI/Kerberos Interoperability with GSSAPI" | docs | Canonical SSPI-vs-GSSAPI call mapping table. | Cite directly in code comments where the two APIs diverge. |

## R7: Credential acquisition modes

`go-mssqldb` supports three acquisition modes via the `krb5-*` connection-string keys. We adopt the same three:

| Mode | Connection-string trigger | GSSAPI calls | Use case |
|------|---------------------------|--------------|----------|
| **CredCache** (default) | `authenticator=krb5` with no `krb5-keytabfile` and no `Password` | `gss_acquire_cred(GSS_C_NO_NAME, GSS_C_INDEFINITE, SPNEGO, GSS_C_INITIATE, ...)` — picks up the default ccache from `KRB5CCNAME` or `/tmp/krb5cc_<uid>`. | Interactive user after `kinit`. |
| **Keytab** | `krb5-keytabfile=/path/to/file.keytab` + `User Id=svc@REALM` (or `User Id` + `krb5-realm`) | `krb5_kt_resolve` → `krb5_get_init_creds_keytab` → `gss_acquire_cred_from` with `keytab` element. | Service account, CI/CD, container. |
| **Raw** | `User Id=alice;Password=...;krb5-realm=EXAMPLE.COM` + `authenticator=krb5` | `krb5_get_init_creds_password` → `gss_acquire_cred_from` with `ccache=MEMORY:`. | KDC reachable but no client Kerberos config available. |

**Windows note**: SSPI has no analog of keytab / raw modes — `AcquireCredentialsHandle(NULL, "Negotiate", SECPKG_CRED_OUTBOUND, ...)` always uses the current logon session's credentials. Specifying `krb5-keytabfile` together with `authenticator=winsspi` is an error.

**Code paths**: `Krb5Authenticator` constructor branches on which fields are populated and selects the matching code path. Decision made up-front in the constructor, not lazily inside `InitialBytes()`, so configuration errors surface at ATTACH time.

## R8: Error taxonomy

Map underlying GSSAPI/SSPI status codes to user-facing messages. Every message MUST include the raw status text from `gss_display_status` (POSIX) or `FormatMessage` (Windows) so users can find it via web search. Prefix all messages with `MSSQL Kerberos auth failed:`.

| Cause | GSSAPI minor status | SSPI status | User-facing message |
|-------|---------------------|-------------|---------------------|
| No ticket cache | `KRB5_CC_NOTFOUND` / `KRB5_FCC_NOFILE` | `SEC_E_NO_CREDENTIALS` | `"No credentials cache found. Run 'kinit <user>@<REALM>' first."` |
| Expired ticket | `KRB5KRB_AP_ERR_TKT_EXPIRED` | `SEC_E_CONTEXT_EXPIRED` | `"Kerberos ticket expired. Run 'kinit' to refresh."` |
| SPN not found | `KRB5KDC_ERR_S_PRINCIPAL_UNKNOWN` | `SEC_E_TARGET_UNKNOWN` | `"Server principal '<SPN>' not registered in KDC. Verify with 'setspn -L <account>'."` |
| Clock skew | `KRB5KRB_AP_ERR_SKEW` | `SEC_E_TIME_SKEW` | `"Clock skew between client and KDC exceeds 5 minutes. Sync system clock (ntp/chrony)."` |
| KDC unreachable | `KRB5_KDC_UNREACH` | `SEC_E_NO_AUTHENTICATING_AUTHORITY` | `"KDC unreachable. Check /etc/krb5.conf and network connectivity to the AD domain controllers."` |
| Bad password (raw mode) | `KRB5KRB_AP_ERR_BAD_INTEGRITY` | n/a | `"Kerberos preauthentication failed (bad password)."` |
| Wrong realm in keytab | `KRB5_KT_KVNO_NOTFOUND` | n/a | `"No matching key in keytab '<path>' for principal '<user@realm>'."` |
| Server rejected (LOGINACK ERROR) | propagated from server `0xAA` ERROR token | propagated from server | `"SQL Server rejected Kerberos credentials: <server error text>"` |

These are surfaced from the strategy's `InitialBytes()` / `NextBytes()` (throw `IOException` with the formatted message), bubble up to `TdsConnection::Login()`, and surface to the user at ATTACH time. Same fail-fast pattern as existing `ValidateConnection` errors.

## R9: Test infrastructure — containerized KDC

**Decision**: Add a second Docker container (`docker/kerberos/`) running MIT KDC alongside the existing SQL Server container. The KDC is configured with realm `EXAMPLE.COM`, a SQL Server SPN registered, and a test principal `alice@EXAMPLE.COM` with a known password. Tests use a per-test ccache by setting `KRB5CCNAME` and run `kinit -k -t /keytabs/alice.keytab alice@EXAMPLE.COM` before each test.

**Rationale**: This is the only way to write deterministic integration tests for Kerberos. CI runners cannot rely on an external AD. MIT KDC in a container is the standard pattern — see `microsoft/go-mssqldb`'s `.github/workflows/kerberos.yml` for the same pattern.

**Files**:
- `docker/kerberos/Dockerfile` — `FROM ubuntu:24.04`, installs `krb5-kdc krb5-admin-server`, copies a pre-baked `kdc.conf` and a startup script
- `docker/kerberos/krb5.conf` — single realm `EXAMPLE.COM`, KDC = container hostname
- `docker/kerberos/init-kdc.sh` — `kdb5_util create -s -P masterpw`, `kadmin.local -q "addprinc -pw password alice@EXAMPLE.COM"`, etc.
- `docker/sql-server-kerberos/init.sql` — registers the SPN, creates a test database
- New `make` target `make docker-kerberos-up` — brings up both containers with a shared network

**Tests**: `test/sql/integrated_auth/krb5_basic.test` and friends. Tagged `[kerberos]`, skipped unless `MSSQL_KERBEROS_TEST=1`.

## R10: Channel binding (EPA) — v1 stub

**Decision**: Define a `ChannelBindings` struct (empty-by-design for v1) and provide `IAuthenticator::SetChannelBinding(const ChannelBindings&)` as a virtual no-op. Log a debug-level message when the server's PRELOGIN advertises `MARS=1` with EPA — do not fail. If a server *requires* EPA, the LOGIN7 response will be an ERROR token; surface the server's error verbatim with a note appended: `"(Hint: server may require channel binding / Extended Protection for Authentication, which is not implemented in v1.)"`.

**Rationale**: Channel binding requires extracting the TLS peer certificate's signature and binding it into the SPNEGO blob (`tls-server-end-point` channel binding type, RFC 5929). This is mechanically straightforward but adds a TLS-internals dependency we'd rather not take on for the first cut. Stubbing keeps the interface forward-compatible.

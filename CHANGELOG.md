# Changelog

All notable changes to the DuckDB MSSQL Extension are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Integrated Authentication (Kerberos)** for POSIX hosts (spec 042, phases 1-3).
  Adds the `IAuthenticator` multi-round interface, parser support for the
  `microsoft/go-mssqldb` connection-string surface, LOGIN7 `fIntSecurity`
  wiring, `0xED` SSPI continuation tokens, and a POSIX Kerberos backend via
  system GSSAPI. Self-contained `test/kerberos/` docker-compose stack (KDC +
  SQL Server + test-client) — no real Active Directory required.
  - New connection-string keys (verbatim from `go-mssqldb`): `authenticator`,
    `krb5-configfile`, `krb5-keytabfile`, `krb5-credcachefile`, `krb5-realm`,
    `service_principal_name`.
  - Aliases: `Trusted_Connection=yes`, `Integrated Security=SSPI/true` —
    resolve to `krb5` on POSIX, `winsspi` on Windows.
  - Three credential modes on POSIX (Linux only for keytab + raw):
    credential cache (default, uses `kinit` ticket), keytab, raw credentials
    (secret-only).
  - macOS supports credential-cache mode (uses `GSS.framework`); keytab and
    raw modes are rejected at construction time with a clear error pointing
    at the Linux container path.
  - Verbatim GSSAPI status text in errors plus actionable hints (no
    ccache → run kinit; clock skew → ntp/chrony; SPN not registered →
    setspn -L; etc.) per spec 042 R8.
  - New end-user documentation: `Kerberos.md` (mirrors `AZURE.md`).
  - Windows SSPI (`winsspi` authenticator) is Phase 4 — pending. WSL2 Ubuntu
    is the supported testing path on Windows in the meantime.

### Security

- Hardened FEDAUTH JWT debug logging: `tds_connection.cpp` previously
  hex-dumped the first 20 bytes of the access token at debug level 2.
  Replaced with size-only logging plus `(contents redacted)`.
- Raw-credentials Kerberos mode is SECRET-ONLY by design — cleartext
  `Password` is rejected in any connection string when integrated auth is
  selected. Defends against cleartext passwords in connection-string logs.
- Per-connection krb5 overrides (`krb5-configfile`, `krb5-credcachefile`)
  apply through `gss_acquire_cred_from` `cred_store` elements per instance,
  not via process-global `setenv()`. Thread-safe vs concurrent `getenv` on
  pool worker threads.
- Raw-mode `MEMORY:` ccache is destroyed after `gss_acquire_cred_from`
  copies credentials internally, so cleartext credentials don't linger in
  MIT's process-global ccache registry.

### Changed

- README's stale "Windows Authentication: Only SQL Server authentication is
  supported" limitation removed. Windows SSPI is now scoped as "Phase 4
  pending" with WSL2 documented as the interim testing path.
- README's Secret Fields and Key Aliases tables expanded with Kerberos rows.
- `docs/architecture.md` Authentication Strategy Pattern section updated to
  document the new `IAuthenticator` layered interface and the
  `IntegratedAuthStrategy` adapter.
- `docs/TESTING.md` gained a Kerberos Tests section covering the docker-compose
  stack and WSL2 testing.

### Internal

- New `src/include/tds/auth/iauthenticator.hpp` — three-method multi-round
  interface (`InitialBytes` / `NextBytes` / `Free`), modeled on
  `microsoft/go-mssqldb`'s `integratedauth.IntegratedAuthenticator`. No
  DuckDB headers — the TDS auth layer is reusable outside DuckDB.
- New `src/tds/auth/krb5_authenticator.{hpp,cpp}` — POSIX GSSAPI
  implementation. SPNEGO mechanism. Inline GSS OID literals to work around
  macOS GSS.framework not exporting the well-known OID symbols.
- New `src/include/tds/auth/integrated_auth_strategy.hpp` — adapter wrapping
  `IAuthenticator` in the existing `AuthenticationStrategy` interface.
- `src/tds/tds_protocol.cpp` gains `BuildLogin7WithSSPI` (sets
  `OptionFlags2.fIntSecurity`, writes SPNEGO blob into LOGIN7's SSPI field;
  `cbSSPILong` fallback for blobs > 65 535 bytes) and `BuildSSPIMessage`
  (continuation packet, type `0x11`).
- `src/tds/tds_token_parser.cpp` recognizes `TokenType::SSPI = 0xED`.
- `src/tds/tds_connection.cpp` gains `AuthenticateIntegrated()` — drives the
  full SPNEGO continuation loop on `0xED` tokens, with an 8-round cap to
  detect cross-realm misconfiguration.
- `src/connection/mssql_pool_manager.cpp` gains
  `GetOrCreatePoolWithIntegratedAuth` — each pool connection builds a fresh
  `Krb5Authenticator` so kinit-refreshed tickets are picked up on the next
  fill. Logs verbatim GSSAPI errors to stderr on pool refill failures.
- `CMakeLists.txt` adds `ENABLE_KRB5` option (default ON on POSIX),
  pkg-config GSSAPI discovery on Linux, `find_library(GSS_FRAMEWORK GSS)` on
  macOS, `secur32` linkage hook for Windows (Phase 4).

## [0.1.18] - 2026-02-24

### Added

- XML data type support (spec 041). XML columns read as VARCHAR; BCP write
  path; clear errors for INSERT-with-RETURNING / UPDATE on XML columns.

### Fixed

- UDT type alias crash in catalog metadata queries (issue #81).

## Earlier history

Earlier versions are tracked in git history under `specs/NNN-*/` directories.
Notable recent specs:

- **041-xml-type-support** — XML column read/write (TDS type 0xF1).
- **040-fix-datetimeoffset-nbc** — DATETIMEOFFSET in NBC row reader.
- **039-order-pushdown** — ORDER BY pushdown to SQL Server (experimental).
- **037-replace-libcurl-httplib** — Replaced libcurl with bundled cpp-httplib
  for Azure OAuth2.
- **036-azure-token-docs** — Azure AD documentation expansion.
- **034-duckdb-v15-upgrade** — DuckDB v1.5 upgrade.
- **033-fix-catalog-scan** — Catalog metadata cache fix.
- **032-fedauth-token-provider** — Manual access token support for Azure AD.
- **031-connection-fedauth-refactor** — Auth strategy pattern introduction.
- **027-ctas-bcp-integration** — CTAS via BCP protocol.
- **024-mssql-copy-bcp** — COPY TO via BCP.
- **020-multi-statement-scan** — Multi-statement support in `mssql_scan`.
- **001-azure-token-infrastructure** — Initial Azure AD support.

See `specs/` for the full feature design history.

[Unreleased]: https://github.com/oluies/mssql-extension/compare/v0.1.18...HEAD
[0.1.18]: https://github.com/oluies/mssql-extension/releases/tag/v0.1.18

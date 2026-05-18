# Changelog

All notable changes to the DuckDB MSSQL Extension are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **`mssql_close_all()`** scalar function (spec 047 FR-013). Closes every
  diagnostic-API connection opened via `mssql_open()` in one call; returns
  the count of handles closed. Idempotent — a second call after a full
  close returns 0. Recommended shutdown hook for hosts that use the
  diagnostic API but do not track individual handles. Marked `[DEPRECATED]`
  from registration: lives in the same group as `mssql_open` / `mssql_close`
  / `mssql_ping` and will be removed alongside them in a future major
  release once the catalog-bound API covers all diagnostic needs (FR-010).

### Changed

- **`mssql_open` / `mssql_close` / `mssql_ping` are now documented as
  `[DEPRECATED]`** (spec 047 FR-010). They remain functional and are kept
  for backward compatibility. Prefer ATTACH + the catalog-bound functions
  (`mssql_scan`, `mssql_exec`, `mssql_pool_stats`) which integrate with
  DuckDB's catalog lifecycle and the per-catalog pool ownership introduced
  in spec 047. The handle manager singleton these three functions share is
  the last extension-internal process-wide state and will be removed
  together with the functions themselves.

### Security

- **Azure TokenCache cross-instance aliasing** fixed (spec 047 FR-012).
  Pre-047, two DuckDB instances in the same process that each defined a
  secret with the same name (e.g. `mssql_secret`) shared a single
  TokenCache row keyed by `secret_name` alone — instance B could silently
  authenticate with instance A's already-acquired token even when the two
  secrets resolved to different Azure principals. The cache key is now
  namespaced by `(DatabaseInstance address, cache_key)`; tokens from
  different instances are independent. The `OnDetach` invalidation path
  is scoped to the calling instance's namespace so a sibling instance
  sharing the secret name keeps its token.

- **ATTACH credentials are now validated eagerly** by default (spec 047
  FR-011). Wrong passwords / unreachable hosts surface as ATTACH errors
  instead of being deferred to the first query. Error messages never
  contain the password (audited via sentinel substring assertion in
  `test/sql/attach/attach_validates_credentials.test`). Opt out with
  `lazy_validation true` for container/orchestration scenarios; ceiling
  controlled by the new `mssql_attach_validation_timeout` setting.

### Internal

- **Process-wide singletons removed** (spec 047, closes [issue #96](https://github.com/hugr-lab/mssql-extension/issues/96)):
  `MssqlPoolManager`, `MSSQLContextManager`, and `MSSQLResultStreamRegistry`
  are gone. Connection pools are now owned per-`MSSQLCatalog` via
  `unique_ptr`; result streams live on the catalog as
  `RegisterStream` / `RetrieveStream` methods. Two attached MSSQL
  databases under different ATTACH aliases no longer alias to the same
  pool, and ATTACH/DETACH cycles in a Python-style loop (the issue #96
  repro) succeed indefinitely.

- **Windows SSPI** integrated authentication (spec 042 Phase 4). `WinSspiAuthenticator`
  via `secur32.dll`'s Negotiate package. Uses the current Windows logon session — no
  `kinit` needed. Same connection-string surface as POSIX (`Trusted_Connection=yes` /
  `authenticator=winsspi`). Mirrors the structure of `Krb5Authenticator`; shares the
  `IAuthenticator` interface and the SPNEGO continuation loop in
  `TdsConnection::AuthenticateIntegrated`. Linked against `secur32.lib` from the
  Windows SDK — no third-party dependency.

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

### Fixed

- Linux build with Kerberos enabled now links `libkrb5` explicitly. Previous
  builds failed at the link step with `undefined reference to symbol
  'krb5_free_error_message'` on distros where `krb5-gssapi.pc` doesn't
  transitively pull in `libkrb5` (Ubuntu 24.04 is the documented case).
  Affects spec 042 raw-credentials mode users on Linux only — macOS uses
  `GSS.framework` which bundles all symbols. Configure-time warnings now
  cite both Debian (`libkrb5-dev`) and RHEL (`krb5-devel`) package names.

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

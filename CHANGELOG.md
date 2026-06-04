# Changelog

All notable changes to the DuckDB MSSQL Extension are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- **dbt segfault with `threads >= 2`** (spec 052, closes
  [#126](https://github.com/hugr-lab/mssql-extension/issues/126)).
  Catalog entries (`MSSQLTableEntry`, `MSSQLSchemaEntry`) switched from
  `unique_ptr` to `shared_ptr` ownership; concurrent first-load of the
  same table is coordinated via per-table singleflight so only one
  thread issues the SQL Server round trip (waiters re-check the cache).
  Lifetime extension across `Invalidate()` / `OnDetach` is done via
  per-`ClientContext` bind-time anchors registered as
  `ClientContextState`: every `MSSQLSchemaEntry::LookupEntry` /
  `MSSQLCatalog::LookupSchema` / catalog `Scan` callback path stashes
  the `shared_ptr` into the per-context `MSSQLBindAnchors` holder, which
  DuckDB releases via the `QueryEnd` hook. In-flight binders therefore
  hold every entry they touched alive for the full bind+execute span;
  release happens naturally at end of query, with no catalog-lifetime
  accumulation. Audit of `MSSQLMetadataCache::GetTableMetadata` confirms
  its only caller (`MSSQLTableSet::LoadSingleEntry`) copies fields
  immediately; contract pinned in the header.
  `MSSQLStatisticsProvider` returns by value, no pointer-handout surface.
- **`MSSQLTableEntry::EnsurePKLoaded` double-free under thread stress**
  (spec 052). Two threads both saw the load flag false, both fetched, and
  both move-assigned to `pk_info_` — the second move freed the loser's
  previous `vector<PKColumnInfo>` while the first thread still held it.
  Caught by AddressSanitizer during the spec 052 invalidation-race soak.
  Fixed with a `pk_load_mutex_` + `std::atomic<bool> pk_loaded_`
  acquire/release publication so the lock-free fast path stays correct
  under the C++ memory model.
- **`MSSQLCatalog::RefreshCache` connection leak on TDS hiccup** (spec
  052). Under thread stress (~318 invalidations / 30 s), SQL Server
  occasionally returns a transient TDS error mid-Refresh. The exception
  propagated past `connection_pool_->Release`, leaving one connection
  checked out and tripping `~ConnectionPool`'s quiescence-contract
  assert at catalog teardown. Wrapped the `Refresh` call in try/release.
- **`MSSQLTableEntry::EnsurePKLoaded` / `GetStorageInfo` connection
  leak on TDS hiccup** (spec 052). Both functions intentionally swallow
  exceptions to fall back to `pk_info_.exists = false` / cached
  `approx_row_count_`, but the outer `catch (...)` never released the
  pool-owned connection acquired inside the try. Every SELECT bind
  calls `EnsurePKLoaded`, so under scenario 5/8 stress a single TDS
  hiccup inside `PrimaryKeyInfo::Discover` stranded one connection in
  `active_connections_` for the lifetime of the pool. Nested an inner
  try/release/throw around the SQL Server I/O so the connection is
  returned BEFORE the outer fallback catch runs.
- **`ConnectionPool::Shutdown` cleanup-thread strand on quiescence
  violation** (spec 052). `D_ASSERT` in DuckDB-debug builds throws
  `InternalException` rather than calling `abort()`. The throw fired
  while `cleanup_thread_` was still joinable; `~ConnectionPool noexcept`
  caught the exception, but `~std::thread` on the joinable thread then
  called `std::terminate()`. Reordered: signal + join the cleanup thread
  and close pooled connections FIRST, then emit the warning and assert
  at the end. The warning is the operator-visibility signal; the
  trailing assert preserves the debug invariant without stranding
  resources on its unwind path.

### Added

- `test/cpp/test_concurrent_reads.cpp` scenarios 5-8 (spec 052
  US2/US3 + concurrent-write acceptance): 30 s soak runs that exercise
  every spec 052 lifetime guarantee under AddressSanitizer/UBSan.
  Scenario 5 — 4 readers + invalidator at 50 ms cadence
  (~2500 reads × ~300 invalidations). Scenario 6 — scenario 5 plus a
  `duckdb_schemas()` / `duckdb_tables()` walker exercising the bulk-scan
  anchor path (~1200 reads × ~260 invalidations × ~200 schema walks).
  Scenario 7 — 4 writers + 1 reader on one shared table with disjoint PK
  ranges (~550 INSERT/UPDATE/DELETE cycles + ~500 reads). Scenario 8 —
  4 pure-write threads (~2700 INSERTs / 30 s).
- `.github/workflows/concurrency-tests.yml` job that rebuilds the
  extension with ASan/UBSan and runs `test-concurrent-reads` on every
  PR that touches the catalog or singleflight surfaces. Includes an
  8 GB swap on `/mnt` to absorb the link-time RAM peak, a
  `TestDB`-creation step for scenarios 4-8, and `LD_PRELOAD`-ed
  `libasan`/`libubsan` on the test binary run only (so the preload
  doesn't leak into vcpkg's compiler probe).
### Security

- **Wipe bearer credentials on destruction.** `MSSQLConnectionInfo` gains a
  user-declared destructor that `OPENSSL_cleanse`s `password` and
  `access_token`; `~MSSQLCatalog` wipes the cached `fedauth_token_utf16le_`
  byte vector. `OPENSSL_cleanse` defeats dead-store elimination, so secrets
  do not linger in heap-recycled memory after the owning
  `shared_ptr<MSSQLConnectionInfo>` or `MSSQLCatalog` is destroyed.
  Rule-of-five compliance: copy/move ctors and assigns on
  `MSSQLConnectionInfo` are explicitly `= default`-ed so that user-declaring
  the destructor doesn't silently disable move generation.

## [0.2.0] - 2026-05-20

Major release: integrated authentication (Kerberos + Windows SSPI),
process-wide singleton cleanup, security hardening, and a deep codec
refactor. Closes [#82](https://github.com/hugr-lab/mssql-extension/issues/82)
(custom Application Name) and [#96](https://github.com/hugr-lab/mssql-extension/issues/96)
(ATTACH/DETACH-in-Python-loop crash class).

### Added

- **Custom Application Name in connection string** (spec 047 FR-014,
  closes [#82](https://github.com/hugr-lab/mssql-extension/issues/82)).
  ADO.NET keys `Application Name` / `ApplicationName` / `App Name` /
  `application_name` (case-insensitive), URI query parameter
  `applicationname`, and MSSQL secret fields `application_name`
  (canonical) / `applicationname` (fallback) propagate to LOGIN7
  `program_name` — visible as `APP_NAME()` /
  `sys.dm_exec_sessions.program_name`. Empty falls back to the
  extension default (`"DuckDB MSSQL Extension"`); values exceeding
  128 UTF-16 code units are clamped client-side so what the user sees
  in `APP_NAME()` equals what we sent.
- **`lazy_validation` ATTACH option** + **`mssql_attach_validation_timeout`
  setting** (spec 047 FR-011). Eager ATTACH validation is on by default
  (wrong password / unreachable host fail ATTACH instead of being
  deferred to the first query); opt out per ATTACH with
  `lazy_validation true` to preserve the pre-047 lazy behaviour
  (useful for container / orchestrator startup where the SQL Server
  may not yet be reachable). The setting bounds the eager round-trip;
  `0` (default) inherits `mssql_connection_timeout`.
- **`mssql_close_all()`** scalar function (spec 047 FR-013). Closes every
  diagnostic-API connection opened via `mssql_open()` in one call; returns
  the count of handles closed. Idempotent — a second call after a full
  close returns 0. Recommended shutdown hook for hosts that use the
  diagnostic API but do not track individual handles. Marked `[DEPRECATED]`
  from registration: lives in the same group as `mssql_open` / `mssql_close`
  / `mssql_ping` and will be removed alongside them in a future major
  release once the catalog-bound API covers all diagnostic needs (FR-010).

### Changed

- **LOGIN7 default `program_name` unified to `"DuckDB MSSQL Extension"`**
  (spec 047 FR-014 side-effect). Pre-047 SQL auth sent `"DuckDB"` and
  integrated auth sent `"DuckDB MSSQL Extension"`. The single resolution
  point (`ResolveAppName` helper, called by every auth path) now sends
  `"DuckDB MSSQL Extension"` uniformly when no `application_name` is
  supplied. Observable change for SQL-auth users who previously saw
  `APP_NAME() = 'DuckDB'`; they will now see `'DuckDB MSSQL Extension'`
  unless they explicitly set `Application Name=DuckDB`.
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

- **Type codec consolidation** (spec 045). Per-type encoding/decoding/literal/DDL
  logic consolidated into 9 family modules under `src/codec/`:
  boolean/integer/float/decimal/money/string/binary/datetime/uuid. Each
  `<family>_codec.cpp` owns `EncodeToBcp` / `DecodeFromTds` /
  `FormatSqlLiteral` / `FormatDdlTypeName` for its types. Dispatch via
  `FamilyFromLogicalType` switch in `literal_format.cpp` + `type_family.cpp`.
  5 LogicalType-side dispatch sites collapsed; net −762 LOC across dispatch
  sites (3243→2481, −23.5%). Bonus: TIMESTAMP_MS/NS/S/TZ now round-trip
  losslessly through SQL Server DATETIME2(3/7/0/7) with full
  type-transparency. Closes [issue #91](https://github.com/hugr-lab/mssql-extension/issues/91)
  (BCP nvarchar character-vs-byte length) and [#89](https://github.com/hugr-lab/mssql-extension/issues/89)
  (VIEW catalog-vs-runtime type divergence). No new vcpkg deps. Per-row bench
  (1M rows): within 5% gate vs spec-044 baseline.

- **Named instance resolver** (spec 045 phases 0-2). SQL Server Browser
  (UDP 1434) discovery for named instances. Mock-browser test stack under
  `test/named-instance/`.

- **UTF-16 codec consolidation** (spec 044). Finishes the simdutf migration
  started in spec 043 — every legacy `Utf16LE*` call site moves to the
  simdutf-backed wrapper. simdutf becomes the production UTF-16 codec; the
  legacy hand-rolled converter survives only as a private invalid-input
  fallback. Includes microbenchmark (`make bench-utf16`) and an end-to-end
  before/after benchmark (`test/bench/bench_codec_e2e.sh`).

- **LOGIN7 non-ASCII fix + simdutf foundation** (spec 043). Non-ASCII bytes
  in LOGIN7 username/password/database fields no longer get corrupted by the
  hand-rolled UTF-16 converter. Adds simdutf as a vcpkg dependency
  (statically linked, MIT). Foundation for spec 044's full migration.

### CI / Build

- **Tier-1 lint and security checks** added: CodeQL (C++), gitleaks,
  shellcheck, hadolint, yamllint, codespell. Dependabot updates enabled.
  PR prompt-injection scanner for review descriptions.
- **CodeQL speedup** (3-part): target restriction + vcpkg cache + submodule
  trim. Cuts CodeQL job runtime substantially on PR triggers.
- **Kerberos integration job** in CI: spins up the
  `test/kerberos/docker-compose.yml` KDC + SQL Server + test-client stack
  for every PR touching the integrated-auth path.
- **Drive-by fix**: 9 codec headers (spec 045) had `class ColumnMetadata` /
  `class BCPColumnMetadata` forward declarations while the real
  definitions are `struct`. MSVC mangles `class` and `struct`
  differently (clang/gcc don't), producing 16 unresolved-external LNK2019
  errors at link time. Latent regression — last successful MSVC build on
  main was 2026-05-15, BEFORE spec 045 merged. Fixed all 9 forward
  declarations.

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

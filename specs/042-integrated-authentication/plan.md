# Implementation Plan: Integrated Authentication (Kerberos / SSPI)

**Branch**: `042-integrated-authentication` | **Date**: 2026-05-12 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/042-integrated-authentication/spec.md`

## Summary

Add SPNEGO/Kerberos integrated authentication to the extension. POSIX uses system GSSAPI (`libgssapi_krb5` / `GSS.framework`); Windows uses SSPI (`secur32.dll`). Connection-string surface follows `microsoft/go-mssqldb` verbatim (`authenticator=krb5`, `krb5-keytabfile`, etc.) plus pyodbc-compatible aliases (`Trusted_Connection=yes`, `Integrated Security=SSPI`). The TDS layer gains an `IAuthenticator` abstraction (three methods: `InitialBytes`, `NextBytes`, `Free`) that drives the multi-packet SSPI exchange via `0xED` continuation tokens. Existing SQL-auth and Azure-AD paths are refactored behind the same abstraction first as a no-behavior-change step, then Kerberos is added.

## Technical Context

**Language/Version**: C++17 (C++11-compatible for ODR on Linux)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (vcpkg), existing TDS protocol layer, system GSSAPI (POSIX) / `secur32.dll` (Windows)
**Storage**: N/A (remote SQL Server via TDS, in-memory auth state per-connection)
**Testing**: SQLLogicTest (integration; requires SQL Server + containerized KDC for Kerberos tests), C++ unit tests for connection-string parsing
**Target Platform**: Linux x86_64, Linux ARM64, macOS ARM64, Windows x64
**Project Type**: Single (DuckDB extension)
**Performance Goals**: Authenticated connection establishment ≤ +50 ms vs SQL-auth (one extra round trip for SPNEGO continuation). No measurable per-query overhead — pooled connections reuse the authenticated session identically to SQL-auth.
**Constraints**: No DuckDB headers in `src/tds/auth/`; no static linking of Kerberos (system library); compile-time gate via `ENABLE_KRB5` flag.
**Scale/Scope**: ~1500 LoC across two new authenticators + ~200 LoC interface + ~300 LoC connection-string parsing + ~150 LoC LOGIN7 SSPI loop. Two new tests directories under `test/sql/integrated_auth/` and `test/cpp/`.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Pre-Research | Post-Design | Notes |
|-----------|-------------|-------------|-------|
| I. Native and Open | PASS | PASS | System GSSAPI / SSPI are OS-native; not a vendored TDS/ODBC driver. SPNEGO mechanism is RFC-standard (RFC 4178). |
| II. Streaming First | PASS | PASS | Authentication completes before any data transfer; no streaming changes. |
| III. Correctness over Convenience | PASS | PASS | All known failure modes (no ticket, wrong SPN, clock skew, KDC unreachable, conflicting keys) produce distinct, actionable errors instead of falling through to generic messages. |
| IV. Explicit State Machines | PASS | PASS | The SSPI continuation loop is a small explicit state machine (`Initial` → `Continue` → `Complete`) in `TdsConnection::Login()`, parallel to the existing FEDAUTH loop. |
| V. DuckDB-Native UX | PASS | PASS | Standard `ATTACH ... TYPE mssql`, standard `CREATE SECRET`, standard error reporting. `Trusted_Connection=yes` is the pyodbc-canonical phrasing. |
| VI. Incremental Delivery | PASS | PASS | Phase 1 (refactor existing auth behind `IAuthenticator`, no behavior change) is independently mergeable. Phase 2 (Kerberos POSIX) ships P1+P2+P3 stories. Phase 3 (SSPI Windows) is independent. |

## Project Structure

### Documentation (this feature)

```text
specs/042-integrated-authentication/
├── spec.md
├── plan.md              # This file
├── research.md          # Phase 0 output
├── quickstart.md        # Phase 1 output
├── tasks.md             # Phase 1 output
└── checklists/
    └── requirements.md
```

### Source Code (repository root)

```text
src/
├── tds/
│   ├── auth/
│   │   ├── iauthenticator.cpp           # (header-only interface, but TU exists for vtable anchor)
│   │   ├── sql_auth_strategy.cpp        # EXISTING - unchanged behavior, refactored to expose IAuthenticator (returns nullptr for sql_auth path)
│   │   ├── fedauth_strategy.cpp         # EXISTING - unchanged behavior, refactored similarly
│   │   ├── manual_token_strategy.cpp    # EXISTING - unchanged
│   │   ├── auth_strategy_factory.cpp    # EXISTING - add Kerberos/SSPI dispatch
│   │   ├── krb5_authenticator.cpp       # NEW (POSIX, compiled when ENABLE_KRB5=ON)
│   │   ├── winsspi_authenticator.cpp    # NEW (Windows, compiled when _WIN32)
│   │   └── integrated_auth_strategy.cpp # NEW - adapter from IAuthenticator into AuthenticationStrategy
│   ├── tds_protocol.cpp                 # MODIFY - LOGIN7 fIntSecurity bit + SSPI field writer
│   ├── tds_token_parser.cpp             # MODIFY - recognize 0xED SSPI continuation tokens
│   └── tds_connection.cpp               # MODIFY - SSPI continuation loop in Login()
├── connection/
│   └── mssql_settings.cpp               # (no changes for v1 — no new global settings)
├── mssql_storage.cpp                    # MODIFY - parse authenticator, krb5-*, Trusted_Connection
├── mssql_secret.cpp                     # MODIFY - parse new secret fields
└── include/
    ├── tds/
    │   └── auth/
    │       ├── iauthenticator.hpp       # NEW - 3-method interface, NO duckdb headers
    │       ├── krb5_authenticator.hpp   # NEW
    │       ├── winsspi_authenticator.hpp # NEW
    │       └── integrated_auth_strategy.hpp # NEW
    └── mssql_storage.hpp                # MODIFY - extend MSSQLConnectionInfo with auth_method, krb5_*

test/
├── sql/
│   └── integrated_auth/                 # NEW - Kerberos integration tests
│       ├── krb5_basic.test              # P1: cred-cache, basic ATTACH + SELECT
│       ├── krb5_keytab.test             # P2: keytab mode
│       ├── krb5_errors.test             # P3: missing ticket, wrong SPN, clock skew
│       ├── trusted_connection_alias.test # P3: Trusted_Connection / Integrated Security aliases
│       └── conflicts.test               # P3: conflicting connection-string keys
└── cpp/
    └── test_integrated_auth_parsing.cpp # NEW - unit tests for ParseConnectionString / ParseUri / FromSecret with new keys

test/kerberos/                           # NEW - three-container compose stack
├── docker-compose.yml                   # kdc + sql + test-client on shared network
├── kdc/
│   ├── Dockerfile                       # MIT Kerberos on Ubuntu
│   ├── krb5.conf                        # Realm EXAMPLE.COM
│   └── init-kdc.sh                      # kdb5_util create + addprinc testuser, MSSQLSvc/sql.example.com:1433
├── sql/
│   └── init.sql                         # SQL logins mapped to AD principals
└── test-client/
    ├── Dockerfile                       # Ubuntu + krb5-user + mssql.duckdb_extension
    └── run-tests.sh                     # Driver: kinit + run [kerberos]-tagged tests

docs/
├── architecture.md                      # MODIFY - update Authentication Strategy Pattern section
└── kerberos.md                          # NEW - end-user documentation (mirrors AZURE.md depth)

README.md                                # MODIFY - remove Windows-auth limitation; add Key Alias rows; add Kerberos section pointer
```

**Structure Decision**: Auth code lives entirely under `src/tds/auth/` (already exists for `SqlAuthStrategy` and `FedAuthStrategy`). The new `IAuthenticator` interface header is the only file with a strict no-DuckDB-headers rule — the rest of `src/tds/auth/` can use `string`/`vector` but does already include some DuckDB-flavored types (it lives inside the DuckDB build). The interface header is the seam at which the TDS layer becomes reusable.

## Architecture: How the new abstraction fits

```
┌──────────────────────────────────────────────────────────────┐
│   AuthStrategyFactory::Create(MSSQLConnectionInfo)            │
│                                                                │
│   selects exactly one of:                                      │
│                                                                │
│   ┌────────────────────┐  ┌────────────────────┐              │
│   │ SqlAuthStrategy    │  │ FedAuthStrategy    │              │
│   │ (existing,         │  │ (existing,         │              │
│   │  no IAuthenticator)│  │  no IAuthenticator)│              │
│   └────────────────────┘  └────────────────────┘              │
│                                                                │
│   ┌────────────────────────────────────────────┐              │
│   │ IntegratedAuthStrategy  (NEW)              │              │
│   │   wraps:                                    │              │
│   │   ┌─────────────────────────────────────┐  │              │
│   │   │ IAuthenticator (NEW interface)      │  │              │
│   │   │   InitialBytes() / NextBytes() / Free│  │              │
│   │   │                                       │              │
│   │   │  one of:                              │              │
│   │   │    Krb5Authenticator   (POSIX)        │              │
│   │   │    WinSspiAuthenticator (Windows)     │              │
│   │   └─────────────────────────────────────┘  │              │
│   └────────────────────────────────────────────┘              │
└──────────────────────────────────────────────────────────────┘
                                │
                                ▼
                  TdsConnection::Login()
                                │
            ┌───────────────────┴─────────────────────┐
            ▼                                          ▼
   strategy.GetLogin7Options()              strategy.GetAuthenticator()
   (existing path, returns                  (new — returns nullptr unless
    user/password fields)                    Integrated Auth)
            │                                          │
            ▼                                          ▼
   build LOGIN7 with                         build LOGIN7 with
   fIntSecurity=0                            fIntSecurity=1,
                                             SSPI field = InitialBytes(),
                                             user/password fields empty
            │                                          │
            ▼                                          ▼
   read LOGINACK + DONE                      loop:
                                                receive 0xED token
                                                next_blob = NextBytes(token)
                                                if next_blob empty: break
                                                send SSPI Message packet
                                              read LOGINACK + DONE
```

Anchors to the seven-layer architecture (`docs/architecture.md`):

- **Layer 3 (Connection Management)**: `MSSQLConnectionInfo` gains `auth_method` enum + `krb5_*` fields. `mssql_storage.cpp` and `mssql_secret.cpp` parse them.
- **Layer 4 (TDS Protocol)**: `src/tds/auth/` gains the new `IAuthenticator` interface and two implementations. `TdsConnection::Login()` gains the SSPI continuation loop. `tds_protocol.cpp` learns to write the SSPI field; `tds_token_parser.cpp` learns to recognize `0xED`.
- **No changes** to catalog, table scan, DML, COPY, transactions, or settings layers.

## Migration order (CRITICAL)

The refactor MUST land before any Kerberos code:

1. **PR 1** (commits T001–T005): Introduce `IAuthenticator` header. Add `GetAuthenticator()` virtual method to `AuthenticationStrategy` returning `nullptr` by default. Run all existing tests — must pass unchanged. **No new behavior.** This PR is reviewable in isolation as a pure refactor.
2. **PR 2** (commits T006–T010): Connection-string parsing + CMake + LOGIN7 wiring + SSPI loop scaffolding. Still no Kerberos backend — `Trusted_Connection=yes` returns a "not yet implemented" error. Verifies the parser changes don't regress existing flows.
3. **PR 3** (commits T011–T015): `Krb5Authenticator` (POSIX). End-to-end tests with the containerized KDC. This is the v1 MVP.
4. **PR 4** (commits T016–T018): `WinSspiAuthenticator` (Windows). Lights up Windows users. May be deferred to v2 (see spec Open Questions §2).
5. **PR 5** (commits T019–T021): Docs (`docs/kerberos.md`, README updates, architecture diagram).

This staging gives reviewers four small, focused PRs instead of one large diff.

## CMake changes

```cmake
# In CMakeLists.txt, after vcpkg setup

option(ENABLE_KRB5 "Enable Kerberos integrated authentication" ON)

if(ENABLE_KRB5 OR WIN32)
    if(WIN32)
        target_link_libraries(${EXTENSION_NAME} PRIVATE secur32)
        target_compile_definitions(${EXTENSION_NAME} PRIVATE MSSQL_ENABLE_SSPI=1)
        list(APPEND MSSQL_AUTH_SOURCES src/tds/auth/winsspi_authenticator.cpp)
    elseif(APPLE)
        find_library(GSS_FRAMEWORK GSS REQUIRED)
        target_link_libraries(${EXTENSION_NAME} PRIVATE ${GSS_FRAMEWORK})
        target_compile_definitions(${EXTENSION_NAME} PRIVATE MSSQL_ENABLE_KRB5=1)
        list(APPEND MSSQL_AUTH_SOURCES src/tds/auth/krb5_authenticator.cpp)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GSSAPI REQUIRED IMPORTED_TARGET krb5-gssapi)
        target_link_libraries(${EXTENSION_NAME} PRIVATE PkgConfig::GSSAPI)
        target_compile_definitions(${EXTENSION_NAME} PRIVATE MSSQL_ENABLE_KRB5=1)
        list(APPEND MSSQL_AUTH_SOURCES src/tds/auth/krb5_authenticator.cpp)
    endif()
endif()
```

`MSSQL_ENABLE_KRB5` and `MSSQL_ENABLE_SSPI` are compile-time defines. Code paths in `auth_strategy_factory.cpp` use `#if defined(MSSQL_ENABLE_KRB5)` to gate inclusion. With both defines absent, `authenticator=krb5` produces the error from FR-012.

## Integration Tests

### Containerized KDC (`test/kerberos/`)

Three-service `docker-compose` stack — no external Active Directory required:

- **`kdc`** — MIT Kerberos KDC on Ubuntu, realm `EXAMPLE.COM`. Principals: `testuser@EXAMPLE.COM` (password `testpass`) and `MSSQLSvc/sql.example.com:1433@EXAMPLE.COM` (random key, exported to a keytab mounted into the `sql` service).
- **`sql`** — `mcr.microsoft.com/mssql/server:2022-latest` configured to authenticate against `kdc`. Service keytab mounted at `/var/opt/mssql/secrets/mssql.keytab`.
- **`test-client`** — Ubuntu container with the built `mssql.duckdb_extension`, `krb5-user`, and a `run-tests.sh` driver. The test harness runs `kinit testuser@EXAMPLE.COM <<< 'testpass'` before each test and uses isolated per-test ccaches via `KRB5CCNAME`.

Local run:
```bash
cd test/kerberos
docker-compose up -d
docker-compose exec test-client kinit testuser@EXAMPLE.COM <<< 'testpass'
docker-compose exec test-client ./run-tests.sh
docker-compose down
```

### Test Files (under `test/sql/integrated_auth/`)

**Group**: `[kerberos]` — tagged so CI can skip when `MSSQL_KERBEROS_TEST != 1`.

| File | Description | Validates |
|------|-------------|-----------|
| `krb5_basic.test` | `kinit testuser@EXAMPLE.COM` + ATTACH `Trusted_Connection=yes` to `sql.example.com` + `SELECT TOP 1 name FROM sys.tables` | FR-001, FR-002, FR-006, SC-001 |
| `krb5_keytab.test` | ATTACH with `krb5-keytabfile=/keytabs/testuser.keytab;User Id=alice;krb5-realm=EXAMPLE.COM` (no ccache present); verify query works | FR-004, SC-003 |
| `krb5_pool.test` | 20 concurrent queries on a Kerberos-attached database; verify `mssql_pool_stats()` shows reuse | FR-011, SC-006 |
| `krb5_errors.test` | (a) no ccache → `"no credentials cache"`; (b) `kinit` then `kdestroy` → `"ticket expired"` or `"no credentials"`; (c) wrong SPN via `service_principal_name` override → `"server principal unknown"` | SC-005 |
| `trusted_connection_alias.test` | `Trusted_Connection=yes` and `Integrated Security=SSPI` and `Integrated Security=true` all resolve identically; in CI on POSIX, all three should ATTACH successfully | FR-002 |
| `conflicts.test` | `Trusted_Connection=yes;User Id=alice` → ATTACH validation error; `Trusted_Connection=yes` + Azure secret → ATTACH validation error; `authenticator=krb5` + `authenticator=winsspi` → ATTACH validation error | FR-008 |

### C++ unit tests (`test/cpp/test_integrated_auth_parsing.cpp`)

Pure parser tests; no SQL Server needed. Cover:

- `FromConnectionString("Server=x;Database=y;Trusted_Connection=yes")` → `auth_method=KRB5` on POSIX, `WINSSPI` on Windows
- All `go-mssqldb` key forms parse correctly: `authenticator`, `krb5-configfile`, `krb5-keytabfile`, `krb5-credcachefile`, `krb5-realm`, `krb5-dnslookupkdc`
- All conflict cases produce the expected validation error string
- URI form: `mssql://host/db?authenticator=krb5&krb5-realm=EXAMPLE.COM`
- Secret form: `CREATE SECRET ... (TYPE mssql, host '...', database '...', authenticator 'krb5', krb5_keytabfile '...')`

### Existing Test Verification (regression)

All of these MUST pass unchanged after each PR:

- `test/sql/attach/` — secret, connection string, URI forms
- `test/sql/azure/` — all FEDAUTH paths
- `test/sql/integration/` — pool, TLS, large data
- `test/sql/transaction/`, `test/sql/insert/`, `test/sql/dml/`, `test/sql/copy/`, `test/sql/ctas/` — every existing flow

## Complexity Tracking

| Concern | Justification |
|---------|---------------|
| Two parallel authenticator implementations (POSIX vs Windows) | Required — GSSAPI and SSPI are different APIs, no portable wrapper exists in the C/C++ ecosystem that is small enough to vendor. `go-mssqldb` makes the same split. |
| `IAuthenticator` layered under `AuthenticationStrategy` instead of replacing it | The existing strategy interface is FEDAUTH-shaped; replacing it would force the FEDAUTH code into a single-shot `InitialBytes()` shape it doesn't naturally fit. Layering is a 50-line addition vs ~500 lines of churn for replacement. |
| Containerized KDC for CI | Only deterministic way to test Kerberos. Same pattern as `go-mssqldb`'s upstream CI. |
| Compile-time gate (`ENABLE_KRB5`) | Lets distributors who can't link GSSAPI (e.g., a hardened static build) still ship the extension; the auth path simply errors. |

No constitution violations.

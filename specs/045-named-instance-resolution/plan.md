# Implementation Plan: SQL Server Named-Instance Resolution

**Branch**: `045-named-instance-resolution` | **Date**: 2026-05-16 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/045-named-instance-resolution/spec.md`
**Research**: [research.md](./research.md) — wire format, mock-browser design, code placement, error taxonomy

## Summary

Resolve `Server=host\instance` connection strings via the SQL Server Browser
service ([MS-SQLR]) before opening the TDS socket. Closes issue #77.

The change is small and bounded: a new `InstanceResolver` under
`src/connection/`, two new connection-info fields (`instance_name`,
`server_spec`), surgical edits in `src/mssql_storage.cpp`
(ADO.NET + URI parsers) and `src/tds/tds_connection.cpp` (LOGIN7
`ServerName` keeps the instance), and a self-contained docker-compose
test stack under `test/named-instance/` modelled on `test/kerberos/`.
No new vcpkg dependencies. ~250 lines of new C++, ~60 lines of Python
for the mock browser, ~150 lines of test harness.

## Technical Context

**Language/Version**: C++17 source, C++11-compatible ABI per the [[feedback_extension_version_stamp]] ODR rule on Linux.
**Dependencies**: stdlib UDP socket only (BSD sockets on POSIX, Winsock2 on Windows — both already linked). Python 3 for the mock-browser test image.
**Storage**: None. Resolution is stateless per-call; no cache (per [research.md §R5](./research.md#r5-caching-revisited)).
**Testing**: GoogleTest C++ unit tests for the parser (no network), docker-compose integration tests for end-to-end (mock browser + SQL Server + test client). Targets macOS host + Linux CI; Windows + SSPI + named-instance verified manually per [research.md §R10](./research.md#r10-open--windows-test-path).
**Target Platform**: Linux x86_64/ARM64, macOS ARM64, Windows x64.
**Project Type**: DuckDB extension (single project, no client/server split).
**Performance Goals**: SC-001 — < 200ms end-to-end attach against a healthy LAN browser.
**Constraints**: No new vcpkg packages (NFR-003). Must not regress spec-042 integrated-auth (SC-005). Errors must distinguish Browser-broken from SQL-Server-broken (FR-007).
**Scale/Scope**: One resolver call per connection-pool fill (not per query). Default pool size is 64 (`mssql_connection_limit`); resolution storm bounded at 64 × `pool_count` UDP packets per ATTACH burst — negligible.

## Constitution Check

**Bounded blast radius**: Resolver lives in `src/connection/`, called only from `MSSQLConnectionInfo::FromConnectionString`. The TDS layer is untouched apart from the LOGIN7 `ServerName` field. Failure modes are localised to ATTACH-time errors; no impact on running connections.

**Reversible**: One CMake-toggleable setting (`mssql_named_instance_resolution`) lets users opt out at runtime. Removing the feature is a single-PR revert.

**Explicit error surface**: Four distinct error categories (research.md §R8), each with a recommended user action. No silent fallbacks (e.g. *not* "if Browser fails, try port 1433 anyway" — that would mask config errors and is precisely why the current behaviour fails opaquely).

**No new external dependencies**: stdlib sockets only. The mock browser is a Python test fixture, not shipped in the extension binary.

**Test parity with `test/kerberos/`**: Mirrors the existing self-contained docker stack pattern, including the multi-stage Dockerfile that builds the extension inside Linux so macOS hosts can run integration tests.

## Project Structure

### Documentation (this feature)

```text
specs/045-named-instance-resolution/
├── spec.md         # what + why
├── research.md     # MC-SQLR wire format, mock design, code placement, settings
├── plan.md         # this file
├── tasks.md        # phased task list
└── quickstart.md   # bring-up + manual repro (written in Phase 2)
```

### Source Code (added or modified)

```text
src/
├── connection/
│   ├── instance_resolver.cpp           # NEW — ~150 LOC
│   └── (mssql_settings.cpp)            # MOD — register two new settings
├── include/
│   └── connection/
│       └── instance_resolver.hpp       # NEW — public interface
├── include/
│   └── mssql_storage.hpp               # MOD — new fields on MSSQLConnectionInfo
├── mssql_storage.cpp                   # MOD — invoke resolver in FromConnectionString;
│                                       #       fix ParseConnectionString + ParseUri
│                                       #       to split host vs instance
└── tds/
    └── tds_connection.cpp              # MOD — populate tds_server_name_ from
                                        #       host + instance_name (not host only)
test/
├── cpp/
│   └── test_instance_resolver.cpp      # NEW — parser unit tests (canned UDP bytes)
├── sql/
│   └── named_instance/
│       ├── resolve_basic.test          # NEW — happy path
│       ├── resolve_unknown.test        # NEW — instance not found
│       ├── resolve_browser_down.test   # NEW — timeout error message
│       └── resolve_explicit_port.test  # NEW — instance + ,port skips browser
└── named-instance/                     # NEW — mirrors test/kerberos/
    ├── docker-compose.yml
    ├── mock-browser/
    │   ├── Dockerfile
    │   └── browser.py
    ├── sql/
    │   ├── Dockerfile                  # bakes init.sql like test/kerberos/sql/
    │   └── init.sql
    ├── test-client/
    │   ├── Dockerfile                  # multi-stage; builds extension in Linux
    │   └── run-tests.sh
    └── README.md
```

**Structure decision**: Single-project layout — same as every prior spec. Resolver under `src/connection/` because it's a connection-string concern (research.md §R4). Test stack under `test/named-instance/` to match `test/kerberos/`.

## Phase 0: Wire format + parser

**Deliverable**: A parser that turns canned `SVR_RESP` byte buffers into `vector<BrowserInstance>`, with unit tests covering happy path, multi-instance, truncated payload, garbage bytes, missing `tcp;` field, malformed key/value pairs.

**Files**:
- `src/include/connection/instance_resolver.hpp` (new)
- `src/connection/instance_resolver.cpp` (new — parser only, no UDP yet)
- `test/cpp/test_instance_resolver.cpp` (new)

**No network in this phase.** The parser is pure: `vector<BrowserInstance> ParseBrowserResponse(string_view raw)`. Failures throw `IOException` with the diagnostic hex dump from research.md §R8.

**Exit criteria**: `make test` passes; parser handles every canned input from the unit tests; coverage report shows 100% line coverage on the parser TU.

## Phase 1: UDP transport + end-to-end resolver

**Deliverable**: `InstanceResolver::Resolve(host, instance, timeout) → expected<uint16_t, ResolveError>`. Sends the `CLNT_UCAST_INST` query, parses the response, returns the TCP port. One retry on timeout. Tested against the mock browser (Phase 1 of the test stack — Phase 2 below).

**Files**:
- `src/connection/instance_resolver.cpp` (add `Resolve`, UDP socket code via `tds_platform.hpp` shims)

**Cross-platform**: Use the same socket-creation pattern as `src/tds/tds_socket.cpp`. UDP-specific calls (`SOCK_DGRAM`, `recvfrom` with timeout via `SO_RCVTIMEO`) are minimal — no need for a new abstraction layer.

**Exit criteria**: Resolver returns the mock browser's advertised port for a known instance; returns the expected typed error for each of the four failure modes in research.md §R8.

## Phase 2: Test stack — mock browser + SQL Server + client

**Deliverable**: `cd test/named-instance && docker compose up -d --build && docker compose exec test-client /run-tests.sh` passes on macOS Apple Silicon and Linux x86_64 / ARM64.

**Files**:
- `test/named-instance/docker-compose.yml`
- `test/named-instance/mock-browser/{Dockerfile,browser.py}`
- `test/named-instance/sql/{Dockerfile,init.sql}`
- `test/named-instance/test-client/{Dockerfile,run-tests.sh}`
- `test/named-instance/README.md`

**Compose layout**: Two-hostname variant (research.md §R3) — mock browser on `browser.example.com`, SQL Server on `sql.example.com:11433`. Browser's `tcp;` field points its responses at `sql.example.com:11433`. Test client attaches via `Server=browser.example.com\TESTINST`, resolver returns `11433`, TDS connect goes to `sql.example.com:11433`, attach succeeds.

**Mock-browser fault injection**: `MOCK_BROWSER_MODE` env var supports `truncate`, `garbage`, `silent`, `slow` for negative tests.

**Exit criteria**: All four `test/sql/named_instance/*.test` files pass inside the test-client container. Stack starts cleanly on first run on a fresh macOS Docker Desktop install.

## Phase 3: Plumbing — connection string parsers + LOGIN7

**Deliverable**: `Server=host\instance` is accepted by ADO.NET, URI (with `%5C`), and secret forms. The resolver is invoked at `FromConnectionString` time. LOGIN7 `ServerName` carries `host\instance`. Default SPN under integrated auth uses the discovered port.

**Files**:
- `src/mssql_storage.cpp` — split host vs instance in `ParseConnectionString` + `ParseUri`; invoke resolver; populate `instance_name`.
- `src/include/mssql_storage.hpp` — new fields on `MSSQLConnectionInfo`.
- `src/tds/tds_connection.cpp` — `tds_server_name_` composition uses `host + "\\" + instance_name` when `instance_name` is non-empty; remove the "we don't use SQL Browser service" comment on line 340.
- `src/connection/mssql_settings.cpp` — register `mssql_browser_timeout_seconds` (default 3, range 1..30) and `mssql_named_instance_resolution` (default true).

**Spec-042 interaction**: No code change in `src/tds/auth/krb5_authenticator.cpp`. The discovered port lands in `info.port` before SPN derivation runs, so `MSSQLSvc/<host>:<discovered_port>` falls out naturally (research.md §R6).

**Exit criteria**: SQL test suite under `test/sql/named_instance/` passes against the mock-browser stack. Manual smoke against the Kerberos stack confirms SPN derivation is unchanged.

## Phase 4: Polish — quickstart + CLAUDE.md + release notes

**Deliverable**:
- `specs/045-named-instance-resolution/quickstart.md` — bring-up commands, manual repro steps for issue #77, Windows SSPI manual-test recipe (research.md §R10 path b).
- `CLAUDE.md` updated with new settings and a one-line entry under "Recent Changes".
- `Kerberos.md` cross-references named-instance support in the SPN section.
- A small section in the user-facing README (if one exists) — defer this if the README is unmaintained, document in `CLAUDE.md` only.

**Exit criteria**: Manual repro per `quickstart.md` works against a real Windows host (deferred to release-checklist; not a CI gate).

## Risk Register

| # | Risk | Likelihood | Mitigation |
|---|------|------------|------------|
| R1 | Docker DNS round-robins between the two hostname aliases, breaking the single-hostname compose variant | Medium (per research.md §R3) | Use the two-hostname variant from the start; documented in plan Phase 2. |
| R2 | UDP 1434 blocked by host firewall (especially on macOS dev boxes) | High in dev, low in CI | Default 3s timeout + clear error message ("SQL Browser unreachable"). Document the firewall check in quickstart.md. |
| R3 | Dynamic-port change between resolver and TCP connect (TOCTOU) | Very low (window is < 10ms) | Connection-pool retry on initial connect failure already handles this; if it bites, fail with a re-attach hint. No code change. |
| R4 | Real SQL Server in unusual configs (clusters, AlwaysOn AGs) emits different Browser fields | Low | Parser ignores unknown fields; key off `InstanceName` + `tcp;` only. Clusters get a follow-up if anyone files a bug. |
| R5 | Windows SSPI + named instance untested in CI | Medium | Manual smoke per quickstart.md. Linux Kerberos path exercises all the resolver code; SSPI differs only in the SPN-acquisition side, which spec 042 already covers. |
| R6 | A user with `mssql_named_instance_resolution=false` and `Server=host\instance` hits a worse error than today | Low | Parse-time error: "named-instance resolution is disabled; set `mssql_named_instance_resolution=true` or use `Server=host,port`". |

## Decisions Locked

- **No port caching in v1** (research.md §R5).
- **No LocalDB pipe support** (spec.md "Out of Scope").
- **URI requires `%5C`**, no auto-decode of literal `\` (research.md §R7).
- **Two new settings**, no retry-count setting (research.md §R9).
- **Mock browser in Python**, not Go or C++ (research.md §R2).
- **Two-hostname compose layout**, not single-alias trick (research.md §R3).
- **Windows + SSPI + named instance is manual-test only**, not CI-gated (research.md §R10 → plan §Risk R5).

## Cross-spec links

- [[042-integrated-authentication]] — SPN derivation under integrated auth uses the resolved port.
- [[031-connection-fedauth-refactor]] — same `MSSQLConnectionInfo` plumbing.
- [[028-ansi-connection-options]] — ADO.NET parser entry point.

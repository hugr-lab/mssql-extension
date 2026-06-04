---
description: "Task list for spec 045 — SQL Server Named-Instance Resolution"
---

# Tasks: Named-Instance Resolution

**Input**: Design documents from `/specs/045-named-instance-resolution/`
**Prerequisites**: spec.md, plan.md, research.md (all present). `quickstart.md` is a Phase 4 deliverable.

**Legend**:
- `[P0]` Phase 0 — wire format + pure parser, no network
- `[P1]` Phase 1 — UDP transport + end-to-end resolver
- `[P2]` Phase 2 — docker test stack (mock browser + SQL + client)
- `[P3]` Phase 3 — connection-string plumbing + LOGIN7 + settings
- `[P4]` Phase 4 — docs + manual-test recipe
- `[T]` Test-only task (not shipped in extension binary)
- `[X]` Cross-phase or refactor

## Ordering note

Phases run **in numeric order**. Phase 2 (docker stack) can start as soon as
Phase 0 (parser) lands — the mock browser does not depend on the resolver
itself — so a contributor can split the work. Phase 3 needs Phase 1 (resolver
API) and Phase 2 (test stack) both green to be testable end-to-end.

---

## Phase 0 — Wire format + pure parser

### T001 [P0] Define types in header
`src/include/connection/instance_resolver.hpp` (NEW)
- `struct BrowserInstance { string server_name; string instance_name; string version; uint16_t tcp_port; bool tcp_enabled; }`
- `struct ResolveError { enum Kind { Unreachable, InstanceNotFound, TcpDisabled, Malformed } kind; string message; }`
- `vector<BrowserInstance> ParseBrowserResponse(string_view raw);` — throws `IOException` with hex-dump message on malformed input.
- `class InstanceResolver { public: static expected<uint16_t, ResolveError> Resolve(const string& host, const string& instance, int timeout_seconds); };` — declaration only.
- **No** DuckDB headers in this file (mirrors [[feedback_iauthenticator_layering]] for `tds/auth/iauthenticator.hpp` — the resolver is logically reusable outside DuckDB).

### T002 [P0] Implement `ParseBrowserResponse`
`src/connection/instance_resolver.cpp` (NEW, parser section only)
- State machine over `string_view`: scan key, scan value separated by `;`, end of record on `;;` or end of buffer.
- Build `map<string,string>` per record, then materialise `BrowserInstance`.
- `tcp_enabled = record.count("tcp") > 0 && !record["tcp"].empty()`.
- Malformed input → `IOException("malformed SQL Browser response: " + hex_dump(raw, 32))`.

### T003 [T][P0] Unit tests — parser happy paths
`test/cpp/test_instance_resolver.cpp` (NEW)
- Single instance with all standard fields.
- Multiple instances in one response.
- Case-insensitive instance match (`testinst` matches `TESTINST` advertisement).
- Instance with TCP disabled (`tcp` field absent) → `tcp_enabled = false`.

### T004 [T][P0] Unit tests — parser error paths
Same file as T003.
- Truncated payload (RespSize > actual buffer).
- Random garbage bytes.
- Missing leading `0x05` opcode → `IOException`.
- Empty `RespData`.
- `RespData` not NUL-terminated.

### T005 [P0] CMake wiring
`CMakeLists.txt`
- Add `src/connection/instance_resolver.cpp` to extension sources.
- Add `test/cpp/test_instance_resolver.cpp` to the C++ test binary.
- No new external dependencies.

**Phase 0 exit gate**: `make test` green on macOS and Linux; new tests cover both success and all malformed-input cases.

---

## Phase 1 — UDP transport + end-to-end resolver

### T006 [P1] Implement `InstanceResolver::Resolve` (POSIX)
`src/connection/instance_resolver.cpp`
- Open `SOCK_DGRAM` socket, `connect()` to `host:1434/udp` (lets us use `send`/`recv` instead of `sendto`/`recvfrom`).
- Build `CLNT_UCAST_INST` packet: `0x04` + instance ASCII + `0x00`.
- `SO_RCVTIMEO` set to `timeout_seconds` seconds.
- One send → one recv (up to 1472 bytes — MTU cap). On `EAGAIN`/`EWOULDBLOCK`, retry **once** (FR-006). After the retry, return `ResolveError{Unreachable}`.
- On recv, hand the buffer to `ParseBrowserResponse`. Find the matching `BrowserInstance` by case-insensitive `instance_name`. Return its `tcp_port`.
- Distinguish the four error cases per research.md §R8.

### T007 [P1] Windows port
`src/connection/instance_resolver.cpp` — `#ifdef _WIN32` branch
- Use Winsock2 (`WSAStartup` already initialised by the TDS layer — spec 019).
- `SOCKET` instead of `int`, `closesocket` instead of `close`.
- `SO_RCVTIMEO` takes `DWORD` milliseconds on Winsock, not `timeval` — handle the divergence with a tiny helper rather than `#ifdef`'ing every call site.

### T008 [P1] Hostname normalisation
`src/connection/instance_resolver.cpp` (before the `Resolve` socket code)
- Translate `(local)` → `localhost`.
- Translate leading `.` (alone) → `localhost`.
- Keep IPv6-literal hosts as-is (no normalisation needed; UDP send goes to whatever family `getaddrinfo` returns).
- FR-011 covered.

### T009 [T][P1] Unit-ish test — loopback UDP echo
`test/cpp/test_instance_resolver.cpp` (extend)
- Spin a UDP listener on `127.0.0.1:<ephemeral>` inside the test process; have it reply with a canned `SVR_RESP`.
- `Resolve("127.0.0.1", "TESTINST", 1)` returns the canned port.
- Trick: the resolver hard-codes port 1434, so the test passes the listener port via a test-only `Resolve` overload (`Resolve_ForTest(host, port, instance, timeout)`) that the production `Resolve` calls with port=1434.
- Negative: listener silent → returns `ResolveError{Unreachable}` after 1s timeout + 1 retry (≈ 2s wall).

### T010 [T][P1] Settings registration
`src/connection/mssql_settings.cpp`
- `mssql_browser_timeout_seconds` (INTEGER, default `3`, range `1..30`).
- `mssql_named_instance_resolution` (BOOLEAN, default `true`).
- Add to the settings table in `CLAUDE.md` (defer the doc edit to T024 in Phase 4 to keep this task surgical).

**Phase 1 exit gate**: T009 passes deterministically (`make test` 5× consecutive). No flakiness on the timeout path (set `timeout_seconds = 1` not `3` for the negative test to keep test wall-time tolerable).

---

## Phase 2 — Docker test stack

### T011 [T][P2] Mock-browser Python implementation
`test/named-instance/mock-browser/browser.py` (NEW)
- Skeleton from research.md §R2.
- Two preconfigured instances: `TESTINST` (port from `SQL_TCP_PORT` env, default 11433) and `SECONDARY` (`SQL_TCP_PORT_2`, default 11434 — even though the compose stack only runs one SQL Server today, the second entry exercises the multi-instance parse path).
- Fault-injection modes via `MOCK_BROWSER_MODE`: `truncate`, `garbage`, `silent`, `slow`.
- Logs every request to stdout so `docker compose logs mock-browser` is useful.

### T012 [T][P2] Mock-browser Dockerfile
`test/named-instance/mock-browser/Dockerfile` (NEW)
- `FROM python:3.12-slim`
- `COPY browser.py /app/browser.py`
- `EXPOSE 1434/udp`
- `CMD ["python3", "/app/browser.py"]`

### T013 [T][P2] SQL Server init
`test/named-instance/sql/init.sql` (NEW)
- `CREATE DATABASE NamedInstTest;`
- `USE NamedInstTest; CREATE TABLE Probe (id INT, payload NVARCHAR(100)); INSERT INTO Probe VALUES (1, N'spec045 lives');`

### T014 [T][P2] SQL Server Dockerfile
`test/named-instance/sql/Dockerfile` (NEW)
- Same multi-stage init-script-baker pattern as `test/kerberos/sql/Dockerfile`, but listen on TCP 11433 (set via `MSSQL_TCP_PORT` env or `mssql.conf`). The non-default port is deliberate — proves the resolver is actually doing work, not just defaulting back to 1433.

### T015 [T][P2] Test-client Dockerfile
`test/named-instance/test-client/Dockerfile` (NEW)
- Multi-stage build of the extension inside `mcr.microsoft.com/devcontainers/cpp:ubuntu-22.04` (mirrors `test/kerberos/test-client/Dockerfile`).
- Runtime stage: `ubuntu:22.04` + `duckdb` CLI + the freshly-built `mssql.duckdb_extension`.

### T016 [T][P2] Test runner
`test/named-instance/test-client/run-tests.sh` (NEW)
- Runs every `test/sql/named_instance/*.test` against the live stack.
- Bails with a clear message if `sql.example.com:11433` isn't reachable (e.g. compose dependency ordering glitch).

### T017 [T][P2] Compose file
`test/named-instance/docker-compose.yml` (NEW)
- Three services: `sql`, `mock-browser`, `test-client`.
- Two-hostname variant from plan.md Phase 2.
- Healthchecks: `sql` waits for init.sql; `mock-browser` is up as soon as the process binds 1434/udp; `test-client` `depends_on` both healthy.
- `command: ["sleep", "infinity"]` on test-client so the user can `docker compose exec`.

### T018 [T][P2] README for the stack
`test/named-instance/README.md` (NEW)
- One-page how-to: bring up, run tests, tear down. Cross-link to issue #77 and to `test/kerberos/README.md` for the precedent.

### T019 [T][P2] SQLLogicTest cases
`test/sql/named_instance/*.test` (NEW — four files per plan.md Phase 2)
- `resolve_basic.test` — `ATTACH 'Server=browser.example.com\TESTINST;...'`, `SELECT id, payload FROM ... .Probe`.
- `resolve_unknown.test` — `Server=browser.example.com\NONESUCH` expects `MSSQL Error.*not found on host`.
- `resolve_browser_down.test` — `MOCK_BROWSER_MODE=silent`; expects `SQL Browser unreachable` within ~5s wall.
- `resolve_explicit_port.test` — `Server=browser.example.com\TESTINST,11433` skips the browser (verify by stopping the mock; attach still works).

**Phase 2 exit gate**: `docker compose up -d --build && docker compose exec test-client /run-tests.sh` passes from a cold start on macOS Apple Silicon and Linux x86_64.

---

## Phase 3 — Connection-string plumbing + LOGIN7

### T020 [P3] Parse `host\instance` in ADO.NET form
`src/mssql_storage.cpp`
- After the `,port` split at lines 694–702, split the host part on `\`:
  - No `\` → `instance` stays empty.
  - `\` with empty suffix → `InvalidInputException("empty instance name after backslash in Server")`.
  - `\` with suffix → validate against `[A-Za-z0-9_$#]{1,16}` (FR-010); store on `MSSQLConnectionInfo::instance_name`.
- Add `instance_name` (string) and `server_spec` (parsed-once struct) fields to `MSSQLConnectionInfo` in `src/include/mssql_storage.hpp`.

### T021 [P3] Parse `%5C` in URI form
`src/mssql_storage.cpp` — `ParseUri`
- URL-decode the host token before the same split-on-`\` logic from T020.
- Bare literal `\` in URI → parse-time `InvalidInputException` pointing at `%5C` (User Story 2 AS#3).

### T022 [P3] Invoke resolver in `FromConnectionString`
`src/mssql_storage.cpp`
- After host/port/instance are parsed, before returning:
  ```cpp
  if (!result->instance_name.empty() && !explicit_port_given) {
      if (!DBConfig::GetConfig(*context).options.set_options["mssql_named_instance_resolution"]) {
          throw InvalidInputException(
              "named-instance resolution is disabled; set mssql_named_instance_resolution=true "
              "or use Server=host,port");
      }
      auto port = InstanceResolver::Resolve(result->host, result->instance_name,
                                            settings.browser_timeout_seconds);
      if (!port.ok()) throw IOException(port.error().message);
      result->port = port.value();
  }
  ```
- The `context`-availability detail: `FromConnectionString` is sometimes called without a `ClientContext` (in secret resolution paths). Plumb the settings via a struct passed in by the caller rather than a global lookup — mirrors how `mssql_connection_timeout` is already threaded.

### T023 [P3] Populate `tds_server_name_` from instance
`src/tds/tds_connection.cpp`
- When `info_->instance_name` is non-empty, `tds_server_name_ = info_->host + "\\" + info_->instance_name` (LOGIN7 `ServerName` field).
- When empty, `tds_server_name_ = info_->host` (unchanged).
- Delete the misleading "we don't use SQL Browser service" comment at line 340.

### T024 [X] CLAUDE.md + Kerberos.md updates
- New settings in the settings table.
- One-line entry under "Recent Changes": `045-named-instance-resolution: …`.
- Kerberos.md SPN section: brief note that SPN derivation uses the discovered port for named instances.

**Phase 3 exit gate**: All four `test/sql/named_instance/*.test` pass against the live mock-browser stack; spec-042 Kerberos test suite (`docker compose exec test-client /run-tests.sh` in `test/kerberos/`) passes unchanged.

---

## Phase 4 — Docs + manual test recipe

### T025 [P4] quickstart.md
`specs/045-named-instance-resolution/quickstart.md` (NEW)
- Bring-up commands for the mock-browser stack.
- Manual repro for issue #77: install SQL Server Developer with two named instances on a Windows host, attach from DuckDB CLI.
- Windows + SSPI + named-instance manual-test recipe (research.md §R10 option b).

### T026 [P4] Issue-tracker update
- Comment on #77 with: spec link, what shipped, how to test, the manual workaround that's no longer needed.

### T027 [P4] PR description
- Standard template: summary, what changed, test plan (mock stack commands), screenshots/log snippets, link to spec.

**Phase 4 exit gate**: Issue #77 closed. Maintainer-team smoke test (one of us with a real multi-instance Windows install) reports back on the manual repro.

---

## Out-of-scope (not in this task list)

These were considered and explicitly deferred — *do not* add them to this PR:

- LocalDB pipe transport.
- Browser response caching.
- Auto-decoding of literal `\` in URI form.
- A Windows GHA runner for named-instance integration tests.
- A retry-count setting (one retry is hard-coded; revisit if needed).
- Hidden-instance probing (out-of-band of MC-SQLR).

## Suggested PR split

This spec can ship in one PR or two. Recommended split if reviewing in two:

1. **#1 (this PR)**: Phase 0–2 — parser + resolver + test stack, *no behaviour change to user-visible connect path yet*. The resolver exists but nothing calls it. Reviewable as pure additive code with strong test coverage.
2. **#2**: Phase 3–4 — connection-string parser changes, LOGIN7 wiring, docs. Small surgical diff that becomes trivial to review once #1 is in.

Single-PR is acceptable too — total diff is ~800 lines including tests and the docker stack.

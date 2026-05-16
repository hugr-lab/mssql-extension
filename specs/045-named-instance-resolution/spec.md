# Feature Specification: SQL Server Named-Instance Resolution (SQL Browser)

**Feature Branch**: `045-named-instance-resolution`
**Created**: 2026-05-16
**Status**: Draft
**Resolves**: [#77 — Unable to connect to the named instance](https://github.com/hugr-lab/mssql-extension/issues/77)
**Input**: User request: "Add support for `host\instance` server strings, like every other SQL Server client does."

## Problem Statement

Users with multiple SQL Server instances on a single host — the standard developer-laptop and shared-test-server pattern on Windows — install each engine as a **named instance** (e.g. `MSSQLSERVER` plus `SS2022` plus `SS2019`). Every instance other than the first listens on a **dynamic TCP port** chosen at service startup. Microsoft's reference page for [database engine instances](https://learn.microsoft.com/en-us/sql/database-engine/configure-windows/database-engine-instances-sql-server) is explicit that named instances are accessed by `host\instance` and that the port discovery happens via the **SQL Server Browser** service on UDP **1434** ([MC-SQLR]).

Today the extension treats the whole `host\instance` token as a DNS name:

```sql
ATTACH 'Server=localhost\SS2022;Database=db;User Id=sa;Password=...' AS db (TYPE mssql);
-- Error: Cannot resolve hostname 'localhost\SS2022'
```

The connection-string parser in `src/mssql_storage.cpp` splits on `,` (port) only, not on `\` (instance), so `host` ends up as the literal `"localhost\\SS2022"` and `getaddrinfo()` rejects it. The TDS connection layer does strip the instance for routing-redirect targets (`src/tds/tds_connection.cpp:340–347`, with the comment *"we don't use SQL Browser service"*) but never for the initial connect, and never to discover the actual port. Reporter @Stan-RED hit this with three coexisting engines on one host; @VGSML (contributor) confirmed it is unimplemented and recommended the manual fixed-port workaround.

This feature adds a real **SQL Server Browser** client that:

1. Recognises `host\instance` in the `Server` field (ADO.NET, URI, secret).
2. Sends a `CLNT_UCAST_INST` UDP query to `host:1434`.
3. Parses the `tcp;<port>` token out of the response.
4. Hands `(host, discovered_port)` to the existing TDS connect path.

The user-visible surface — `Server=host\instance` with no extra knobs — matches every other SQL Server client (pyodbc, `sqlcmd`, JDBC, `go-mssqldb`).

## User Scenarios & Testing *(mandatory)*

### User Story 1 — Connect to a named instance on a Windows dev box (Priority: P1)

A developer has SQL Server 2019, 2022, and a Developer-edition Express install on `localhost`, each as a named instance (`SS2019`, `SS2022`, `SQLEXPRESS`). The 2022 instance is on a dynamic port that changes whenever the service restarts. They attach using the standard `host\instance` form, with no port specified:

```sql
ATTACH 'Server=localhost\SS2022;Database=AdventureWorks;User Id=sa;Password=...;TrustServerCertificate=yes'
  AS adw (TYPE mssql);
SELECT TOP 1 name FROM adw.sys.tables;
```

The extension queries SQL Browser on `localhost:1434/udp`, gets back the current TCP port for `SS2022`, connects there, and the query succeeds. Restarting the SQL Server service may change the dynamic port — the next ATTACH still works because each new connection re-resolves.

**Why this priority**: This is the *only* user-visible feature in this spec. P1 is what fixes #77 for the reporter and the user who commented "this would be a very useful enhancement".

**Independent Test**: Bring up the test stack (see User Story 4); from the test client, `ATTACH 'Server=mock-browser-host\TESTINST;...'` and run a `SELECT 1`. Delivers end-to-end named-instance resolution against a faithful mock Browser.

**Acceptance Scenarios**:

1. **Given** SQL Browser is running on the target host and the named instance exists, **When** the user attaches with `Server=host\instance`, **Then** the connection succeeds and catalog queries return rows.
2. **Given** the user supplies `Server=host\instance,1433`, **When** they attach, **Then** the explicit port is honoured and SQL Browser is **not** contacted (the instance name is still sent in LOGIN7 `ServerName`).
3. **Given** SQL Browser is unreachable (firewall, service stopped), **When** the user attaches with `Server=host\instance`, **Then** ATTACH fails fast (within `mssql_browser_timeout_seconds`, default 3s) with an error containing `"SQL Browser unreachable"` and the resolved address.
4. **Given** SQL Browser responds but the requested instance is not in the response, **When** the user attaches, **Then** ATTACH fails with `"instance '<name>' not found on host '<host>'; available instances: <list>"`.
5. **Given** a subsequent ATTACH/connection-pool refill, **When** the dynamic port has changed since the last resolve, **Then** the next connect re-resolves and picks up the new port (no stale-port caching by default).

### User Story 2 — URI and secret forms accept `host\instance` (Priority: P2)

The same `host\instance` form works in the alternate connection-string surfaces:

- URI: `mssql://sa:pw@localhost%5CSS2022/db` (URL-encoded backslash, since `\` is not a valid URI host character).
- Secret: `CREATE SECRET (TYPE mssql, host 'localhost\SS2022', database 'db', user 'sa', password '...')`.

**Why this priority**: Consistency across our three input surfaces. Users who paste a URI from CI config or pull from a secret should not have to know which surface the resolver is plumbed through.

**Acceptance Scenarios**:

1. **Given** a URI with `%5C` between host and instance, **When** the user attaches, **Then** the resolver is invoked with the decoded `host\instance` pair.
2. **Given** a secret with `host = 'localhost\SS2022'`, **When** the user attaches via the secret, **Then** the resolver is invoked identically to ADO.NET form.
3. **Given** a URI with a literal backslash (`mssql://sa:pw@localhost\SS2022/db`), **When** the user attaches, **Then** ATTACH fails with a parse-time error pointing at `%5C`, since the URI grammar forbids the bare character.

### User Story 3 — Integrated auth (Kerberos / SSPI) against a named instance (Priority: P2)

A user with `Trusted_Connection=yes` connects to `host\instance`. The resolver discovers the port; the existing integrated-auth code derives the SPN from the **original** `host` and the **discovered** port — `MSSQLSvc/host.fqdn:<discovered-port>` — matching AD's default SPN registration for named instances.

**Why this priority**: Spec 042 shipped Kerberos/SSPI; named-instance support is incomplete without it interoperating, and AD-joined SQL Server users on dev boxes are the most likely hitters of #77.

**Acceptance Scenarios**:

1. **Given** a Kerberos TGT and `Server=host\instance;Trusted_Connection=yes`, **When** the user attaches, **Then** SPN derivation uses the discovered port and the SPNEGO negotiation succeeds.
2. **Given** the user supplies an explicit `service_principal_name=MSSQLSvc/fixed-spn:1433`, **When** they attach with `host\instance`, **Then** the explicit SPN is used (override takes precedence over derivation), and resolution still discovers the port for the TCP connect.

### User Story 4 — Mock Browser docker stack runs in CI (Priority: P1, *test infrastructure*)

The CI build brings up a self-contained 3-service compose stack: a SQL Server container (one instance, fixed port — the Linux image has no Browser concept), a small UDP responder that mimics SQL Server Browser's `CLNT_UCAST_INST` reply pointing at the SQL Server container on a non-default port, and the test client. Tests run end-to-end on macOS hosts (no Windows runner needed).

**Why this priority**: Without this, the only way to test the feature is a hand-installed Windows SQL Server with two named instances — slow, flaky, and incompatible with the project's existing macOS-first developer workflow.

**Acceptance Scenarios**:

1. **Given** `cd test/named-instance && docker compose up -d --build`, **When** the developer runs `docker compose exec test-client /run-tests.sh`, **Then** the suite passes on Linux and on macOS (Apple Silicon) without modification.
2. **Given** the suite, **When** it executes, **Then** it covers: (a) successful resolve, (b) unknown-instance, (c) Browser unreachable, (d) explicit-port skip-resolver, (e) malformed-response handling.

### Edge Cases

- **Empty instance name** (`Server=localhost\`): reject at parse time, mirroring `sqlcmd`.
- **Instance name with whitespace or `;`**: reject at parse time (the instance grammar in [MC-SQLR] is `[A-Za-z0-9_$#]`).
- **Browser reachable but returns a UDP packet > 65535 bytes / truncated**: cap at one MTU (1472 bytes), parse what we got, fail-clean on truncation rather than reading past the buffer.
- **Browser returns multiple instances**: pick the one whose `InstanceName` matches the requested instance **case-insensitively**, matching SQL Server's own comparison (`Latin1_General_CI_AS` for identifiers).
- **IPv6 hosts**: the UDP send goes to the same address family that DNS resolves, no special-casing required.
- **`(local)\instance` and `.\instance`** (legacy `sqlcmd` forms): resolve to `localhost` before the UDP send.
- **Concurrent attaches racing the same Browser**: each connection resolves independently — Browser is cheap (one UDP RTT) and a tiny shared cache risks stale ports after service restarts. *No caching in v1.* Revisit only if benchmarks show measurable cost.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: When the `Server` field in ADO.NET, URI, or secret form contains `\` and no explicit port, the extension MUST treat the suffix after `\` as a SQL Server instance name and resolve it via SQL Server Browser before connecting.
- **FR-002**: When the `Server` field contains both `\instance` and `,port`, the extension MUST use the explicit port and MUST NOT contact SQL Browser. The instance name is still passed in LOGIN7 `ServerName`.
- **FR-003**: The resolver MUST send a `CLNT_UCAST_INST` packet (`0x04` + ASCII instance name + `0x00`) to `host:1434/udp` as specified in [MC-SQLR] §2.2.1.
- **FR-004**: The resolver MUST parse the `SVR_RESP` body — a NUL-terminated `key;value;key;value;…` ASCII string with one record per instance — and extract the `tcp;<port>` token for the matching `InstanceName`. Comparison is case-insensitive.
- **FR-005**: The resolver MUST time out after `mssql_browser_timeout_seconds` (default **3s**). The default has to be short because the resolver is on the critical path of every connect for a named-instance attach.
- **FR-006**: The resolver MUST retry the UDP send **once** on timeout before failing (UDP is lossy; the cost of a single retry is small relative to the cost of a spurious ATTACH failure).
- **FR-007**: On unreachable Browser, unknown instance, or malformed response, the extension MUST surface a typed error distinguishable from generic TCP-connect failures, so users can tell "Browser broken" apart from "SQL Server broken".
- **FR-008**: The resolved `(host, port)` MUST flow through the existing TDS connect path unchanged. `tds_server_name_` (LOGIN7 `ServerName`) MUST keep the original `host\instance` form, not the discovered port.
- **FR-009**: When `Trusted_Connection=yes` and `Server=host\instance` are combined, the default SPN MUST be derived as `MSSQLSvc/<host.fqdn>:<discovered-port>` — *not* `MSSQLSvc/host\instance` (which AD does not register). An explicit `service_principal_name=` override takes precedence.
- **FR-010**: The instance grammar MUST be enforced at parse time: `[A-Za-z0-9_$#]{1,16}` per [MC-SQLR] §2.2.2. Reject empties and out-of-grammar characters with a clear error.
- **FR-011**: The legacy `(local)\instance` and `.\instance` aliases MUST be normalised to `localhost\instance` before resolution.
- **FR-012**: The `mssql_named_instance_resolution` setting (default `true`) MUST allow disabling the resolver entirely; with it `false`, a `host\instance` server string fails at parse time with "named-instance resolution disabled". Gives an escape hatch for environments that strip outbound UDP.
- **FR-013**: All error messages MUST include both the original `host\instance` token and (where known) the resolved address, so support questions on the issue tracker have enough context to triage without a reproducer.

### Non-Functional Requirements

- **NFR-001** (Performance): Resolution overhead on a healthy LAN MUST be < 10ms typical (one UDP RTT), and the implementation MUST NOT block other connections — the UDP send is per-connection and self-contained.
- **NFR-002** (Portability): The resolver MUST work on Linux, macOS, and Windows. UDP socket APIs differ enough (Winsock vs BSD) that the implementation reuses `tds_platform.hpp` shims rather than introducing a new abstraction.
- **NFR-003** (No new dependencies): UDP socket + parse — no new vcpkg packages. The whole resolver is `< 400` lines of C++ plus tests.
- **NFR-004** (Test independence): The mock-browser test stack MUST work on macOS Docker Desktop without modification, following the `test/kerberos/` precedent.

### Key Entities

- **InstanceServerSpec**: parsed `Server` field — `{ host: string, instance: optional<string>, explicit_port: optional<uint16_t> }`. Produced by the connection-string parsers, consumed by the resolver.
- **BrowserResponse**: parsed Browser reply — `vector<BrowserInstance>` where `BrowserInstance = { name, server, instance, tcp_port, version, is_clustered }`. Only `tcp_port` for the matching `instance` flows further; the rest is kept for diagnostic error messages ("available instances: …").
- **BrowserResolver**: free function or small class with a single method `Resolve(host, instance, timeout) → expected<port, error>`. Holds no state — one UDP socket per call.

## Out of Scope (v1)

- **Port caching across connections**. Adds complexity for a sub-millisecond saving on second-and-later attaches. Revisit after benchmarks.
- **`(localdb)` / SQL Server LocalDB pipe instances**. LocalDB uses named pipes, not TCP; a separate transport, not just a resolver.
- **`MSSQL_SERVERNAME` Windows registry probe**. We use only the documented MC-SQLR wire protocol so the same code path works identically on Linux and macOS.
- **Browser multicast discovery** (`CLNT_BCAST_EX`). The user has the host already; we never need to enumerate hosts.
- **Hiding the `instance` field**. Some SQL Server installs have a "hide instance" flag that suppresses Browser advertisement — those installs require fixed-port configuration regardless of client. We document this as a known limitation and point at the same MS docs (`configure-a-server-to-listen-on-a-specific-tcp-port`) that @VGSML already referenced on the issue.

## Success Criteria *(mandatory)*

- **SC-001**: A clean attach with `Server=localhost\SS2022` against the mock-browser stack returns rows in `< 200ms` end-to-end (resolver + connect + query).
- **SC-002**: Issue #77 reporter @Stan-RED can attach all three coexisting instances (`SS2019`, `SS2022`, `SQLEXPRESS`) on a single Windows host without specifying ports.
- **SC-003**: The pyodbc-equivalent connection string `Server=host\instance;Database=db;User Id=sa;Password=…` works unmodified in DuckDB. (Spot-check against pyodbc on the same host.)
- **SC-004**: CI green on Linux + macOS Apple Silicon with the mock-browser stack, no Windows runner required for the resolver tests.
- **SC-005**: Spec-042 integrated-auth tests still pass when re-pointed at the mock-browser-fronted SQL Server (regression: SPN derivation honours the discovered port).

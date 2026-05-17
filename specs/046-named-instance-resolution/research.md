# Research: SQL Server Named-Instance Resolution (spec 046)

Resolved questions and design decisions. All items are decision-locked unless flagged `OPEN`.

## R1. Wire format — `CLNT_UCAST_INST` and `SVR_RESP`

Source: [MC-SQLR] (Microsoft Open Specifications, latest revision 20.0
dated 2025-10-31; downloadable PDF at
`https://winprotocoldoc.z19.web.core.windows.net/MC-SQLR/[MC-SQLR].pdf`),
cross-referenced against `microsoft/go-mssqldb/msdsn/browser.go` and the
wire captures from `Microsoft.Data.SqlClient`.

### Wire-level string encoding

**The protocol-level encoding is MBCS, not ASCII.** [MC-SQLR] §2.2
(Message Syntax) states verbatim:

> All integer fields are represented in little-endian format. All text
> strings are represented as a multibyte character set (MBCS) string
> [MS-UCODEREF] on the current system code page of the server and the
> client. (The system code page on the client and the system code page
> on the server are assumed to be common.) They are not case-sensitive.

Confirmed by the per-field text in §2.2.3 (request) and §2.2.5
(response):

> §2.2.3 INSTANCENAME (variable): A variable-length null-terminated
> multibyte character set (MBCS) string ... MUST be no greater than 32
> bytes in length, not including the null terminator.

> §2.2.5 RESP_DATA (variable): A variable-length MBCS string that does
> not need to be byte-aligned. The maximum size of RESP_DATA MUST be
> 1,024 bytes if the server is responding to a CLNT_UCAST_INST request.

### Why "ASCII in practice" is still right for our implementation

In every realistic deployment the bytes that travel over the wire are
ASCII, because:

- **Keys** (`ServerName`, `InstanceName`, `IsClustered`, `Version`,
  `tcp`, `np`) are ASCII literals fixed by the spec.
- **Instance names**: §2.2.5 itself caps them: *"INSTANCENAME MUST be no
  greater than 255 bytes but SHOULD be no greater than 16 MBCS
  characters."* SQL Server itself further restricts instance names at
  install time to `[A-Za-z0-9_$#]{1,16}` (verified against the SSMS
  installer regex), so all production instance names ARE ASCII.
- **`tcp` value**: decimal port digits — ASCII.
- **`IsClustered`**: `"Yes"`/`"No"` — ASCII.
- **`Version`**: dotted-quad like `15.0.4123.1` — ASCII.
- **`np` value**: a `\\HOST\pipe\...` UNC pipe path. The hostname could
  in principle be a non-ASCII NetBIOS name under the server's active
  codepage, but we don't consume `np` at all.
- **`ServerName`**: typically the engine's NetBIOS or DNS hostname; both
  are practically ASCII (RFC 952/1123 DNS, and Microsoft's NetBIOS rules
  predate IDN). A server with a non-ASCII NetBIOS name under e.g. CP932
  is theoretically possible but extremely rare.

### Implications for the parser

The parser stores the body as a `std::string` byte buffer and treats
fields as opaque byte sequences. This is correct under MBCS:

1. **Tokenisation** keys off the `;` byte (0x3B) which is the same in
   every MBCS codepage (0x00–0x7F is invariant ASCII). No risk of a
   trail byte of a CP932 DBCS character colliding with `;`.
2. **Instance-name case-fold match** uses `std::tolower((unsigned char)
   c)` over both strings. This is correct for ASCII alphabetic
   characters (the only ones that can appear in a valid SQL Server
   instance name) and a no-op for digits and underscore. It does NOT
   do correct case-folding for non-ASCII MBCS bytes — but those can't
   appear in a valid instance name per Microsoft's own constraints.
3. **`tcp` value parse** uses `std::stoi` which only consumes ASCII
   digits — same in every MBCS codepage.
4. **Pass-through of `ServerName`** as raw bytes: any non-ASCII MBCS
   bytes survive the parser intact and reach the caller verbatim. The
   downstream consumer (`tds_connection.cpp` for the TCP connect, the
   integrated-auth strategy for the SPN) gets the original bytes; in
   the pathological case of a non-ASCII NetBIOS hostname under a
   foreign codepage, what the caller does with those bytes is the
   caller's problem — we do not corrupt them.

So the spec says MBCS, the parser respects MBCS in the byte-stream
sense, and the practical wire content is invariably ASCII. The earlier
internal documentation calling the wire format "ASCII" was imprecise
shorthand and has been corrected to "MBCS (ASCII in practice for the
documented fields)".

### Request — `CLNT_UCAST_INST`

```
+---------+----------------------+------+
| 0x04    | InstanceName (MBCS)  | 0x00 |
+---------+----------------------+------+
   1 byte    1..32 bytes           1
```

- Opcode `0x04` selects the unicast-by-name query (vs `0x02`
  `CLNT_UCAST_DAC` for dedicated admin connection, `0x03`
  `CLNT_UCAST_EX` for all instances on the host, `0x01` `CLNT_BCAST_EX`
  for subnet broadcast).
- Instance name is MBCS, NUL-terminated, max 32 bytes including the NUL
  per the spec — *but* SQL Server only ever registers names of
  `[A-Za-z0-9_$#]{1,16}` (the engine itself rejects longer at install
  time), and we enforce the tighter range at parse time (FR-010). The
  32-byte cap is the wire limit, not a permission to send longer.
- Sent as a single UDP datagram to `host:1434`.

### Response — `SVR_RESP`

```
+---------+----------+---------------------+
| 0x05    | RespSize | RespData (MBCS)     |
+---------+----------+---------------------+
   1 byte    2 bytes    RespSize bytes
                        NUL-terminated
```

- `RespSize` is little-endian.
- `RespData` is a NUL-terminated MBCS string of `key;value;` pairs, one
  **logical record** per instance, concatenated. The record separator
  is implicit — `;;` marks end of record, but in practice SQL Server
  emits `IsClustered;No;` then immediately the next `ServerName;…` for
  the next instance, so the parser must key off the appearance of a
  second `ServerName;` to delimit records.
- Standard keys for `CLNT_UCAST_INST` (always emitted, in this order):
  `ServerName`, `InstanceName`, `IsClustered`, `Version`, `tcp`, `np`
  (named pipes — present even when disabled, value `\\.\pipe\...`).
- A record without a `tcp;` entry means TCP is disabled on that
  instance — we surface that as an explicit error ("instance found but
  TCP transport not enabled"), not as "instance not found".

### Decision

Implement the parser as a single state machine over the MBCS byte
buffer (treated as opaque bytes; the only bytes the state machine ever
compares against are ASCII-invariant `;` and key literals): scan key,
scan value, until end of buffer or `;;`. Build
`vector<map<string, string>>`. Then `for each record: if
record["InstanceName"].lower() == requested.lower(): return
stoi(record["tcp"])`. ~80 lines of C++.

## R2. Mock Browser — language and shape

**Decision**: Python 3 + stdlib `socket`. ~60 lines. Lives at `test/named-instance/mock-browser/browser.py`.

Alternatives considered:

- **Go**: nicest concurrency story, but adds a second build toolchain to the test image.
- **C++ reusing our own parser inverted**: looks neat but couples test infra to the implementation under test (a parser bug would mask itself).
- **`nmap --script ms-sql-info`**: not a server, just a client.

Python wins because every dev box already has `python3` and Docker's `python:3.12-slim` image is < 50MB. The mock implements `CLNT_UCAST_INST` only — broadcast / `UCAST_EX` return an empty response.

Skeleton:

```python
import socket, os, struct

INSTANCES = {
    # InstanceName (case-insensitive) -> (server_name, tcp_port, version)
    "TESTINST": ("sql.example.com", int(os.environ.get("SQL_TCP_PORT", "11433")), "15.0.4123.1"),
    "SECONDARY": ("sql.example.com", int(os.environ.get("SQL_TCP_PORT_2", "11434")), "15.0.4123.1"),
}

def build_response(records):
    body = ""
    for name, (server, port, version) in records:
        body += f"ServerName;{server};InstanceName;{name};IsClustered;No;Version;{version};tcp;{port};;"
    body_bytes = body.encode("ascii") + b"\x00"
    return bytes([0x05]) + struct.pack("<H", len(body_bytes)) + body_bytes

def handle(data, addr, sock):
    if not data or data[0] != 0x04:
        return  # silently drop non-UCAST_INST queries
    requested = data[1:].rstrip(b"\x00").decode("ascii", errors="replace")
    match = [(n, v) for n, v in INSTANCES.items() if n.lower() == requested.lower()]
    sock.sendto(build_response(match), addr)

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", 1434))
    while True:
        data, addr = sock.recvfrom(4096)
        try: handle(data, addr, sock)
        except Exception as e: print("err:", e)

if __name__ == "__main__":
    main()
```

The mock honours an env var `MOCK_BROWSER_MODE` for fault injection: `truncate` (drop the final NUL), `garbage` (send 8 random bytes), `silent` (don't reply at all), `slow` (sleep 5s before replying — exercises the resolver's 3s timeout + retry path).

## R3. Docker compose layout

Three services on a private bridge network, mirroring `test/kerberos/`:

```
test/named-instance/
├── docker-compose.yml
├── mock-browser/
│   ├── Dockerfile        # FROM python:3.12-slim; COPY browser.py
│   └── browser.py
├── sql/
│   └── init.sql          # CREATE DATABASE NamedInstTest; CREATE TABLE ...
├── test-client/
│   ├── Dockerfile        # multi-stage: build extension, copy into runtime
│   └── run-tests.sh
└── README.md
```

Services:

| service        | hostname              | image                                     | exposed   |
|----------------|-----------------------|-------------------------------------------|-----------|
| `sql`          | `sql.example.com`     | `mcr.microsoft.com/mssql/server:2022-latest` (started on port 11433, not 1433, to prove the resolver actually does something) | 11433/tcp |
| `mock-browser` | `sql.example.com`     | local Python build                        | 1434/udp  |
| `test-client`  | `client.example.com`  | local multi-stage build of the extension  | —         |

**Key choice**: `mock-browser` shares the `sql.example.com` hostname alias with the SQL Server container — Docker networking lets two services answer on different ports of the same hostname only if they don't both bind 0.0.0.0 inside their containers. We instead give `mock-browser` and `sql` **distinct service names** but both `aliases: [sql.example.com]` on the network. The mock binds `0.0.0.0:1434/udp`, the SQL container binds `0.0.0.0:11433/tcp` — no conflict because Docker DNS resolves `sql.example.com` to two IPs and the client picks based on port. *Verify on first compose-up*: if Docker DNS round-robins, fall back to two distinct hostnames (`browser.example.com` for the mock, `sql.example.com` for SQL) and the mock points its `tcp;` field at `sql.example.com:11433`. The client only resolves `browser.example.com` for the UDP send, then resolves whatever hostname the Browser returned for the TCP connect.

**Decision**: start with the **two-hostname** layout because it's robust and matches real-world Browser semantics (Browser can advertise a different host than itself, e.g. failover-cluster scenarios). The single-alias trick is a micro-optimisation that buys nothing.

## R4. Resolver placement in code

```
src/connection/instance_resolver.{hpp,cpp}   // new file, ~250 LOC
src/include/connection/instance_resolver.hpp
test/cpp/test_instance_resolver.cpp          // parser unit tests, no network
```

**Why under `connection/`, not `tds/`**: the resolver is a connection-string concern, not a TDS protocol concern. It runs *before* the first byte of TDS hits the wire. Placing it in `tds/` would force `tds_connection.cpp` to know about UDP, which it doesn't today.

**Entry point**: `MSSQLConnectionInfo::FromConnectionString` already centralises parsing in `src/mssql_storage.cpp`. After it sets `result->host` and `result->port`, a new branch:

```cpp
if (server_spec.instance.has_value() && !server_spec.explicit_port.has_value()) {
    auto resolved = InstanceResolver::Resolve(result->host, *server_spec.instance,
                                              settings.browser_timeout_seconds);
    if (resolved.is_error()) throw IOException(resolved.error_message());
    result->port = resolved.value();
}
result->instance_name = server_spec.instance.value_or("");  // for LOGIN7 ServerName + SPN
```

The `instance_name` field on `MSSQLConnectionInfo` is new and consumed by `tds_connection.cpp` when composing `tds_server_name_` (replacing the current `host` → `host\instance` reconstruction logic, which today only works for routing-redirect targets).

## R5. Caching: revisited

Spec text already commits to "no caching in v1". Restating *why* for future reviewers:

- Resolution is one UDP RTT on a healthy LAN. Measured at ~2ms in dev; 10ms is the conservative upper bound (NFR-001).
- Dynamic ports change on every SQL Server service restart. A cache means the first attach *after* a service restart silently connects to a stale port and gets either a TCP RST or — worse, in a multi-instance setup — a connection to the *wrong* instance.
- The work to do caching safely (TTL + invalidation on connect failure) is non-trivial relative to the work to skip caching (zero).
- Connection pooling already reuses authenticated sessions; the resolver only fires for pool-fill, not for query execution.

If someone shows a benchmark where the 2ms matters, we'll revisit. Until then: no cache.

## R6. SPN derivation under integrated auth

The current SPN derivation in `src/tds/auth/krb5_authenticator.cpp` builds `MSSQLSvc/<host>:<port>` from `info.host` and `info.port`. After spec 046:

- `info.host` is unchanged (the original hostname token, instance stripped).
- `info.port` is the **discovered** port from the Browser response.
- The default SPN therefore becomes `MSSQLSvc/<host>:<discovered_port>`, which is what AD registers for named instances by default ([the SQL Server docs page on SPN registration](https://learn.microsoft.com/en-us/sql/database-engine/configure-windows/register-a-service-principal-name-for-kerberos-connections) is explicit on this).

**No code change in the auth path** — the resolver writes the discovered port into `info.port` before auth ever runs.

**Override path** is also unchanged: `service_principal_name=` short-circuits derivation. FR-009 explicit.

## R7. URI parsing — backslash handling

The URI grammar (RFC 3986 §3.2.2 host) does not permit `\` in `host`. Three options:

1. **Reject literal `\`**, require `%5C` (current spec — FR via User Story 2 AS#3).
2. **Tolerate literal `\`** by URL-decoding in a pre-pass — most users won't know to encode.
3. **Tolerate, with a deprecation warning** — soften over one release.

**Decision**: option 1. The URI form is the niche surface (ADO.NET is what most users paste); tolerating malformed URIs is a lifetime of paper cuts (every parser disagreement is a new bug). A clear error pointing at `%5C` is one Google search away from a fix.

## R8. Error taxonomy

Four distinct failure modes; each MUST produce a distinct error so support questions are triagable from the message alone:

| Failure                       | Message prefix                                            | Suggested action                                       |
|-------------------------------|-----------------------------------------------------------|--------------------------------------------------------|
| Browser unreachable (timeout) | `SQL Browser unreachable at <host>:1434/udp after Ns`     | Check firewall (UDP 1434), `SQL Server Browser` service |
| Browser reachable, instance unknown | `instance '<name>' not found on host '<host>'; available: <list>` | Check spelling, `SELECT @@SERVERNAME` on the instance  |
| Instance found, TCP disabled  | `instance '<name>' exists but TCP transport is disabled`  | Enable TCP/IP in SQL Server Configuration Manager      |
| Malformed Browser response    | `malformed SQL Browser response: <hex dump first 32 bytes>` | File a bug — should never happen with real SQL Server  |

The hex dump in case 4 is intentional: we've seen security middleboxes mangle UDP 1434 responses, and the dump is the only diagnostic that survives the user-bug-report game of telephone.

## R9. Settings names — bikeshed-locked

Two new DuckDB settings:

| Setting                                | Default | Range         |
|----------------------------------------|---------|---------------|
| `mssql_browser_timeout_seconds`        | `3`     | `1..30`       |
| `mssql_named_instance_resolution`      | `true`  | `true`/`false`|

Naming follows the existing `mssql_<thing>_<unit>` convention (cf. `mssql_connection_timeout`, `mssql_idle_timeout`). No setting for retry count — one retry is hard-coded (FR-006). If we ever need to disable the retry, a setting is a one-line addition.

## R10. OPEN — Windows test path

The mock-browser stack is Linux-containers-only. SSPI integrated-auth on a real named instance is the one combination that won't be exercised by the CI mock stack (because the mock SQL Server has no Browser of its own and the test client uses Kerberos via the `test/kerberos/` stack on Linux, not SSPI). Options:

- **(a)** Add a Windows runner job to `.github/workflows/ci.yml` that installs SQL Server Express with a named instance and runs a slimmed-down smoke test. Slow (~10 min) but real.
- **(b)** Mark "Windows + SSPI + named instance" as manually verified in the release checklist, document the manual repro steps, do not gate CI on it.
- **(c)** Defer to a follow-up spec.

Lean toward **(b)** because the Linux Kerberos path already exercises every byte of the resolver and the SPN-derivation logic, and option (a) would more than double total CI wall time. *Decision deferred to plan.md.*

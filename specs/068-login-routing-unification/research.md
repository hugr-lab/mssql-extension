# Phase 0 Research — spec 068

All findings are against `571e465` (main after PR #254/#255), which is the base
this branch is built on. Line numbers below are from that commit.

The spec's decisions D1–D4 are taken as settled. This document resolves the
implementation unknowns those decisions leave open, and records three code
findings the spec did not have.

---

## R1 — The attempt callback's return type

**Question**: the spec sketches `bool RunWithRoutingHops(const std::function<bool()> &attempt)`.
Can a `bool` carry the outcome?

**Decision**: no — the callback returns a tri-state enum.

```cpp
enum class LoginAttemptOutcome : uint8_t { Success, Route, Failure };
```

**Rationale**: D1's whole point is that route-without-LOGINACK is neither
success nor failure. With a `bool` the driver would have to read `has_routing_`
as a side channel on the `false` return — which is precisely the
"success-flag-plus-hidden-flag" arrangement that produced Gap 1 in the first
place. The enum makes the contract unrepresentable-if-wrong: a login helper
that sees ROUTING **must** say `Route`, and there is no `false` for the driver
to misinterpret. It also removes the `bool` overload ambiguity when the
callback is a lambda returning an implicitly-convertible expression.

**Alternatives considered**:
- `bool` + `has_routing_` side channel (spec's sketch) — rejected as above.
- `std::optional<RoutedTarget>` return — expresses Route/Success but collapses
  Failure into "empty", the same defect mirrored.
- Exceptions for failure — rejected: the TDS layer reports login failure via
  `last_error_` + `false` everywhere, and `TdsConnection` is constructed on
  pool-refill worker threads where the existing contract is no-throw.

---

## R2 — Who owns the state-machine transitions

**Question**: the three auth paths currently transition
`ConnectionState` in different places. Where do transitions live after the
extraction?

**Finding (new — not in the spec)**: the paths are inconsistent today.

- `AuthenticateWithFedAuth` (`tds_connection.cpp:243`): the **wrapper** stores
  `Disconnected` on each failure and `Idle` on success; the helpers
  (`DoPreloginWithFedAuth`, `DoLogin7WithFedAuth`) only set `last_error_`.
- `AuthenticateIntegrated` (`:662`): the **method body itself** stores
  `Disconnected` and calls `socket_->Close()` at nine separate failure points,
  interleaved with the SPNEGO loop.
- `Authenticate` (`:149`): wrapper-owned, like fedauth.

**Decision**: the driver owns every transition. The attempt callback sets
`last_error_` and returns an outcome; it must not touch `state_` or close the
socket. The nine in-body transitions in `AuthenticateIntegrated` move out to the
driver as part of the extraction.

**Rationale**: constitution IV. A hop needs the socket closed *and reopened*
with the state still `Authenticating` — if a callback stores `Disconnected`
mid-loop, the next `socket_->Connect()` succeeds while the state machine says
the connection is dead, and the eventual `Idle` store papers over it. Keeping
the transition set in one function is also what makes the fake-server test able
to assert on it.

**Note**: the hop loop calls `socket_->Connect()` directly, **not**
`TdsConnection::Connect()` — the latter requires the `Disconnected` state and
would re-enter the state machine. This is what today's fedauth loop does
(`:372`) and it is preserved deliberately.

---

## R3 — Can the fake-server hop test link without DuckDB?

**Question**: D4 layer 2 needs a real `TdsConnection` driving a real socket.
The existing standalone TDS tests link four files and `-lsimdutf`. Does pulling
in `TdsConnection` drag in DuckDB (and therefore a full `make` before the test
can build, i.e. the heavy CI job)?

**Decision**: no DuckDB needed. Link set is:

```
test/cpp/test_login_routing_hops.cpp
src/tds/tds_connection.cpp src/tds/tds_socket.cpp
src/tds/tds_packet.cpp src/tds/tds_protocol.cpp src/tds/tds_types.cpp
src/tds/encoding/utf16.cpp
src/tds/tls/tds_tls_context.cpp src/tds/tls/tds_tls_impl.cpp
-lssl -lcrypto -lsimdutf
```

**Evidence**: `src/tds/tds_connection.cpp` includes only
`tds/tds_connection.hpp`, `<cstdio/cstdlib/cstring>` and a platform header
(`windows.h` / `unistd.h`). Its header pulls `tds/auth/iauthenticator.hpp`
(pure-virtual, DuckDB-free by the standing rule recorded in CLAUDE.md),
`tds_platform.hpp`, `tds_protocol.hpp`, `tds_socket.hpp`, `tds_types.hpp`.
`tds_socket.hpp` pulls `tls/tds_tls_context.hpp`, hence the two TLS TUs and
OpenSSL. Nothing reaches into `duckdb/`.

**Consequence**: the test goes in the **light** CI job alongside
`test_login7_encoding` / `test_login_error_state` / `test_skip_form_equivalence`
(`.github/workflows/ci.yml` ~365–395), not behind the dispatch-gated heavy
build. That matters: per the repo's own history, tests that only run in the
dispatch-gated job effectively do not run (see the integration-suite
false-green history, issue #212).

**Alternative considered**: add it to `STANDALONE_TEST_SOURCES` (`Makefile:396`),
which links the built extension archive + `libduckdb_static.a`. Rejected —
it requires `make` (release) first and would land the test in the gated job for
no benefit.

---

## R4 — Fake-server test design and flake budget

**Decision**: one test binary, two `SOCK_STREAM` listeners bound to
`127.0.0.1:0` (kernel-assigned ports, read back with `getsockname`), each
served by a `std::thread` running a hand-rolled TDS responder that speaks
exactly three things: a PRELOGIN response with `ENCRYPT_NOT_SUP`, a LOGIN7
response, and nothing else. `use_encrypt=false` throughout, so no TLS keys and
no OpenSSL handshake in the test path.

Scenarios pinned (all four are D4's list):

1. Single hop, SQL auth: listener A answers LOGIN7 with ROUTING → B; B answers
   LOGINACK + DONE. Assert `Authenticate()` returns true, `GetState()==Idle`,
   and the connection's host/port are B's.
2. Route-without-LOGINACK (the Gap-1 shape) still follows the hop.
3. Hop-limit abort: a listener that routes **to itself** must fail after
   exactly 5 hops with an error naming the count and the last routed target.
4. Per-hop packet-id reset: listener B asserts the first packet it receives has
   `packet_id == 1`.

**Flake mitigations**, decided up front rather than after the first red CI:
- bind port 0, never a fixed port;
- `SO_REUSEADDR`; accept with a poll timeout, so a hung accept fails the test
  in seconds instead of hanging the job;
- the responder threads are joined and the listeners closed in a scope guard,
  including on assertion failure (the existing `CHECK` macro calls
  `std::exit(1)`, so cleanup is process teardown — acceptable, and noted so
  nobody adds an fd-leak assertion later);
- overall test wall-clock budget: a few hundred ms. `DEFAULT_CONNECTION_TIMEOUT`
  is only reached on a genuine bug.

**If it flakes anyway**: gate behind an env var (`MSSQL_TEST_FAKE_SERVER=1`)
rather than delete — the spec's own instruction.

**Windows**: `TdsSocket` already calls `WSAStartup` and `tds_platform.hpp`
provides the `ssize_t` shim. The test's own listener code needs the same
`#ifdef _WIN32` split. Windows CI is manual-dispatch and currently red for
unrelated MSVC reasons (#165), so the Windows leg of this test is
"compiles, run manually", stated here so `/speckit-tasks` does not schedule
verification that cannot happen.

---

## R5 — Routed-target normalization details

**Decision**: lift `AuthenticateWithFedAuth`'s inline block (`:309–369`) into a
private helper, unchanged in behaviour:

```cpp
// Returns {tcp_host, port}; sets tds_server_name_ as a side effect.
void NormalizeRoutedTarget(const std::string &routed_server, uint16_t routed_port,
                           std::string &out_host, uint16_t &out_port);
```

Behaviour preserved verbatim: trailing `:digits` wins over the ENVCHANGE port;
`hostname\instance` (port stripped) becomes `tds_server_name_` for the LOGIN7
ServerName field; everything from `\` onward is stripped for DNS/TCP; no SQL
Browser lookup.

**UTF-8 safety**: the routed server arrives as UTF-16LE and is decoded to UTF-8
at `tds_protocol.cpp:895`. Scanning for `':'` (0x3A) and `'\\'` (0x5C) over
UTF-8 is safe — continuation bytes are all ≥ 0x80, so no multi-byte sequence can
contain either. `std::isdigit` is called with the `unsigned char` cast already
present at `:323`. No change needed; recorded because an IDN routed hostname is
plausible on Azure and someone will ask.

**Out of scope, reaffirmed**: a routed `host\instance` with **no** port does not
trigger a Browser lookup. The error message names the routed string verbatim.

---

## R6 — Module placement

**Decision**: everything stays in `src/tds/tds_connection.cpp` +
`tds_connection.hpp`. No `src/tds/routing/` module.

**Rationale**: the driver plus the normalizer is ~120 lines with exactly one
caller (`TdsConnection` itself), and both touch private members (`host_`,
`port_`, `tds_server_name_`, `next_packet_id_`, `tls_enabled_`,
`fedauth_echo_`, `socket_`). Extracting them would mean either a friend class or
widening the member surface — cost with no reuse on the other side.

---

## R7 — SQL-auth LOGIN7 sends the wrong ServerName after a hop (new finding)

**Not in the spec.** `DoLogin7` builds LOGIN7 with `host_` as the ServerName
(`tds_connection.cpp:847`):

```cpp
TdsPacket login = TdsProtocol::BuildLogin7(host_, username, password, ...);
```

The integrated path already does the right thing
(`tds_server_name_.empty() ? host_ : tds_server_name_`, `:725`), and the fedauth
builder is passed the same. Once SQL auth can hop, `host_` is the
**instance-stripped DNS name** while the server expects `hostname\instance` in
ServerName — exactly the Fabric shape D2 calls out.

**Decision**: `DoLogin7` adopts the same `tds_server_name_.empty() ? host_ : …`
expression. On a first attempt `tds_server_name_` is set to `host_` by the
driver (as `AuthenticateWithFedAuth:255` does today), so **non-routing SQL auth
is byte-identical** to current behaviour.

---

## R8 — Per-hop state resets: what the current loop resets, and what it misses

Audited every member that a second login attempt on a fresh socket could carry
over. The fedauth loop resets `has_routing_`, `routed_server_`, `routed_port_`,
`next_packet_id_`, `tls_enabled_`, `fedauth_echo_` (`:359–364`).

| Member | Carried over? | Verdict |
|---|---|---|
| `next_packet_id_` | reset to 1 | correct; pinned by fake-server scenario 4 |
| `tls_enabled_` | reset | correct — new socket, new handshake |
| `fedauth_echo_` | reset | correct; harmless to reset on the other two paths |
| `has_routing_` / `routed_server_` / `routed_port_` | reset | correct — and the driver must **also** clear them *before the first* attempt, so a reused `TdsConnection` cannot inherit a stale route |
| `spid_`, `negotiated_packet_size_` | overwritten by the next successful login | fine |
| `utf8_support_acked_` | **assigned**, not OR'd, by `NoteFeatureAcks` (`tds_connection.hpp:224`) | fine — a gateway's UTF8 ack cannot survive onto a routed server that does not ack. Checked explicitly because it would have been a silent wrong-decode bug |
| `tds_server_name_` | deliberately **kept** — it is the hop's output | correct |
| `original_sni_hostname_` | untouched by the loop | unused on this path; leave alone |
| `database_` | set only on success | fine |

**Decision**: the driver's reset block is the fedauth six, plus clearing the
routing triple before the first attempt. No other member needs it.

---

## R9 — Integrated auth: what the factory closes over

**Question**: D3 changes the parameter to a `(host, port) → IAuthenticator`
callable. Does the SPN actually follow?

**Verified**: `AuthStrategyFactory::Create` derives the SPN from
`info.host` / `info.port` (`auth_strategy_factory.cpp`, `DeriveSpn`:
`"MSSQLSvc/" + info.host + ":" + std::to_string(info.port)`), and returns
`info.service_principal_name` **verbatim** when the user set one. So a factory
that copies `MSSQLConnectionInfo` and overwrites `host`/`port` per hop gets the
routed SPN for free, and an explicit override survives hops with no extra code —
D3's requirement falls out of the existing derivation rather than needing new
logic.

**Decision on the factory's host argument**: pass the **TCP/DNS host**
(instance-stripped), not `tds_server_name_`. AD registers
`MSSQLSvc/<fqdn>:<port>`; a `host\instance` string is not a valid SPN component.

**Both call sites already hold the info by value** — `mssql_catalog.cpp:168`
(`MSSQLConnectionInfo info_copy = *connection_info_;`) and
`mssql_storage.cpp:1303` (`MSSQLConnectionInfo &info` parameter). The change is
local, as the spec states. The `nullptr`-authenticator branch
(`tds_connection.cpp:676`, the "compiled without Kerberos support" message)
moves to "factory returned null on the first call" and keeps its wording.

**Free()**: `authenticator->Free()` is called once on success (`:770`). Per hop,
the previous hop's authenticator must be `Free()`d before the next is built —
the driver does this on the `Route` outcome. Missing it leaks a GSSAPI context
per hop.

---

## R10 — Documentation surface

**Decision**: four files, each with a specific claim to change.

- `docs/tds-protocol.md` — ENVCHANGE 20 section: the D1 contract, the hop limit,
  the no-Browser-on-hop rule.
- `docs/architecture.md` — routing is a *connection-level* behaviour, not an
  Azure-only one; the per-path table becomes a single row.
- `Kerberos.md` — SPN on a hop: derived for the routed host; explicit
  `service_principal_name=` survives verbatim.
- `DATAMODEL.md` — the connection layer's login step is a hop loop; per CLAUDE.md
  this is exactly the "alters an end-to-end flow" trigger that requires updating
  it in the same PR.

The user-visible behaviour change (SQL auth stops "succeeding" against a routing
gateway) goes in the changelog, per the spec's risk section.

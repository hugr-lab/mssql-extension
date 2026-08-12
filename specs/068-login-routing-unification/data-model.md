# Phase 1 Data Model — spec 068

No persisted data and no DuckDB-visible types. The "entities" here are the
in-memory types and the connection-object state that the hop driver reads and
writes. They are listed with the same rigour a storage model would get because
the bugs this spec fixes are all state-lifetime bugs.

---

## 1. `LoginAttemptOutcome` (new enum)

`src/include/tds/tds_connection.hpp`, namespace `duckdb::tds`.

| Value | Meaning | Driver action |
|---|---|---|
| `Success` | LOGINACK received, no ROUTING | stop the loop, transition to `Idle` |
| `Route` | ROUTING received (**with or without** LOGINACK) | close socket, normalize target, reset per-hop state, reconnect, run the attempt again |
| `Failure` | neither LOGINACK nor ROUTING | stop the loop, transition to `Disconnected`, close socket, `last_error_` already set by the callback |

**Invariant (D1)**: a login helper that observes `LoginResponse::has_routing ==
true` returns `Route`. It never returns `Success` and never returns `Failure`,
whatever `LoginResponse::success` says. This is the entire contract of the
feature; every other rule below serves it.

Backed by `uint8_t`. C++11-compatible (`enum class` with fixed underlying type
is C++11).

---

## 2. `AuthenticatorFactory` (new type alias)

`src/include/tds/tds_connection.hpp`, namespace `duckdb::tds`.

```cpp
using AuthenticatorFactory =
    std::function<std::shared_ptr<IAuthenticator>(const std::string &host, uint16_t port)>;
```

| Field | Rule |
|---|---|
| `host` argument | the **TCP/DNS** host of the current hop — instance-stripped, no port suffix. Never `tds_server_name_` (research.md R9) |
| `port` argument | the current hop's TCP port |
| return value | a fresh authenticator, or `nullptr` |
| `nullptr` on the **first** call | maps to the existing "compiled without Kerberos / SSPI support" error, wording preserved |
| `nullptr` on a **hop** | `Failure` with an error naming the routed host and the SPN that would have been requested (the field-diagnosability mitigation from the spec's risk section) |

**Lifetime**: the factory is called once per login attempt. The authenticator
from attempt *N* is `Free()`d before attempt *N+1* is built. The factory itself
must outlive the `AuthenticateIntegrated` call — both call sites pass a lambda
capturing `MSSQLConnectionInfo` **by value**, so this holds without further
work.

**Validation rule**: the factory must not capture the connection object or any
`ClientContext` — it runs on pool-refill worker threads (issue #178/#179
contract).

---

## 3. `RoutedTarget` (transient, not a stored member)

Produced by `NormalizeRoutedTarget` from `LoginResponse::routed_server` +
`routed_port`.

| Field | Source | Rule |
|---|---|---|
| `tcp_host` | routed_server, `:port` suffix stripped, everything from `\` stripped | used for `socket_->Connect()` and for TLS SNI |
| `tcp_port` | trailing `:digits` if present and in 1..65535, else `LoginResponse::routed_port` | the suffix wins — Fabric sends both and they can differ |
| `tds_server_name` | routed_server with `:port` stripped but `\instance` **kept** | written to the member `tds_server_name_`; goes in the LOGIN7 ServerName field |

Malformed input (empty routed_server, port 0, unparseable suffix) does not
throw: the suffix parse falls back to the ENVCHANGE port, and an empty host
fails at `socket_->Connect()` with an error naming the routed string verbatim.

---

## 4. `TdsConnection` state touched by the driver

State transitions (`ConnectionState`), driver-owned:

```
Authenticating ──attempt→Success──→ Idle
       │
       ├──attempt→Route──→ (socket close, reconnect) ──→ Authenticating   [≤ 5 times]
       │
       ├──attempt→Failure──→ Disconnected
       └──hop limit exceeded / reconnect failed──→ Disconnected
```

`Authenticating` is never left during a hop — the loop uses `socket_->Connect()`
directly, not `TdsConnection::Connect()` (research.md R2).

Per-attempt member state:

| Member | Reset before **every** attempt | Notes |
|---|---|---|
| `has_routing_`, `routed_server_`, `routed_port_` | yes | includes before the *first* attempt, so a reused connection cannot inherit a stale route (R8) |
| `next_packet_id_` | yes → 1 | pinned by fake-server scenario 4 |
| `tls_enabled_` | yes → false | new socket, new handshake |
| `fedauth_echo_` | yes → false | fedauth-only meaning; harmless on the other paths |
| `tds_server_name_` | **no** — written by the hop | initialized to `host_` before the first attempt, on all three paths |
| `spid_`, `negotiated_packet_size_`, `utf8_support_acked_`, `database_` | no | each is assigned (not accumulated) by a successful login; verified in R8 |

---

## 5. Ordering invariant inside the login helpers (D1)

For each of `DoLogin7`, `DoLogin7WithFedAuth`, and the integrated response
handler, the required statement order after `ParseLoginResponse` is:

1. `NoteFeatureAcks(login_response)`
2. copy `has_routing` / `routed_server` / `routed_port` into the members
3. if `has_routing_` → return `Route` *(before any `success` check)*
4. if `!success` → set `last_error_`, return `Failure`
5. `spid_` / `negotiated_packet_size_` / `ApplyNegotiatedFraming()`, return `Success`

Step 3 preceding step 4 **is** the Gap-1 fix. Today `DoLogin7WithFedAuth` runs
them as 4→5→2 (`tds_connection.cpp:618`, `:633`) and `DoLogin7` omits 2–3
entirely.

A subtlety for the integrated path: the SPNEGO continuation loop reads a
response per round, and a ROUTING answer can arrive at any round. The check
therefore lives inside the round loop, ahead of both the `success` branch
(`:761`) and the `has_sspi_token` rejection branch (`:774`) — otherwise a
routing response with no SSPI token would be misreported as "auth rejected by
server (no SSPI token, no LOGINACK)".

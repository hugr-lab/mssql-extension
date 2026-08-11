# Spec 068 — Login routing for every auth path, not just FEDAUTH

**Status**: DRAFT
**Branch**: `spec/068-login-routing`
**Follows**: PR #254 (the TDS 7.2+ DONE-token size fix, merged e2d2822). That PR
fixed the parse desync that #88/#164 actually hit; this spec closes the gap it
exposed on the way: TDS login-time routing (ENVCHANGE type 20) is honoured on
exactly one of our three authentication paths.

## 1. Reconnaissance — what exists and where it stops

`ParseLoginResponse` parses the ROUTING ENVCHANGE completely and correctly
(`tds_protocol.cpp` ~832): `has_routing`, `routed_server`, `routed_port` land on
`LoginResponse`, with a level-1 debug line. The parser is not the problem.

**The one working consumer** is `AuthenticateWithFedAuth`
(`tds_connection.cpp:243–391`). It owns a hop loop with everything a hop needs,
built and battle-tested for the Azure SQL / Fabric gateways:

- `MAX_ROUTING_HOPS = 5`, full PRELOGIN + TLS + LOGIN7 per hop;
- routed-target normalization: Fabric sends
  `hostname.pbidedicated.windows.net\INSTANCE:port`, so it extracts a trailing
  `:port` (digits only), keeps `hostname\instance` (no port) as
  `tds_server_name_` for the LOGIN7 ServerName field, and strips `\instance`
  for DNS/TCP — the SQL Browser is deliberately not consulted on a hop;
- per-hop state resets: `next_packet_id_ = 1`, `tls_enabled_ = false`,
  `fedauth_echo_ = false`, routing fields cleared;
- TLS SNI follows the routed hostname, not the original gateway (go-mssqldb
  behaviour).

**Gap 1 — ordering inside the working path.** `DoLogin7WithFedAuth` returns on
`!login_response.success` (~line 618) *before* it copies
`login_response.has_routing` into the connection (~line 633). A gateway that
answers the token with ROUTING + DONE and **no LOGINACK** — legal per MS-TDS,
the session on this socket is not usable either way — parses as
`success=false, has_routing=true` and dies as "authentication failed" without
the loop ever seeing the redirect. Route-*with*-LOGINACK (what the Azure
gateways we've met actually send) is the only shape that works today.

**Gap 2 — SQL auth.** `Authenticate` (149) → `DoPrelogin` + `DoLogin7` is a
straight line: `DoLogin7` (835) reads only `success`, never `has_routing`. A
routing answer — Azure SQL with the **Redirect** connection policy (the default
for traffic originating inside Azure), Azure SQL Managed Instance, on-prem
AlwaysOn read-only routing for read-intent sessions — is reported as a failed
login. With LOGINACK present the failure is silent and worse: we'd keep talking
to a gateway that told us to leave.

**Gap 3 — integrated auth.** `AuthenticateIntegrated` (662) has the same
blindness, plus a problem the other two paths don't have: its credential is
**bound to the target's SPN**. The authenticator arrives pre-built for
`MSSQLSvc/<original-host>:<port>` (the pool factory in `mssql_catalog.cpp:169`
constructs it per connection, from `MSSQLConnectionInfo`); a service ticket for
the gateway's SPN will not validate on the routed host. A hop therefore needs a
**fresh authenticator for the new host:port**, not a retry with the old blob.

**Callers.** All three entry points are called from exactly two places each:
the pool connection factory (`mssql_catalog.cpp` 156/194/222) and the
ATTACH-time eager validation (`mssql_storage.cpp` 1111/1179/1338, plus the
deprecated `mssql_diagnostic.cpp:144`). Routing support added inside
`TdsConnection` reaches all of them; only the integrated path needs its caller
signature touched (D3).

**Why now.** Nobody with a routing trace is currently stuck — DantasB's Synapse
case was the DONE desync, not routing. But Viroxken's Azure SQL case (#164) is
still unconfirmed: if his retest on #254 artifacts still fails and the new
`DoLogin7` byte dump shows ENVCHANGE 20, this spec is his fix. And any Azure MI
user connecting from inside Azure hits Gap 2 on their first ATTACH.

## 2. Decisions

### D1 — the routing contract: `has_routing` outranks `success`

One rule, applied uniformly: **if the login response carries ROUTING, the
attempt's outcome on this socket is "hop", regardless of whether a LOGINACK was
also present.** MS-TDS is explicit that a routed session is not usable, so
treating route-with-LOGINACK as success (keep talking to the gateway) and
route-without-LOGINACK as failure (give up) are both wrong — and we currently
do one of each depending on the path.

Concretely: `DoLogin7`, `DoLogin7WithFedAuth`, and the integrated response
handling copy `has_routing` / `routed_server` / `routed_port` into the
connection **before** any early return on `!success`. The failure return
happens only when there is neither success nor routing.

### D2 — one hop driver, three thin wrappers

Extract the loop body of `AuthenticateWithFedAuth` into a private driver:

```cpp
// Runs attempt() against the current target, follows ROUTING up to
// MAX_ROUTING_HOPS times (reconnect + full handshake per hop), returns the
// final attempt's verdict. attempt() sees host_/port_/tds_server_name_
// already retargeted.
bool TdsConnection::RunWithRoutingHops(const std::function<bool()> &attempt);
```

The routed-target normalization (port suffix, instance strip,
`tds_server_name_`) moves to a named helper so the hop driver stays readable.
`Authenticate`, `AuthenticateWithFedAuth`, and `AuthenticateIntegrated` become
wrappers passing their per-attempt lambda (PRELOGIN flavour + LOGIN7 flavour).
Per-hop state resets stay in the driver — they are the part that's easy to
forget and impossible to test path-by-path.

Not in scope: re-resolving a routed `host\instance` **without** a port via the
SQL Browser (`mssql_named_instance_resolution` runs at ATTACH only). The
fedauth loop never needed it — every observed gateway supplies the port — and
inventing UDP traffic mid-login on a hunch is how timeouts happen. If a real
target ever omits the port, the error message names the routed string verbatim.

### D3 — integrated auth takes an authenticator factory

`AuthenticateIntegrated`'s `shared_ptr<IAuthenticator>` parameter becomes

```cpp
std::function<std::shared_ptr<tds::IAuthenticator>(const std::string &host, uint16_t port)>
```

The pool factory and the validation path already hold `MSSQLConnectionInfo` by
value at the call site; they build the strategy inside the callable with
`info.host/info.port` overridden to the driver's current target. First attempt
therefore behaves exactly as today (fresh authenticator per connection, spec
042 semantics preserved); a hop gets a ticket for the routed host's SPN.

An explicit `service_principal_name=` override is passed through **unchanged**
on every hop — the user pinned it, we don't second-guess it. Documented in
`Kerberos.md` alongside the existing SPN-derivation rules.

### D4 — testing without a routing server

No environment we control emits ROUTING (the docker SQL Server doesn't route;
the Kerberos KDC stack doesn't; the Azure test target's policy is whatever the
subscription says). Three layers instead:

1. **Parser pins** (extend `test_login_error_state.cpp`): route-with-LOGINACK
   (`has_routing` + `success` both true), route-without-LOGINACK (`has_routing`
   true, `success` false — the Gap-1 shape), routed-target field extraction,
   and ROUTING sitting *behind* a DONEINPROC run (composition with the #254
   fix — the shape a routing Synapse would send).
2. **Hop-driver pins**: a loopback fake TDS server (plain C++ test, two
   listener sockets, `use_encrypt=false` so no TLS keys needed) that answers
   PRELOGIN and replies to LOGIN7 with ROUTING → second listener completes the
   login. Pins: single hop end-to-end for SQL auth, hop-limit abort at
   `MAX_ROUTING_HOPS`, route-without-LOGINACK following the hop, per-hop packet
   id reset. First fake-server test in the repo; if it turns out flaky on CI it
   gets a `require-env` gate rather than deletion.
3. **Live smoke**: dispatch the Azure Test job on the branch (it runs against
   real Azure SQL; whether it routes depends on the gateway, but the fedauth
   path regression-checks either way), plus the Kerberos e2e stack for the
   integrated-path refactor.

## 3. Acceptance criteria

- ROUTING is followed on all three auth paths; route-without-LOGINACK follows
  the hop instead of failing (D1), on every path.
- `AuthenticateWithFedAuth` behaviour against today's Azure SQL / Fabric
  gateways is unchanged (same hop count, same SNI/ServerName handling) — the
  refactor is observable only in code structure.
- Integrated-auth hops present a ticket for the routed host's SPN; the
  explicit-SPN override survives hops verbatim.
- Hop limit stays 5 and aborts with an error naming the count and the last
  routed target.
- All parser + hop-driver pins green; Kerberos e2e green; full CI green.
- `docs/` (architecture/tds-protocol) and `Kerberos.md` updated: routing is a
  connection-level behaviour, table of which paths follow it (all, after this
  spec).

## 4. Risks

- **The SPN-over-hop guess.** No AD environment with a routing front-end exists
  in our test surface; the "fresh authenticator per hop" design mirrors
  go-mssqldb but ships unverified against real AD + MI. Mitigation: the error
  path names the SPN it tried, so a field report is diagnosable in one round.
- **Fake-server test flakiness** on CI runners (port allocation, accept
  timing). Mitigation: bind port 0, pass the bound port through; gate behind
  `require-env` only if it actually flakes.
- **Behavioural change on route-with-LOGINACK for SQL auth**: today it
  "succeeds" against the gateway; after D1 it hops. That is the correct
  behaviour per spec, but any user who somehow depended on talking to the
  gateway (none known, and the gateway kills routed sessions) would see a
  change. Called out in the changelog.

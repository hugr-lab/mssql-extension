# Contract — `TdsConnection` login/routing interface

The extension's external surface (SQL functions, settings, ATTACH options) does
**not** change in spec 068. The contract that changes is the internal C++ one
between `TdsConnection` and its callers, and it is a breaking one for a single
method. It is specified here at the same level of detail an external API would
get, because the two affected callers are in a different layer
(`src/catalog/`, `src/`) than the definition (`src/tds/`).

---

## C1 — Unchanged signatures, changed behaviour

```cpp
bool Authenticate(const std::string &username, const std::string &password,
                  const std::string &database, bool use_encrypt = false,
                  const std::string &app_name = "");

bool AuthenticateWithFedAuth(const std::string &database,
                             const std::vector<uint8_t> &fedauth_token,
                             bool use_encrypt = true,
                             const std::string &app_name = "");
```

| Aspect | Before | After |
|---|---|---|
| `Authenticate` on a ROUTING response with LOGINACK | returns `true`, session bound to the gateway | follows the hop; returns `true` bound to the routed server |
| `Authenticate` on ROUTING without LOGINACK | returns `false`, `"Authentication failed …"` | follows the hop |
| `AuthenticateWithFedAuth` on ROUTING with LOGINACK | follows the hop | unchanged, byte-identical |
| `AuthenticateWithFedAuth` on ROUTING without LOGINACK | returns `false`, `"Azure AD authentication failed"` | follows the hop |
| Either, no routing | unchanged | unchanged — same packets, same ServerName, same hop count (0) |
| Post-condition on `true` | `GetState() == Idle`, `GetHost()`/`GetPort()` = the connect target | same, except the target may now be a routed one |

**Caller impact**: none. `mssql_catalog.cpp:222`, `mssql_storage.cpp:1111`/`1179`
and `mssql_diagnostic.cpp:144` compile and behave as before on non-routing
servers; the diagnostic path gains routing support for free.

---

## C2 — Breaking signature change

```cpp
// BEFORE
bool AuthenticateIntegrated(const std::string &database,
                            std::shared_ptr<tds::IAuthenticator> authenticator,
                            bool use_encrypt = true,
                            const std::string &app_name = "",
                            size_t login7_max_packet = 0);

// AFTER
bool AuthenticateIntegrated(const std::string &database,
                            AuthenticatorFactory authenticator_factory,
                            bool use_encrypt = true,
                            const std::string &app_name = "",
                            size_t login7_max_packet = 0);
```

**Deliberately not overloaded.** A `shared_ptr` overload kept for compatibility
would silently reuse the gateway's service ticket on a hop — the exact bug D3
exists to prevent. Two call sites is a cheap price for making the wrong thing
unwritable.

**Caller obligations**:

1. Build the authenticator **inside** the callable, from a
   `MSSQLConnectionInfo` copy whose `host`/`port` are overwritten with the
   callable's arguments.
2. Return `nullptr` rather than throwing; exceptions thrown by
   `AuthStrategyFactory::Create` must be caught in the callable and reported the
   way each site already reports them (stderr line in the pool factory,
   `InvalidInputException` in the ATTACH validator — note the validator must
   surface the message *after* `AuthenticateIntegrated` returns, since the
   callable itself cannot throw across the TDS layer).
3. Capture by value only. No `ClientContext`, no reference to the catalog
   (issue #178/#179: the pool factory runs on worker threads).

Reference shape for `src/catalog/mssql_catalog.cpp:194`:

```cpp
factory = [info_copy, app_name]() -> std::shared_ptr<tds::TdsConnection> {
    auto conn = std::make_shared<tds::TdsConnection>();
    /* ... SetRequestedPacketSize / SetRequestUtf8Support / Connect as today ... */
    auto auth_factory = [info_copy](const std::string &host, uint16_t port)
        -> std::shared_ptr<tds::IAuthenticator> {
        MSSQLConnectionInfo hop_info = info_copy;   // per-hop copy
        hop_info.host = host;                       // SPN follows the routed host
        hop_info.port = port;                       // explicit service_principal_name
        try {                                       //   is honoured verbatim by DeriveSpn
            auto strategy = tds::AuthStrategyFactory::Create(hop_info);
            return strategy ? strategy->GetAuthenticator() : nullptr;
        } catch (const std::exception &e) {
            fprintf(stderr, "[MSSQL POOL] integrated-auth: %s\n", e.what());
            return nullptr;
        }
    };
    if (!conn->AuthenticateIntegrated(info_copy.database, auth_factory,
                                      info_copy.use_encrypt, app_name,
                                      info_copy.login7_max_packet)) { /* ... */ }
    return conn;
};
```

---

## C3 — Private driver contract

```cpp
enum class LoginAttemptOutcome : uint8_t { Success, Route, Failure };

// Runs `attempt` against the current target and follows ROUTING up to
// MAX_ROUTING_HOPS (5) times, with a full reconnect + handshake per hop.
// Pre:  state_ == Authenticating, socket_ connected, host_/port_ set.
// Post: true  -> state_ == Idle, socket_ connected to the final target;
//       false -> state_ == Disconnected, socket_ closed, last_error_ set.
bool RunWithRoutingHops(const std::function<LoginAttemptOutcome()> &attempt);
```

Guarantees to the callback:

- `host_`, `port_`, `tds_server_name_` are already retargeted for this attempt.
- Per-hop state is already reset (see data-model.md §4).
- The socket is connected and un-TLS'd; the callback performs its own PRELOGIN.

Obligations on the callback:

- Set `last_error_` when returning `Failure`.
- Never touch `state_` and never call `socket_->Close()`.
- Return `Route` whenever `has_routing_` was set, without consulting `success`.

Driver-side error strings (stable, since field reports quote them):

| Condition | Message |
|---|---|
| hop limit exceeded | `"Too many routing hops (N) - aborting; last routed target was <server>:<port>"` |
| reconnect failed | `"Failed to connect to routed server <host>:<port>: <socket error>"` |

The first extends today's `"Too many routing hops (N) - aborting"`
(`tds_connection.cpp:294`) with the target, per the spec's acceptance criteria.

---

## C4 — What is explicitly **not** in the contract

- No SQL Browser (UDP 1434) resolution on a hop, regardless of
  `mssql_named_instance_resolution`. A routed `host\instance` with no port
  fails with the routed string quoted.
- No new setting, no new SQL function, no change to `mssql_pool_stats` output.
- No change to `ParseLoginResponse` or to `LoginResponse`'s fields —
  `src/tds/tds_protocol.cpp` is untouched by this feature.
- Hop limit stays 5 and stays a `constexpr`, not a setting.

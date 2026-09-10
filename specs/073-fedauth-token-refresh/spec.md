# Spec 073 — The Azure AD token is captured once at ATTACH and never refreshed

Issue [#302](https://github.com/hugr-lab/mssql-extension/issues/302): an attached
Azure SQL catalog cannot open a new connection after the script has run for a
while — `MSSQL: Failed to acquire connection from pool (timeout)` — while the
connection it already has keeps working, and `DETACH` / `ATTACH` fixes it.

The issue carries a source analysis. The reconnaissance below checked each of
its claims against the code rather than taking them as read: four hold, one
does not explain what it was offered for, and one thing it did not say turns
out to be the design constraint the fix has to be built around.

Reconnaissance, including a live reproduction on the reporter's line. Implementation is a separate PR.

## 0. How everything below was measured

- Code: `main` at `5fadf8a` and later, and `origin/duckdb-v1.5.5` — the branch
  v0.2.4 (the reporter's version) and v0.2.5 were released from. Every line
  number below was read on both unless stated.
- **Token lifetime**, straight from Azure AD for this project's test service
  principal (`client_credentials`, scope `https://database.windows.net/.default`):
  `expires_in = 3599` — **60 minutes** — with a 65-minute `nbf`→`exp` window.
- **The diagnostic half, reproduced locally with no Azure at all**: a catalog
  whose factory cannot succeed (`Password=WRONGPASS`, `lazy_validation true`),
  `mssql_acquire_timeout = 5`, one scan.
- **The token half, live**: stock DuckDB v1.5.5 with community `mssql` 0.2.5
  (the reporter's line), a service-principal secret, `mssql_idle_timeout = 60`
  so the first connection is reaped, `mssql_acquire_timeout = 600` (the
  reporter's value), a 75-minute sleep, then a statement that needs a fresh
  connection. Result:

  ```text
  T0        pool 1 total / 1 idle / created 1 / closed 0; two FEDAUTH logins OK
  T+75min   pool 0 total / 0 idle / created 1 / closed 1   (reaped at 60 s)
            SELECT mssql_exec('az','SELECT 1')
            IO Error: MSSQL: Failed to acquire connection from pool (timeout)
            Run Time (s): real 600.319
            pool 0 / 0 / created 1 / closed 1 / acquire_timeout_count 1
            DETACH; ATTACH; SELECT mssql_exec(...)  ->  1
  ```

  Inside the 600 s: one `Acquire`, **two** creation attempts, and the wire for
  each is in F7.

## 1. The findings

### F1 — The pool factory holds the token bytes from ATTACH, forever

`src/catalog/mssql_catalog.cpp:143-160` (both branches):

```cpp
auto token = fedauth_token_utf16le_;
factory = [host, port, database, encrypt, token, app_name, ...]() {
    auto conn = std::make_shared<tds::TdsConnection>();
    if (!conn->Connect(host, port)) {
        return nullptr;
    }
    if (!conn->AuthenticateWithFedAuth(database, token, encrypt, app_name)) {
        return nullptr;
    }
    return conn;
};
```

`token` is the UTF-16LE FEDAUTH payload `ATTACH` built from a token that was
valid at that moment. Every connection the pool creates afterwards presents the
same bytes. Nothing in the factory consults `TokenCache`, checks `exp`, or
mints a new one. The issue's first claim holds exactly.

Against a 60-minute token that is the whole story of the report: the two-table
statement 21 minutes after a refresh succeeded; the one 2 h 23 min after a
refresh failed; a fresh `ATTACH` immediately before the big `UPDATE` let it run
for 33 minutes and 2,583 acquisitions. The connection that already exists keeps
working because SQL Server validates the token at login only — a live session
is never re-authenticated — which is why the pool statistics look healthy
(`1 idle, 0 closed`) right up to the statement that needs a second one.

### F2 — A failed creation is treated as an exhausted pool, and the reason is thrown away

`src/tds/tds_connection_pool.cpp:161-211`, `Acquire`:

```cpp
if (stats_.total_connections < config_.connection_limit) {
    lock.unlock();
    conn = CreateNewConnection();       // = factory_()
    lock.lock();
    if (conn) { ... return conn; }
}
...
available_cv_.wait_for(lock, std::chrono::milliseconds(remaining));
```

When the factory returns `nullptr`, control falls through to the same
`wait_for` a *full* pool uses — waiting for a `Release()` — and `available_cv_`
is notified by `Release` and shutdown only (`:254`, `:263`, `:276`, `:87`). If
nothing is going to be released, the wait runs to `acquire_timeout` and returns
`nullptr`, and `ConnectionProvider` (`mssql_connection_provider.cpp:129`) turns
that into the one message the reporter saw.

The server's reason exists and is dropped. `DoLogin7WithFedAuth` records it —
`tds_connection.cpp:717` `"Azure AD authentication failed (error N) ..."`,
`:720` with the message — into `last_error_`, and the factory returns `nullptr`
without reading it. Measured locally with a wrong password:

```text
IO Error: MSSQL: Failed to acquire connection from pool (timeout)
Run Time (s): real 5.202
acquire_count=2  acquire_timeout_count=2  connections_created=0
```

Five seconds, six `DoLogin7` attempts, and not one word of *"Login failed for
user 'sa'"*. An expired token reads identically to a full pool, a wrong
password, and an unreachable server.

### F3 — `mssql_connection_timeout` never reaches a pool refill

All three factories call `conn->Connect(host, port)` with the timeout argument
omitted (`mssql_catalog.cpp:153`, `:173`, `:233`), so
`DEFAULT_CONNECTION_TIMEOUT` = 30 s applies. `TdsConnection::SetConnectTimeout`
(`tds_connection.cpp:147`) exists and the routing-hop code uses it — *"the
caller's timeout, not the compiled-in default"* — but no factory calls it.

Then `DoLogin7WithFedAuth` reads both login responses with the constant
spelled out — `:597` and `:674`, `ReceiveMessage(response,
DEFAULT_CONNECTION_TIMEOUT * 1000)` — so a login that the server holds or drops
costs 30 s regardless of any setting. The reporter's `mssql_connection_timeout
= 600` governed ATTACH-time validation and nothing after it.

### F4 — The refresh infrastructure exists, has no callers, and cannot be called from where it is needed

The issue names `TokenCache::GetToken`, `FedAuthStrategy::IsTokenExpired` and
`AuthStrategyFactory::BuildTokenAcquirer`. All three exist. `BuildTokenAcquirer`
has **no callers**, and this is what it does (`auth_strategy_factory.cpp:361`):

```cpp
// Capture context by reference - caller must ensure context lifetime
return [&context](const std::string &secret_name, ...) -> std::string {
```

A pool factory runs on whatever thread asks the pool, for as long as the
catalog lives, long after the `ClientContext` that ran `ATTACH` is gone — the
same constraint issue #178 recorded for `ReleaseBcpConnectionOnError`. A
`ClientContext&` captured by reference is exactly the thing it cannot hold. So
"wire up the existing acquirer" is not a fix; it is a use-after-free.

What `AcquireToken(ClientContext &, ...)` actually needs from the context is
two things (`azure_token.cpp:416`, `:432` → `azure_secret_reader.cpp:24-28`):
`*context.db`, and `SecretManager::Get(context)` +
`CatalogTransaction::GetSystemCatalogTransaction(context)` to read the secret.
Both have `DatabaseInstance &` forms on the 2.0 pin
(`secret_manager.hpp:107`, `catalog_transaction.hpp:39`), and the device-code
path does not touch the context at all. A `DatabaseInstance`-bound acquirer is
therefore implementable, and `DatabaseInstance` is what the catalog already
outlives nothing of.

### F5 — The 31 seconds: 30 of them are F3's login read, and the code waits `acquire_timeout`

The issue attributes them to the pool factory's 30 s connect default. Close,
but the wrong 30 seconds, and it is not the whole number. Measured (§ 0):
with `acquire_timeout = 600` the statement fails after **600.3 s**, exactly
the code's prediction — after a failed creation `Acquire` waits for a release
until the deadline (F2). `ConnectionProvider::GetConnection` defaults
`timeout_ms` to -1 and every catalog-internal `Acquire()` uses the same
default, on `main` and `duckdb-v1.5.5` alike.

Inside those 600 s the pool made **two** creation attempts, and each one is
~30 s long for the reason in F7: the server never answers, and the login read
is the hard-coded 30 s of F3. So the reporter's 31 s is one such attempt
followed by a deadline that had already passed — which is what
`acquire_timeout = 30` produces, and `acquire_timeout = 600` cannot. Their
`SET mssql_acquire_timeout = 600` did not reach the session that attached;
that is a host-side matter (DuckDB.NET, judging by the C# mention) and not
this extension's. The fix does not depend on it: W2 makes a failed creation
fail fast and say why, and W3 makes the 30 s a setting.

### F7 — Azure SQL does not say "expired"; it hangs up

The wire, from the § 0 run, for the two creation attempts inside the wait:

```text
attempt 1   PRELOGIN ok → TLS → LOGIN7 sent → (no FEDAUTHINFO) → connection closed
attempt 2   PRELOGIN ok → TLS → LOGIN7 → FEDAUTHINFO received → FEDAUTH_TOKEN sent
            → (no LOGINACK, no ERROR token) → connection closed
```

Not once did an ERROR token arrive. The gateway drops the connection on an
expired token, so what `DoLogin7WithFedAuth` records is
`"Failed to receive LOGINACK after FEDAUTH_TOKEN: <socket error>"` (`:675`),
never an AADSTS message — and it records it only after the 30 s read of F3
has run out. Two consequences for the design:

- **the expiry has to be diagnosed on the client**, from the token's `exp`
  (the JWT parser and `IsTokenExpired` already exist for exactly this), before
  a socket is opened — the server will not supply the words;
- **surfacing `last_error_` (W2) is necessary but not sufficient** for this
  case: it would say "failed to receive", which is true and unhelpful. W1's
  own message is the one that names the token.

### F6 — The template already exists in the next `case`

`mssql_catalog.cpp:167-200`, the integrated-auth factory, builds a fresh
authenticator **per connection** so a `kinit`-refreshed ticket is picked up,
and on failure prints `conn->GetLastError()` before returning `nullptr`. It is
the shape the Azure factory should have had: credentials resolved at creation
time, and the reason preserved when creation fails.

## 2. The work

### W1 — Resolve the token at creation time, not at ATTACH

The factory captures `DatabaseInstance &`, the secret name and the tenant
override — never the token, never the client secret — and on each creation:

1. asks `TokenCache` (keyed by `DatabaseInstance`, spec 047 FR-012) for the
   secret's token; the cache already refuses one within `IsTokenExpired`'s
   300 s margin — and this check runs **before any socket is opened**, because
   the server will not diagnose expiry for us (F7);
2. on a miss, re-reads the secret from `SecretManager::Get(db)` under
   `GetSystemTransaction(db)` and mints a new token — an
   `AcquireToken(DatabaseInstance &, ...)` overload alongside the existing one,
   sharing the body;
3. builds the FEDAUTH bytes from the string — `BuildFedAuthExtension` gains a
   form that takes the token rather than the context.

Providers that cannot mint silently — `access_token` (a fixed string), and the
interactive ones after their first use — fail with a message that says what
happened and what to do: *"Azure AD token for secret 'x' expired at <time>;
DETACH and ATTACH to authenticate again"*. That is the same outcome as today,
minus the thirty-one seconds and the word "timeout".

### W2 — A failed creation is a failure, not a full pool

The pool keeps the last creation error (a string, under `pool_mutex_`), and:

- when creation fails and **no connection is active** — nothing can be
  released, so waiting is pointless — `Acquire` returns at once;
- when others are active, it keeps waiting as now, since a release may still
  serve the request, but re-tries creation with a backoff rather than on every
  wake;
- on any `nullptr` return the provider's message carries the reason:
  `Failed to acquire connection from pool 'x' after 5 s: Azure AD
  authentication failed (error 18456): Login failed for user '<token-identified
  principal>'`.

### W3 — Plumb `mssql_connection_timeout` where it was meant to go

Every factory calls `conn->SetConnectTimeout(pool_config.connection_timeout)`
before `Connect`, and `DoLogin7WithFedAuth` reads its responses with
`connect_timeout_seconds_ * 1000`, as the routing-hop path already does.

### W4 — Tests

- **Pool, server-free, gates a PR**: a factory that fails; with nothing active,
  `Acquire` returns promptly and the error is readable; with one connection
  active, it waits and returns the error on timeout.
- **Azure lane** (`make azure-test`, manual dispatch): `PROVIDER access_token`
  with a real but already-expired token exercises W1's cannot-refresh message
  deterministically, since the client rejects it before the server would.
- **The 60-minute path** cannot be shortened — Azure AD does not issue
  short-lived tokens on request — so it stays a documented manual run: the
  script from § 0, recorded in the azure lane's README with its expected
  output before and after.

### W5 — Documentation

`AZURE.md` and `website/docs/connection/azure.md`: the token lives 60 minutes;
which providers refresh silently and which cannot; the new message and what
to do about it; and that `mssql_connection_timeout` now applies to pool
refills, which it did not.

## 3. What this spec does not propose

- **Keeping the ATTACH-time token as a fallback.** It is expired; it is good
  for nothing.
- **Re-authenticating idle connections.** The server does not re-validate a
  live session, and neither should the pool. What ages is the ability to open
  a *new* one.
- **Fixing `BuildTokenAcquirer` in place.** Its contract — a `ClientContext`
  captured by reference — is the problem, not a detail. It is replaced by the
  `DatabaseInstance`-bound acquirer and removed, along with the strategy-side
  `IsTokenExpired` overrides if the `mssql_open` path turns out to be their only
  consumer.

## 4. Risks

- **Secret access from a worker thread.** `SecretManager::Get(db)` with a
  system transaction is what DuckDB itself uses off-context; the factory runs
  with `pool_mutex_` released, so no lock is held across it. Worth an explicit
  check for a `SecretManager` lock the pool's cleanup thread could contend on.
- **W2 changes `Acquire`'s timing** when creation fails with nothing active —
  from `acquire_timeout` to immediate. Intended; any caller that relied on the
  wait was relying on a timeout to tell it about a wrong password.
- **The reporter is on the 1.5.5 line.** The fix lands on `main` and is
  backported to `duckdb-v1.5.5` for a 0.2.6, the same route v0.2.5 took.

## 5. Acceptance

- A service-principal catalog attached for longer than the token's lifetime
  opens a new connection without `DETACH` / `ATTACH` — the § 0 script, before
  and after.
- A catalog whose factory cannot succeed fails with the server's reason in the
  message and, with nothing active, without waiting for `acquire_timeout`.
- `mssql_connection_timeout` bounds a pool refill's connect and login reads.
- Pool unit test in `STANDALONE_TEST_SOURCES`; azure-lane test for the
  cannot-refresh message.

# Spec 073 — The Azure AD token is captured once at ATTACH and never refreshed

Issue [#302](https://github.com/hugr-lab/mssql-extension/issues/302): an attached
Azure SQL catalog cannot open a new connection after the script has run for a
while — `MSSQL: Failed to acquire connection from pool (timeout)` — while the
connection it already has keeps working, and `DETACH` / `ATTACH` fixes it.

The issue carries a source analysis. The reconnaissance below checked each of
its claims against the code rather than taking them as read: four hold (F1,
F2, F3, F6), one does not explain what it was offered for (F5), and two things
it did not say turn out to be what the fix has to be built around — the
refresh path it points at is unreachable (F4), and the server never says
"expired" (F7).

Reconnaissance, a live reproduction on the reporter's line, and the
implementation, in one PR. Where § 2 says "now", it describes the code as
merged; the first draft of this document proposed mechanisms the code did not
have, and review caught each one before it was built.

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

### F4 — The refresh infrastructure exists, is unreachable, and could not be called from where it is needed

The issue names `TokenCache::GetToken`, `FedAuthStrategy::IsTokenExpired` and
`AuthStrategyFactory::BuildTokenAcquirer`. All three exist. `BuildTokenAcquirer`
has exactly one caller — `CreateFedAuth` (`auth_strategy_factory.cpp:349`) —
and **`CreateFedAuth` is itself unreachable**: the Azure branch of
`AuthStrategyFactory::Create` (`:321-327`) runs only when given a
`ClientContext`, and both live `Create` call sites (`mssql_catalog.cpp:193`,
`mssql_storage.cpp:1412`) are the spec-068 integrated-auth hop factories,
which pass none. The first draft of this document said "no callers"; the truth
is one caller that nothing reaches, which is the same finding with a longer
removal list (§ 3). This is what `BuildTokenAcquirer` does
(`auth_strategy_factory.cpp:361`):

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

### F6 — The template already exists in the next `case`

`mssql_catalog.cpp:167-200`, the integrated-auth factory, builds a fresh
authenticator **per connection** so a `kinit`-refreshed ticket is picked up,
and on failure prints `conn->GetLastError()` before returning `nullptr`. It is
the shape the Azure factory should have had: credentials resolved at creation
time, and the reason preserved when creation fails.

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

## 2. The work

### W1 — Resolve the token at creation time, not at ATTACH

Two auth methods shared the old factory `case`, and they get different
treatment, because only one of them has anything to refresh from.

**`AZURE_AD` (a secret)**: the factory captures `DatabaseInstance *`, the
secret name and the tenant override — never the token, never the client
secret — and on each creation calls `AcquireToken(DatabaseInstance &,
secret, tenant, allow_interactive = false)`, a second entry point sharing the
body of the context one. It re-reads the secret through `SecretManager::Get(db)`
under `GetSystemTransaction(db)`, and builds the FEDAUTH bytes with
`BuildFedAuthData(token)`. `service_principal`, `cli` and `env` mint silently;
an interactive chain refuses by name (nobody is at a terminal inside a query on
a worker thread).

**`MANUAL_TOKEN` (`access_token=` given to ATTACH)**: the captured bytes *are*
the credential — there is no secret to re-resolve, and `~MSSQLCatalog`
cleanses them for that reason — so the factory keeps them, and gains the check
below. The first draft of this document dropped this method; review caught it.

**Expiry is diagnosed from the token's own `exp`, on the client, before any
socket is opened.** The server will not do it (F7), and `TokenCache` cannot:
its `expires_at` was real for `service_principal` only and a fabricated
`now + 3600` for `access_token`, `cli` and `env` — so a cache hit proved the
entry was *young*, not that the JWT was *unexpired*, and a token minted 55
minutes before it reached `CREATE SECRET` walked straight into the F7 hangup.
Now `AcquireToken` parses the JWT (`ParseJwtClaims`) and records **its** `exp`
for every provider; a fixed token past its `exp` fails at once; `MANUAL_TOKEN`
parses the raw token once at factory build and tests it per creation. The
messages:

```text
Azure AD access token in secret 'x' expired at 2026-09-10 08:49:00 UTC;
a fixed token cannot be refreshed -- DETACH and ATTACH with a new one

Azure AD token for secret 'x' has expired and its credential chain needs
interactive authentication, which cannot run from a pooled connection;
DETACH and ATTACH to authenticate again
```

That is the same outcome as before for those two cases, minus the wait and
the word "timeout".

### W2 — A failed creation is a failure, not a full pool

**The channel.** `ConnectionFactory` is `std::function<shared_ptr<TdsConnection>()>`
and the `TdsConnection` holding `last_error_` dies inside the closure — the
first draft promised to "surface the reason" through a signature with nowhere
to put it. The contract now: the factory **throws** `ConnectionException` with
the reason (all three factories do, in place of `return nullptr`), and
`CreateNewConnection` catches and hands the reason back to `Acquire`, which
records it under `pool_mutex_` and returns nullptr — the factory signature
is unchanged, so `test_connection_pool.cpp` and `test_tls_connection.cpp`
keep their factories. A later success **clears** the recorded error: a pool
that has recovered has nothing to report (review 1538 — the first draft kept
it forever, and every later exhaustion timeout was rendered as that stale
creation failure).
`ErrorData(e).RawMessage()`, not `what()`: on the 2.0 line a DuckDB exception's
`what()` is its JSON serialization, and the first cut pasted
`{"exception_type":"Connection",...}` into the user's error.

**The wait.** In `Acquire`, after a failed creation:

- **no connection active** — nothing can be released, so waiting only runs out
  the clock — return at once, **every time**: no backoff applies here, each
  Acquire dials once and reports its own result (the first draft applied the
  backoff on an empty pool too, and back-to-back failing statements stalled
  250 ms .. 4 s each before the same error — review 1538);
- **others active** — a release may still serve the request, so keep waiting
  for the caller's budget, but retry creation on a backoff (250 ms doubling to
  4 s) rather than on every wakeup; each attempt against a server that hangs
  up costs a full login read.

**The message.** `Acquire(timeout, &why)` says why **this call** failed —
"could not create a connection: <reason>" or "timed out (N active of M,
limit L)", the latter carrying this call's own creation failure when it made
one on the way. Per call, not pool state: a timeout on a healthy pool is never
rendered with a reason another thread hit earlier (the first draft's
pool-global renderer did exactly that — review 1538). Used by
`ConnectionProvider` and by every catalog-internal `Acquire()` caller that
throws (schema lookup, table scan, table loading, DDL, cache refresh, preload
— a metadata load is the likeliest *first* thing to need a fresh connection
after the token expires):

```text
MSSQL: Failed to acquire connection: pool 'az' could not create a connection:
Azure AD token for secret 'sp': Azure AD access token in secret 'sp' expired
at 2026-09-10 08:49:00 UTC; a fixed token cannot be refreshed -- ...
```

and `mssql_pool_stats()` gains `creation_failures` and `last_create_error` —
the two numbers missing while the reporter watched "1 idle, 0 closed" for two
hours.

### W3 — Plumb `mssql_connection_timeout` where it was meant to go

Every factory passes `pool_config_.connection_timeout` to `Connect(host, port,
timeout)` (there is no setter; `Connect` stores it), and **all eight**
login-phase reads that spelled out `DEFAULT_CONNECTION_TIMEOUT` now use
`connect_timeout_seconds_` — not only the two in `DoLogin7WithFedAuth`
(`:597`, `:674`) that the first draft named, but the PRELOGIN response and TLS
enable on the FEDAUTH path (`:500`, `:531`), their SQL-auth twins (`:214`,
`:245`), and the integrated-auth reads (`:865`, `:1002`). Without the other
six, a gateway that accepts the dial and goes quiet still cost up to 60 s of
un-configurable wait per attempt, and F3's acceptance criterion would not
have held. Anything that never calls `Connect` with a timeout keeps the
default.

**`0` means the default, not "no timeout".** The setting is registered with
`ValidateNonNegative`, so `0` was always legal and, while the factories
ignored it, harmless. Now that it reaches every login read a literal `0` is
`poll(fd, 1, 0)` — an instant "Socket timeout" on every pooled connection
(review 1538). `TdsConnection::Connect` — the one boundary every path
crosses — maps `<= 0` to `DEFAULT_CONNECTION_TIMEOUT`. Not to an infinite
poll, unlike `mssql_metadata_timeout`'s `0`: a dial or a login read that never
completes must not hang a pool refill forever. `connection_timeout_zero.test`
pins it.

### W4 — Tests

- **Pool, server-free, gates a PR** — `test/cpp/test_pool_creation_failure.cpp`,
  a **new** file in `STANDALONE_TEST_SOURCES`. Not `test_connection_pool.cpp`:
  that file exists and is wired into nothing, but it is not revivable as a
  standalone test — its factory dials a real server with real credentials.
  The new one throws, returns nullptr, or hands back an unconnected
  `TdsConnection` to stand in for an active one. Link surface is the usual
  standalone lane's (`libmssql_extension.a` pulls `TdsConnection`, hence
  OpenSSL/simdutf from vcpkg), which `make test-cpp-run` already satisfies.
  Cases: throwing factory fails fast with the reason and is *not* counted as
  an acquire timeout; a silent nullptr still gets a reason; three failing
  Acquires back to back on an empty pool each return at once with their own
  attempt's reason; one active → waits the budget, 2–4 attempts in 800 ms on
  the backoff, reported as a timeout that carries this call's creation
  failure; a success clears the recorded error; and the review-1538 day — one
  failure, then a healthy pool at its limit — is reported as exhaustion, not
  as the morning's error. Does not compile against the pre-fix archive.
- **`test/sql/regression/issue_302.test`** — wrong password, `lazy_validation`,
  `acquire_timeout = 30`. On the pre-fix code: "(timeout)" and 92 s for three
  statements. Now: `could not create a connection: Login failed for user` at
  once, pool 0/0.
- **`test/sql/tds_connection/pool_stats_no_credentials.test`** (the SC-005
  redaction gate) greps `last_create_error` as well as `db`, and provokes a
  creation failure with a sentinel password so the new column is non-empty
  when grepped — its header had stated that `db` was the only VARCHAR, which
  W2 made untrue (review 1538, carried from 1536).
- **`test/sql/regression/connection_timeout_zero.test`** — W3's edge.
- **Azure lane** (`make azure-test`, manual dispatch):
  `test/sql/azure/fedauth_expired_access_token.test` — `PROVIDER access_token`
  with a well-formed token whose `exp` is 1000000000 (2001-09-09), refused at
  ATTACH with *"expired at 2001-09-09"*. Deterministic **only because** W1
  diagnoses from the JWT's `exp` — the first draft assumed the cache did, and
  the review pointed out it did not (it stored `now + 3600`).
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
- **Fixing `BuildTokenAcquirer` in place, or removing it here.** Its contract
  — a `ClientContext` captured by reference — is the problem, not a detail,
  and it is superseded by the `DatabaseInstance`-bound `AcquireToken`. It is
  *not* removed in this PR because it does not come alone: its only caller is
  `CreateFedAuth`, which is unreachable (F4), and unhooking that means the
  Azure branch of `AuthStrategyFactory::Create`, `FedAuthStrategy::SetTokenAcquirer`
  / `GetFedAuthToken`, and the `TokenAcquirer` typedef. That is a
  dead-code removal with its own diff, and it should not ride on a bug fix.

## 4. Risks

- **Secret access from a worker thread.** `SecretManager::Get(db)` with a
  system transaction is what DuckDB itself uses off-context; the factory runs
  with `pool_mutex_` released, so no pool lock is held across it. The
  contention that can exist is between **concurrent `Acquire` callers on
  request threads** each running the factory — the cleanup thread never calls
  it (it only reaps idle connections). Two such callers may both find the cache
  stale and both mint a token; the second `SetToken` wins and both connections
  are valid. Acceptable, and bounded by the pool limit.
- **W2 changes `Acquire`'s timing** when creation fails with nothing active —
  from `acquire_timeout` to immediate. Intended; any caller that relied on the
  wait was relying on a timeout to tell it about a wrong password.
- **The reporter is on the 1.5.5 line, and this is not backported.** Releases
  now come from `main` on the duckdb 2.0 line only; v0.2.5 was the last from
  `duckdb-v1.5.5`. They get this in the next 2.0-line release.

## 5. Acceptance

- A service-principal catalog attached for longer than the token's lifetime
  opens a new connection without `DETACH` / `ATTACH` — the § 0 script, before
  and after.
- A catalog whose factory cannot succeed fails with the server's reason in the
  message and, with nothing active, without waiting for `acquire_timeout`.
- `mssql_connection_timeout` bounds a pool refill's connect and login reads.
- Pool unit test in `STANDALONE_TEST_SOURCES`; azure-lane test for the
  cannot-refresh message.

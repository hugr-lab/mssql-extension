# Spec 081 — Connecting to a hostname that resolves to more than one address

**Status**: DRAFT. Not implemented. Triaged from a report, not yet measured on the
hardware that produced it.
**Closes**: [#122](https://github.com/hugr-lab/mssql-extension/issues/122)
(`ATTACH` can last 30s on Windows with multiple network adapters).
**Relates to**: [#324](https://github.com/hugr-lab/mssql-extension/issues/324)
(parallel pool warm-up), **shipped** in `b66d149`. It imposes no ordering on this
spec: `ConnectionPool::Prewarm` (`tds_connection_pool.cpp`) dials on one
`std::thread` per connection, so N connections cost about one dial rather than N,
and every prewarm login reaches `TdsSocket::Connect` through the pool factories,
so W1 applies to it with no change. What #324 does add is an fd note for AC-4: N
prewarm threads each staggering through the candidate list hold up to
N × candidates sockets at once. Spec 073 W3 (the connect timeout now reaches the
dial) is what makes the mitigation in §5 possible at all.

One defect, reported with a correct root-cause analysis attached: **the dial hands
the full connection timeout to every address `getaddrinfo` returns, one after the
other.** A hostname with 9 candidates and a dead first candidate costs 9 × the
timeout, not the timeout.

> **Revision note.** The triage that produced this spec first claimed a second
> defect — that `mssql_connection_timeout` never reached the pool's dial, leaving
> users no workaround. That was read off a stale local `main`. Spec 073 W3 fixed it
> before this spec was written. §5 keeps the retraction, because "there is a
> workaround" changes this issue's priority and the next reader should not have to
> rediscover that.

---

## 0. What was verified, and what was not

Everything in §1 is verified by reading `origin/main` at `b660f13` and is
reproducible with `grep`. References are anchored by **function name** rather than
line number wherever both would do: `#382` and `#386` have already moved
`mssql_catalog.cpp` and `mssql_storage.cpp` once since this spec was drafted
against `e6ab3a2`, and re-verifying against `b660f13` is what retired F3 (below)
and corrected §5. Nothing in §1 has been **measured**, because the failure
needs a Windows host with several NICs and a hostname whose first DNS answer
blackholes — the reporter has one, CI does not.

That gap is deliberate and it bounds what this spec may claim. It does not claim a
speed-up figure. It claims that a code path exists which spends `N × timeout` where
it should spend `timeout`, and that removing it cannot make anything slower. Before
§6 is signed off, one of the two confirmations there has to come from real
multi-NIC hardware.

Where a platform behaviour is asserted below, it is either cited to Microsoft's
documentation or to a measurement **already recorded in this repository**
(`test/cpp/test_login_routing_hops.cpp`), or **measured by this spec's own probe**
(`probe_dial_outcomes.cpp` in this directory, run by `./run_probe.sh`). No platform
claim here is asserted from memory or from reading a comment: the one this spec
inherited — which of macOS and Linux drops a SYN to a bound-but-unlistening port —
was ambiguous in the comment it came from, so §4 now carries the measurement on
both platforms instead of the citation.

---

## 1. The finding

### F1 — the dial gives every candidate the full budget, in sequence

`TdsSocket::Connect()`, `src/tds/tds_socket.cpp:178-243`:

```cpp
for (rp = result; rp != nullptr; rp = rp->ai_next) {   // :179
    fd_ = socket(...); SetNonBlocking(true); connect(...);
    ...
    if (WaitForReady(true, timeout_seconds * 1000)) {  // :219   <-- full budget, per address
```

There is no overall deadline. The budget is not a budget; it is a per-candidate
allowance, and the loop is free to spend it as many times as DNS gives it addresses.

This is invisible whenever the first address answers — which is every CI run, every
`127.0.0.1` test, and most developer machines. It becomes the whole user experience
when the first address does **not** answer and does not refuse either:

- a link-local IPv6 answer on a host with no IPv6 route,
- an address belonging to an adapter that is down (Docker Desktop's vEthernet, a
  disconnected VPN, WSL2's host-side interface),
- a firewall configured to DROP rather than REJECT.

A refusal (RST) is cheap and self-correcting; the loop moves on in microseconds. A
**silently dropped SYN** is the expensive case, and it is exactly the case a
multi-NIC Windows box manufactures. The reporter observes 4–9 candidates from one
hostname. At the default 30s that is a 120–270s ceiling, and the 30s they actually
saw is the cheapest shape of the bug (one dead candidate, then a live one).

`Microsoft.Data.SqlClient` has not behaved this way since .NET 8; it dials
candidates in parallel on a stagger. A user comparing the two reasonably concludes
the extension is broken, and they are not wrong.

### F2 — the candidate list is never pruned

`src/tds/tds_socket.cpp:164`:

```cpp
hints.ai_flags = 0;
```

`AF_UNSPEC` with no `AI_ADDRCONFIG` asks the resolver for every family regardless of
what the host can actually route. `AI_ADDRCONFIG` restricts answers to families for
which the host has a configured non-loopback address, and it is supported on Linux,
macOS and Windows. It does not fix F1 — a machine with real IPv6 and several IPv4
adapters still gets a long list — but it removes the most common source of dead
leading candidates for the price of one constant.

### F3 — RETRACTED: `ATTACH` crosses the dial once, not twice

This spec's first draft claimed `ATTACH` pays F1 twice — once for the spec 047
validation round trip, once for the first pooled connection — and that the issue's
30s therefore allowed 60s before a single query ran. **That is not what main
does.** `MSSQLCatalog::ValidateThroughPool` acquires the validation connection
*through the pool* and ends with `connection_pool_->Release(connection)`
(`src/catalog/mssql_catalog.cpp`), so the connection it logged in stays in the pool
and the first query reuses it. A plain `ATTACH` is one dial and one login.

The budget still changes hands across it — the validation login runs under
`mssql_attach_validation_timeout` (default `0` ⇒ inherit
`mssql_connection_timeout`) and every later one under `mssql_connection_timeout`,
which `ValidateThroughPool` implements by storing each into the shared
`connect_timeout_` atomic around the acquire — but that is one dial's budget, not
two.

With `mssql_min_connections > 0` there are two dial *phases*: the validation, then
one `Prewarm` batch whose logins run concurrently (see **Relates to** above). That
is still one dial's worth of wall clock per phase, not N.

Retained as a numbered finding rather than deleted, because the doubled figure was
quoted once and the next reader should not have to re-derive that it is wrong.

### F4 — `WaitForReady` cannot see more than one socket

`WaitForReady(bool, int)` (`src/include/tds/tds_socket.hpp:147`) polls exactly
`fd_`. `SetNonBlocking(bool)` (`tds_socket.cpp:850`) likewise operates on `fd_`.
Neither can express "wait on these four candidate sockets", which is why F1 is
written the way it is. This is a design note, not a defect, but it determines the
shape of W1: the fix is not a smaller timeout argument, it is a second dialling
routine that owns its own fds and installs only the winner into `fd_`.

---

## 2. The work

### W1 — staggered parallel connect (RFC 8305), replacing the loop

Dial candidates on a stagger rather than in sequence:

1. Start a non-blocking `connect()` to the first candidate.
2. Every `stagger_ms` (default **250 ms**, the RFC 8305 recommendation and
   SqlClient's value), if nothing has connected yet, start the next candidate
   **without abandoning** the ones already in flight.
3. `poll()` all in-flight sockets together. `getsockopt(SO_ERROR)` decides success
   for any socket that reports an event — never the poll flags alone (see W1.3).
4. The first socket to report success wins. Close every other socket, install the
   winner in `fd_`, and continue with the existing `TCP_NODELAY` /`SO_NOSIGPIPE`
   /`SetNonBlocking(false)` tail at `tds_socket.cpp:255-265`, unchanged.
5. The caller's `timeout_seconds` becomes what its name has always implied: a
   deadline for the **whole** attempt, not a per-candidate allowance.

Properties this has to hold, in the order they matter:

- **A working first address costs nothing.** No second socket is created before
  250 ms, so the common path is byte-for-byte the current path.
- **A dead candidate costs 250 ms of latency, not 30s.**
- **The total is bounded by `timeout_seconds`** for the first time.
- **Candidate order is preserved.** `getaddrinfo` has already applied RFC 6724
  sorting; W1 staggers that order, it does not re-rank it.

**W1.1 — where the code goes.** A free function in `tds_socket.cpp` owning a
`std::vector` of candidate fds, not a `TdsSocket` member. Per F4, the existing
members are `fd_`-shaped and must stay that way; `TdsSocket` keeps its single-socket
invariant, which is the whole reason the class is easy to reason about.

**W1.2 — one error message for the whole attempt.** Today each failing candidate
overwrites `last_error_`, so the message a user sees is whichever address happened
to be last, with no indication that four others were tried. W1 collects per-candidate
outcomes and reports one message naming the host, the number of candidates, and the
distinct failures — "the address that failed last" is not a diagnosis, and issue
#122 exists partly because nobody could see what the client was doing. This is
Principle III: an operation that cannot succeed must say what it actually attempted.

**W1.3 — `SO_ERROR` is the only success oracle.** Windows maps `poll` to `WSAPoll`
(`tds_socket.cpp:21`). Microsoft documents that a failed TCP connect is signalled
as `POLLHUP | POLLERR | POLLWRNORM` **only as of Windows 10 2004**; on older builds
`WSAPoll` did not report a failed connect at all. Treating the revents as the answer
is therefore not portable across the very Windows versions this issue lives on.
Checking `SO_ERROR` whenever any event fires is correct on every platform and every
build, and the current code already does this for its single socket — W1 keeps that
and drops nothing.

### W2 — `AI_ADDRCONFIG`

Set it in `hints.ai_flags` at `tds_socket.cpp:164`. Independent of W1, one line,
and it shortens the list W1 has to stagger through.

Not sufficient alone, and must not be sold as the fix: a host with a genuine IPv6
address and three IPv4 adapters keeps every one of them.

---

## 3. What this spec does not propose

- **Caching the winning address.** Tempting, and wrong: the reporter explicitly
  reports the failure as *intermittent* — the same address succeeds on one attempt
  and times out on the next. A cache would pin the extension to an address that was
  good once, converting an intermittent 30s stall into a permanent one. RFC 8305
  addresses this by re-racing, and so does W1.
- **Lowering `DEFAULT_CONNECTION_TIMEOUT`.** 30s is a correct ceiling for a WAN dial
  to Azure SQL. The defect is that the ceiling is charged N times, not that it is
  30s.
- **Threading the caller's budget through the login-phase reads.** Spec 068 scoped
  that out deliberately and documented why (`src/include/tds/tds_connection.hpp`,
  the `connect_timeout_seconds_` comment). It is a real gap and it is not this one.
- **Anything about named instances.** The `host\instance` UDP 1434 path (spec 045 /
  #205) has its own resolution problem. W1 sits below it: once a host and port are
  known, this is how they get dialled.

---

## 4. Testing

The precedent is `test/cpp/test_login_routing_hops.cpp` — a `FakeTdsServer` that
binds `127.0.0.1`, accepts on a thread, and is driven by `make test-login-routing-hops`
(`Makefile:500`). W1's tests extend it rather than inventing a harness.

Three cases, in the order of how much they are worth:

1. **A live first candidate is not slowed down.** Dial a `FakeTdsServer` and assert
   the connect completes well inside one stagger interval. Cheap, portable,
   protects the common path forever.
2. **A live candidate behind a dead one wins.** Needs an address that swallows a
   SYN without refusing.
3. **Total time is bounded by the budget when every candidate is dead** — and is
   **not** satisfied trivially fast. See "the assertion shape" below: this case
   needs a lower bound as well as an upper one.

**The blackhole already exists in this repository, and CI already runs it.**
`TestHopHonoursCallerConnectTimeout` (`test/cpp/test_login_routing_hops.cpp`) has a
`FakeTdsServer` gateway route the client to **`192.0.2.1`** (RFC 5737 TEST-NET-1)
and asserts the hop gives up on the caller's 2s budget. It is in the
`make test-login-routing-hops` list and green on every PR, so `192.0.2.1` blackholes
in the CI environment as a matter of record, not conjecture. W1's cases 2 and 3
should reuse that address and cite this test rather than treat the blackhole as an
unsolved problem.

**The assertion shape is the real trap, and that same test demonstrates it.** Its
only timing assertion is `CHECK(secs < 15)` — an **upper** bound. On a host with no
default route, `192.0.2.1` answers `EHOSTUNREACH` in ~0s, and that check passes just
as happily as it does on the blackhole. The test therefore cannot distinguish the
branch it is named for from the branch it is not, which is the failure this spec
elsewhere calls worse than no test — already shipped. W1 must not copy it:

- case 3 and **AC-2** need a **lower** bound too (elapsed ≥ one stagger interval, or
  ≥ some stated fraction of the budget), or
- an assertion on the per-candidate outcomes W1.2 collects, which is the stronger
  form because it names *why* each candidate failed rather than inferring it from a
  clock.

**Where the candidate list comes from.** The routing path above already delivers an
arbitrary address into `TdsSocket::Connect` with **no new API surface at all**, which
covers a single blackhole candidate. What it cannot express is a *multi-candidate*
list, because a routing hop names one host and `Connect` then resolves it — so
cases 2 and 3 still need a seam taking a prepared list of `sockaddr`s, with the
production path filling it from `getaddrinfo`.

> `[NEEDS CLARIFICATION]` Whether the injectable seam is acceptable API surface on
> `TdsSocket`, or should be file-local with a test-only declaration. Settle in
> `/speckit-plan`. Note that the routing precedent above needs neither, so the seam
> is only as wide as the multi-candidate cases require.

### The platform split, measured

This was a `[BLOCKING]` question in the first revision of this spec: which platform
drops a SYN to a bound-but-unlistening `127.0.0.1` port. The draft asserted "drops
on macOS, refused on Linux" and built the injectable-seam design on it, while the
comment it cited (`TestUnreachableRoutedTargetFails`) closed with "That is a Linux
behaviour, not a portable one", which reads as the opposite attribution.

**Measured on both platforms** with `probe_dial_outcomes.cpp` in this directory
(`./run_probe.sh` builds it natively and, from a Mac, in a `gcc:13` container).
The probe dials exactly as `TdsSocket::Connect` does — non-blocking `connect()`,
`poll()`, `getsockopt(SO_ERROR)` — so it measures what the extension's own dial
sees:

| | bound, never `listen()` | nothing bound (control) | `192.0.2.1` |
|---|---|---|---|
| **macOS** (Darwin 27.2.0, arm64) | **SYN DROPPED** — `connect()` sits **7.8s**, then `ETIMEDOUT` | REFUSED, 0 ms | no answer within 3s |
| **Linux** (linuxkit 7.0.12, aarch64, container) | **REFUSED** — `ECONNREFUSED`, 0 ms | REFUSED, 0 ms | no answer within 3s |

**The draft's reading was right**, and the 7.8s it quoted reproduces to the
millisecond. The cited comment is not wrong, only ambiguously worded: its "That"
refers to *getting an RST*, which is indeed the Linux behaviour and is indeed not
portable — but it sits next to a sentence about the drop and reads as if it
describes it. Worth rewording that comment; it costs the next reader the same
half hour twice.

**So the design conclusion stands.** CI runs on Linux, where the
bound-but-unlistening trick yields a **refusal** — the wrong branch. Cases 2 and 3
therefore do need an address supplied to them, and the seam above is justified.

**And `192.0.2.1` blackholes in both environments, including the Linux container**
— the CI shape. So `TestHopHonoursCallerConnectTimeout` really does take the
timeout branch in CI today. That makes it the right address to reuse, and leaves
the criticism of its assertion shape exactly where it was: the test cannot *prove*
which branch it took, and on a host with no default route it would flip to
`EHOSTUNREACH` and still pass.

Two caveats on the Linux row, stated rather than papered over: it is Docker
Desktop's linuxkit kernel on arm64, not a GitHub `ubuntu` runner, and the
container's `192.0.2.1` result depends on it having a default route via the docker
bridge (a CI runner does too). RST-vs-drop for a bound-but-unlistening socket is
core TCP-stack behaviour and transfers; if a reader wants the runner itself on
record, `run_probe.sh` is cheap to drop into the existing Linux CI lane.

---

## 5. Retracted — do not re-propose

**"`mssql_connection_timeout` never reaches the pool's dial, so there is no
workaround."** False as of spec 073 W3. `TdsConnection::Connect` clamps a
non-positive budget to the compiled-in default and stores it in
`connect_timeout_seconds_` (`src/tds/tds_connection.cpp:187-192`), and all four
catalog connection factories capture the catalog's shared `connect_timeout_`
(`std::shared_ptr<std::atomic<int>>`, initialised from
`pool_config_.connection_timeout` in the `MSSQLCatalog` ctor) and read it at dial
time.

Consequences, all three of which matter:

- **There is a workaround today, and it only works BEFORE `ATTACH`.**
  `SET mssql_connection_timeout = 3` cuts the per-candidate allowance to 3s,
  turning the reporter's 30s stall into 3s. But the setting is read **once per
  catalog, at ATTACH**: `LoadPoolConfig(ClientContext&)`
  (`src/connection/mssql_settings.cpp`) has exactly one call site, in the ATTACH
  path of `src/mssql_storage.cpp`, and its answer is frozen into `pool_config_` and
  from there into `connect_timeout_`, which nothing writes again after
  `ValidateThroughPool` restores it. So:

  ```sql
  SET mssql_connection_timeout = 3;   -- first
  ATTACH '...' AS db (TYPE mssql);    -- then this dial is bounded at 3s
  ```

  The reverse order does nothing for that catalog, and nothing at all for an
  `ATTACH` already in flight — which is exactly the operation #122 reports as
  slow. **Any advice posted on #122 must state the ordering**, or it will be tried
  the wrong way round and read as "the workaround does not work". The same holds
  for `mssql_attach_validation_timeout`, the more targeted lever for the ATTACH
  dial itself: also read at ATTACH, also pre-ATTACH only.
- It is a blunt instrument either way — it also shortens the legitimate ceiling for
  a slow WAN dial to Azure SQL.
- **#122 is therefore not urgent, only wrong** — but "not urgent" rests on users
  being able to reorder two statements, not on the stall being avoidable once
  `ATTACH` has run.

---

## 6. Acceptance

- **AC-1** — On a host whose hostname resolves to several addresses with a
  blackholing first candidate, `ATTACH` completes in under 2s with the default
  `mssql_connection_timeout`. Confirmed on the reporter's multi-NIC Windows machine
  or an equivalent; per §0 this cannot be signed off from CI alone.
- **AC-2** — With every candidate dead, `Connect` returns within
  `timeout_seconds` ± one stagger interval, and the error names the host, the number
  of candidates tried, and the distinct failures. Asserted with a **lower** bound as
  well as an upper one (§4, "the assertion shape"): an upper bound alone is also
  satisfied by an environment that fails every candidate instantly, so on its own it
  does not witness the timeout branch at all. In-flight candidates are never
  abandoned, so the deadline must close **all** of them at `timeout_seconds` — a
  late-started candidate cannot extend the attempt past it.
- **AC-3** — A single-address host and a live-first-candidate host show no
  regression against `main` in the harness of §4 case 1.
- **AC-4** — Exactly one socket is open when `Connect` returns true, and none when
  it returns false. Asserted, not assumed: W1 is the first code in this class to
  hold more than one fd at a time, and a leaked candidate fd is the failure mode it
  invites. Run the §4 cases under ASan/LSan with an fd-count assertion. Assert it on
  the **prewarm** path too: with `mssql_min_connections > 0`, `ConnectionPool::Prewarm`
  runs one dialling thread per connection, so the pool holds up to
  N × candidates sockets at once and that is where a leaked candidate fd multiplies.
- **AC-5** — No behavioural change on the `127.0.0.1` paths: the full `test/cpp`
  suite and the integration suite pass unchanged.

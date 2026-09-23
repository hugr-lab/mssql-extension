# Spec 081 — Connecting to a hostname that resolves to more than one address

**Status**: DRAFT. Not implemented. Triaged from a report, not yet measured on the
hardware that produced it.
**Closes**: [#122](https://github.com/hugr-lab/mssql-extension/issues/122)
(`ATTACH` can last 30s on Windows with multiple network adapters).
**Relates to**: [#324](https://github.com/hugr-lab/mssql-extension/issues/324)
(parallel pool warm-up) — a warm-up that opens N connections up front multiplies
whatever the dial costs, so this lands first. Spec 073 W3 (the connect timeout now
reaches the dial) is what makes the mitigation below possible at all.

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

Everything in §1 is verified by reading `origin/main` at `e6ab3a2` and is
reproducible with `grep`. Nothing in §1 has been **measured**, because the failure
needs a Windows host with several NICs and a hostname whose first DNS answer
blackholes — the reporter has one, CI does not.

That gap is deliberate and it bounds what this spec may claim. It does not claim a
speed-up figure. It claims that a code path exists which spends `N × timeout` where
it should spend `timeout`, and that removing it cannot make anything slower. Before
§6 is signed off, one of the two confirmations there has to come from real
multi-NIC hardware.

Where a platform behaviour is asserted below, it is either cited to Microsoft's
documentation or to a measurement **already recorded in this repository**
(`test/cpp/test_login_routing_hops.cpp`, which measured the macOS/Linux split on
connecting to a bound-but-unlistening port). No new platform claim is invented here.

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

### F3 — `ATTACH` crosses the dial twice

`ATTACH` runs the spec 047 validation round trip (`src/mssql_storage.cpp:1837`,
budget from `mssql_attach_validation_timeout`, default `0` ⇒ inherit
`mssql_connection_timeout`), and then the first pooled connection dials again. Both
cross `TdsSocket::Connect`, so both pay F1. The issue title says 30s; the mechanism
allows twice that before a single query runs.

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
2. **A live candidate behind a dead one wins.** Needs an address that swallows a SYN
   without refusing. This is the hard part and it is *already documented in this
   repository*: `test_login_routing_hops.cpp` measured that a bound-but-unlistening
   `127.0.0.1` port **drops** the SYN on macOS (`connect()` sits ~7.8s) but is
   **refused** on Linux. So that trick gives a blackhole on macOS and a refusal on
   Linux — the wrong branch on the platform CI runs.
3. **Total time is bounded by the budget when every candidate is dead.**

For 2 and 3, the candidate list must be **injectable** — a seam taking a prepared
list of `sockaddr`s, with the production path filling it from `getaddrinfo`. Then a
test supplies a blackhole address explicitly rather than hoping the kernel provides
one. `192.0.2.1` (RFC 5737 TEST-NET-1) is the conventional choice, but its behaviour
depends on the host's routing table: with a default route the SYN leaves and is
dropped (blackhole, what we want); with none, `EHOSTUNREACH` arrives immediately
(fast failure, wrong branch). So the blackhole address is an input to the test, with
a documented default, and case 2 skips with a clear message when the environment
cannot provide one. A test that silently exercises the refusal branch while claiming
to test the timeout branch is worse than no test.

> `[NEEDS CLARIFICATION]` Whether the injectable seam is acceptable API surface on
> `TdsSocket`, or should be file-local with a test-only declaration. Settle in
> `/speckit-plan`.

---

## 5. Retracted — do not re-propose

**"`mssql_connection_timeout` never reaches the pool's dial, so there is no
workaround."** False as of spec 073 W3. `TdsConnection::Connect` now clamps a
non-positive budget to the compiled-in default and stores it
(`src/tds/tds_connection.cpp:172-192`), and all four catalog connection factories
pass `pool_config_.connection_timeout` into the dial
(`src/catalog/mssql_catalog.cpp:210, 255, 278, 342`).

Consequences, both of which matter:

- **There is a workaround today.** `SET mssql_connection_timeout = 3` cuts the
  per-candidate allowance to 3s, turning the reporter's 30s stall into 3s. Worth
  saying on #122 now rather than after W1 ships. It is a blunt instrument — it also
  shortens the legitimate ceiling for a slow WAN dial — but it is real and it is
  available in released builds.
- **#122 is therefore not urgent, only wrong.** The absence of any workaround was
  the only thing that would have made it a release blocker.

---

## 6. Acceptance

- **AC-1** — On a host whose hostname resolves to several addresses with a
  blackholing first candidate, `ATTACH` completes in under 2s with the default
  `mssql_connection_timeout`. Confirmed on the reporter's multi-NIC Windows machine
  or an equivalent; per §0 this cannot be signed off from CI alone.
- **AC-2** — With every candidate dead, `Connect` returns within
  `timeout_seconds` ± one stagger interval, and the error names the host, the number
  of candidates tried, and the distinct failures.
- **AC-3** — A single-address host and a live-first-candidate host show no
  regression against `main` in the harness of §4 case 1.
- **AC-4** — Exactly one socket is open when `Connect` returns true, and none when
  it returns false. Asserted, not assumed: W1 is the first code in this class to
  hold more than one fd at a time, and a leaked candidate fd is the failure mode it
  invites. Run the §4 cases under ASan/LSan with an fd-count assertion.
- **AC-5** — No behavioural change on the `127.0.0.1` paths: the full `test/cpp`
  suite and the integration suite pass unchanged.

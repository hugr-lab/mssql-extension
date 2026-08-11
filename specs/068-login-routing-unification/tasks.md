---

description: "Task list for spec 068 — login routing for every auth path"
---

# Tasks: Login routing for every auth path

**Input**: Design documents from `/specs/068-login-routing-unification/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/tds_connection_routing.md, quickstart.md

**Tests**: INCLUDED. The spec makes testing decision D4 with acceptance criterion
"All parser + hop-driver pins green", so test tasks are in scope and are written
before the implementation they pin.

**Organization**: grouped by user story. spec.md is written as decisions
(D1–D4) rather than user stories, so the three stories below are derived from
its three gaps — one per authentication path, prioritised by user impact.

**Base**: `571e465` or later (must include PR #254/#255).

## Format: `[ID] [P?] [Story] Description`

- **[P]**: can run in parallel (different files, no dependencies)
- **[Story]**: US1 / US2 / US3
- Exact file paths in every description

## Path Conventions

Single C++ tree: `src/` (implementation), `test/cpp/` (standalone unit tests),
`docs/` + repo-root `*.md` (documentation). Line numbers cited below are from
`571e465`.

## ⚠️ Parallelism reality check

Nearly all implementation lands in **one file**, `src/tds/tds_connection.cpp`.
Those tasks are therefore **not** marked `[P]` even where they are logically
independent — two agents editing that file concurrently will conflict. Genuine
parallelism exists between: implementation vs. `test/cpp/*` vs. `docs/*`, and
among the doc tasks. This is stated up front so nobody reads the sparse `[P]`
markers as an oversight.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: build and CI wiring for the new test binary, before any code needs it

- [X] T001 Add a `test-login-routing-hops` target to `Makefile` (near `test-login-error-state`, ~line 279) linking the TDS-only set from research.md R3: `test/cpp/test_login_routing_hops.cpp src/tds/tds_connection.cpp src/tds/tds_socket.cpp src/tds/tds_packet.cpp src/tds/tds_protocol.cpp src/tds/tds_types.cpp src/tds/encoding/utf16.cpp src/tds/tls/tds_tls_context.cpp src/tds/tls/tds_tls_impl.cpp` plus `-lssl -lcrypto -lsimdutf -pthread`
- [X] T002 [P] Create `test/cpp/test_login_routing_hops.cpp` skeleton — file-header comment (purpose, manual compile line, `make test-login-routing-hops`), the `CHECK` macro copied from `test/cpp/test_login_error_state.cpp:49`, and a `main` that prints a check count and exits 0 with zero cases registered
- [X] T003 [P] Add the `test_login_routing_hops` group to `.github/workflows/ci.yml` in the **light** job alongside `test_login_error_state` (~line 369), with a comment stating why it needs no DuckDB build (research.md R3) — the heavy job is dispatch-gated and effectively does not run

**Checkpoint**: `make test-login-routing-hops` builds and passes vacuously; the link set is proven before any logic depends on it.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: the routing types, the normalizer, the hop driver, and the migration of the one already-working path onto it

**⚠️ CRITICAL**: no user story can begin until T004–T010 are done. T008–T009 are a **behaviour-preserving refactor** — the FEDAUTH path must emit `Route` in exactly the same circumstances as today (`success && has_routing`). The D1 ordering change for that path is US2, deliberately held back so the refactor can be reviewed as a no-op.

- [X] T004 Add `enum class LoginAttemptOutcome : uint8_t { Success, Route, Failure }` to `src/include/tds/tds_connection.hpp` in namespace `duckdb::tds`, with the D1 invariant as a comment (data-model.md §1)
- [X] T005 Add `using AuthenticatorFactory = std::function<std::shared_ptr<IAuthenticator>(const std::string &host, uint16_t port)>;` to `src/include/tds/tds_connection.hpp`, plus `#include <functional>`; document that `host` is the TCP/DNS host, never `tds_server_name_` (data-model.md §2, research.md R9)
- [X] T006 Declare the private members `bool RunWithRoutingHops(const std::function<LoginAttemptOutcome()> &attempt);` and `void NormalizeRoutedTarget(const std::string &routed_server, uint16_t routed_port, std::string &out_host, uint16_t &out_port);` in `src/include/tds/tds_connection.hpp` (private section, near the existing `DoLogin7` declarations ~line 291)
- [X] T007 Implement `TdsConnection::NormalizeRoutedTarget` in `src/tds/tds_connection.cpp` by lifting the block at lines 309–369 verbatim — trailing `:digits` beats the ENVCHANGE port, `tds_server_name_` keeps `\instance`, DNS host strips it, no SQL Browser lookup (data-model.md §3, research.md R5)
- [X] T008 Implement `TdsConnection::RunWithRoutingHops` in `src/tds/tds_connection.cpp`: `MAX_ROUTING_HOPS = 5` with the current `while (hop <= MAX)` shape preserved, per-attempt reset of `has_routing_`/`routed_server_`/`routed_port_`/`next_packet_id_`/`tls_enabled_`/`fedauth_echo_` **including before the first attempt** (research.md R8), `socket_->Close()` + `NormalizeRoutedTarget` + `socket_->Connect()` on `Route`, and sole ownership of the `Idle`/`Disconnected` transitions (research.md R2)
- [X] T009 Add the two stable driver error strings from contracts/ C3 to `RunWithRoutingHops` in `src/tds/tds_connection.cpp`: hop-limit `"Too many routing hops (N) - aborting; last routed target was <server>:<port>"` (extends the bare message at line 294) and reconnect-failure naming host, port and the socket error
- [X] T010 Change `DoLogin7WithFedAuth` in `src/tds/tds_connection.cpp` (line ~450) to return `LoginAttemptOutcome`, and rewrite `AuthenticateWithFedAuth` (lines 243–391) as a thin wrapper passing a lambda that runs `DoPreloginWithFedAuth` + `DoLogin7WithFedAuth`; set `tds_server_name_ = host_` before the first attempt as line 255 does today. **No ordering change yet** — `Route` still requires `success`
- [X] T011 [P] Add a fake-TDS-server helper to `test/cpp/test_login_routing_hops.cpp`: listener bound to `127.0.0.1:0` with `SO_REUSEADDR`, port read back via `getsockname`, served on a `std::thread`, answering PRELOGIN with `ENCRYPT_NOT_SUP` and LOGIN7 with a caller-supplied token stream; `accept` uses a poll timeout so a hang fails in seconds (research.md R4)
- [X] T012 [P] Add builders to `test/cpp/test_login_routing_hops.cpp` for a ROUTING ENVCHANGE token (type 20: protocol byte 0, LE port, UTF-16LE server with char-count length) and for LOGINACK + DONE, so each scenario composes its own login response
- [X] T013 Pin the foundational refactor in `test/cpp/test_login_routing_hops.cpp`: a **zero-hop** FEDAUTH-shaped login (LOGINACK + DONE, no ROUTING) reaches `GetState() == Idle` with the first packet's id equal to 1 — proves T010 changed no behaviour

**Checkpoint**: driver exists, FEDAUTH runs through it unchanged, fake server works. User stories can start.

---

## Phase 3: User Story 1 — SQL auth follows routing (Priority: P1) 🎯 MVP

**Goal**: A user ATTACHing Azure SQL with the Redirect connection policy (the default for traffic originating inside Azure), Azure SQL Managed Instance, or an on-prem AlwaysOn read-intent endpoint gets a working connection instead of a bogus "Authentication failed" — and, in the LOGINACK case, stops silently transacting against a gateway that told the client to leave. This is spec.md Gap 2.

**Why P1**: it is the only gap that can currently produce *silent* wrong behaviour (constitution III), and it affects the default policy for in-Azure clients. It is also the path with the most callers — `mssql_catalog.cpp:222`, `mssql_storage.cpp` validation, and the deprecated `mssql_diagnostic.cpp:144`, which inherits the fix for free.

**Independent Test**: `make test-login-routing-hops` — listener A answers LOGIN7 with ROUTING → listener B, B answers LOGINACK + DONE; `Authenticate()` returns true and the connection reports B's host/port. No SQL Server, no Azure.

### Tests for User Story 1 ⚠️ write first, confirm they fail

- [X] T014 [P] [US1] Add a parser pin to `test/cpp/test_login_error_state.cpp`: route-**with**-LOGINACK sets `has_routing == true` **and** `success == true`, with `routed_server`/`routed_port` extracted (D4 layer 1)
- [X] T015 [P] [US1] Add a parser pin to `test/cpp/test_login_error_state.cpp`: ROUTING sitting **behind** a DONEINPROC run is still parsed — the composition with PR #254 that a routing Synapse would send
- [X] T016 [P] [US1] Add hop scenario 1 to `test/cpp/test_login_routing_hops.cpp`: single hop under `Authenticate()`, asserting return `true`, `GetState() == Idle`, and final host/port equal to listener B's
- [X] T017 [P] [US1] Add hop scenario 4 to `test/cpp/test_login_routing_hops.cpp`: listener B asserts the first packet it receives has `packet_id == 1`, pinning the per-hop sequence reset
- [X] T018 [P] [US1] Add hop scenario 3 to `test/cpp/test_login_routing_hops.cpp`: a listener routing **to itself** aborts after exactly 5 hops, with `GetLastError()` containing both the count and the last routed target

### Implementation for User Story 1

- [X] T019 [US1] Change `DoLogin7` in `src/tds/tds_connection.cpp` (line 835) to return `LoginAttemptOutcome` and apply the data-model.md §5 statement order: `NoteFeatureAcks` → copy routing fields → return `Route` if `has_routing_` → `Failure` on `!success` (keeping the issue-#164 state-byte message at line 883) → `Success`
- [X] T020 [US1] Fix the LOGIN7 ServerName in `DoLogin7` (`src/tds/tds_connection.cpp:847`): pass `tds_server_name_.empty() ? host_ : tds_server_name_` to `BuildLogin7`, matching the integrated path at line 725. Without this a routed `host\instance` sends the instance-stripped name (research.md R7)
- [X] T021 [US1] Rewrite `Authenticate` in `src/tds/tds_connection.cpp` (line 149) as a `RunWithRoutingHops` wrapper whose lambda runs `DoPrelogin(use_encrypt)` then `DoLogin7(...)`; initialise `tds_server_name_ = host_` before the first attempt; delete the now-duplicated state transitions at lines 159/167/174
- [X] T022 [US1] Verify no caller change is needed for `src/catalog/mssql_catalog.cpp:222`, `src/mssql_storage.cpp` SQL-auth validation, and `src/connection/mssql_diagnostic.cpp:144`; add a one-line comment at the diagnostic call site noting it now follows routing via the shared driver

**Checkpoint**: SQL auth hops. US1 is shippable on its own — the other two paths still behave exactly as before.

---

## Phase 4: User Story 2 — a routed FEDAUTH login without LOGINACK follows the hop (Priority: P2)

**Goal**: Close spec.md Gap 1. A gateway that answers the FEDAUTH token with ROUTING + DONE and **no** LOGINACK — legal per MS-TDS — currently dies as "Azure AD authentication failed" because `DoLogin7WithFedAuth` returns on `!success` (line 618) before capturing routing (line 633). After this, it hops.

**Why P2**: real but unconfirmed in the field (the Azure gateways met so far send LOGINACK). It is a two-statement move once Foundational is in.

**Independent Test**: parser pin (route-without-LOGINACK yields `has_routing == true`, `success == false`) plus a fake-server scenario where listener A omits LOGINACK — the login must still complete on B.

### Tests for User Story 2 ⚠️ write first, confirm they fail

- [X] T023 [P] [US2] Add the Gap-1 parser pin to `test/cpp/test_login_error_state.cpp`: ROUTING + DONE with no LOGINACK parses as `has_routing == true`, `success == false`, routed fields populated
- [X] T024 [P] [US2] Add hop scenario 2 to `test/cpp/test_login_routing_hops.cpp`: listener A routes **without** LOGINACK and the connection still completes against listener B

### Implementation for User Story 2

- [X] T025 [US2] Apply the data-model.md §5 order to `DoLogin7WithFedAuth` in `src/tds/tds_connection.cpp`: move the routing capture (lines 633–639) **above** the `!success` return (line 618) and return `Route` from there, so `Route` no longer requires `success`
- [X] T026 [US2] Re-read `AuthenticateWithFedAuth` after T025 and confirm the wrapper needs no change — the driver already treats `Route` uniformly; record the confirmation in the PR description rather than adding code

**Checkpoint**: routing is honoured regardless of LOGINACK on both the SQL and FEDAUTH paths.

---

## Phase 5: User Story 3 — integrated auth follows routing with a per-hop SPN (Priority: P3)

**Goal**: Close spec.md Gap 3 (D3). A Kerberos/SSPI login that is routed needs a service ticket for the **routed host's** SPN, not a retry of the gateway's. `AuthenticateIntegrated` takes an `AuthenticatorFactory` instead of a pre-built authenticator.

**Why P3**: smallest affected population, largest unverifiable surface — no AD + routing front-end exists in any environment we control (spec.md risk 1). It also carries the only breaking internal signature change, so it is the story most worth landing last and reviewing hardest.

**Independent Test**: `test/kerberos/` docker stack still authenticates end to end (proves the factory produces the same first-attempt behaviour as today), plus a fake-server hop scenario asserting the factory is invoked a second time with the **routed** host and port.

### Tests for User Story 3 ⚠️ write first, confirm they fail

- [X] T027 [P] [US3] Add a hop scenario to `test/cpp/test_login_routing_hops.cpp` that drives `AuthenticateIntegrated` with a stub `IAuthenticator` and records every `(host, port)` the factory is called with; assert two calls, the second carrying listener B's host and port
- [X] T028 [P] [US3] Extend that scenario to assert the first authenticator's `Free()` was called before the second was constructed (research.md R9 — otherwise a GSSAPI context leaks per hop)
- [X] T029 [P] [US3] Add a scenario where the factory returns `nullptr` on the **hop** (not the first call) and assert the error names the routed host — the spec's field-diagnosability mitigation

### Implementation for User Story 3

- [X] T030 [US3] Change `AuthenticateIntegrated`'s second parameter in `src/include/tds/tds_connection.hpp` (line ~74) and `src/tds/tds_connection.cpp` (line 662) from `std::shared_ptr<tds::IAuthenticator>` to `AuthenticatorFactory`, **without** adding a compatibility overload (contracts/ C2 states why)
- [X] T031 [US3] Restructure `AuthenticateIntegrated` in `src/tds/tds_connection.cpp` as a `RunWithRoutingHops` wrapper: the lambda calls the factory with the current `host_`/`port_`, runs `DoPrelogin`, sends LOGIN7 with the SSPI blob, and drives the SPNEGO round loop; move all nine in-body `state_.store(Disconnected)` + `socket_->Close()` pairs out to the driver (research.md R2)
- [X] T032 [US3] Inside that SPNEGO round loop, apply the data-model.md §5 ordering: check `has_routing` **before** the `success` branch (line 761) and before the `has_sspi_token` rejection (line 774), otherwise a routing answer with no SSPI token is misreported as "auth rejected by server (no SSPI token, no LOGINACK)"
- [X] T033 [US3] Preserve the null-authenticator error in `src/tds/tds_connection.cpp`: a factory returning `nullptr` on the **first** call keeps the existing "compiled without Kerberos / SSPI support … Rebuild with -DENABLE_KRB5=ON" wording from line 677
- [X] T034 [US3] Call `authenticator->Free()` on the `Route` outcome in `src/tds/tds_connection.cpp` before the next hop's factory call, matching the existing success-path `Free()` at line 770
- [X] T035 [US3] Update the pool factory in `src/catalog/mssql_catalog.cpp` (lines 169–200) to pass a per-hop lambda that copies `info_copy`, overwrites `host`/`port` with the callable's arguments, and calls `AuthStrategyFactory::Create` inside a `try` — reference shape in contracts/ C2. Keep the existing stderr diagnostics
- [X] T036 [US3] Update `ValidateIntegratedAuthConnection` in `src/mssql_storage.cpp` (lines 1303–1345) the same way; the `InvalidInputException` must be thrown **after** `AuthenticateIntegrated` returns, since the callable cannot throw across the TDS layer
- [X] T037 [US3] Confirm `AuthStrategyFactory::DeriveSpn` in `src/tds/auth/auth_strategy_factory.cpp` needs no change — it already derives from `info.host`/`info.port` and returns an explicit `service_principal_name` verbatim, so both D3 requirements fall out of the existing code (research.md R9)

**Checkpoint**: all three paths follow routing. The spec's acceptance criterion "ROUTING is followed on all three auth paths" is met.

---

## Phase 6: Polish & Cross-Cutting Concerns

- [X] T038 [P] Update `docs/tds-protocol.md`: ENVCHANGE 20 section gains the D1 contract (`has_routing` outranks `success`), the hop limit, and the no-SQL-Browser-on-hop rule
- [X] T039 [P] Update `docs/architecture.md`: routing is a connection-level behaviour, not an Azure-only one; the per-path table collapses to a single row
- [X] T040 [P] Update `Kerberos.md`: on a hop the SPN is derived for the routed host; an explicit `service_principal_name=` survives every hop verbatim
- [X] T041 [P] Update `DATAMODEL.md` connection-layer section (and its diagram): login is now a hop loop — CLAUDE.md makes this mandatory in the same PR when an end-to-end flow changes
- [X] T042 [P] Add a changelog entry for the behaviour change: SQL auth against a routing gateway that also sends LOGINACK now hops instead of "succeeding" against the gateway (spec.md risk 3)
- [X] T043 Run `clang-format` **version 14** over the touched files — `/opt/homebrew/opt/llvm@14/bin/clang-format -i src/tds/tds_connection.cpp src/include/tds/tds_connection.hpp src/catalog/mssql_catalog.cpp src/mssql_storage.cpp test/cpp/test_login_routing_hops.cpp test/cpp/test_login_error_state.cpp`; the homebrew default (v22) formats macro continuations differently and CI will disagree
> **Status of T044–T047 (the four open items).** Everything that can be verified
> on this machine was: both unit suites are green (56 + 45 checks), every
> translation unit that includes `tds_connection.hpp` was syntax-checked against
> the pinned DuckDB submodule, and the routing pins were confirmed to FAIL under
> the pre-068 behaviour before being confirmed green under the fix. What remains
> needs infrastructure this worktree does not have: a completed vcpkg/DuckDB
> build tree (`make debug`), a SQL Server container, a Kerberos stack, an Azure
> subscription, and a Windows runner.

- [ ] T044 Run the quickstart.md sequence steps 1–4: `make test-login-error-state`, `make test-login-routing-hops`, `make`, `make test`, then `make docker-up && make integration-test && make docker-down` — the docker server does not route, which is exactly what proves the zero-hop path unchanged
- [ ] T045 Run the `test/kerberos/` docker stack (`docker compose up -d --build`, `docker compose exec test-client /run-tests.sh`, `docker compose down -v`) to validate US3's signature change end to end; note in the PR that it exercises no hop
- [ ] T046 Dispatch the Azure Test workflow on the branch as FEDAUTH smoke, and record in the PR whether the gateway routed (subscription-dependent) so the next reader knows what was actually covered
- [ ] T047 Verify the Windows leg compiles: the fake-server listener code in `test/cpp/test_login_routing_hops.cpp` needs the same `#ifdef _WIN32` split `TdsSocket` uses. Windows CI is dispatch-only and currently red for unrelated MSVC reasons (#165), so state plainly in the PR that this was compile-checked, not run (research.md R4)
- [X] T048 Grep the diff for credential leakage before opening the PR — no FEDAUTH token bytes, SSPI blobs, or passwords in any new log line; the redaction comment at `src/tds/tds_connection.cpp:573` is the standard

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: no dependencies
- **Foundational (Phase 2)**: needs Phase 1 (T002 creates the file T011–T013 extend). **Blocks all user stories**
- **US1 (Phase 3)**, **US2 (Phase 4)**, **US3 (Phase 5)**: each depends only on Phase 2
- **Polish (Phase 6)**: T038–T042 can start as soon as the story they document is done; T043–T048 need all desired stories complete

### User Story Dependencies

- **US1 (P1)**: independent. Touches `DoLogin7` + `Authenticate` only
- **US2 (P2)**: independent of US1. Touches `DoLogin7WithFedAuth` only
- **US3 (P3)**: independent of US1 and US2. Touches `AuthenticateIntegrated` + its two callers

The three stories were scoped along path boundaries precisely so no story depends on another. Note that the D1 ordering fix appears once per story rather than as a shared task — that is deliberate: each path's login helper has a different response-handling shape (a single parse, a two-phase token exchange, and a multi-round SPNEGO loop), and there is no shared function to change.

### Within Each User Story

- Tests first, confirmed failing, then implementation
- Header declarations before definitions
- `src/tds/` before its callers

### Parallel Opportunities

- T002 ‖ T003 (test file ‖ CI yaml)
- T011 ‖ T012 within Foundational (both in the test file, but distinct helpers — sequence them if one agent)
- Test tasks within a story: T014 ‖ T015 ‖ T016 ‖ T017 ‖ T018; T023 ‖ T024; T027 ‖ T028 ‖ T029
- Doc tasks T038 ‖ T039 ‖ T040 ‖ T041 ‖ T042
- **Whole stories** in parallel across people — but T019/T021 (US1), T025 (US2) and T031/T032 (US3) all edit `src/tds/tds_connection.cpp`, so parallel story work means merge conflicts in one file. With one agent, run the stories in priority order.

---

## Parallel Example: User Story 1

```bash
# All US1 pins, written before any implementation:
Task: "Route-with-LOGINACK parser pin in test/cpp/test_login_error_state.cpp"
Task: "ROUTING-behind-DONEINPROC parser pin in test/cpp/test_login_error_state.cpp"
Task: "Single-hop SQL-auth scenario in test/cpp/test_login_routing_hops.cpp"
Task: "Per-hop packet-id reset scenario in test/cpp/test_login_routing_hops.cpp"
Task: "Hop-limit abort scenario in test/cpp/test_login_routing_hops.cpp"

# Then implementation, strictly sequential — one file:
#   T019 -> T020 -> T021 -> T022 in src/tds/tds_connection.cpp
```

---

## Implementation Strategy

### MVP First (User Story 1 only)

1. Phase 1 Setup (T001–T003)
2. Phase 2 Foundational (T004–T013) — **the refactor must be reviewable as a no-op**
3. Phase 3 US1 (T014–T022)
4. **STOP and VALIDATE**: `make test-login-routing-hops` green, `make integration-test` unchanged against the non-routing docker server
5. Shippable: Azure MI / Redirect-policy / AlwaysOn read-intent users are unblocked, and nothing silently talks to a gateway any more

### Incremental Delivery

1. Setup + Foundational → driver in place, FEDAUTH provably unchanged
2. + US1 → SQL auth hops (MVP)
3. + US2 → route-without-LOGINACK honoured on FEDAUTH too
4. + US3 → integrated auth hops with a per-hop SPN
5. + Polish → docs, changelog, live smoke

Each increment is independently mergeable; US3 can be split into its own PR if the SPN-over-hop risk warrants separate review.

---

## Notes

- `[P]` = different files, no dependencies. Sparse here on purpose — see the parallelism note at the top
- Verify each test fails before implementing the task it pins
- Commit per task or per logical group; stop at any checkpoint to validate
- Do not add a `shared_ptr` compatibility overload for `AuthenticateIntegrated` (contracts/ C2)
- Do not introduce SQL Browser lookups on a hop (spec D2, contracts/ C4)
- `src/tds/` must stay DuckDB-free — no DuckDB header may enter `tds_connection.hpp` or `iauthenticator.hpp`

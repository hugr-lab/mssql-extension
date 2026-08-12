# Implementation Plan: Login routing for every auth path

**Branch**: `task/spec-068-4f420d` (spec text arrives via `spec/068-login-routing`, PR #257) | **Date**: 2026-08-11 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/068-login-routing-unification/spec.md`

## Summary

TDS login-time routing (ENVCHANGE type 20) is parsed correctly but consumed on
exactly one of three authentication paths. This plan extracts the hop loop that
already works on the FEDAUTH path into a single private driver on
`TdsConnection`, makes all three paths go through it, and fixes the ordering bug
that makes a route-without-LOGINACK response look like an authentication
failure.

Technical approach, in one line each:

- A private `RunWithRoutingHops(attempt)` owns the loop, the routed-target
  normalization, the per-hop state resets, and the state-machine transitions.
- The per-attempt callback returns a **tri-state** outcome
  (`Success` / `Route` / `Failure`) rather than the `bool` the spec sketches —
  route-without-LOGINACK is exactly the case a `bool` cannot express, and it is
  the bug D1 exists to fix. See [research.md](./research.md) R1.
- `AuthenticateIntegrated` takes a `(host, port) → IAuthenticator` factory so a
  hop gets a service ticket for the routed host's SPN.
- Tests are three layers: parser pins (no server), a loopback fake-TDS-server
  hop test (no server, no DuckDB link), live smoke on Azure + the Kerberos
  stack.

## Technical Context

**Language/Version**: C++17 in the TDS layer, but **C++11-compatible ABI**
constraints apply repo-wide (no structured bindings; ODR hazard on Linux — see
CLAUDE.md "Build Troubleshooting"). `std::function` and lambdas are already used
throughout `mssql_catalog.cpp`, so the driver signature adds no new requirement.

**Primary Dependencies**: none new. Existing: OpenSSL (vcpkg, TLS), simdutf
(UTF-16 decode). The `src/tds/` layer has **zero DuckDB dependencies** and this
must stay true — `RunWithRoutingHops` and the authenticator-factory typedef live
behind `tds/tds_connection.hpp` and `tds/auth/iauthenticator.hpp`, neither of
which may include a DuckDB header.

**Storage**: N/A (connection-establishment path only; no persisted state).

**Testing**:
- Parser pins extend `test/cpp/test_login_error_state.cpp` (TDS-only link:
  `tds_packet.cpp tds_protocol.cpp tds_types.cpp encoding/utf16.cpp` +
  `-lsimdutf`), run by `make test-login-error-state` and by the CI heavy job.
- New `test/cpp/test_login_routing_hops.cpp` — loopback fake TDS server. Links
  the same TDS-only set **plus** `tds_connection.cpp tds_socket.cpp
  tls/tds_tls_context.cpp tls/tds_tls_impl.cpp` and `-lssl -lcrypto`. Verified
  self-contained: `tds_connection.cpp` includes only its own headers plus libc
  (see research.md R3), so this does **not** require a DuckDB build and belongs
  in the light CI job next to the other TDS-only groups.
- Live: Azure Test workflow dispatch (fedauth regression) + `test/kerberos/`
  docker stack (integrated-path refactor).

**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC + MinGW). The
fake-server test uses BSD sockets; Windows needs the existing
`tds_platform.hpp` shims plus `WSAStartup`, which `TdsSocket` already performs
(research.md R4 records the gate decision if that proves awkward).

**Project Type**: DuckDB extension (single C++ tree, `src/` + `test/`).

**Performance Goals**: no measurable change on the non-routing path. The driver
adds one `std::function` indirection per **login**, not per query or per row;
`AuthenticateWithFedAuth`'s hop count and packet sequence must be byte-identical
to today against the Azure/Fabric gateways.

**Constraints**: `MAX_ROUTING_HOPS = 5` preserved exactly, including the current
off-by-design shape (`while (routing_hop <= MAX_ROUTING_HOPS)` → up to 5 hops /
6 login attempts). No SQL Browser UDP traffic on a hop. No credential material
(FEDAUTH token bytes, SSPI blobs) in any new log line — the existing redaction
comments at `tds_connection.cpp:573` are the standard.

**Scale/Scope**: three public entry points, two internal login helpers, two
callers of the integrated path, one deprecated diagnostic caller. Estimated
±350 LOC in `src/tds/tds_connection.cpp`, ~40 in its header, ~25 across the two
callers, ~400 in new tests.

## Constitution Check

*GATE: evaluated before Phase 0 and re-evaluated after Phase 1 design. Result:
**PASS** both times, no violations, Complexity Tracking left empty.*

| Principle | Assessment |
|---|---|
| **I. Native and Open** | Pure TDS protocol work in our own client. No ODBC/FreeTDS/MS libraries introduced. PASS |
| **II. Streaming First** | Login path only; no result buffering touched. Not applicable, no violation. PASS |
| **III. Correctness over Convenience** | This is the principle the feature serves. Today a routed login on SQL auth either fails with a wrong message (route-without-LOGINACK) or **silently keeps talking to a gateway that told us to leave** (route-with-LOGINACK) — the second is exactly the "silent, ambiguous behaviour" the principle forbids. Post-change, an unfollowable route aborts with an error naming the hop count and the last routed target verbatim. PASS |
| **IV. Explicit State Machines** | Strengthened. Today the `Authenticating → Idle/Disconnected` transitions are duplicated across three methods and, in `AuthenticateIntegrated`, are interleaved with I/O error handling. The driver becomes the single owner of those transitions and of the per-hop reset set; the attempt callbacks perform no transitions at all (research.md R2). All transitions covered by the fake-server test. PASS |
| **V. DuckDB-Native UX** | No catalog/type surface change. A user ATTACHing an Azure MI or read-intent AlwaysOn endpoint simply stops seeing a bogus login failure. PASS |
| **VI. Incremental Delivery** | Delivered as one coherent connection-level behaviour with no half-state: after this, "which auth paths follow routing" has one answer (all) instead of three. Each layer of D4's test plan is independently runnable. PASS |

Two constitution-adjacent notes carried into the design rather than waived:

1. **Version Baseline** says SQL text goes as UTF-16LE — the routed server name
   arrives UTF-16LE and is already decoded through `ReadUTF16LE`
   (`tds_protocol.cpp:895`). Normalization operates on the decoded UTF-8 string;
   the `:port` / `\instance` scan is byte-wise ASCII and safe on UTF-8
   (research.md R5).
2. **Explicit State Machines** requires cancellation-safety; routing happens
   strictly before the connection is `Idle`, so no ATTENTION interaction exists.
   No change needed, recorded so the next reader does not re-derive it.

## Project Structure

### Documentation (this feature)

```text
specs/068-login-routing-unification/
├── spec.md              # Input (PR #257)
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/
│   └── tds_connection_routing.md   # Phase 1 output — internal C++ contract
└── tasks.md             # Phase 2 output (/speckit-tasks — NOT created here)
```

### Source Code (repository root)

```text
src/
├── include/
│   └── tds/
│       ├── tds_connection.hpp          # +LoginAttemptOutcome, +AuthenticatorFactory,
│       │                               #  +RunWithRoutingHops / NormalizeRoutedTarget decls,
│       │                               #  AuthenticateIntegrated signature change
│       └── auth/iauthenticator.hpp     # unchanged (DuckDB-free boundary preserved)
├── tds/
│   ├── tds_connection.cpp              # the whole change: driver + 3 wrappers +
│   │                                   #  D1 ordering in DoLogin7 / DoLogin7WithFedAuth /
│   │                                   #  integrated response handling
│   └── tds_protocol.cpp                # unchanged (parser is already correct)
├── catalog/mssql_catalog.cpp           # :194 integrated factory closure
├── mssql_storage.cpp                   # :1338 ValidateIntegratedAuthConnection
└── connection/mssql_diagnostic.cpp     # :144 — no edit; inherits routing via Authenticate()

test/
└── cpp/
    ├── test_login_error_state.cpp      # +4 parser pins (D4 layer 1)
    └── test_login_routing_hops.cpp     # NEW — loopback fake TDS server (D4 layer 2)

docs/
├── architecture.md                     # routing as a connection-level behaviour
└── tds-protocol.md                     # ENVCHANGE 20 handling + path table
Kerberos.md                             # SPN-over-hop rules
DATAMODEL.md                            # connection layer: login is now a hop loop
Makefile / .github/workflows/ci.yml     # wire test_login_routing_hops
```

**Structure Decision**: no new directories or modules. The feature is a
refactor plus a bug fix inside one existing translation unit
(`src/tds/tds_connection.cpp`), with a signature change that ripples to exactly
two call sites. Adding a `src/tds/routing/` module was considered and rejected
in research.md R6 — the routed-target normalization is ~40 lines with one
caller, and moving it out of the TU would buy a header for no reuse.

## Complexity Tracking

> No Constitution Check violations. Table intentionally empty.

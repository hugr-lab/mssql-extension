# Implementation Plan: TLS Connection Support

**Branch**: `005-tls-connection-support` | **Date**: 2026-01-16 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/005-tls-connection-support/spec.md`

## Summary

Add optional TLS encryption to the DuckDB MS SQL extension via the `use_encrypt` secret option. When enabled, TLS is negotiated through the TDS PRELOGIN protocol, then the existing TCP socket is wrapped with a TLS layer before LOGIN7 authentication. The implementation uses an embeddable TLS library (mbedTLS) for cross-platform support without system dependencies.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB main branch (extension API), mbedTLS 3.x (TLS library via vcpkg)
**Storage**: In-memory (TLS context per connection)
**Testing**: DuckDB test framework + integration test with TLS-enabled SQL Server (Docker)
**Target Platform**: Linux, macOS, Windows (cross-platform)
**Project Type**: Single project (DuckDB extension)
**Performance Goals**: TLS handshake adds ≤500ms latency; per-query encryption overhead negligible
**Constraints**: Static linking required (no system OpenSSL), trust-server-certificate mode only
**Scale/Scope**: Same as existing connection pool (up to 64 concurrent connections per context)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | ✅ PASS | mbedTLS is open-source (Apache 2.0), no proprietary drivers |
| II. Streaming First | ✅ PASS | TLS is transparent transport layer; streaming unchanged |
| III. Correctness over Convenience | ✅ PASS | No fallback to plaintext on TLS failure (fail securely) |
| IV. Explicit State Machines | ✅ PASS | TLS handshake is explicit step after PRELOGIN, before LOGIN7 |
| V. DuckDB-Native UX | ✅ PASS | `use_encrypt` exposed via standard secret creation syntax |
| VI. Incremental Delivery | ✅ PASS | TLS is opt-in; existing plaintext connections unaffected |

**Gate Result**: PASS - All principles satisfied.

## Project Structure

### Documentation (this feature)

```text
specs/005-tls-connection-support/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (N/A - no API contracts for this feature)
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
src/
├── include/
│   ├── tds/
│   │   ├── tds_socket.hpp          # Existing - add TLS support
│   │   ├── tds_tls_context.hpp     # NEW - TLS context wrapper
│   │   ├── tds_connection.hpp      # Existing - add use_encrypt flow
│   │   └── tds_protocol.hpp        # Existing - parameterize PRELOGIN encryption
│   └── mssql_secret.hpp            # Existing - rename use_ssl to use_encrypt
├── tds/
│   ├── tds_socket.cpp              # Existing - integrate TLS send/receive
│   ├── tds_tls_context.cpp         # NEW - mbedTLS wrapper implementation
│   ├── tds_connection.cpp          # Existing - TLS handshake after PRELOGIN
│   └── tds_protocol.cpp            # Existing - conditional ENCRYPT_ON
└── mssql_secret.cpp                # Existing - rename use_ssl to use_encrypt

tests/
├── integration/
│   └── test_tls_connection.cpp     # NEW - TLS integration tests
└── sql/
    └── tls/
        └── test_tls_basic.test     # NEW - SQL-level TLS tests
```

**Structure Decision**: Extends existing single-project structure. New TLS functionality is encapsulated in `tds_tls_context.hpp/cpp` which wraps mbedTLS. The existing `TdsSocket` class gains TLS capability through composition (optional TLS context).

## Complexity Tracking

> No violations - no tracking required.

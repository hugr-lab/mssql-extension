# Implementation Plan: FEDAUTH Token Provider Enhancements

**Branch**: `032-fedauth-token-provider` | **Date**: 2026-02-06 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/032-fedauth-token-provider/spec.md`

## Summary

Enhance Azure AD authentication to support user-provided access tokens (ACCESS_TOKEN option) and environment-based service principal credentials (credential_chain with `env` chain). Integrates with existing FedAuthStrategy pattern by adding JWT parsing for expiration/audience validation and a new environment-based token acquisition path.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard, but C++11 compatible for ODR)
**Primary Dependencies**: libcurl (OAuth2 HTTP), OpenSSL (TLS), DuckDB Azure extension (secret management)
**Storage**: In-memory token cache (TokenCache singleton)
**Testing**: DuckDB sqllogictest, C++ unit tests (no SQL Server for JWT parsing tests)
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW)
**Project Type**: DuckDB extension (single project)
**Performance Goals**: Token validation < 1ms, connection establishment < 5 seconds
**Constraints**: C++11 compatible (no C++17-only features), no external JWT library dependency
**Scale/Scope**: Single feature addition to existing Azure authentication infrastructure

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | ✅ PASS | No MS libraries; uses existing libcurl/OpenSSL |
| II. Streaming First | ✅ N/A | Authentication, not data streaming |
| III. Correctness over Convenience | ✅ PASS | Token validation with clear error messages |
| IV. Explicit State Machines | ✅ PASS | Extends existing AuthenticationStrategy pattern |
| V. DuckDB-Native UX | ✅ PASS | ACCESS_TOKEN as standard ATTACH option |
| VI. Incremental Delivery | ✅ PASS | P1 (manual token) can ship before P2 (env-based) |

**Gate Result**: PASS - No violations, design aligns with all constitution principles.

## Project Structure

### Documentation (this feature)

```text
specs/032-fedauth-token-provider/
├── spec.md              # Feature specification
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (N/A - no external API)
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
src/
├── azure/
│   ├── azure_token.cpp           # MODIFY: Add env-based token acquisition
│   ├── azure_secret_reader.cpp   # MODIFY: Support ACCESS_TOKEN in secrets
│   └── jwt_parser.cpp            # NEW: Base64 decode + JSON claim extraction
├── include/
│   └── azure/
│       └── jwt_parser.hpp        # NEW: JWT parsing interface
├── mssql_secret.cpp              # MODIFY: Add ACCESS_TOKEN secret option
├── mssql_storage.cpp             # MODIFY: Handle ACCESS_TOKEN ATTACH option
└── tds/auth/
    └── fedauth_strategy.cpp      # MODIFY: Token expiration checks

test/
├── cpp/
│   └── test_jwt_parser.cpp       # NEW: Unit tests for JWT parsing
└── sql/azure/
    ├── azure_access_token.test   # NEW: Manual token auth tests
    └── azure_env_provider.test   # NEW: Environment-based auth tests
```

**Structure Decision**: Single project extension. New files follow existing `src/azure/` and `test/` conventions.

## Complexity Tracking

> No Constitution violations - table not required.

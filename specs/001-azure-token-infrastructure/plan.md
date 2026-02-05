# Implementation Plan: Azure Token Infrastructure (Phase 1)

**Branch**: `001-azure-token-infrastructure` | **Date**: 2026-02-05 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/001-azure-token-infrastructure/spec.md`

## Summary

Implement Azure AD token acquisition infrastructure for the MSSQL extension, enabling users to validate Azure credentials before attempting database connections. This phase establishes the foundation for Phase 2 TDS FEDAUTH integration by adding:

1. Azure secret reading from DuckDB's `TYPE azure` infrastructure
2. Token acquisition via Azure SDK (service principal, CLI, managed identity)
3. Interactive Device Code Flow (RFC 8628) for MFA users
4. Thread-safe token caching with expiration handling
5. `mssql_azure_auth_test()` function for credential validation
6. Extended MSSQL secret validation accepting `azure_secret` parameter

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB (main branch), Azure SDK for C++ (`azure-identity-cpp` via vcpkg), OpenSSL (vcpkg), DuckDB Azure extension (runtime)
**Storage**: In-memory (token cache, no persistence required)
**Testing**: SQLLogicTest (`test/sql/`), C++ unit tests (`test/cpp/`)
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW)
**Project Type**: Single (DuckDB extension)
**Performance Goals**: Token acquisition < 5 seconds, cached token retrieval < 1ms
**Namespace**: `duckdb::mssql::azure` for all Azure authentication code
**Constraints**: Use Azure SDK for non-interactive flows; custom Device Code Flow (RFC 8628) for interactive auth (not supported by SDK)
**Scale/Scope**: Token cache keyed by secret name, ~10-100 concurrent secrets max

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | Uses Azure SDK (MIT license) matching DuckDB Azure; no ODBC/FreeTDS |
| II. Streaming First | N/A | Phase 1 is infrastructure only; no data streaming |
| III. Correctness over Convenience | PASS | Explicit error codes from Azure AD; validation at secret creation |
| IV. Explicit State Machines | PASS | Token cache has explicit valid/expired states |
| V. DuckDB-Native UX | PASS | Uses standard DuckDB secret infrastructure; consistent with DuckDB Azure |
| VI. Incremental Delivery | PASS | Phase 1 delivers testable infrastructure before Phase 2 connections |

**Result**: All gates pass. No violations to justify.

## Project Structure

### Documentation (this feature)

```text
specs/001-azure-token-infrastructure/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (N/A - no external APIs)
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
src/
├── azure/                           # NEW: Azure authentication module
│   ├── azure_token.cpp              # Token acquisition implementation (uses Azure SDK)
│   ├── azure_secret_reader.cpp      # Read Azure secrets from SecretManager
│   ├── azure_test_function.cpp      # mssql_azure_auth_test() function
│   └── azure_device_code.cpp        # NEW: Device Code Flow (RFC 8628)
├── include/azure/                   # NEW: Azure module headers
│   ├── azure_token.hpp              # TokenResult, AcquireToken interface
│   ├── azure_secret_reader.hpp      # AzureSecretInfo struct
│   ├── azure_test_function.hpp      # Function registration
│   └── azure_device_code.hpp        # NEW: Device Code Flow structs
├── mssql_secret.cpp                 # MODIFY: Add azure_secret handling
├── include/mssql_secret.hpp         # MODIFY: Add MSSQL_SECRET_AZURE_SECRET
└── mssql_extension.cpp              # MODIFY: Register test function

test/
├── cpp/                             # C++ unit tests (no SQL Server required)
│   ├── test_azure_token_cache.cpp   # NEW: Token cache thread-safety
│   ├── test_azure_secret_reader.cpp # NEW: Secret validation logic
│   └── test_azure_device_code.cpp   # NEW: Device code parsing, polling tests
├── manual/                          # NEW: Manual test procedures
│   └── azure_device_code_test.md    # Device code flow manual testing
└── sql/
    └── azure/                       # NEW: Azure-specific SQL tests
        ├── azure_secret_validation.test     # MSSQL secret with azure_secret
        ├── azure_auth_test_function.test    # mssql_azure_auth_test() tests
        └── azure_device_code.test           # Device code error handling
```

**Structure Decision**: Single project extension following existing layout. New `src/azure/` directory mirrors established patterns (`src/tds/`, `src/catalog/`, etc.). Tests separated by type: C++ unit tests for logic without SQL Server, SQL tests for integration.

## Post-Design Constitution Re-Check

*GATE: Re-validated after Phase 1 design completion.*

| Principle | Status | Design Verification |
|-----------|--------|---------------------|
| I. Native and Open | PASS | Azure SDK (MIT) for non-interactive; custom Device Code Flow for interactive |
| II. Streaming First | N/A | Infrastructure only |
| III. Correctness over Convenience | PASS | Azure AD error codes preserved; fail-fast validation |
| IV. Explicit State Machines | PASS | CachedToken has explicit IsValid() check |
| V. DuckDB-Native UX | PASS | Uses SecretManager API, consistent with DuckDB Azure extension |
| VI. Incremental Delivery | PASS | Phase 1 testable via mssql_azure_auth_test() |

**Result**: All gates pass post-design. Ready for task generation.

## Complexity Tracking

> No constitution violations requiring justification.

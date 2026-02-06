# Implementation Plan: TDS FEDAUTH Integration (Phase 2)

**Branch**: `001-tds-fedauth-integration` | **Date**: 2026-02-05 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/001-tds-fedauth-integration/spec.md`

## Summary

Integrate Azure AD tokens from Phase 1 into the TDS authentication flow using the FEDAUTH protocol extension. This enables actual database connections to Azure SQL Database and Microsoft Fabric Warehouse. Key technical changes:

1. Extend PRELOGIN with FEDAUTHREQUIRED option (0x06) for Azure endpoints
2. Add FEDAUTH feature extension to LOGIN7 packet with UTF-16LE encoded token
3. Implement TLS hostname verification for Azure endpoints
4. Handle FEDAUTHINFO server response
5. Maintain backward compatibility with SQL authentication

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (vcpkg), libcurl (vcpkg for Phase 1 OAuth2)
**Storage**: In-memory (connection pool state, token cache)
**Testing**: DuckDB unittest framework (SQLLogicTest + C++ unit tests)
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW/Rtools 4.2)
**Project Type**: Single project (DuckDB extension)
**Performance Goals**: Connection establishment <5s, catalog queries <5s, zero memory leaks
**Constraints**: No external TDS libraries (native implementation), OpenSSL for TLS only
**Scale/Scope**: Azure SQL Database, Microsoft Fabric Warehouse, existing SQL Server support unchanged

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Evidence |
| --------- | ------ | -------- |
| **I. Native and Open** | PASS | Native TDS implementation, no ODBC/JDBC, no Microsoft libraries |
| **II. Streaming First** | PASS | No change to streaming architecture, FEDAUTH only affects connection |
| **III. Correctness over Convenience** | PASS | Clear error messages required, explicit auth flow branching |
| **IV. Explicit State Machines** | PASS | Auth state is explicit (detect azure_secret → acquire token → FEDAUTH vs SQL auth) |
| **V. DuckDB-Native UX** | PASS | Uses existing MSSQL secret infrastructure, same ATTACH syntax |
| **VI. Incremental Delivery** | PASS | Phase 2 builds on Phase 1 token infrastructure, testable independently |

**Row Identity Model**: No impact (FEDAUTH only affects authentication, not row operations)

**Version Baseline**: Azure SQL uses TDS 7.4+, compatible with existing implementation

## Project Structure

### Documentation (this feature)

```text
specs/001-tds-fedauth-integration/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (N/A - internal protocol)
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
src/
├── azure/
│   ├── azure_token.cpp           # [EXISTS] Token acquisition (Phase 1)
│   ├── azure_secret_reader.cpp   # [EXISTS] Read Azure secrets (Phase 1)
│   ├── azure_device_code.cpp     # [EXISTS] Device code flow (Phase 1)
│   ├── azure_test_function.cpp   # [EXISTS] mssql_azure_auth_test() (Phase 1)
│   └── azure_fedauth.cpp         # [NEW] FEDAUTH token encoding
├── tds/
│   ├── tds_protocol.cpp          # [MODIFY] Add FEDAUTHREQUIRED to PRELOGIN, FEDAUTH to LOGIN7
│   ├── tds_connection.cpp        # [MODIFY] Auth flow branching
│   ├── tds_token_parser.cpp      # [MODIFY] Handle FEDAUTHINFO response
│   └── tls/
│       ├── tds_tls_impl.cpp      # [EXISTS] TLS implementation
│       └── tds_tls_context.cpp   # [MODIFY] Add hostname verification for Azure
├── connection/
│   └── mssql_connection_provider.cpp  # [MODIFY] Read azure_secret from MSSQL secret
├── include/
│   ├── azure/
│   │   └── azure_fedauth.hpp     # [NEW] FEDAUTH data structures
│   ├── tds/
│   │   ├── tds_types.hpp         # [MODIFY] Add FEDAUTH constants
│   │   └── tds_connection.hpp    # [MODIFY] ConnectionParams with azure_auth fields
│   └── mssql_platform.hpp        # [NEW] IsAzureEndpoint(), IsFabricEndpoint() helpers

test/
├── cpp/
│   ├── test_fedauth_encoding.cpp     # [NEW] Unit test for FEDAUTH packet encoding
│   ├── test_hostname_verification.cpp # [NEW] Unit test for TLS hostname matching
│   └── test_mssql_secret_azure.cpp   # [NEW] Unit test for azure_secret validation
└── sql/
    └── azure/                        # [NEW] Azure integration tests
        ├── azure_service_principal.test
        ├── azure_cli_auth.test
        ├── azure_error_handling.test
        ├── azure_catalog.test
        ├── azure_dml.test
        ├── azure_copy.test
        └── fabric_warehouse.test
```

**Structure Decision**: Existing extension structure preserved. New files added to `src/azure/` and `src/include/azure/` for FEDAUTH encoding. TDS protocol files modified in-place. New test files in `test/cpp/` and `test/sql/azure/`.

## Complexity Tracking

> No violations - Constitution Check passed without exceptions.

## Implementation Phases

### Phase 0: Research (Complete)

See [research.md](./research.md) for:

- TDS FEDAUTH protocol details from MS-TDS specification
- PRELOGIN FEDAUTHREQUIRED option format
- LOGIN7 FEDAUTH feature extension format
- TLS hostname verification patterns for OpenSSL
- Token encoding (UTF-16LE) requirements

### Phase 1: Design

#### Data Model

See [data-model.md](./data-model.md) for:

- `FedAuthData` structure (library type, token bytes)
- `ConnectionParams` extensions (use_azure_auth, azure_secret_name)
- `EndpointType` enumeration (AzureSQL, Fabric, OnPremises)

#### Contracts

N/A - This feature implements an internal protocol (TDS FEDAUTH), not an external API. The "contract" is the TDS protocol specification from Microsoft, not a service contract we define.

#### Quick Start

See [quickstart.md](./quickstart.md) for developer setup and verification steps.

### Phase 2: Tasks

Generated by `/speckit.tasks` command after plan approval.

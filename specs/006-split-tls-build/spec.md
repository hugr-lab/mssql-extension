# Feature Specification: Split TLS Build Configuration

**Feature Branch**: `006-split-tls-build`
**Created**: 2026-01-16
**Status**: Implemented
**Input**: Resolve mbedTLS duplicate symbol conflicts between DuckDB's bundled mbedTLS and vcpkg's full mbedTLS

## Problem Statement

The MSSQL extension requires full TLS support (SSL/TLS handshake, certificate handling) via mbedTLS. However, DuckDB bundles a **crypto-only** version of mbedTLS that lacks TLS headers (`ssl.h`, `net_sockets.h`, etc.). When linking vcpkg's full mbedTLS for TLS functionality, symbol conflicts occur with DuckDB's bundled crypto symbols.

### Investigation Summary

We investigated multiple approaches to resolve the symbol conflicts:

1. **Use DuckDB's bundled mbedTLS** - Failed: DuckDB's mbedTLS is crypto-only, missing required TLS headers (`mbedtls/ssl.h`, `mbedtls/net_sockets.h`)

2. **Remove `-force_load` linker flag** - Failed: Still resulted in 230+ duplicate symbol errors on macOS

3. **Link only mbedtls/mbedx509 without mbedcrypto** - Failed: mbedTLS 3.x TLS 1.3 implementation uses PSA Crypto API, which requires mbedcrypto. Resulted in undefined PSA symbols.

4. **Build mbedTLS from source with symbol prefix (MBEDTLS_SYMBOL_PREFIX)** - Investigated but not viable: mbedTLS doesn't have built-in symbol prefixing support comparable to other libraries.

5. **macOS `--allow-multiple-definition`** - Not available: macOS linker doesn't support this flag (Linux-only).

### Final Solution

Since no solution could provide TLS in the static build without symbol conflicts, we implemented a **split build configuration**:

- **Static extension**: TLS stub implementation that returns "TLS not available - use loadable extension"
- **Loadable extension**: Full TLS with vcpkg mbedTLS, all symbols hidden via visibility controls

This approach completely avoids the symbol conflict in the static build by not linking mbedTLS at all.

## Implementation

### TLS Library Targets

Two TLS library targets are built in `src/tls/CMakeLists.txt`:

| Target | Type | Implementation | mbedTLS Dependency |
|--------|------|----------------|-------------------|
| `mssql_tls_static` | OBJECT | Stub (`tds_tls_stub.cpp`) | None |
| `mssql_tls_loadable` | STATIC | Full TLS (`tds_tls_impl.cpp`, `tds_tls_context.cpp`) | vcpkg mbedTLS |

### Static Extension (TLS Stub)

The static extension uses `mssql_tls_static` which provides stub implementations:

- `TlsImpl::Initialize()` returns `false` with error "TLS not available in static build"
- `TlsTdsContext::Initialize()` returns `false` with `TlsErrorCode::TLS_NOT_AVAILABLE`
- All other TLS methods return error states

Users who need encrypted connections must use the loadable extension.

### Loadable Extension (Full TLS)

The loadable extension uses `mssql_tls_loadable` with full mbedTLS:

- Links `MbedTLS::mbedtls`, `MbedTLS::mbedx509`, `MbedTLS::mbedcrypto` (PRIVATE)
- Symbol visibility set to hidden (`-fvisibility=hidden`)
- Only exports `mssql_duckdb_cpp_init` via platform-specific mechanisms:
  - macOS: `-exported_symbols_list`
  - Linux: `--version-script`
  - Windows: `.def` file

### Files Changed

- `src/tls/tds_tls_stub.cpp` - New stub implementation
- `src/tls/CMakeLists.txt` - Two-target build configuration
- `CMakeLists.txt` - Static uses stub, loadable uses full TLS
- `Makefile` - `MSSQL_TEST_DSN_TLS` not exported (TLS tests skipped)
- `test/TLS_TESTING.md` - Documentation for TLS testing with loadable extension

## User Scenarios

### User Story 1 - Static Extension Without TLS (Priority: P1)

As a developer using the static extension, I want clear feedback that TLS is not available so that I know to use the loadable extension for encrypted connections.

**Acceptance Scenarios**:

1. **Given** a static extension build, **When** attempting a TLS connection with `?encrypt=true`, **Then** an error message states "TLS not available in static build - use loadable extension"
2. **Given** a static extension build, **When** building the extension, **Then** no mbedTLS libraries are linked and no duplicate symbol errors occur
3. **Given** a static extension build, **When** running `make integration-test`, **Then** TLS tests are skipped with message "require-env MSSQL_TEST_DSN_TLS"

### User Story 2 - Loadable Extension With Full TLS (Priority: P1)

As a developer using the loadable extension, I want TLS connections to work without symbol conflicts so that I can connect to SQL Server with encryption.

**Acceptance Scenarios**:

1. **Given** a loadable extension, **When** loaded into standalone DuckDB with `-unsigned`, **Then** the extension loads without symbol conflicts
2. **Given** a loadable extension, **When** connecting with `?encrypt=true`, **Then** TLS handshake completes and queries execute successfully
3. **Given** a loadable extension, **When** examining exported symbols, **Then** only `mssql_duckdb_cpp_init` is exported

### User Story 3 - Clear Documentation (Priority: P2)

As a developer, I want clear documentation on TLS testing so that I can verify TLS functionality with the loadable extension.

**Acceptance Scenarios**:

1. **Given** `test/TLS_TESTING.md`, **When** following the guide, **Then** I can install matching DuckDB, build the extension, and verify TLS connections
2. **Given** `test/README.md`, **When** reading about TLS, **Then** I am directed to `TLS_TESTING.md` for detailed instructions

## Requirements

### Functional Requirements

- **FR-001**: Static extension MUST NOT link any mbedTLS libraries
- **FR-002**: Static extension MUST provide stub TLS implementation returning "not available" errors
- **FR-003**: Loadable extension MUST link vcpkg mbedTLS with PRIVATE visibility
- **FR-004**: Loadable extension MUST hide all symbols except `mssql_duckdb_cpp_init`
- **FR-005**: TLS tests MUST be skipped by default in `make integration-test`
- **FR-006**: Documentation MUST explain how to test TLS with the loadable extension

### Non-Functional Requirements

- **NFR-001**: Build MUST succeed on macOS, Linux, and Windows without symbol conflicts
- **NFR-002**: Error messages MUST clearly indicate the solution (use loadable extension)

## Success Criteria

- **SC-001**: Static extension builds with zero mbedTLS-related linker warnings/errors
- **SC-002**: Static extension returns clear error when TLS is attempted
- **SC-003**: Loadable extension TLS connections work (verified manually)
- **SC-004**: `make integration-test` passes (7 tests pass, 3 TLS tests skipped)
- **SC-005**: `make test` passes (6 unit tests)
- **SC-006**: `[query]` tests pass (5 tests)
- **SC-007**: `[tds_connection]` tests pass (4 tests)

## Test Results

```
[sql] unit tests:        6/6 passed (113 assertions)
[integration] tests:     7/10 passed, 3 skipped (TLS tests)
[query] tests:           5/5 passed (39 assertions)
[tds_connection] tests:  4/4 passed (32 assertions)
```

TLS functionality verified manually with loadable extension:
- TLSv1.2 handshake successful
- Cipher: TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256
- Queries execute correctly over encrypted connection

## Limitations

1. **No TLS in static extension**: Users requiring encryption must use the loadable extension
2. **Manual TLS testing**: TLS tests cannot run in the standard test runner (requires loadable extension)
3. **Version matching**: Loadable extension must match the DuckDB CLI version exactly

## See Also

- `test/TLS_TESTING.md` - How to test TLS with the loadable extension
- `specs/005-tls-connection-support/` - Original TLS implementation spec

# Feature Specification: Windows CI Build Support

**Feature Branch**: `016-windows-ci-build`
**Created**: 2026-01-21
**Status**: Implemented
**Input**: User description: "Fix Windows build errors in community-extensions CI and add Windows CI workflow to test builds locally before community-extensions submission."

## Problem Statement

The DuckDB community-extensions CI workflow fails when building the MSSQL extension on Windows. The build encounters C++ compilation errors including "unknown override specifier" and syntax errors, indicating platform compatibility issues with the source code.

**Relevant Links**:
- Failed workflow: https://github.com/duckdb/community-extensions/actions/runs/21186683029/job/60988594969?pr=1104
- PR: https://github.com/duckdb/community-extensions/pull/1104

**Root Cause Analysis**:
The errors are likely caused by:
1. **Missing `ssize_t` type definition**: Windows MSVC does not define `ssize_t` (a POSIX type). The code uses `ssize_t` in multiple header files without Windows-specific handling.
2. **Potential socket type mismatches**: Windows uses `SOCKET` type (typedef for `UINT_PTR`) while the code uses `int fd_`.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Windows Build in Community Extensions CI (Priority: P1)

As an extension maintainer, I want the MSSQL extension to build successfully on Windows in the community-extensions CI, so that the extension can be published and available to Windows users.

**Why this priority**: This is the primary goal - without passing Windows builds, the extension cannot be fully published.

**Independent Test**: Submit the PR to community-extensions and verify the Windows build job completes successfully.

**Acceptance Scenarios**:

1. **Given** the MSSQL extension source code, **When** the community-extensions CI runs the Windows build job (MSVC), **Then** the compilation completes without errors.

2. **Given** the Windows-built extension, **When** the CI runs smoke tests, **Then** the extension loads successfully in DuckDB.

---

### User Story 2 - Local Windows Build Testing (Priority: P2)

As a developer, I want to test Windows builds locally in our CI workflow before submitting to community-extensions, so that I can catch Windows-specific issues early.

**Why this priority**: Enables faster iteration and prevents failed PRs to community-extensions.

**Independent Test**: Trigger the local CI workflow with Windows build enabled and verify it completes.

**Acceptance Scenarios**:

1. **Given** the local CI workflow is configured for Windows, **When** a developer triggers a manual workflow run, **Then** the Windows build executes using the same toolchain as community-extensions (MSVC/VS2022).

2. **Given** the Windows CI job completes, **When** checking the artifacts, **Then** a Windows-compatible extension binary is produced.

---

### User Story 3 - Build Parity Verification (Priority: P3)

As a developer, I want to verify that our Windows build configuration matches the community-extensions CI configuration, so that local builds accurately predict community CI results.

**Why this priority**: Ensures consistency between local and community CI environments.

**Independent Test**: Compare build configurations and verify they use matching compiler settings.

**Acceptance Scenarios**:

1. **Given** our CI uses MSVC/VS2022, **When** comparing to community-extensions CI, **Then** the compiler version and settings match.

2. **Given** both CI environments build the extension, **When** comparing build outputs, **Then** no unexpected differences exist in warnings or behaviors.

---

### Edge Cases

- What happens if OpenSSL fails to build on Windows?
  - The vcpkg manifest should handle Windows-compatible OpenSSL builds; any issues should produce clear error messages.

- How does the system handle different Visual Studio versions?
  - The CI should specify VS2022 explicitly to match community-extensions; older versions are not supported.

- What if socket functions behave differently on Windows?
  - Windows-specific socket handling code already exists; compilation issues should be fixed by proper type definitions.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The source code MUST compile without errors on Windows using MSVC (Visual Studio 2022).

- **FR-002**: The `ssize_t` type MUST be defined for Windows builds (typically as `intptr_t` or `SSIZE_T`).

- **FR-003**: Socket-related types MUST use appropriate Windows types (`SOCKET` instead of `int` for socket handles where necessary, or proper compatibility handling).

- **FR-004**: The local CI workflow MUST include Windows build jobs that match the community-extensions CI configuration (both MSVC and MinGW).

- **FR-005**: The Windows MSVC build job MUST use the same vcpkg triplet as community-extensions (`x64-windows-static-release`).

- **FR-006**: The Windows MinGW build job MUST use the same vcpkg triplet as community-extensions (`x64-mingw-static`).

- **FR-007**: The Windows CI jobs MUST cache vcpkg packages for faster subsequent builds.

- **FR-008**: Both MSVC and MinGW builds MUST produce working `.duckdb_extension` files that can be loaded on Windows.

- **FR-009**: The Windows CI jobs MUST be manually triggerable for PRs via workflow_dispatch.

### Configuration Approach

Based on research of the community-extensions CI infrastructure:

1. **Community-extensions uses TWO Windows toolchains**:
   - **MSVC (VS2022)**: Primary Windows build using Visual Studio 2022 Enterprise
   - **MinGW (Rtools 4.2)**: Secondary Windows build using GCC-based MinGW toolchain
2. **vcpkg triplets**:
   - MSVC: `x64-windows-static-release`
   - MinGW: `x64-mingw-static`
3. **No Docker**: Windows builds run natively on `windows-latest` runner.

### Key Entities

- **tds_socket.hpp/cpp**: Contains socket code with `ssize_t` usage and `fd_` member.
- **tds_tls_impl.hpp/cpp**: Contains TLS implementation with `ssize_t` return types.
- **tds_connection.hpp/cpp**: Contains `ReceiveData` method with `ssize_t` return type.
- **.github/workflows/ci.yml**: Local CI workflow to be extended with Windows build job.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The community-extensions CI Windows build job completes successfully (0 compilation errors).

- **SC-002**: Local CI Windows build job completes successfully when triggered manually.

- **SC-003**: The produced Windows extension can be loaded in DuckDB and basic functions work (version check passes).

- **SC-004**: Build time for Windows is comparable to Linux builds (within 2x duration).

## Assumptions

- The community-extensions CI uses Visual Studio 2022 Enterprise on `windows-latest` runner.
- The vcpkg manifest (`vcpkg.json`) already supports Windows-compatible package builds.
- The `ssize_t` issue is the primary cause of the "unknown override specifier" errors (MSVC parses code differently when types are undefined).
- Existing Windows-specific socket code (`#ifdef _WIN32` blocks) is mostly correct but incomplete.

## Out of Scope

- ARM64 Windows builds (already excluded in `description.yml`).
- Setting up SQL Server integration tests on Windows CI (load-only smoke tests are sufficient).
- Windows installer or distribution packaging.

# Implementation Plan: Windows CI Build Support

**Branch**: `016-windows-ci-build` | **Date**: 2026-01-21 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/016-windows-ci-build/spec.md`

## Summary

Fix Windows build errors in the DuckDB community-extensions CI by adding missing `ssize_t` type definition for MSVC, and add local Windows CI testing capability with both MSVC and MinGW build configurations to match community-extensions infrastructure.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB main branch (extension API), OpenSSL (via vcpkg), WinSock2 (Windows)
**Storage**: N/A (build/CI configuration only)
**Testing**: Smoke test (extension load verification)
**Target Platform**: Windows 10+ (x64), via MSVC and MinGW compilers
**Project Type**: DuckDB extension
**Performance Goals**: N/A (build configuration)
**Constraints**: Must match community-extensions CI toolchain exactly
**Scale/Scope**: 4 header files to modify, 1 workflow file to extend

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | No new dependencies; uses standard Windows headers |
| II. Streaming First | N/A | Build configuration only |
| III. Correctness over Convenience | PASS | Fixes compilation errors properly |
| IV. Explicit State Machines | N/A | Build configuration only |
| V. DuckDB-Native UX | N/A | Build configuration only |
| VI. Incremental Delivery | PASS | Source fix first, then CI workflow |

**Result**: All applicable gates pass. No violations to justify.

## Project Structure

### Documentation (this feature)

```text
specs/016-windows-ci-build/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── quickstart.md        # Phase 1 output
└── tasks.md             # Phase 2 output (created by /speckit.tasks)
```

### Source Code Changes

```text
src/
└── include/
    └── tds/
        ├── tds_platform.hpp     # NEW: Windows platform compatibility
        ├── tds_socket.hpp       # MODIFY: Include tds_platform.hpp
        ├── tds_connection.hpp   # MODIFY: Include tds_platform.hpp
        └── tls/
            ├── tds_tls_context.hpp  # MODIFY: Include tds_platform.hpp
            └── tds_tls_impl.hpp     # MODIFY: Include tds_platform.hpp

.github/
└── workflows/
    └── ci.yml               # MODIFY: Add Windows build jobs
```

**Structure Decision**: Minimal changes - add one new header file for platform compatibility, modify existing headers to include it, and extend CI workflow.

## Implementation Approach

### Phase 1: Source Code Fix

1. **Create `tds_platform.hpp`** with Windows-specific `ssize_t` typedef
2. **Include platform header** in all files using `ssize_t`:
   - `tds_socket.hpp`
   - `tds_connection.hpp`
   - `tds_tls_context.hpp`
   - `tds_tls_impl.hpp`

### Phase 2: CI Workflow Extension

1. **Add Windows MSVC build job**:
   - Runner: `windows-latest`
   - Triplet: `x64-windows-static-release`
   - Compiler setup: VS2022 vcvars64.bat

2. **Add Windows MinGW build job**:
   - Runner: `windows-latest`
   - Triplet: `x64-mingw-static`
   - Compiler setup: Rtools 4.2 (via r-lib/actions/setup-r)

3. **Configure caching** for vcpkg packages per triplet

4. **Add smoke test** to verify extension loads

### Phase 3: Verification

1. Push to branch and trigger local CI
2. Verify both MSVC and MinGW builds pass
3. Create new tag with fixes
4. Update community-extensions PR
5. Verify community-extensions CI passes

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Additional Windows-specific issues beyond ssize_t | Medium | Medium | MinGW uses GCC which may catch different issues |
| vcpkg package incompatibility on Windows | Low | High | Using same triplets as community-extensions |
| Rtools setup complexity | Medium | Low | Following exact community-extensions workflow |

## Complexity Tracking

No constitution violations to justify.

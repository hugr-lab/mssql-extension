# Implementation Plan: Split TLS Build Configuration

**Branch**: `006-split-tls-build` | **Date**: 2026-01-16 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/006-split-tls-build/spec.md`

## Summary

Resolve mbedTLS duplicate symbol conflicts by splitting the TLS library build into two variants: one for static extension linking and one for loadable extension linking. This eliminates the unsafe `-force_load` and `--allow-multiple-definition` linker workarounds that cause crashes in Debug builds.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB (main branch), mbedTLS (vcpkg 3.6.4)
**Storage**: N/A (build system only)
**Testing**: CMake build verification, symbol inspection (nm/objdump), runtime TLS connection tests
**Target Platform**: macOS, Linux, Windows
**Project Type**: Single project (DuckDB extension)
**Performance Goals**: N/A (build-time change, no runtime performance impact)
**Constraints**: Single TLS codebase compiled twice with different link strategies
**Scale/Scope**: ~4 CMake files modified, ~1 new .def file, 0 runtime code changes expected

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | No change - still uses native TDS with mbedTLS |
| II. Streaming First | PASS | No change - build system only |
| III. Correctness over Convenience | PASS | Eliminates undefined behavior from duplicate symbols |
| IV. Explicit State Machines | PASS | No change to TLS state machine |
| V. DuckDB-Native UX | PASS | No change to user-facing behavior |
| VI. Incremental Delivery | PASS | Build fix independent of feature work |

**Gate Result**: PASS - No violations, no justification needed.

## Project Structure

### Documentation (this feature)

```text
specs/006-split-tls-build/
├── plan.md              # This file
├── research.md          # Phase 0 output - critical finding about DuckDB mbedTLS
├── data-model.md        # Phase 1 output - CMake target entity model
├── quickstart.md        # Phase 1 output - build verification guide
├── contracts/           # Phase 1 output - N/A for build system change
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
# Existing structure - files to modify
CMakeLists.txt                    # Root build config (modify)
src/tls/
├── CMakeLists.txt                # TLS build config (major rewrite)
├── tds_tls_impl.cpp              # TLS implementation (no change expected)
├── tds_tls_impl.hpp              # TLS interface (no change expected)
├── tds_tls_context.cpp           # TLS context wrapper (no change expected)
├── tds_tls_context.hpp           # TLS context interface (no change expected)
├── mbedtls_compat.h              # Existing compat header (may extend)
└── mbedtls_prefix.h              # Existing prefix header (unused)

# DuckDB bundled mbedTLS (read-only reference)
duckdb/third_party/mbedtls/
├── include/mbedtls/              # Crypto-only headers (NO SSL/TLS)
└── library/                      # Sources (not directly used)

# Platform-specific symbol hiding
# macOS: extension_exports.txt (generated, existing)
# Linux: extension_version.map (generated, existing)
# Windows: mssql_extension.def (new file needed)
```

**Structure Decision**: Single project, modify existing src/tls/ directory. Add Windows .def file for symbol export control.

## Complexity Tracking

> No violations to justify - Constitution Check passed.

## Phase 0 Research Summary

See [research.md](./research.md) for full details.

### Critical Finding

DuckDB's bundled mbedTLS (`duckdb/third_party/mbedtls/`) is a **crypto-only subset** that lacks SSL/TLS headers (`ssl.h`, `ctr_drbg.h`, etc.). The original spec assumption that static builds could use DuckDB's bundled mbedTLS is **not feasible**.

### Revised Approach

Both static and loadable extension targets must use **vcpkg mbedTLS**. The conflict mitigation strategy:

1. **Static extension**: Remove `-force_load` and `--allow-multiple-definition`. Let natural linking resolve symbols - SSL APIs come only from vcpkg (no DuckDB conflict), crypto APIs may use DuckDB's where loaded first.

2. **Loadable extension**: Continue current approach with symbol hiding via exported_symbols_list/version script/.def file.

### Research Conclusions

| Topic | Decision |
|-------|----------|
| DuckDB bundled mbedTLS | Cannot use - lacks SSL/TLS APIs |
| Both targets mbedTLS source | vcpkg |
| Static conflict mitigation | Remove force-load, rely on natural linking |
| Loadable symbol hiding | Keep current approach, add Windows .def |
| Compatibility layer | Existing mbedtls_compat.h is sufficient |

## Phase 1 Design Artifacts

- [data-model.md](./data-model.md) - CMake target entity model
- [quickstart.md](./quickstart.md) - Build verification guide
- [contracts/](./contracts/) - Not applicable (build system change)

## Implementation Approach

### CMake Changes Required

1. **src/tls/CMakeLists.txt**: Split into two targets
   - `mssql_tls_static` (OBJECT library) - for static extension
   - `mssql_tls_loadable` (STATIC library) - for loadable extension
   - Both compile same sources with different definitions

2. **Root CMakeLists.txt**:
   - Static extension: Link TLS objects, remove force-load flags
   - Loadable extension: Link TLS static library
   - Add Windows .def file generation

3. **New file**: Windows module definition file for symbol export

### Key Decisions

| Decision | Rationale |
|----------|-----------|
| OBJECT library for static | Flexibility in linking, no archive overhead |
| STATIC library for loadable | Full encapsulation with transitive mbedTLS |
| Remove force-load flags | Root cause of crashes - SSL symbols don't conflict |
| Keep vcpkg for both | DuckDB bundled lacks required SSL/TLS APIs |

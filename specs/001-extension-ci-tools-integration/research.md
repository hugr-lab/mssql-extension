# Research: DuckDB Extension CI Tools Integration

**Date**: 2026-01-20
**Feature**: 001-extension-ci-tools-integration

## Research Summary

This document captures research findings for integrating `duckdb/extension-ci-tools` with the mssql-extension repository.

---

## 1. Extension CI Tools Repository Structure

**Source**: https://github.com/duckdb/extension-ci-tools

### Key Files

| Path | Purpose |
|------|---------|
| `makefiles/duckdb_extension.Makefile` | Standard Makefile include for all extensions |
| `scripts/` | Build and utility scripts |

### Provided Makefile Targets

| Target | Description |
|--------|-------------|
| `release` | Build extension in Release mode |
| `debug` | Build extension in Debug mode |
| `test` | Run tests (delegates to `test_release`) |
| `test_release` | Run tests against release build |
| `test_debug` | Run tests against debug build |
| `set_duckdb_version` | Checkout specific DuckDB version |
| `clean` | Remove build artifacts |
| `format` | Run code formatting |
| `tidy-check` | Run clang-tidy |

### Required Variables

| Variable | Required | Description |
|----------|----------|-------------|
| `EXT_NAME` | Yes | Extension name (e.g., `mssql`) |
| `EXT_CONFIG` | Yes | Path to extension_config.cmake |
| `EXT_FLAGS` | No | Additional CMake flags |
| `EXT_RELEASE_FLAGS` | No | Release-specific CMake flags |
| `EXT_DEBUG_FLAGS` | No | Debug-specific CMake flags |

---

## 2. set_duckdb_version Target

### Implementation

```makefile
set_duckdb_version:
	cd duckdb && git checkout $(DUCKDB_GIT_VERSION)
```

### Requirements

- `duckdb/` directory must be a git submodule (not a clone)
- Submodule must be initialized before target execution
- `DUCKDB_GIT_VERSION` environment variable must be set

### Usage by Community CI

```bash
DUCKDB_GIT_VERSION=v1.4.3 make set_duckdb_version
```

---

## 3. Community Extension Examples

### Spatial Extension (duckdb/duckdb_spatial)

**Makefile pattern**:
```makefile
PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
EXT_NAME=spatial
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Custom targets below include line
format:
	find src/spatial -iname *.hpp -o -iname *.cpp | xargs clang-format -i
```

**Observations**:
- Uses hybrid approach (include + custom targets)
- Overrides `format` target for custom source layout
- Preserves all CI-required targets from include

### HTTPfs Extension (duckdb/duckdb_httpfs)

**Makefile pattern**:
```makefile
PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
EXT_NAME=httpfs
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

include extension-ci-tools/makefiles/duckdb_extension.Makefile
```

**Observations**:
- Minimal Makefile (8 lines)
- No custom targets needed
- Pure standard tooling

---

## 4. extension_config.cmake Format

### Standard Format

```cmake
duckdb_extension_load(<extension_name>
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    [INCLUDE_DIR <path>]
    [LOAD_TESTS]
)
```

### Current mssql-extension Format

```cmake
duckdb_extension_load(mssql
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
```

**Assessment**: Already compatible. No changes needed.

---

## 5. vcpkg Integration

### Challenge

The mssql-extension uses vcpkg for OpenSSL (TLS support). The extension-ci-tools Makefile has its own vcpkg handling via `VCPKG_TOOLCHAIN_PATH`.

### Solution

Pass vcpkg configuration via `EXT_FLAGS`:

```makefile
VCPKG_DIR := $(PROJ_DIR)vcpkg
VCPKG_TOOLCHAIN := $(VCPKG_DIR)/scripts/buildsystems/vcpkg.cmake

ifneq ($(wildcard $(VCPKG_TOOLCHAIN)),)
    EXT_FLAGS := -DCMAKE_TOOLCHAIN_FILE="$(VCPKG_TOOLCHAIN)" -DVCPKG_MANIFEST_DIR="$(PROJ_DIR)"
endif
```

### Rationale

- `EXT_FLAGS` is passed to all CMake invocations by the included Makefile
- Conditional check allows CI to work without vcpkg (TLS stub used in CI)
- Local builds can bootstrap vcpkg for full TLS support

---

## 6. Test Behavior in CI

### Challenge

Community CI doesn't have SQL Server available. Integration tests must be skipped.

### Current Behavior

The existing test infrastructure uses Catch2 tags:
- `[sql]` - Unit tests (no SQL Server required)
- `[integration]` - Integration tests (SQL Server required)

### Solution

The extension-ci-tools `test` target runs the DuckDB test runner. Tests that fail to connect to SQL Server will fail gracefully or be tagged to skip.

**Verification needed**: Ensure `[integration]` tests are not run by default `test` target.

---

## 7. Version Compatibility

### Supported Branches

| Branch | DuckDB Version | Status |
|--------|---------------|--------|
| `main` | main | Active |
| `v1.4.3` | v1.4.3 | Active |
| `v1.4.2` | v1.4.2 | Deprecated |

### Recommendation

Use `main` branch of extension-ci-tools for development, align with DuckDB version for releases.

---

## 8. Gotchas and Considerations

1. **Submodule initialization order**: `duckdb` and `extension-ci-tools` submodules must be initialized before any make target
2. **Variable scope**: Variables defined after `include` don't affect included Makefile behavior
3. **Target override**: Custom targets with same name as included targets will override them
4. **Path handling**: Use `$(PROJ_DIR)` prefix for all paths to ensure portability
5. **CI environment**: No vcpkg, no SQL Server, minimal dependencies

---

## 9. References

- [duckdb/extension-ci-tools](https://github.com/duckdb/extension-ci-tools)
- [duckdb/duckdb_spatial](https://github.com/duckdb/duckdb_spatial)
- [duckdb/extension-template](https://github.com/duckdb/extension-template)
- [DuckDB Extension Development](https://duckdb.org/docs/extensions/development)

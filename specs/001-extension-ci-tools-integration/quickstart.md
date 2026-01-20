# Quickstart: Extension CI Tools Integration

**Feature**: 001-extension-ci-tools-integration

## Overview

This guide covers implementing the DuckDB extension-ci-tools integration for the mssql-extension.

## Prerequisites

- Git with submodule support
- GNU Make
- CMake 3.20+
- C++17 compiler (clang or gcc)

## Implementation Steps

### Step 1: Add extension-ci-tools Submodule

```bash
cd /path/to/mssql-extension
git submodule add https://github.com/duckdb/extension-ci-tools.git extension-ci-tools
git submodule update --init --recursive
```

### Step 2: Update .gitmodules

Verify `.gitmodules` contains:

```ini
[submodule "duckdb"]
	path = duckdb
	url = https://github.com/duckdb/duckdb.git
	branch = main

[submodule "extension-ci-tools"]
	path = extension-ci-tools
	url = https://github.com/duckdb/extension-ci-tools.git
```

### Step 3: Replace Makefile

Replace the existing Makefile with the hybrid version from `plan.md` that:
- Includes extension-ci-tools Makefile first
- Adds custom targets after (Docker, integration tests, vcpkg)

### Step 4: Verify extension_config.cmake

Current file should already be compatible:

```cmake
duckdb_extension_load(mssql
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
```

### Step 5: Update README.md

Add the "Building with DuckDB Extension CI Tools" section from `plan.md`.

## Verification

### Test 1: set_duckdb_version

```bash
DUCKDB_GIT_VERSION=v1.4.3 make set_duckdb_version
# Expected: DuckDB submodule checked out to v1.4.3
```

### Test 2: Release Build

```bash
make vcpkg-setup  # First time only
make release
# Expected: Extension built at build/release/extension/mssql/mssql.duckdb_extension
```

### Test 3: Test Target

```bash
make test
# Expected: Unit tests pass, integration tests skipped (no SQL Server)
```

### Test 4: Extension Load

```bash
./build/release/duckdb -c "LOAD mssql; SELECT 1;"
# Expected: Query executes successfully
```

### Test 5: Custom Targets

```bash
make help
# Expected: Shows both CI and custom targets

make docker-status
# Expected: Reports container status (running or not)
```

## Troubleshooting

### "No rule to make target 'set_duckdb_version'"

**Cause**: extension-ci-tools submodule not initialized

**Fix**:
```bash
git submodule update --init --recursive
```

### "Cannot find vcpkg toolchain"

**Cause**: vcpkg not bootstrapped

**Fix**:
```bash
make vcpkg-setup
```

### Build fails with OpenSSL errors

**Cause**: vcpkg dependencies not installed

**Fix**:
```bash
rm -rf build vcpkg
make vcpkg-setup
make release
```

## Files Changed

| File | Action |
|------|--------|
| `extension-ci-tools/` | ADD (submodule) |
| `.gitmodules` | MODIFY (add submodule entry) |
| `Makefile` | REPLACE (hybrid approach) |
| `README.md` | MODIFY (add build section) |
| `extension_config.cmake` | VERIFY (no changes needed) |

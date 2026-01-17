# Quickstart: Verifying Split TLS Build

**Date**: 2026-01-16
**Feature**: 006-split-tls-build

## Prerequisites

- CMake 3.21+
- C++17 compiler (Clang, GCC, or MSVC)
- vcpkg with mbedTLS installed
- DuckDB submodule initialized

## Build Verification Steps

### 1. Configure Build

```bash
# From repository root
mkdir -p build && cd build

# Configure with vcpkg toolchain
cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Debug
```

### 2. Build Static Extension

```bash
cmake --build . --target mssql_extension -v 2>&1 | tee static_build.log
```

**Verification checklist**:
- [ ] Build succeeds without errors
- [ ] No "duplicate symbol" warnings in output
- [ ] No `-force_load` in link command: `grep -i force_load static_build.log` should be empty
- [ ] No `--allow-multiple-definition` in link command: `grep -i allow-multiple static_build.log` should be empty

### 3. Build Loadable Extension

```bash
cmake --build . --target mssql_loadable_extension -v 2>&1 | tee loadable_build.log
```

**Verification checklist**:
- [ ] Build succeeds without errors
- [ ] Symbol hiding flags present in link command

### 4. Verify Symbol Export (Loadable Extension)

#### macOS

```bash
# Check exported symbols
nm -gU build/mssql.duckdb_extension | grep -v "^$"

# Expected: Only mssql_duckdb_cpp_init visible
# Should see something like:
# 0000000000001234 T _mssql_duckdb_cpp_init
```

#### Linux

```bash
# Check exported symbols
nm -D build/mssql.duckdb_extension | grep " T "

# Expected: Only mssql_duckdb_cpp_init visible
```

#### Windows

```bash
# Check exported symbols
dumpbin /EXPORTS build/mssql.duckdb_extension

# Expected: Only mssql_duckdb_cpp_init in exports table
```

### 5. Verify No mbedTLS Symbol Export (Loadable)

```bash
# macOS/Linux
nm -gU build/mssql.duckdb_extension 2>/dev/null | grep mbedtls || \
nm -D build/mssql.duckdb_extension 2>/dev/null | grep mbedtls

# Expected: No output (all mbedTLS symbols should be hidden)
```

### 6. Runtime Verification

#### Test with Static Extension

```bash
# Run DuckDB with static extension built-in
./duckdb

# In DuckDB shell:
D SELECT * FROM duckdb_extensions();
-- Verify mssql extension is loaded

D ATTACH 'mssql://server=localhost;user=test;password=test;encrypt=true' AS mssql_db;
-- Should connect successfully if SQL Server is available
-- Key verification: No crash during TLS handshake
```

#### Test with Loadable Extension

```bash
# Run vanilla DuckDB
duckdb

# In DuckDB shell:
D INSTALL 'build/mssql.duckdb_extension';
D LOAD mssql;
-- Should load without symbol conflict errors

D ATTACH 'mssql://server=localhost;user=test;password=test;encrypt=true' AS mssql_db;
-- Should connect successfully
-- Key verification: No symbol collision during TLS handshake
```

## Troubleshooting

### "duplicate symbol" errors during static build

**Cause**: Old linker flags still present
**Solution**: Verify CMakeLists.txt changes removed `-force_load` and `--allow-multiple-definition`

### "undefined symbol mbedtls_*" during loadable extension load

**Cause**: mbedTLS symbols not linked into loadable extension
**Solution**: Verify mssql_tls_loadable links MbedTLS::mbedcrypto

### TLS handshake crash in static build

**Cause**: Mismatched mbedTLS implementations between DuckDB crypto and our TLS
**Solution**: Review link order, ensure mbedtls and mbedx509 come from vcpkg before DuckDB

### "symbol not found: mssql_duckdb_cpp_init" when loading extension

**Cause**: Symbol export not configured correctly
**Solution**: Verify exported_symbols_list (macOS), version script (Linux), or .def file (Windows)

## Success Criteria Checklist

- [ ] SC-001: Static build - zero duplicate symbol warnings
- [ ] SC-002: Static runtime - no crash during TLS handshake
- [ ] SC-003: Loadable exports exactly one symbol
- [ ] SC-004: Loadable loads without symbol conflict
- [ ] SC-005: TLS connections work on both builds
- [ ] SC-006: Builds succeed on macOS, Linux, Windows
- [ ] SC-007: Same source files used for both targets
- [ ] SC-008: No vcpkg mbedTLS in static link command

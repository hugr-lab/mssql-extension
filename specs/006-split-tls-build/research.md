# Research: Split TLS Build Configuration

**Date**: 2026-01-16
**Feature**: 006-split-tls-build

## Critical Finding: DuckDB's Bundled mbedTLS Lacks SSL/TLS Support

### Discovery

DuckDB's bundled mbedTLS at `duckdb/third_party/mbedtls/include/mbedtls/` is a **crypto-only subset** that does NOT include SSL/TLS functionality:

**Headers present** (54 total): AES, SHA, RSA, entropy, cipher, GCM, etc.
**Headers MISSING**: `ssl.h`, `ssl_ciphersuites.h`, `ssl_cookie.h`, `ssl_ticket.h`, `ctr_drbg.h`, `net_sockets.h`, `x509.h`, `x509_crt.h`

The extension's TLS implementation (`src/tls/tds_tls_impl.cpp`) requires:
- `mbedtls/ssl.h` - SSL context and configuration
- `mbedtls/ctr_drbg.h` - Counter-mode DRBG random generator
- `mbedtls/entropy.h` - Entropy source (present but limited)

**Conclusion**: The original spec assumption that DuckDB's bundled mbedTLS could be used for static builds is **incorrect**. DuckDB only bundles mbedTLS for cryptographic operations (SHA, AES, RSA for signature verification), not for TLS/SSL connections.

---

## Revised Approach

### Decision: Use vcpkg mbedTLS for Both Targets with Symbol Isolation

Since DuckDB's bundled mbedTLS cannot provide TLS functionality, both the static and loadable extension targets must use vcpkg mbedTLS. The conflict arises only when:
1. Static build links vcpkg mbedTLS symbols that clash with DuckDB's bundled crypto symbols
2. Loadable build exports symbols that clash when dynamically loaded

### Rationale

- The symbols that conflict are primarily the crypto primitives (AES, SHA, RSA) that both DuckDB and our TLS code use
- DuckDB's crypto subset and vcpkg's full mbedTLS have different configurations and potentially different implementations
- Symbol prefixing via macros is the cleanest solution to eliminate conflicts

### Alternatives Considered

| Alternative | Rejected Because |
|------------|------------------|
| Use DuckDB bundled mbedTLS | Missing ssl.h and TLS APIs entirely |
| Link both and use -force_load | Current approach, causes crashes and undefined behavior |
| Build mbedTLS from source with custom prefix | Complex, requires patching mbedTLS sources |
| Use symbol versioning | Doesn't work for static linking |

---

## Research Topic 1: Symbol Prefixing Strategy

### Decision: Compile-time macro prefixing via mbedtls_prefix.h

The existing `mbedtls_prefix.h` file provides the foundation. When `MSSQL_PREFIX_MBEDTLS` is defined, all mbedTLS function calls are renamed to `mssql_mbedtls_*` at compile time.

**Implementation approach**:
1. Build vcpkg mbedTLS normally (no source changes needed)
2. Compile TLS code with `-DMSSQL_PREFIX_MBEDTLS` and `-include mbedtls_prefix.h`
3. At link time, use `objcopy --redefine-syms` (Linux) or equivalent to rename the actual symbols in the vcpkg mbedTLS libraries

**Issue with this approach**: The macro prefixing renames calls in our code, but the vcpkg library still has unprefixed symbols. We need the library symbols to also be prefixed.

### Alternative: Use mbedTLS MBEDTLS_SYMBOL_PREFIX

mbedTLS supports compile-time symbol prefixing via:
```cmake
-DMBEDTLS_SYMBOL_PREFIX=mssql_
```

This would require building mbedTLS from source with this option, not using vcpkg's pre-built binaries.

### Final Decision: Hybrid approach

For **static extension**:
- Use vcpkg mbedTLS with visibility hidden
- The static extension links into DuckDB, which already has crypto symbols
- Since our TLS code is separate from DuckDB's crypto needs, and we use different APIs (ssl.h vs just aes.h), the actual runtime conflict is limited
- Remove `-force_load` and `--allow-multiple-definition` - let the linker choose first definition
- The TLS library should use weak symbols for crypto primitives if possible

For **loadable extension**:
- Continue current approach: link vcpkg mbedTLS with `-fvisibility=hidden`
- Export only `mssql_duckdb_cpp_init`
- Symbol hiding via exported_symbols_list/version script/.def file already works

---

## Research Topic 2: DuckDB vs vcpkg mbedTLS Symbol Overlap Analysis

### What DuckDB bundled mbedTLS provides (from mbedtls_config.h):
- `MBEDTLS_AES_C` - AES encryption
- `MBEDTLS_SHA256_C`, `MBEDTLS_SHA1_C` - SHA hashing
- `MBEDTLS_RSA_C` - RSA operations
- `MBEDTLS_BIGNUM_C` - Big number arithmetic
- `MBEDTLS_GCM_C`, `MBEDTLS_CCM_C` - Authenticated encryption
- `MBEDTLS_PK_C`, `MBEDTLS_PK_PARSE_C` - Public key parsing
- `MBEDTLS_PEM_PARSE_C` - PEM format parsing
- `MBEDTLS_ENTROPY_C` - Entropy gathering

### What our TLS code needs:
- `mbedtls_ssl_*` - SSL context, config, handshake
- `mbedtls_ctr_drbg_*` - Random number generation
- `mbedtls_entropy_*` - Entropy source
- `mbedtls_strerror` - Error messages

### Overlap Analysis

The **overlapping symbols** are:
- `mbedtls_entropy_*` functions - both use entropy
- Underlying crypto (SHA, AES) used internally by SSL

The **non-overlapping symbols** (TLS-specific, only in vcpkg):
- All `mbedtls_ssl_*` functions
- `mbedtls_ctr_drbg_*` functions
- `mbedtls_x509_*` functions

### Implication for Static Build

When linking statically:
- If DuckDB's symbols are resolved first, our TLS code may use DuckDB's entropy implementation
- The SSL APIs (`mbedtls_ssl_*`) come only from vcpkg, no conflict
- The conflict is primarily in the **entropy and RNG** subsystem

---

## Research Topic 3: CMake Implementation Pattern

### Decision: Two-target TLS build with different compile flags

```cmake
# src/tls/CMakeLists.txt

set(TLS_SOURCES
    tds_tls_impl.cpp
    tds_tls_context.cpp
)

# Common compile options
set(TLS_COMPILE_OPTIONS
    -fvisibility=hidden
    -include${CMAKE_CURRENT_SOURCE_DIR}/mbedtls_compat.h
)

# Find vcpkg mbedTLS (required for both)
find_package(MbedTLS CONFIG REQUIRED)

# --- Target for STATIC extension ---
add_library(mssql_tls_static OBJECT ${TLS_SOURCES})
target_compile_features(mssql_tls_static PRIVATE cxx_std_17)
target_compile_definitions(mssql_tls_static PRIVATE MSSQL_TLS_BACKEND_STATIC=1)
target_compile_options(mssql_tls_static PRIVATE ${TLS_COMPILE_OPTIONS})
target_include_directories(mssql_tls_static PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
# Link mbedTLS headers only (for compilation), actual linking done at root
target_link_libraries(mssql_tls_static PRIVATE MbedTLS::mbedtls)

# --- Target for LOADABLE extension ---
add_library(mssql_tls_loadable STATIC ${TLS_SOURCES})
target_compile_features(mssql_tls_loadable PRIVATE cxx_std_17)
target_compile_definitions(mssql_tls_loadable PRIVATE MSSQL_TLS_BACKEND_LOADABLE=1)
target_compile_options(mssql_tls_loadable PRIVATE ${TLS_COMPILE_OPTIONS})
target_include_directories(mssql_tls_loadable PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_link_libraries(mssql_tls_loadable PUBLIC
    MbedTLS::mbedtls
    MbedTLS::mbedx509
    MbedTLS::mbedcrypto
)
```

### Root CMakeLists.txt Changes

```cmake
# For static extension - link object files, rely on DuckDB's crypto where possible
target_sources(mssql_extension PRIVATE $<TARGET_OBJECTS:mssql_tls_static>)
# Link only the SSL portions, not full crypto (let DuckDB provide that)
target_link_libraries(mssql_extension PRIVATE MbedTLS::mbedtls MbedTLS::mbedx509)
# REMOVE: -force_load and --allow-multiple-definition

# For loadable extension - link full static library with hidden symbols
target_link_libraries(mssql_loadable_extension PRIVATE mssql_tls_loadable)
# Symbol hiding already in place via exported_symbols_list/version script
```

---

## Research Topic 4: Windows .def File

### Decision: Generate .def file similar to macOS/Linux symbol exports

```cmake
# For Windows loadable extension
if(WIN32)
    set(EXTENSION_DEF_FILE "${CMAKE_CURRENT_BINARY_DIR}/mssql_extension.def")
    file(WRITE ${EXTENSION_DEF_FILE} "EXPORTS\n    mssql_duckdb_cpp_init\n")
    target_sources(mssql_loadable_extension PRIVATE ${EXTENSION_DEF_FILE})
endif()
```

The .def file explicitly lists exported symbols. All others are hidden by default in Windows DLLs when using `/DEF:` linker option.

---

## Research Topic 5: Compatibility Layer Requirements

### Existing mbedtls_compat.h Coverage

Currently handles:
- `PSA_HASH_MAX_SIZE` - missing from DuckDB config
- `PSA_MAC_MAX_SIZE` - missing from DuckDB config
- `mbedtls_f_rng_t` typedef - missing from DuckDB config

### Additional Requirements for Static Build

When the static extension uses DuckDB's entropy implementation (which may be invoked indirectly), we need to ensure:
1. The entropy source is properly seeded
2. The RNG interface is compatible

Since we use `mbedtls_ctr_drbg_*` which DuckDB doesn't have, this comes entirely from vcpkg.

### Decision: Minimal changes to mbedtls_compat.h

The existing compat header should suffice. The key insight is:
- SSL/TLS APIs come from vcpkg (no DuckDB equivalent)
- CTR_DRBG comes from vcpkg (no DuckDB equivalent)
- Only entropy has overlap, and that's compatible

---

## Summary of Decisions

| Topic | Decision | Rationale |
|-------|----------|-----------|
| Static build mbedTLS source | vcpkg (not DuckDB bundled) | DuckDB bundled lacks ssl.h |
| Symbol conflict mitigation | Remove force-load, rely on natural linking | SSL symbols don't conflict, only crypto does |
| Static TLS library type | OBJECT library | Flexibility in linking |
| Loadable TLS library type | STATIC library | Full encapsulation with hidden symbols |
| Windows symbol hiding | .def file | Platform-standard approach |
| Compatibility layer | Keep existing mbedtls_compat.h | Already sufficient |
| Compile definitions | MSSQL_TLS_BACKEND_STATIC / MSSQL_TLS_BACKEND_LOADABLE | Allows conditional code if needed |

---

## Updated Spec Impact

The original spec assumed DuckDB bundled mbedTLS could be used for static builds. This research shows that's not possible. The updated approach:

1. **FR-009 revision**: DuckDB-flavor TLS target cannot use DuckDB's bundled headers (they lack SSL). Instead, use vcpkg headers for both targets.
2. **FR-007 revision**: Both targets use find_package(MbedTLS) from vcpkg.
3. **FR-001 remains valid**: Two library variants, but the difference is linking strategy and symbol visibility, not header source.

The key goal remains the same: eliminate `-force_load` and `--allow-multiple-definition` to prevent crashes.

# Data Model: Split TLS Build Configuration

**Date**: 2026-01-16
**Feature**: 006-split-tls-build

## Overview

This feature involves CMake build configuration entities, not runtime data models. The "data model" here describes the CMake targets, variables, and their relationships.

## CMake Target Entities

### mssql_tls_static (OBJECT Library)

**Purpose**: TLS implementation compiled for static extension linking

| Property | Value |
|----------|-------|
| Type | OBJECT library |
| Sources | tds_tls_impl.cpp, tds_tls_context.cpp |
| Compile Definition | `MSSQL_TLS_BACKEND_STATIC=1` |
| Include Directories | src/tls, src/include, vcpkg mbedTLS |
| Link Libraries | MbedTLS::mbedtls (headers only, linking deferred) |
| Visibility | Hidden (-fvisibility=hidden) |
| Consumer | mssql_extension (static) |

**State**: Compiled → Object files → Linked into static extension

### mssql_tls_loadable (STATIC Library)

**Purpose**: TLS implementation as self-contained static library for loadable extension

| Property | Value |
|----------|-------|
| Type | STATIC library |
| Sources | tds_tls_impl.cpp, tds_tls_context.cpp |
| Compile Definition | `MSSQL_TLS_BACKEND_LOADABLE=1` |
| Include Directories | src/tls, src/include, vcpkg mbedTLS |
| Link Libraries | MbedTLS::mbedtls, MbedTLS::mbedx509, MbedTLS::mbedcrypto |
| Visibility | Hidden (-fvisibility=hidden) |
| Consumer | mssql_loadable_extension |

**State**: Compiled → Archive → Linked into loadable extension

### mssql_extension (Static Extension)

**Purpose**: DuckDB extension linked statically into DuckDB binary

| Property | Value |
|----------|-------|
| TLS Source | $<TARGET_OBJECTS:mssql_tls_static> |
| mbedTLS Link | MbedTLS::mbedtls, MbedTLS::mbedx509 (no mbedcrypto, use DuckDB's) |
| Symbol Export | All symbols visible (part of DuckDB binary) |
| Linker Flags | NO -force_load, NO --allow-multiple-definition |

### mssql_loadable_extension (Loadable Extension)

**Purpose**: DuckDB extension loaded at runtime as shared library

| Property | Value |
|----------|-------|
| TLS Source | mssql_tls_loadable static library |
| mbedTLS Link | Transitive via mssql_tls_loadable |
| Symbol Export | Only mssql_duckdb_cpp_init |
| macOS | exported_symbols_list |
| Linux | version script |
| Windows | .def file |

## CMake Variables

| Variable | Scope | Purpose |
|----------|-------|---------|
| `TLS_SOURCES` | src/tls | List of TLS implementation source files |
| `TLS_COMPILE_OPTIONS` | src/tls | Common compile flags for both targets |
| `EXTENSION_DEF_FILE` | Root (Windows) | Path to generated .def file |

## Relationships

```
┌─────────────────────────────────────────────────────────────┐
│                     vcpkg mbedTLS                           │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │ mbedtls     │  │ mbedx509    │  │ mbedcrypto          │  │
│  │ (SSL/TLS)   │  │ (X.509)     │  │ (AES, SHA, RSA...)  │  │
│  └──────┬──────┘  └──────┬──────┘  └──────────┬──────────┘  │
└─────────│────────────────│─────────────────────│────────────┘
          │                │                     │
          ▼                ▼                     │
    ┌─────────────────────────────┐              │
    │   mssql_tls_static          │              │
    │   (OBJECT library)          │              │
    │   MSSQL_TLS_BACKEND_STATIC  │              │
    └──────────────┬──────────────┘              │
                   │                             │
                   ▼                             │
    ┌─────────────────────────────┐              │
    │   mssql_extension           │◄─────────────┘ (DuckDB provides crypto)
    │   (Static Extension)        │
    │   Links: mbedtls, mbedx509  │
    │   NO mbedcrypto (from DuckDB)│
    └─────────────────────────────┘

          │                │                     │
          ▼                ▼                     ▼
    ┌─────────────────────────────────────────────┐
    │   mssql_tls_loadable                        │
    │   (STATIC library)                          │
    │   MSSQL_TLS_BACKEND_LOADABLE                │
    │   Links: mbedtls, mbedx509, mbedcrypto      │
    └──────────────────────┬──────────────────────┘
                           │
                           ▼
    ┌─────────────────────────────────────────────┐
    │   mssql_loadable_extension                  │
    │   (Loadable Extension - .duckdb_extension)  │
    │   Exports: mssql_duckdb_cpp_init ONLY       │
    │   All mbedTLS symbols hidden                │
    └─────────────────────────────────────────────┘
```

## Build Configuration Matrix

| Target | mbedtls | mbedx509 | mbedcrypto | Visibility | Symbol Export |
|--------|---------|----------|------------|------------|---------------|
| mssql_tls_static | headers | - | - | hidden | N/A (object) |
| mssql_tls_loadable | link | link | link | hidden | N/A (archive) |
| mssql_extension | link | link | DuckDB's | - | all (part of DuckDB) |
| mssql_loadable_extension | transitive | transitive | transitive | hidden | init symbol only |

## Compile Definition Semantics

### MSSQL_TLS_BACKEND_STATIC

- Indicates compilation for static extension target
- May be used for conditional code if API differences arise
- Currently expected: no conditional code needed

### MSSQL_TLS_BACKEND_LOADABLE

- Indicates compilation for loadable extension target
- May be used for conditional code if API differences arise
- Currently expected: no conditional code needed

## Validation Rules

1. **Source Parity**: Both TLS targets MUST compile identical source files
2. **No Duplicate Definition**: Static extension MUST NOT use -force_load or --allow-multiple-definition
3. **Symbol Hiding**: Loadable extension MUST export only mssql_duckdb_cpp_init
4. **Platform Coverage**: Symbol export mechanism MUST exist for macOS, Linux, and Windows

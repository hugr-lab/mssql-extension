# Contracts: Split TLS Build Configuration

**Date**: 2026-01-16
**Feature**: 006-split-tls-build

## Not Applicable

This feature is a build system change with no API contracts. There are no:
- REST APIs
- GraphQL schemas
- Public interfaces
- Data formats

The "contracts" for this feature are the CMake target interfaces defined in `data-model.md`.

## CMake Interface Contract

The only "contract" is the CMake target linkage:

### For Static Extension Consumers

```cmake
# Root CMakeLists.txt contract
target_sources(mssql_extension PRIVATE $<TARGET_OBJECTS:mssql_tls_static>)
target_link_libraries(mssql_extension PRIVATE MbedTLS::mbedtls MbedTLS::mbedx509)
```

### For Loadable Extension Consumers

```cmake
# Root CMakeLists.txt contract
target_link_libraries(mssql_loadable_extension PRIVATE mssql_tls_loadable)
```

### Symbol Export Contract

| Platform | Mechanism | Exported Symbol |
|----------|-----------|-----------------|
| macOS | exported_symbols_list | `_mssql_duckdb_cpp_init` |
| Linux | version script | `mssql_duckdb_cpp_init` |
| Windows | .def file | `mssql_duckdb_cpp_init` |

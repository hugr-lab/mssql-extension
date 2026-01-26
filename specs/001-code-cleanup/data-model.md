# Data Model: Code Cleanup and Directory Reorganization

## Overview

This is a pure refactoring feature with no data model changes. This document describes the file system structure changes.

## Entity: Directory Structure

### Current State

```text
src/
├── catalog/           # Catalog integration
├── connection/        # Connection pooling
├── delete/            # DELETE operations (to be moved)
├── insert/            # INSERT operations (to be moved)
├── query/             # Query execution
├── table_scan/        # Table scanning
│   └── mssql_table_scan.cpp  # (to be renamed)
├── tds/               # TDS protocol
├── update/            # UPDATE operations (to be moved)
└── include/
    ├── catalog/
    ├── connection/
    ├── delete/        # (to be moved)
    ├── insert/        # (to be moved)
    ├── query/
    ├── table_scan/
    │   └── mssql_table_scan.hpp  # (to be renamed)
    ├── tds/
    └── update/        # (to be moved)
```

### Target State

```text
src/
├── catalog/           # Catalog integration
├── connection/        # Connection pooling
├── dml/               # NEW: Consolidated DML operations
│   ├── delete/        # DELETE operations
│   ├── insert/        # INSERT operations
│   └── update/        # UPDATE operations
├── query/             # Query execution
├── table_scan/        # Table scanning
│   └── table_scan.cpp # RENAMED from mssql_table_scan.cpp
├── tds/               # TDS protocol
└── include/
    ├── catalog/
    ├── connection/
    ├── dml/           # NEW: Consolidated DML headers
    │   ├── delete/
    │   ├── insert/
    │   └── update/
    ├── query/
    ├── table_scan/
    │   └── table_scan.hpp  # RENAMED from mssql_table_scan.hpp
    └── tds/
```

## Entity: CMakeLists.txt Changes

### Source File References

Current:
```cmake
src/table_scan/mssql_table_scan.cpp
src/insert/*.cpp
src/update/*.cpp
src/delete/*.cpp
```

Target:
```cmake
src/table_scan/table_scan.cpp
src/dml/insert/*.cpp
src/dml/update/*.cpp
src/dml/delete/*.cpp
```

### Include Directories

Current:
```cmake
-I${CMAKE_CURRENT_SOURCE_DIR}/src/include/insert
-I${CMAKE_CURRENT_SOURCE_DIR}/src/include/update
-I${CMAKE_CURRENT_SOURCE_DIR}/src/include/delete
```

Target:
```cmake
-I${CMAKE_CURRENT_SOURCE_DIR}/src/include/dml
```

## No Database Schema Changes

This refactoring does not affect:
- SQL Server connectivity
- Data types or mappings
- Query execution
- Any runtime behavior

All changes are compile-time file organization only.

cmake_minimum_required(VERSION 3.21)

# Set extension name - this must match the extension entry point function name
set(TARGET_NAME mssql)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

# Project configuration
project(${TARGET_NAME})

include_directories(src/include src/include/tds/tls src/include/catalog src/include/pushdown)

# Get DuckDB commit hash for version tracking
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/duckdb
    OUTPUT_VARIABLE DUCKDB_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(NOT DUCKDB_COMMIT_HASH)
    set(DUCKDB_COMMIT_HASH "unknown")
endif()

message(STATUS "Building MSSQL extension against DuckDB commit: ${DUCKDB_COMMIT_HASH}")

# =============================================================================
# TLS Libraries (built as separate subproject with two variants)
# =============================================================================
# The TLS library is built in src/tls with its own CMakeLists.txt.
#
# Two TLS library variants are built:
#   - mssql_tls_static (OBJECT library): STUB implementation for static extension
#     Returns "TLS not available" - directs users to loadable extension
#     NO mbedTLS dependency - completely avoids symbol conflicts
#   - mssql_tls_loadable (STATIC library): FULL TLS for loadable extension
#     Uses vcpkg mbedTLS with all symbols hidden via visibility controls
#
# Why this split?
#   DuckDB bundles crypto-only mbedTLS which conflicts with vcpkg's full mbedTLS.
#   macOS doesn't support --allow-multiple-definition, making symbol conflicts fatal.
#   Static build: Stub implementation avoids the conflict entirely.
#   Loadable build: Symbol hiding prevents conflicts when dynamically loaded.
add_subdirectory(src/tds/tls)

# =============================================================================
# Extension Sources (shared between static and loadable builds)
# =============================================================================
set(EXTENSION_SOURCES
    src/mssql_extension.cpp
    src/mssql_secret.cpp
    src/mssql_storage.cpp
    src/mssql_functions.cpp
    # Encoding utilities (in tds namespace)
    src/tds/encoding/utf16.cpp
    src/tds/encoding/decimal_encoding.cpp
    src/tds/encoding/datetime_encoding.cpp
    src/tds/encoding/guid_encoding.cpp
    src/tds/encoding/type_converter.cpp
    # TDS protocol layer (pure C++, no DuckDB dependencies)
    src/tds/tds_types.cpp
    src/tds/tds_packet.cpp
    src/tds/tds_socket.cpp
    src/tds/tds_protocol.cpp
    src/tds/tds_connection.cpp
    src/tds/tds_connection_pool.cpp
    src/tds/tds_column_metadata.cpp
    src/tds/tds_token_parser.cpp
    src/tds/tds_row_reader.cpp
    # Query execution layer (DuckDB integration)
    src/query/mssql_query_executor.cpp
    src/query/mssql_result_stream.cpp
    src/query/mssql_simple_query.cpp
    # Connection management layer (DuckDB integration)
    src/connection/mssql_settings.cpp
    src/connection/mssql_diagnostic.cpp
    src/connection/mssql_pool_manager.cpp
    # Catalog integration layer (DuckDB catalog API)
    src/catalog/mssql_column_info.cpp
    src/catalog/mssql_metadata_cache.cpp
    src/catalog/mssql_catalog.cpp
    src/catalog/mssql_schema_entry.cpp
    src/catalog/mssql_table_entry.cpp
    src/catalog/mssql_table_set.cpp
    src/catalog/mssql_table_function.cpp
    src/catalog/mssql_refresh_function.cpp
    src/catalog/mssql_transaction.cpp
    src/catalog/mssql_ddl_translator.cpp
    src/catalog/mssql_statistics.cpp
    # Pushdown layer (filter/projection optimization)
    src/pushdown/mssql_projection_builder.cpp
    src/pushdown/mssql_filter_translator.cpp
    src/pushdown/mssql_prepared_statement.cpp
)

# Build static extension (linked into DuckDB) - uses TLS objects from mssql_tls_static
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})

# Build loadable extension (.duckdb_extension file) - uses TLS from mssql_tls_loadable
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# =============================================================================
# Static Extension Configuration
# =============================================================================
# The static extension uses TLS STUB implementation (no actual TLS).
# This avoids mbedTLS symbol conflicts with DuckDB's bundled crypto library.
# Users who need TLS should use the loadable extension (.duckdb_extension).

target_compile_options(${EXTENSION_NAME} PRIVATE
    "-Wno-macro-redefined"
)
target_compile_definitions(${EXTENSION_NAME} PRIVATE
    EXT_VERSION_MSSQL="${DUCKDB_COMMIT_HASH}"
    MSSQL_TLS_STUB=1
)
target_include_directories(${EXTENSION_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/tds/tls
)

# Link TLS stub objects into static extension (no mbedTLS dependency)
target_sources(${EXTENSION_NAME} PRIVATE $<TARGET_OBJECTS:mssql_tls_static>)
add_dependencies(${EXTENSION_NAME} mssql_tls_static)

message(STATUS "Static extension configured: TLS STUB (no TLS support - use loadable extension for TLS)")

# =============================================================================
# Loadable Extension Configuration
# =============================================================================
# The loadable extension links the mssql_tls_loadable static library which includes
# all mbedTLS dependencies (mbedtls, mbedx509, mbedcrypto). Symbol visibility controls
# hide all internal symbols, only exporting the extension entry point. This prevents
# conflicts with DuckDB's bundled mbedTLS when the extension is dynamically loaded.

target_compile_options(${LOADABLE_EXTENSION_NAME} PRIVATE
    "-Wno-macro-redefined"
    "-include${CMAKE_CURRENT_SOURCE_DIR}/src/include/tds/tls/mbedtls_compat.h"
)
target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE
    EXT_VERSION_MSSQL="${DUCKDB_COMMIT_HASH}"
)
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/tds/tls
)

# Link the TLS static library (includes full mbedTLS suite via transitive dependencies)
target_link_libraries(${LOADABLE_EXTENSION_NAME} mssql_tls_loadable)

# Symbol export control - hide all symbols except extension entry point
# This is critical to prevent symbol collisions with DuckDB's bundled mbedTLS
if(APPLE)
    # macOS: Use -exported_symbols_list to only export extension entry point
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/extension_exports.txt"
        "_mssql_duckdb_cpp_init\n"
    )
    target_link_options(${LOADABLE_EXTENSION_NAME} PRIVATE
        "-Wl,-exported_symbols_list,${CMAKE_CURRENT_BINARY_DIR}/extension_exports.txt"
    )
    message(STATUS "Loadable extension: macOS symbol export via exported_symbols_list")
elseif(UNIX)
    # Linux: Use version script to only export extension entry point
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/extension_version.map"
        "{ global: mssql_duckdb_cpp_init; local: *; };\n"
    )
    target_link_options(${LOADABLE_EXTENSION_NAME} PRIVATE
        "-Wl,--version-script,${CMAKE_CURRENT_BINARY_DIR}/extension_version.map"
    )
    message(STATUS "Loadable extension: Linux symbol export via version script")
elseif(WIN32)
    # Windows: Use module definition file (.def) to control exports
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mssql_extension.def"
        "LIBRARY mssql\n"
        "EXPORTS\n"
        "    mssql_duckdb_cpp_init\n"
    )
    target_sources(${LOADABLE_EXTENSION_NAME} PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/mssql_extension.def"
    )
    message(STATUS "Loadable extension: Windows symbol export via .def file")
endif()

# =============================================================================
# Installation
# =============================================================================
install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

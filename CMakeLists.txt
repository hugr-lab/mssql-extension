cmake_minimum_required(VERSION 3.21)

# Set extension name - this must match the extension entry point function name
set(TARGET_NAME mssql)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

# Project configuration
project(${TARGET_NAME})

include_directories(src/include src/tls)

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
# TLS Static Library (built as separate subproject)
# =============================================================================
# The TLS library is built in src/tls with its own CMakeLists.txt to ensure
# complete isolation from DuckDB's include paths. DuckDB bundles a stripped-down
# mbedTLS that's incompatible with our needs.
add_subdirectory(src/tls)

# =============================================================================
# Extension Sources (shared between static and loadable builds)
# =============================================================================
set(EXTENSION_SOURCES
    src/mssql_extension.cpp
    src/mssql_secret.cpp
    src/mssql_storage.cpp
    src/mssql_functions.cpp
    # Encoding utilities
    src/encoding/utf16.cpp
    src/encoding/decimal_encoding.cpp
    src/encoding/datetime_encoding.cpp
    src/encoding/guid_encoding.cpp
    src/encoding/type_converter.cpp
    # TDS protocol layer (pure C++, no DuckDB dependencies)
    src/tds/tds_types.cpp
    src/tds/tds_packet.cpp
    src/tds/tds_socket.cpp
    src/tds/tds_protocol.cpp
    src/tds/tds_connection.cpp
    src/tds/connection_pool.cpp
    src/tds/tds_column_metadata.cpp
    src/tds/tds_token_parser.cpp
    src/tds/tds_row_reader.cpp
    # Query execution layer (DuckDB integration)
    src/query/mssql_query_executor.cpp
    src/query/mssql_result_stream.cpp
    # Connection management layer (DuckDB integration)
    src/connection/mssql_settings.cpp
    src/connection/mssql_diagnostic.cpp
    src/connection/mssql_pool_manager.cpp
)

# Build static extension (linked into DuckDB) - uses TLS from mssql_tls library
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})

# Build loadable extension (.duckdb_extension file) - uses TLS from mssql_tls library
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# =============================================================================
# Static Extension Configuration
# =============================================================================
# Link mssql_tls library which includes vcpkg's mbedTLS
# On macOS: Use -force_load to ensure our mbedTLS symbols are used (first wins)
# On Linux: Use --allow-multiple-definition to handle DuckDB's bundled mbedTLS

target_compile_options(${EXTENSION_NAME} PRIVATE
    "-Wno-macro-redefined"
)
target_compile_definitions(${EXTENSION_NAME} PRIVATE
    EXT_VERSION_MSSQL="${DUCKDB_COMMIT_HASH}"
)
target_include_directories(${EXTENSION_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tls
)

# Find mbedTLS for explicit linking (needed because -force_load doesn't transitively link)
find_package(MbedTLS CONFIG REQUIRED)

# Link TLS library with appropriate flags for duplicate symbol handling
if(APPLE)
    # macOS: Force load our library and vcpkg mbedTLS first
    # This ensures our symbols take precedence over DuckDB's bundled mbedTLS
    target_link_libraries(${EXTENSION_NAME}
        "-Wl,-force_load,$<TARGET_FILE:mssql_tls>"
        "-Wl,-force_load,$<TARGET_FILE:MbedTLS::mbedtls>"
        "-Wl,-force_load,$<TARGET_FILE:MbedTLS::mbedx509>"
        "-Wl,-force_load,$<TARGET_FILE:MbedTLS::mbedcrypto>"
    )
elseif(UNIX)
    # Linux: Allow multiple definitions, first one wins
    target_link_libraries(${EXTENSION_NAME}
        "-Wl,--whole-archive"
        mssql_tls
        MbedTLS::mbedtls
        MbedTLS::mbedx509
        MbedTLS::mbedcrypto
        "-Wl,--no-whole-archive"
        "-Wl,--allow-multiple-definition"
    )
else()
    # Other platforms (Windows, etc.)
    target_link_libraries(${EXTENSION_NAME} mssql_tls)
endif()
add_dependencies(${EXTENSION_NAME} mssql_tls)

# =============================================================================
# Loadable Extension Configuration
# =============================================================================
# The loadable extension links the TLS static library directly. Symbol visibility
# is used to hide all mbedTLS symbols, only exporting the extension entry point.
# This prevents conflicts with DuckDB's bundled mbedTLS when the extension is loaded.

target_compile_options(${LOADABLE_EXTENSION_NAME} PRIVATE
    "-Wno-macro-redefined"
    "-include${CMAKE_CURRENT_SOURCE_DIR}/src/tls/mbedtls_compat.h"
)
target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE
    EXT_VERSION_MSSQL="${DUCKDB_COMMIT_HASH}"
)
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tls
)

# Link the TLS static library (includes mbedTLS)
# Use plain signature to match DuckDB's extension build tools
target_link_libraries(${LOADABLE_EXTENSION_NAME} mssql_tls)

# Use symbol visibility to hide all mbedTLS symbols, only exporting entry point
if(APPLE)
    # macOS: Use -exported_symbols_list to only export extension entry point
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/extension_exports.txt"
        "_mssql_duckdb_cpp_init\n"
    )
    target_link_options(${LOADABLE_EXTENSION_NAME} PRIVATE
        "-Wl,-exported_symbols_list,${CMAKE_CURRENT_BINARY_DIR}/extension_exports.txt"
    )
elseif(UNIX)
    # Linux: Use version script to only export extension entry point
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/extension_version.map"
        "{ global: mssql_duckdb_cpp_init; local: *; };\n"
    )
    target_link_options(${LOADABLE_EXTENSION_NAME} PRIVATE
        "-Wl,--version-script,${CMAKE_CURRENT_BINARY_DIR}/extension_version.map"
    )
endif()

# =============================================================================
# Installation
# =============================================================================
install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

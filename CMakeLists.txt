cmake_minimum_required(VERSION 3.21)

# Set extension name - this must match the extension entry point function name
set(TARGET_NAME mssql)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

# Project configuration
project(${TARGET_NAME})

include_directories(src/include)

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

# Set extension sources
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

# Build static extension (linked into DuckDB)
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})

# Build loadable extension (.duckdb_extension file)
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Add version definition to both targets (intentionally overrides DuckDB's definition)
target_compile_options(${EXTENSION_NAME} PRIVATE "-Wno-macro-redefined")
target_compile_options(${LOADABLE_EXTENSION_NAME} PRIVATE "-Wno-macro-redefined")
target_compile_definitions(${EXTENSION_NAME} PRIVATE EXT_VERSION_MSSQL="${DUCKDB_COMMIT_HASH}")
target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE EXT_VERSION_MSSQL="${DUCKDB_COMMIT_HASH}")

# Installation
install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

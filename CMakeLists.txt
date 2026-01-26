cmake_minimum_required(VERSION 3.21)

# Set extension name - this must match the extension entry point function name
set(TARGET_NAME mssql)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

# Project configuration
project(${TARGET_NAME})

include_directories(src/include src/include/tds/tls src/include/catalog src/include/table_scan src/include/dml src/include/dml/insert src/include/dml/update src/include/dml/delete)

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
# DuckDB API Compatibility (Auto-detected)
# =============================================================================
# DuckDB nightly (main) renamed PhysicalOperator::GetData to GetDataInternal.
# We auto-detect this by checking if GetDataInternal exists in the header.
set(DUCKDB_PHYSICAL_OPERATOR_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/src/include/duckdb/execution/physical_operator.hpp")
if(EXISTS "${DUCKDB_PHYSICAL_OPERATOR_HEADER}")
    file(READ "${DUCKDB_PHYSICAL_OPERATOR_HEADER}" PHYSICAL_OPERATOR_CONTENT)
    string(FIND "${PHYSICAL_OPERATOR_CONTENT}" "GetDataInternal" GETDATA_INTERNAL_POS)
    if(GETDATA_INTERNAL_POS GREATER -1)
        set(MSSQL_DUCKDB_NIGHTLY_DETECTED TRUE)
    else()
        set(MSSQL_DUCKDB_NIGHTLY_DETECTED FALSE)
    endif()
else()
    message(WARNING "DuckDB physical_operator.hpp not found, assuming stable API")
    set(MSSQL_DUCKDB_NIGHTLY_DETECTED FALSE)
endif()

if(MSSQL_DUCKDB_NIGHTLY_DETECTED)
    message(STATUS "Detected DuckDB nightly API (GetDataInternal)")
    add_compile_definitions(MSSQL_DUCKDB_NIGHTLY=1)
else()
    message(STATUS "Detected DuckDB stable API (GetData)")
endif()

# =============================================================================
# OpenSSL for TLS support
# =============================================================================
set(OPENSSL_USE_STATIC_LIBS ON)
find_package(OpenSSL REQUIRED)

message(STATUS "TLS using OpenSSL version: ${OPENSSL_VERSION}")
message(STATUS "  OpenSSL include: ${OPENSSL_INCLUDE_DIR}")

# =============================================================================
# Extension Sources
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
    # TLS layer (OpenSSL)
    src/tds/tls/tds_tls_impl.cpp
    src/tds/tls/tds_tls_context.cpp
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
    src/catalog/mssql_primary_key.cpp
    src/catalog/mssql_catalog.cpp
    src/catalog/mssql_schema_entry.cpp
    src/catalog/mssql_table_entry.cpp
    src/catalog/mssql_table_set.cpp
    src/catalog/mssql_table_function.cpp
    src/catalog/mssql_refresh_function.cpp
    src/catalog/mssql_transaction.cpp
    src/catalog/mssql_ddl_translator.cpp
    src/catalog/mssql_statistics.cpp
    # Table scan layer (refactored table scan and filter encoding)
    src/table_scan/table_scan_bind.cpp
    src/table_scan/table_scan_state.cpp
    src/table_scan/table_scan_execute.cpp
    src/table_scan/filter_encoder.cpp
    src/table_scan/function_mapping.cpp
    src/table_scan/table_scan.cpp
    # DML shared layer (UPDATE/DELETE common)
    src/dml/mssql_dml_config.cpp
    src/dml/mssql_rowid_extractor.cpp
    # DML INSERT layer
    src/dml/insert/mssql_insert_config.cpp
    src/dml/insert/mssql_insert_target.cpp
    src/dml/insert/mssql_value_serializer.cpp
    src/dml/insert/mssql_insert_statement.cpp
    src/dml/insert/mssql_batch_builder.cpp
    src/dml/insert/mssql_insert_executor.cpp
    src/dml/insert/mssql_physical_insert.cpp
    src/dml/insert/mssql_returning_parser.cpp
    # DML UPDATE layer
    src/dml/update/mssql_update_target.cpp
    src/dml/update/mssql_update_statement.cpp
    src/dml/update/mssql_update_executor.cpp
    src/dml/update/mssql_physical_update.cpp
    # DML DELETE layer
    src/dml/delete/mssql_delete_target.cpp
    src/dml/delete/mssql_delete_statement.cpp
    src/dml/delete/mssql_delete_executor.cpp
    src/dml/delete/mssql_physical_delete.cpp
)

# Build static extension (linked into DuckDB)
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})

# Build loadable extension (.duckdb_extension file)
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# =============================================================================
# Static Extension Configuration
# =============================================================================
# Note: EXT_VERSION_MSSQL is automatically defined by DuckDB's build system
# via duckdb_extension_generate_version() - do not define it again here.
target_include_directories(${EXTENSION_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/tds/tls
    ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)

message(STATUS "Static extension configured with full TLS support (OpenSSL)")

# =============================================================================
# Loadable Extension Configuration
# =============================================================================
# Note: EXT_VERSION_MSSQL is automatically defined by DuckDB's build system
# via duckdb_extension_generate_version() - do not define it again here.
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/tds/tls
    ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)

# Symbol export control - hide all symbols except extension entry point
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

message(STATUS "Loadable extension configured with full TLS support (OpenSSL)")

# =============================================================================
# Installation
# =============================================================================
install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

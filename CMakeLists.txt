cmake_minimum_required(VERSION 3.21)

# Set extension name - this must match the extension entry point function name
set(TARGET_NAME mssql)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

# Extension version - update this when creating a new release tag
set(MSSQL_EXTENSION_VERSION "0.1.18")

# Project configuration
project(${TARGET_NAME})

# Note: Do NOT set CMAKE_CXX_STANDARD here - DuckDB's build system handles this.
# Using CACHE FORCE would override DuckDB's settings and cause ODR issues.

include_directories(src/include src/include/tds/tls src/include/catalog src/include/table_scan src/include/dml src/include/dml/insert src/include/dml/update src/include/dml/delete src/include/copy src/include/azure)

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
# OpenSSL for TLS support
# =============================================================================
set(OPENSSL_USE_STATIC_LIBS ON)
find_package(OpenSSL REQUIRED)

message(STATUS "TLS using OpenSSL version: ${OPENSSL_VERSION}")
message(STATUS "  OpenSSL include: ${OPENSSL_INCLUDE_DIR}")

# =============================================================================
# simdutf for SIMD-accelerated UTF-8 / UTF-16LE transcoding
# Used by the LOGIN7 non-ASCII fix (spec 043); bulk consumer migration in 044.
# Statically linked via the project's existing vcpkg static triplets.
# =============================================================================
find_package(simdutf CONFIG REQUIRED)
message(STATUS "simdutf: located via vcpkg (target simdutf::simdutf)")

# =============================================================================
# Integrated Authentication (Kerberos / SSPI) — Spec 042
#
# POSIX: dynamic link to system libgssapi_krb5 (Linux) / GSS.framework (macOS)
# Windows: link secur32.dll (Windows SDK)
# Auto-detected — if libraries are missing on POSIX the build still succeeds
# without integrated-auth support, and Trusted_Connection=yes / authenticator=krb5
# produce a "compiled without Kerberos support" error per FR-012.
# =============================================================================
option(ENABLE_KRB5 "Enable Kerberos / SSPI integrated authentication" ON)

set(MSSQL_KRB5_AVAILABLE FALSE)
set(MSSQL_SSPI_AVAILABLE FALSE)

if(ENABLE_KRB5)
    if(WIN32)
        # SSPI ships with the OS. secur32.lib is in the Windows SDK.
        set(MSSQL_SSPI_AVAILABLE TRUE)
        message(STATUS "Integrated Auth: Windows SSPI enabled (secur32)")
    elseif(APPLE)
        find_library(MSSQL_GSS_FRAMEWORK GSS)
        if(MSSQL_GSS_FRAMEWORK)
            set(MSSQL_KRB5_AVAILABLE TRUE)
            message(STATUS "Integrated Auth: macOS Kerberos enabled (${MSSQL_GSS_FRAMEWORK})")
        else()
            message(WARNING "Integrated Auth: GSS.framework not found on macOS; Kerberos disabled")
        endif()
    else()
        # Linux + other POSIX: prefer pkg-config, fall back gracefully if missing.
        # Need both krb5-gssapi (for gss_* symbols) AND krb5 (for krb5_*
        # symbols used by raw-credentials mode -- krb5_init_context,
        # krb5_get_init_creds_password, krb5_get_error_message, etc.).
        # libkrb5 does NOT come in transitively via krb5-gssapi.pc on at
        # least Ubuntu 24.04 -- linking only -lgssapi_krb5 produces
        # "undefined reference to symbol 'krb5_free_error_message@@krb5_3_MIT'".
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(MSSQL_GSSAPI krb5-gssapi)
            pkg_check_modules(MSSQL_KRB5_BASE krb5)
            if(MSSQL_GSSAPI_FOUND AND MSSQL_KRB5_BASE_FOUND)
                set(MSSQL_KRB5_AVAILABLE TRUE)
                message(STATUS "Integrated Auth: Linux Kerberos enabled (GSSAPI ${MSSQL_GSSAPI_VERSION}, krb5 ${MSSQL_KRB5_BASE_VERSION})")
            elseif(NOT MSSQL_GSSAPI_FOUND)
                message(WARNING "Integrated Auth: pkg-config could not locate krb5-gssapi; install Kerberos development headers (libkrb5-dev on Debian/Ubuntu, krb5-devel on RHEL/Fedora) to enable")
            else()
                message(WARNING "Integrated Auth: krb5-gssapi found but base krb5 missing; install Kerberos development headers (libkrb5-dev on Debian/Ubuntu, krb5-devel on RHEL/Fedora) to enable")
            endif()
        else()
            message(WARNING "Integrated Auth: pkg-config not found; Linux Kerberos discovery skipped")
        endif()
    endif()
else()
    message(STATUS "Integrated Auth: disabled (ENABLE_KRB5=OFF)")
endif()

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
    src/tds/encoding/simdutf_wrappers.cpp
    src/tds/encoding/decimal_encoding.cpp
    src/tds/encoding/datetime_encoding.cpp
    src/tds/encoding/guid_encoding.cpp
    src/tds/encoding/type_converter.cpp
    src/tds/encoding/bcp_row_encoder.cpp
    # TDS protocol layer (pure C++, no DuckDB dependencies)
    src/tds/tds_types.cpp
    src/tds/tds_packet.cpp
    src/tds/tds_socket.cpp
    src/tds/tds_protocol.cpp
    src/tds/tds_connection.cpp
    src/tds/tds_connection_pool.cpp
    src/tds/tds_column_metadata.cpp
    # TDS authentication strategies (Spec 031, Spec 032, Spec 042)
    src/tds/auth/sql_auth_strategy.cpp
    src/tds/auth/fedauth_strategy.cpp
    src/tds/auth/manual_token_strategy.cpp
    src/tds/auth/auth_strategy_factory.cpp
    src/tds/auth/krb5_test_function.cpp
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
    src/connection/mssql_connection_provider.cpp
    # Catalog integration layer (DuckDB catalog API)
    src/catalog/mssql_catalog_filter.cpp
    src/catalog/mssql_column_info.cpp
    src/catalog/mssql_metadata_cache.cpp
    src/catalog/mssql_primary_key.cpp
    src/catalog/mssql_catalog.cpp
    src/catalog/mssql_schema_entry.cpp
    src/catalog/mssql_table_entry.cpp
    src/catalog/mssql_table_set.cpp
    src/catalog/mssql_table_function.cpp
    src/catalog/mssql_preload_catalog.cpp
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
    src/table_scan/mssql_optimizer.cpp
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
    # DML CTAS layer (CREATE TABLE AS SELECT)
    src/dml/ctas/mssql_ctas_executor.cpp
    src/dml/ctas/mssql_ctas_planner.cpp
    src/dml/ctas/mssql_physical_ctas.cpp
    # COPY layer (BulkLoadBCP)
    src/copy/bcp_config.cpp
    src/copy/target_resolver.cpp
    src/copy/bcp_writer.cpp
    src/copy/copy_function.cpp
    # Azure AD authentication layer
    src/azure/azure_http.cpp
    src/azure/azure_secret_reader.cpp
    src/azure/azure_token.cpp
    src/azure/azure_test_function.cpp
    src/azure/azure_device_code.cpp
    src/azure/azure_fedauth.cpp
    src/azure/jwt_parser.cpp
)

# Spec 042: conditional inclusion of the platform-specific integrated-auth
# authenticator. POSIX gets krb5_authenticator.cpp when GSSAPI is discovered;
# Windows gets winsspi_authenticator.cpp (Phase 4).
if(MSSQL_KRB5_AVAILABLE)
    list(APPEND EXTENSION_SOURCES src/tds/auth/krb5_authenticator.cpp)
endif()

# Build static extension (linked into DuckDB)
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})

# Build loadable extension (.duckdb_extension file)
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# =============================================================================
# Static Extension Configuration
# =============================================================================
target_include_directories(${EXTENSION_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/tds/tls
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/httplib
    ${OPENSSL_INCLUDE_DIR}
)
target_compile_definitions(${EXTENSION_NAME} PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${EXTENSION_NAME} simdutf::simdutf)
if(WIN32)
    # Windows: static OpenSSL and httplib need Windows system libraries (both MSVC and MinGW)
    target_link_libraries(${EXTENSION_NAME} ws2_32 crypt32)
endif()

# Integrated Authentication (Spec 042) — compile defines + link libs per platform
if(MSSQL_SSPI_AVAILABLE)
    target_compile_definitions(${EXTENSION_NAME} PRIVATE MSSQL_ENABLE_SSPI=1)
    target_link_libraries(${EXTENSION_NAME} secur32)
endif()
if(MSSQL_KRB5_AVAILABLE)
    target_compile_definitions(${EXTENSION_NAME} PRIVATE MSSQL_ENABLE_KRB5=1)
    if(APPLE)
        target_link_libraries(${EXTENSION_NAME} ${MSSQL_GSS_FRAMEWORK})
    else()
        # Order matters: krb5 (base) AFTER gssapi_krb5 so the linker resolves
        # gssapi's references to krb5 symbols, then the krb5 SONAME is added
        # to NEEDED. Without the explicit -lkrb5 the link fails with
        # 'undefined reference to symbol krb5_free_error_message'.
        target_include_directories(${EXTENSION_NAME} PRIVATE
            ${MSSQL_GSSAPI_INCLUDE_DIRS} ${MSSQL_KRB5_BASE_INCLUDE_DIRS})
        target_link_libraries(${EXTENSION_NAME}
            ${MSSQL_GSSAPI_LIBRARIES} ${MSSQL_KRB5_BASE_LIBRARIES})
    endif()
endif()

# Extension version (overrides DuckDB's git-hash-based EXT_VERSION_MSSQL)
target_compile_definitions(${EXTENSION_NAME} PRIVATE MSSQL_VERSION="${MSSQL_EXTENSION_VERSION}")

message(STATUS "Static extension configured with TLS (OpenSSL) and Azure AD authentication (httplib)")

# =============================================================================
# Loadable Extension Configuration
# =============================================================================
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/tds/tls
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/httplib
    ${OPENSSL_INCLUDE_DIR}
)
target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} simdutf::simdutf)
if(WIN32)
    # Windows: static OpenSSL and httplib need Windows system libraries (both MSVC and MinGW)
    target_link_libraries(${LOADABLE_EXTENSION_NAME} ws2_32 crypt32)
endif()

# Integrated Authentication (Spec 042) — compile defines + link libs per platform
if(MSSQL_SSPI_AVAILABLE)
    target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE MSSQL_ENABLE_SSPI=1)
    target_link_libraries(${LOADABLE_EXTENSION_NAME} secur32)
endif()
if(MSSQL_KRB5_AVAILABLE)
    target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE MSSQL_ENABLE_KRB5=1)
    if(APPLE)
        target_link_libraries(${LOADABLE_EXTENSION_NAME} ${MSSQL_GSS_FRAMEWORK})
    else()
        target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE
            ${MSSQL_GSSAPI_INCLUDE_DIRS} ${MSSQL_KRB5_BASE_INCLUDE_DIRS})
        target_link_libraries(${LOADABLE_EXTENSION_NAME}
            ${MSSQL_GSSAPI_LIBRARIES} ${MSSQL_KRB5_BASE_LIBRARIES})
    endif()
endif()

# Hide all symbols except extension entry point to avoid multiple definition errors
if(NOT WIN32)
    target_compile_options(${LOADABLE_EXTENSION_NAME} PRIVATE -fvisibility=hidden)
endif()
# Extension version (overrides DuckDB's git-hash-based EXT_VERSION_MSSQL)
target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE MSSQL_VERSION="${MSSQL_EXTENSION_VERSION}")

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

message(STATUS "Loadable extension configured with TLS (OpenSSL) and Azure AD authentication (httplib)")

# =============================================================================
# Installation
# =============================================================================
install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

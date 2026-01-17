# =============================================================================
# MSSQL Extension - TLS Library (Split Build Configuration)
# =============================================================================
# This builds TWO TLS library targets with different implementations:
#
# 1. mssql_tls_static (OBJECT library) - STUB implementation
#    - For static extension linking
#    - Uses tds_tls_stub.cpp which returns "TLS not available"
#    - NO mbedTLS dependency - avoids symbol conflicts with DuckDB's bundled mbedTLS
#
# 2. mssql_tls_loadable (STATIC library) - FULL implementation
#    - For loadable extension linking
#    - Uses real TLS implementation with vcpkg mbedTLS
#    - All symbols hidden via visibility controls
#
# Why this split?
# - DuckDB bundles a stripped-down mbedTLS (crypto-only, no SSL/TLS headers)
# - We need full mbedTLS from vcpkg for TLS functionality
# - Static build: Symbol conflicts between DuckDB's mbedTLS and vcpkg's mbedTLS
#   cannot be resolved without complex workarounds (macOS doesn't support
#   --allow-multiple-definition). Solution: provide stub that directs users
#   to use the loadable extension for TLS.
# - Loadable build: Hide all symbols except extension init function - no conflict
#
# =============================================================================

cmake_minimum_required(VERSION 3.21)

# =============================================================================
# Target 1: mssql_tls_static (STUB - no TLS, no mbedTLS dependency)
# =============================================================================
# This target provides stub implementations that return "TLS not available".
# Users who need TLS should use the loadable extension.

add_library(mssql_tls_static OBJECT
    tds_tls_stub.cpp
)

target_compile_features(mssql_tls_static PRIVATE cxx_std_17)
target_compile_definitions(mssql_tls_static PRIVATE MSSQL_TLS_BACKEND_STUB=1)
target_compile_options(mssql_tls_static PRIVATE
    "-fvisibility=hidden"
)
target_include_directories(mssql_tls_static PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

message(STATUS "TLS target 'mssql_tls_static' configured (STUB - no TLS support)")

# =============================================================================
# Target 2: mssql_tls_loadable (FULL TLS with vcpkg mbedTLS)
# =============================================================================
# This target provides real TLS functionality using vcpkg's mbedTLS.

# Find mbedTLS from vcpkg
if(NOT TARGET MbedTLS::mbedtls)
    find_package(MbedTLS CONFIG REQUIRED)
endif()

# Get vcpkg mbedTLS include path
get_target_property(MBEDTLS_INCLUDE_DIRS MbedTLS::mbedcrypto INTERFACE_INCLUDE_DIRECTORIES)
message(STATUS "TLS library using mbedTLS from: ${MBEDTLS_INCLUDE_DIRS}")

# Prepend vcpkg mbedTLS include to CMAKE_CXX_FLAGS to ensure it takes precedence
# over DuckDB's bundled mbedTLS headers
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -I${MBEDTLS_INCLUDE_DIRS}")

# Source files for real TLS implementation
set(TLS_SOURCES
    tds_tls_impl.cpp
    tds_tls_context.cpp
)

add_library(mssql_tls_loadable STATIC ${TLS_SOURCES})

target_compile_features(mssql_tls_loadable PRIVATE cxx_std_17)
target_compile_definitions(mssql_tls_loadable PRIVATE MSSQL_TLS_BACKEND_LOADABLE=1)
target_compile_options(mssql_tls_loadable PRIVATE
    "-fvisibility=hidden"
    "-include${CMAKE_CURRENT_SOURCE_DIR}/mbedtls_compat.h"
)

# Add vcpkg mbedTLS includes BEFORE everything else
target_include_directories(mssql_tls_loadable BEFORE PRIVATE
    ${MBEDTLS_INCLUDE_DIRS}
)
target_include_directories(mssql_tls_loadable PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# Link full mbedTLS suite (loadable extension needs everything self-contained)
target_link_libraries(mssql_tls_loadable PRIVATE
    MbedTLS::mbedtls
    MbedTLS::mbedx509
    MbedTLS::mbedcrypto
)

# Filter out DuckDB's global mbedtls include directory from this target
get_target_property(_inc_dirs mssql_tls_loadable INCLUDE_DIRECTORIES)
if(_inc_dirs)
    list(FILTER _inc_dirs EXCLUDE REGEX ".*duckdb.*/mbedtls.*")
    set_target_properties(mssql_tls_loadable PROPERTIES INCLUDE_DIRECTORIES "${_inc_dirs}")
endif()

message(STATUS "TLS target 'mssql_tls_loadable' configured (FULL TLS with vcpkg mbedTLS)")

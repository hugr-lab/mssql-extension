# =============================================================================
# MSSQL Extension - TLS Library
# =============================================================================
# Build a single TLS static library containing mbedTLS.
# This library is linked into both static and loadable extensions.
#
# For loadable: Symbol visibility hides mbedTLS, no conflicts
# For static: We link against our vcpkg mbedTLS which provides full SSL support

cmake_minimum_required(VERSION 3.21)

# Find mbedTLS from vcpkg
if(NOT TARGET MbedTLS::mbedtls)
    find_package(MbedTLS CONFIG REQUIRED)
endif()

# Get vcpkg include path
get_target_property(MBEDTLS_INCLUDE_DIRS MbedTLS::mbedcrypto INTERFACE_INCLUDE_DIRECTORIES)
message(STATUS "TLS library using mbedTLS from: ${MBEDTLS_INCLUDE_DIRS}")

# TLS source files
set(TLS_SOURCES
    tds_tls_impl.cpp
    tds_tls_context.cpp
)

# =============================================================================
# Static library with mbedTLS
# =============================================================================
add_library(mssql_tls STATIC ${TLS_SOURCES})

set_target_properties(mssql_tls PROPERTIES
    INCLUDE_DIRECTORIES "${MBEDTLS_INCLUDE_DIRS};${CMAKE_CURRENT_SOURCE_DIR};${CMAKE_CURRENT_SOURCE_DIR}/../include"
)
target_compile_options(mssql_tls PRIVATE
    "-include${CMAKE_CURRENT_SOURCE_DIR}/mbedtls_compat.h"
    "-fvisibility=hidden"
)
target_link_libraries(mssql_tls PUBLIC
    MbedTLS::mbedtls
    MbedTLS::mbedx509
    MbedTLS::mbedcrypto
)
target_compile_features(mssql_tls PRIVATE cxx_std_17)

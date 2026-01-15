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
)

# Build static extension (linked into DuckDB)
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})

# Build loadable extension (.duckdb_extension file)
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Add version definition to both targets
target_compile_definitions(${EXTENSION_NAME} PRIVATE EXT_VERSION_MSSQL="${DUCKDB_COMMIT_HASH}")
target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE EXT_VERSION_MSSQL="${DUCKDB_COMMIT_HASH}")

# Installation
install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

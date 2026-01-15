# Extension configuration for DuckDB build system
# This file registers the mssql extension with DuckDB

duckdb_extension_load(mssql
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

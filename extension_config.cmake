# Extension configuration for DuckDB build system
# This file registers the mssql extension with DuckDB

duckdb_extension_load(mssql
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# Since DuckDB's 6578b0d5d6e (v2.0-cyanoptera, 2026-09-26) duckdb_extension_load()
# only decides what is built; the shell and the test runner link an extension
# only when a config asks for it. Linked as before the change.
duckdb_extension_statically_link(mssql)

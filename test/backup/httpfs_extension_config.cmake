# Spec 085 PR 2a: test-only extension config. Links httpfs statically into the build the backup
# oracle lane tests with, so the lane reads its backups from R2 through s3:// -- DuckDB's file
# system, the reader's own I/O path (D3) -- rather than a copy made by another client.
#
# httpfs is taken at the commit and with the patches the duckdb submodule itself pins, so the two
# move together on every pin bump. A -dev pin has no published httpfs (INSTALL httpfs fails), which
# is why it is built here rather than installed.
#
# Never referenced from extension_config.cmake: that file ships to community builds.
include(${CMAKE_CURRENT_LIST_DIR}/../../duckdb/.github/config/extensions/httpfs.cmake)
duckdb_extension_statically_link(httpfs)

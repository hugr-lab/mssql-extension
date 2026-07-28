# Spec 054 D2: benchmark-only extension config.
#
# Loads TPC-H (dbgen) for the e2e materialization benches
# (test/bench/bench_tpch_e2e.sh). This file is appended to
# DUCKDB_EXTENSION_CONFIGS by `make bench-build` via the ci-tools
# EXTRA_EXTENSION_CONFIGS hook.
#
# It must NEVER be referenced from extension_config.cmake — that file ships
# to DuckDB community-extension builds, and tpch has no place there.

duckdb_extension_load(tpch)

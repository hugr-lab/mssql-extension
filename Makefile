# Makefile for DuckDB MSSQL Extension
# Compatible with DuckDB Community Extensions CI

PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Extension configuration
EXT_NAME=mssql
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# vcpkg integration
VCPKG_DIR := $(PROJ_DIR)vcpkg
VCPKG_TOOLCHAIN := $(VCPKG_DIR)/scripts/buildsystems/vcpkg.cmake

# Pass vcpkg toolchain to all builds (if vcpkg exists)
ifneq ($(wildcard $(VCPKG_TOOLCHAIN)),)
    EXT_FLAGS := -DCMAKE_TOOLCHAIN_FILE="$(VCPKG_TOOLCHAIN)" -DVCPKG_MANIFEST_DIR="$(PROJ_DIR)"
endif

# Include DuckDB extension CI tools (provides: set_duckdb_version, release, debug, test, etc.)
include extension-ci-tools/makefiles/duckdb_extension.Makefile

#
# Custom targets (preserved from original Makefile)
#

.PHONY: test-cpp vcpkg-setup docker-up docker-down docker-status integration-test test-all test-debug test-simple-query test-multi-instance-pool-isolation test-issue-96-attach-loop test-spec047-us1 test-result-stream-registry-isolation test-spec047-us3 test-token-cache-isolation test-spec047-us-sec test-concurrent-reads bench-build test-column-staging test-row-stager test-row-stager-framing test-index-kind test-load-policy counters-test help

# Bootstrap vcpkg if not present.
# Spec 052 PR #127 CI fix: check for the toolchain file specifically, not just
# the directory — a partially-populated `vcpkg/` (the bare directory without
# the bootstrap-shipped scripts) would otherwise pass the guard, and `make
# debug` would then run without -DCMAKE_TOOLCHAIN_FILE and fail in
# find_package(simdutf).
#
# The original trigger was a GitHub Actions cache restore creating an empty
# `vcpkg/` parent while restoring `vcpkg/installed/`. No workflow caches that
# path any more (the vcpkg binary cache lives in `vcpkg_bincache/`), so that
# specific route is gone — but the file check is strictly more robust than a
# directory check, so it stays.
vcpkg-setup:
	@if [ ! -f "$(VCPKG_TOOLCHAIN)" ]; then \
		echo "Bootstrapping vcpkg..."; \
		if [ ! -d "$(VCPKG_DIR)/.git" ]; then \
			rm -rf $(VCPKG_DIR); \
			git clone https://github.com/microsoft/vcpkg.git $(VCPKG_DIR); \
		fi; \
		$(VCPKG_DIR)/bootstrap-vcpkg.sh; \
	fi

# Docker targets for SQL Server test container
DOCKER_COMPOSE := docker/docker-compose.yml

docker-up:
	@echo "Starting SQL Server test container..."
	docker compose -f $(DOCKER_COMPOSE) up -d sqlserver
	@echo "Waiting for SQL Server to be healthy..."
	@timeout=120; while [ $$timeout -gt 0 ]; do \
		if docker compose -f $(DOCKER_COMPOSE) ps sqlserver | grep -q "healthy"; then \
			echo "SQL Server is ready!"; \
			break; \
		fi; \
		echo "Waiting... ($$timeout seconds remaining)"; \
		sleep 5; \
		timeout=$$((timeout - 5)); \
	done
	@echo "Running init scripts..."
	docker compose -f $(DOCKER_COMPOSE) up sqlserver-init
	@echo "SQL Server is ready for testing!"

docker-down:
	@echo "Stopping SQL Server test container..."
	docker compose -f $(DOCKER_COMPOSE) down

docker-status:
	@echo "SQL Server container status:"
	@docker compose -f $(DOCKER_COMPOSE) ps sqlserver 2>/dev/null || echo "Container not running"
	@echo ""
	@echo "Testing connection..."
	@docker exec mssql-dev /opt/mssql-tools18/bin/sqlcmd -S localhost -U $(MSSQL_TEST_USER) -P '$(MSSQL_TEST_PASS)' -C -Q "SELECT 'Connection OK' AS status" 2>/dev/null || echo "Connection failed - is the container running?"

# Load environment from .env file if it exists
-include .env

# Default test environment variables (can be overridden by .env or command line)
MSSQL_TEST_HOST ?= localhost
MSSQL_TEST_PORT ?= 1433
MSSQL_TEST_USER ?= sa
MSSQL_TEST_PASS ?= TestPassword1
MSSQL_TEST_DB ?= master

# Derived connection strings (computed from base variables)
MSSQL_TEST_DSN = Server=$(MSSQL_TEST_HOST),$(MSSQL_TEST_PORT);Database=$(MSSQL_TEST_DB);User Id=$(MSSQL_TEST_USER);Password=$(MSSQL_TEST_PASS)
MSSQL_TEST_URI = mssql://$(MSSQL_TEST_USER):$(MSSQL_TEST_PASS)@$(MSSQL_TEST_HOST):$(MSSQL_TEST_PORT)/$(MSSQL_TEST_DB)
MSSQL_TEST_DSN_TLS = mssql://$(MSSQL_TEST_USER):$(MSSQL_TEST_PASS)@$(MSSQL_TEST_HOST):$(MSSQL_TEST_PORT)/$(MSSQL_TEST_DB)?encrypt=true
# TestDB connection strings for catalog tests
MSSQL_TESTDB_DSN = Server=$(MSSQL_TEST_HOST),$(MSSQL_TEST_PORT);Database=TestDB;User Id=$(MSSQL_TEST_USER);Password=$(MSSQL_TEST_PASS)
MSSQL_TESTDB_URI = mssql://$(MSSQL_TEST_USER):$(MSSQL_TEST_PASS)@$(MSSQL_TEST_HOST):$(MSSQL_TEST_PORT)/TestDB
# Four COPY tests gate on MSSQL_TEST_SERVER, which nothing has ever set — not
# this Makefile and not CI — so copy_connection_leak, copy_type_mismatch,
# copy_empty_schema and copy_existing_temp have been skipping silently since they
# were written. Same shape as issue #192, one env var lower down.
MSSQL_TEST_SERVER = $(MSSQL_TESTDB_DSN)
# And the same again: MSSQL_TEST_CONNECTION_STRING gates copy_auto_tablock,
# ctas_auto_tablock, ctas_if_not_exists and ddl_if_not_exists, and nothing has
# ever set it either. Two of those cover auto-TABLOCK — the behaviour spec 057
# rewrote — so the tests for that change had never executed once.
MSSQL_TEST_CONNECTION_STRING = $(MSSQL_TESTDB_DSN)

# Export all test environment variables for subprocesses
export MSSQL_TEST_HOST
export MSSQL_TEST_PORT
export MSSQL_TEST_USER
export MSSQL_TEST_PASS
export MSSQL_TEST_DB
export MSSQL_TEST_DSN
export MSSQL_TEST_URI
export MSSQL_TESTDB_DSN
export MSSQL_TESTDB_URI
export MSSQL_TEST_SERVER
export MSSQL_TEST_CONNECTION_STRING
# NOTE: MSSQL_TEST_DSN_TLS is NOT exported by default. Export it manually to
# run TLS-specific tests (requires SQL Server with TLS enabled).
# export MSSQL_TEST_DSN_TLS

# Integration tests - requires SQL Server running
# NOTE: TLS tests are skipped unless MSSQL_TEST_DSN_TLS is exported.
integration-test: release
	@echo "Running integration tests..."
	@echo "NOTE: SQL Server must be running (use 'make docker-up' first)"
	@echo "NOTE: TLS tests skipped unless MSSQL_TEST_DSN_TLS is exported"
	@echo ""
	@echo "Test environment:"
	@echo "  MSSQL_TEST_HOST=$(MSSQL_TEST_HOST)"
	@echo "  MSSQL_TEST_PORT=$(MSSQL_TEST_PORT)"
	@echo "  MSSQL_TEST_USER=$(MSSQL_TEST_USER)"
	@echo "  MSSQL_TEST_DB=$(MSSQL_TEST_DB)"
	@echo "  MSSQL_TEST_DSN=$(MSSQL_TEST_DSN)"
	@echo "  MSSQL_TEST_URI=$(MSSQL_TEST_URI)"
	@echo "  MSSQL_TESTDB_DSN=$(MSSQL_TESTDB_DSN)"
	@echo ""
	@if ! docker compose -f $(DOCKER_COMPOSE) ps sqlserver 2>/dev/null | grep -q "healthy"; then \
		echo "WARNING: SQL Server container not detected. Run 'make docker-up' first."; \
	fi
	@# The path glob, not the group tags. `[integration]` and `[sql]` match only
	@# the 8 files at the TOP level of test/sql — the 164 in subdirectories are
	@# tagged `[mssql]`, and `"[mssql]"` matches NOTHING: sqllogictest registers a
	@# subdirectory file under its path, so the tag never becomes a Catch tag. It
	@# exits 0 having run zero tests, which is why nobody noticed.
	@#
	@# So every `.test` file written since the suite grew subdirectories has been
	@# invisible to this target and to CI. Third instance of the same class on this
	@# branch, and the widest: MSSQL_TEST_SERVER hid 4 files,
	@# MSSQL_TEST_CONNECTION_STRING hides 4 more, this hid 164.
	build/release/test/unittest "test/sql/*" --force-reload

# Run the SQL suite with the performance counters ON (spec 057 step 0b).
#
# Separate from integration-test on purpose. The counters are a code path of
# their own — issue #233 was a crash that ONLY happened with them enabled, on a
# column chunk the stager published as CONSTANT — and a suite that never turns
# them on cannot catch that class again. Kept out of the default run because the
# counters write a summary per stream to stderr, which is noise for everything
# that is not measuring.
counters-test: release
	@echo "Running SQL suite with MSSQL_COUNTERS=1..."
	@echo "NOTE: SQL Server must be running (use 'make docker-up' first)"
	@echo ""
	MSSQL_COUNTERS=1 build/release/test/unittest "test/sql/*" --force-reload

# Run all tests (unit + integration)
test-all: release
	@echo "Running all tests..."
	@echo ""
	@echo "Test environment:"
	@echo "  MSSQL_TEST_HOST=$(MSSQL_TEST_HOST)"
	@echo "  MSSQL_TEST_PORT=$(MSSQL_TEST_PORT)"
	@echo "  MSSQL_TEST_DSN=$(MSSQL_TEST_DSN)"
	@echo ""
	build/release/test/unittest "*mssql*" --force-reload

# Debug test run
test-debug: debug
	@echo "Running tests (debug build)..."
	build/debug/test/unittest "*mssql*" --force-reload

# C++ test sources (TDS layer + query layer - minimal, no DuckDB dependencies)
CPP_TEST_SOURCES := \
    src/tds/tds_connection.cpp \
    src/tds/tds_packet.cpp \
    src/tds/tds_socket.cpp \
    src/tds/tds_types.cpp \
    src/tds/tds_protocol.cpp \
    src/tds/tds_token_parser.cpp \
    src/tds/tds_column_metadata.cpp \
    src/tds/tds_row_reader.cpp \
    src/tds/encoding/utf16.cpp \
    src/tds/tls/tds_tls_stub.cpp \
    src/query/mssql_simple_query.cpp

CPP_TEST_INCLUDES := -I src/include -I duckdb/src/include
CPP_TEST_FLAGS := -std=c++17 -pthread -DMSSQL_TLS_STUB=1 -Wno-deprecated-declarations

# Build and run C++ simple query test
test-simple-query:
	@echo "Building C++ simple query test..."
	@mkdir -p build/test
	$(CXX) $(CPP_TEST_FLAGS) $(CPP_TEST_INCLUDES) \
	    test/cpp/test_simple_query.cpp \
	    $(CPP_TEST_SOURCES) \
	    -o build/test/test_simple_query
	@echo ""
	@echo "Running test..."
	@echo "Test environment:"
	@echo "  MSSQL_TEST_HOST=$(MSSQL_TEST_HOST)"
	@echo "  MSSQL_TEST_PORT=$(MSSQL_TEST_PORT)"
	@echo "  MSSQL_TEST_USER=$(MSSQL_TEST_USER)"
	@echo "  MSSQL_TEST_DB=$(MSSQL_TEST_DB)"
	@echo ""
	build/test/test_simple_query

# Spec 043: LOGIN7 non-ASCII fix + simdutf wrapper unit tests
# Pure in-memory test — does NOT need SQL Server or TLS. Uses the simdutf
# library already built into the debug/release tree by vcpkg.
LOGIN7_TEST_SOURCES := \
    src/tds/tds_packet.cpp \
    src/tds/tds_protocol.cpp \
    src/tds/tds_types.cpp \
    src/tds/encoding/utf16.cpp

LOGIN7_TEST_VCPKG_INSTALLED := build/debug/vcpkg_installed
LOGIN7_TEST_VCPKG_TRIPLET := $(shell ls $(LOGIN7_TEST_VCPKG_INSTALLED) 2>/dev/null | head -n 1)
LOGIN7_TEST_FLAGS := -std=c++17 -pthread -Wno-deprecated-declarations -DMSSQL_BENCH_BUILD
LOGIN7_TEST_INCLUDES := -I src/include -I duckdb/src/include \
    -I $(LOGIN7_TEST_VCPKG_INSTALLED)/$(LOGIN7_TEST_VCPKG_TRIPLET)/include
LOGIN7_TEST_LIBS := -L $(LOGIN7_TEST_VCPKG_INSTALLED)/$(LOGIN7_TEST_VCPKG_TRIPLET)/debug/lib -lsimdutf

test-login7-encoding: debug
	@echo "Building LOGIN7 + simdutf wrapper unit test..."
	@mkdir -p build/test
	@if [ -z "$(LOGIN7_TEST_VCPKG_TRIPLET)" ]; then \
		echo "ERROR: $(LOGIN7_TEST_VCPKG_INSTALLED) has no triplet subdir; run 'make debug' first." >&2; \
		exit 1; \
	fi
	$(CXX) $(LOGIN7_TEST_FLAGS) $(LOGIN7_TEST_INCLUDES) \
	    test/cpp/test_login7_encoding.cpp \
	    $(LOGIN7_TEST_SOURCES) \
	    $(LOGIN7_TEST_LIBS) \
	    -o build/test/test_login7_encoding
	@echo ""
	@echo "Running LOGIN7 + simdutf unit test..."
	build/test/test_login7_encoding

# LOGIN7 response parser tests (issue #164 State byte + #183 length clamps +
# fuzz-found FEDAUTHINFO/ENVCHANGE OOB fixes). Same TDS-only source set as
# test-login7-encoding; mirrors what .github/workflows/ci.yml compiles.
test-login-error-state: debug
	@echo "Building LOGIN7 response-parser unit test..."
	@mkdir -p build/test
	@if [ -z "$(LOGIN7_TEST_VCPKG_TRIPLET)" ]; then \
		echo "ERROR: $(LOGIN7_TEST_VCPKG_INSTALLED) has no triplet subdir; run 'make debug' first." >&2; \
		exit 1; \
	fi
	$(CXX) $(LOGIN7_TEST_FLAGS) $(LOGIN7_TEST_INCLUDES) \
	    test/cpp/test_login_error_state.cpp \
	    $(LOGIN7_TEST_SOURCES) \
	    $(LOGIN7_TEST_LIBS) \
	    -o build/test/test_login_error_state
	@echo ""
	@echo "Running LOGIN7 response-parser unit test..."
	build/test/test_login_error_state

# Spec 053 (#161): lazy GSSAPI/krb5 runtime loader unit test.
# Pure in-memory — does NOT need SQL Server or a KDC. On Linux it links only
# libdl (NOT libgssapi/libkrb5), demonstrating the no-link property: the shim
# resolves gss_*/krb5_* at runtime via dlopen. Headers come from pkg-config.
# On macOS the GSS system framework is linked (the macOS path uses direct
# symbol addresses).
GSSRT_TEST_FLAGS := -std=c++17 -pthread -Wno-deprecated-declarations -DMSSQL_ENABLE_KRB5=1
GSSRT_TEST_INCLUDES := -I src/include
GSSRT_TEST_UNAME := $(shell uname -s)
ifeq ($(GSSRT_TEST_UNAME),Darwin)
GSSRT_TEST_PLATFORM_LIBS := -framework GSS
else
GSSRT_TEST_INCLUDES += $(shell pkg-config --cflags krb5-gssapi krb5 2>/dev/null)
GSSRT_TEST_PLATFORM_LIBS := -ldl
endif

test-gssapi-runtime:
	@echo "Building GSSAPI runtime loader unit test (spec 053)..."
	@mkdir -p build/test
	$(CXX) $(GSSRT_TEST_FLAGS) $(GSSRT_TEST_INCLUDES) \
	    test/cpp/test_gssapi_runtime.cpp \
	    src/tds/auth/gssapi_runtime.cpp \
	    $(GSSRT_TEST_PLATFORM_LIBS) \
	    -o build/test/test_gssapi_runtime
	@echo ""
	@echo "Running gssapi_runtime unit test..."
	build/test/test_gssapi_runtime

# Spec 049 (#85): MSSQLIndexKind / sys.indexes.type mapping unit test.
# Pure in-memory — no SQL Server, no vcpkg, and nothing to link: the mapper is
# in a self-contained header, so -I src/include is the whole dependency.
# This is what keeps the catalog's index_kind honest until the write path
# consumes it (PR #223 review).
INDEX_KIND_TEST_FLAGS := -std=c++17 -pthread -Wno-deprecated-declarations
INDEX_KIND_TEST_INCLUDES := -I src/include

test-index-kind:
	@echo "Building MSSQLIndexKind unit test (spec 049, #85)..."
	@mkdir -p build/test
	$(CXX) $(INDEX_KIND_TEST_FLAGS) $(INDEX_KIND_TEST_INCLUDES) \
	    test/cpp/test_index_kind.cpp \
	    -o build/test/test_index_kind
	@echo ""
	@echo "Running MSSQLIndexKind unit test..."
	build/test/test_index_kind

# Spec 063 D1: MSSQLResolveLoadPolicy — who supplies a bulk-load writer's
# connection, and how many writers there may be.
#
# Pure in-memory, nothing to link: copy/load_policy.hpp is a self-contained
# header of plain types for exactly this reason. Every case it decides is one
# where being wrong is SILENT (a writer handed the pinned connection, a second
# writer whose rows do not roll back, a second writer against a session-scoped
# `#temp` target), so an end-to-end test would see a load that succeeded.
LOAD_POLICY_TEST_FLAGS := -std=c++17 -pthread -Wno-deprecated-declarations
LOAD_POLICY_TEST_INCLUDES := -I src/include

test-load-policy:
	@echo "Building load-policy unit test (spec 063 D1)..."
	@mkdir -p build/test
	$(CXX) $(LOAD_POLICY_TEST_FLAGS) $(LOAD_POLICY_TEST_INCLUDES) \
	    test/cpp/test_load_policy.cpp \
	    -o build/test/test_load_policy
	@echo ""
	@echo "Running load-policy unit test..."
	build/test/test_load_policy

# ---------------------------------------------------------------------------
# Standalone C++ unit tests (no Catch, no SQL Server, own main()).
#
# These 12 files existed in test/cpp/ for a long time WITHOUT being built by
# anything — not CMake, not this Makefile, not CI. Nothing compiled them, so
# nothing noticed when the code moved underneath: four expectations were stale
# (HUGEINT and INTERVAL "unsupported" since spec 045 made them DECIMAL(38,0) and
# NVARCHAR(50); TIMESTAMP as DATETIME2(7); an identifier-quoting case with one
# bracket too many), and one described output the code no longer produced. A
# test nobody runs is documentation that looks like a guarantee.
#
# They link against the built extension archive, so `make` (release) first.
# ---------------------------------------------------------------------------
STANDALONE_TEST_SOURCES := \
    test/cpp/test_ddl_translator.cpp \
    test/cpp/test_ctas_type_mapping.cpp \
    test/cpp/test_catalog_filter.cpp \
    test/cpp/test_insert_bulk_sql.cpp \
    test/cpp/codec/test_binary_codec.cpp \
    test/cpp/codec/test_boolean_codec.cpp \
    test/cpp/codec/test_datetime_codec.cpp \
    test/cpp/codec/test_decimal_codec.cpp \
    test/cpp/codec/test_float_codec.cpp \
    test/cpp/codec/test_integer_codec.cpp \
    test/cpp/codec/test_money_codec.cpp \
    test/cpp/codec/test_string_codec.cpp \
    test/cpp/codec/test_uuid_codec.cpp

STANDALONE_TEST_FLAGS := -std=c++17 -pthread -Wno-deprecated-declarations
STANDALONE_TEST_INCLUDES := -I src/include -I duckdb/src/include
STANDALONE_TEST_VCPKG_LIB := $(firstword $(wildcard build/release/vcpkg_installed/*/lib))
STANDALONE_TEST_UNAME := $(shell uname -s)
ifeq ($(STANDALONE_TEST_UNAME),Darwin)
STANDALONE_TEST_PLATFORM_LIBS := -framework GSS -framework CoreFoundation -framework Security
else
STANDALONE_TEST_PLATFORM_LIBS := -lgssapi_krb5 -ldl -lrt
endif

test-cpp: release
	@echo "Building standalone C++ unit tests..."
	@mkdir -p build/test
	@if [ ! -f build/release/extension/mssql/libmssql_extension.a ]; then \
		echo "ERROR: build/release/extension/mssql/libmssql_extension.a missing; run 'make' first." >&2; \
		exit 1; \
	fi
	@fail=0; \
	libs="build/release/extension/mssql/libmssql_extension.a build/release/src/libduckdb_static.a \
	      $$(find build/release/extension build/release/third_party -name '*.a' 2>/dev/null | grep -v mssql | tr '\n' ' ') \
	      build/release/src/libduckdb_static.a \
	      $(STANDALONE_TEST_VCPKG_LIB)/libssl.a $(STANDALONE_TEST_VCPKG_LIB)/libcrypto.a $(STANDALONE_TEST_VCPKG_LIB)/libsimdutf.a"; \
	for f in $(STANDALONE_TEST_SOURCES); do \
		n=$$(basename $$f .cpp); \
		$(CXX) $(STANDALONE_TEST_FLAGS) $(STANDALONE_TEST_INCLUDES) $$f $$libs $(STANDALONE_TEST_PLATFORM_LIBS) \
		    -o build/test/$$n 2>build/test/$$n.log || { echo "  BUILD FAIL $$n (see build/test/$$n.log)"; fail=1; continue; }; \
		if build/test/$$n >build/test/$$n.out 2>&1; then echo "  PASS $$n"; \
		else echo "  FAIL $$n"; tail -5 build/test/$$n.out; fail=1; fi; \
	done; \
	exit $$fail

# Spec 045: SQL Server Browser parser unit tests (Phase 0).
# Pure unit test — no SQL Server, no vcpkg, no DuckDB linkage required.
# Compiles the resolver TU together with the test driver as a standalone
# binary. Phase 1 will extend the same target with a loopback UDP listener
# test (still no external network).
INSTANCE_RESOLVER_TEST_SOURCES := src/connection/instance_resolver.cpp
INSTANCE_RESOLVER_TEST_FLAGS := -std=c++17 -pthread -Wno-deprecated-declarations
INSTANCE_RESOLVER_TEST_INCLUDES := -I src/include

test-instance-resolver:
	@echo "Building SQL Browser parser unit test (spec 045, Phase 0)..."
	@mkdir -p build/test
	$(CXX) $(INSTANCE_RESOLVER_TEST_FLAGS) $(INSTANCE_RESOLVER_TEST_INCLUDES) \
	    test/cpp/test_instance_resolver.cpp \
	    $(INSTANCE_RESOLVER_TEST_SOURCES) \
	    -o build/test/test_instance_resolver
	@echo ""
	@echo "Running SQL Browser parser unit test..."
	build/test/test_instance_resolver

# TDS token-parser security regression (ASan + UBSan). Reproduces the ERROR/INFO
# under-declared-length heap-buffer-overflow found by fuzzing (fuzz/fuzz_tds_tokens);
# crashes under ASan on a regression. No SQL Server; uses vcpkg simdutf like login7.
TOKEN_SEC_TEST_SOURCES := \
    src/tds/tds_token_parser.cpp \
    src/tds/tds_row_reader.cpp \
    src/tds/tds_column_metadata.cpp \
    src/tds/tds_types.cpp \
    src/tds/encoding/utf16.cpp
TOKEN_SEC_TEST_FLAGS := -std=c++17 -g -O1 -pthread -Wno-deprecated-declarations \
    -fsanitize=address,undefined -fno-sanitize-recover=all
TOKEN_SEC_TEST_INCLUDES := -I src/include -I duckdb/src/include \
    -I $(LOGIN7_TEST_VCPKG_INSTALLED)/$(LOGIN7_TEST_VCPKG_TRIPLET)/include
TOKEN_SEC_TEST_LIBS := -L $(LOGIN7_TEST_VCPKG_INSTALLED)/$(LOGIN7_TEST_VCPKG_TRIPLET)/debug/lib -lsimdutf

test-token-parser-security: debug
	@echo "Building TDS token-parser security regression (ASan+UBSan)..."
	@mkdir -p build/test
	@if [ -z "$(LOGIN7_TEST_VCPKG_TRIPLET)" ]; then \
		echo "ERROR: $(LOGIN7_TEST_VCPKG_INSTALLED) has no triplet subdir; run 'make debug' first." >&2; \
		exit 1; \
	fi
	$(CXX) $(TOKEN_SEC_TEST_FLAGS) $(TOKEN_SEC_TEST_INCLUDES) \
	    test/cpp/test_token_parser_security.cpp \
	    $(TOKEN_SEC_TEST_SOURCES) \
	    $(TOKEN_SEC_TEST_LIBS) \
	    -o build/test/test_token_parser_security
	@echo ""
	@echo "Running TDS token-parser security regression..."
	build/test/test_token_parser_security

# Spec 054 D2: benchmark build — release build with TPC-H (dbgen) enabled for
# the e2e materialization benches (test/bench/bench_tpch_e2e.sh).
# tpch must NOT go into extension_config.cmake (that file ships to community
# builds); test/bench/bench_extension_config.cmake carries it and is prepended
# here via the ci-tools EXTRA_EXTENSION_CONFIGS hook.
# NOTE: a later plain `make`/`make release` reconfigures WITHOUT tpch (cmake
# drops it on re-run) — re-run `make bench-build` before benchmarking.
bench-build:
	EXTRA_EXTENSION_CONFIGS='$(PROJ_DIR)test/bench/bench_extension_config.cmake' $(MAKE) release

# Spec 044: codec microbenchmark — simdutf vs legacy hand-rolled converter.
# Manual target; NOT part of `make test` or any CI workflow.
# Requires `make debug` first to populate build/debug/vcpkg_installed.
BENCH_UTF16_VCPKG_INSTALLED := build/release/vcpkg_installed
BENCH_UTF16_VCPKG_TRIPLET := $(shell ls $(BENCH_UTF16_VCPKG_INSTALLED) 2>/dev/null | head -n 1)
BENCH_UTF16_SOURCES := src/tds/encoding/utf16.cpp
BENCH_UTF16_FLAGS := -std=c++17 -O3 -pthread -Wno-deprecated-declarations -DMSSQL_BENCH_BUILD
BENCH_UTF16_INCLUDES := -I src/include -I duckdb/src/include \
    -I $(BENCH_UTF16_VCPKG_INSTALLED)/$(BENCH_UTF16_VCPKG_TRIPLET)/include
# Link against the RELEASE simdutf (optimized SIMD path). The debug build
# of simdutf disables intrinsics and is dramatically slower; using it for
# a perf benchmark would be misleading.
BENCH_UTF16_LIBS := -L $(BENCH_UTF16_VCPKG_INSTALLED)/$(BENCH_UTF16_VCPKG_TRIPLET)/lib -lsimdutf

bench-utf16: release
	@echo "Building UTF-16 codec microbenchmark (spec 044)..."
	@mkdir -p build/test
	@if [ -z "$(BENCH_UTF16_VCPKG_TRIPLET)" ]; then \
		echo "ERROR: $(BENCH_UTF16_VCPKG_INSTALLED) has no triplet subdir; run 'make release' first." >&2; \
		exit 1; \
	fi
	$(CXX) $(BENCH_UTF16_FLAGS) $(BENCH_UTF16_INCLUDES) \
	    test/cpp/bench_utf16.cpp \
	    $(BENCH_UTF16_SOURCES) \
	    $(BENCH_UTF16_LIBS) \
	    -o build/test/bench_utf16
	@echo ""
	@echo "Running UTF-16 codec microbenchmark..."
	build/test/bench_utf16

# Spec 054 D1: materialization microbenchmark (string decode / fixed decode /
# bcp encode groups). Manual target; NOT part of `make test` or CI.
# Links the RELEASE libduckdb (Vector/DataChunk symbols) + release simdutf,
# compiles the codec sources at -O3 so the timed body matches shipped code.
BENCH_MAT_FLAGS := -std=c++17 -O3 -pthread -Wno-deprecated-declarations -DMSSQL_BENCH_BUILD
BENCH_MAT_SOURCES := $(wildcard src/codec/*.cpp) \
    src/tds/encoding/bcp_row_encoder.cpp \
    src/tds/encoding/utf16.cpp \
    src/tds/encoding/datetime_encoding.cpp \
    src/tds/encoding/decimal_encoding.cpp \
    src/tds/encoding/guid_encoding.cpp

# Deliberately NOT dependent on `release`: re-running the release configure
# here would drop tpch from a `make bench-build` tree (the two targets share
# build/release). Requires an existing release build (either flavor).
bench-materialize:
	@echo "Building materialization microbenchmark (spec 054)..."
	@mkdir -p build/test
	@if [ -z "$(BENCH_UTF16_VCPKG_TRIPLET)" ] || ! ls build/release/src/libduckdb* >/dev/null 2>&1; then \
		echo "ERROR: no release build found; run 'make release' or 'make bench-build' first." >&2; \
		exit 1; \
	fi
	$(CXX) $(BENCH_MAT_FLAGS) $(BENCH_UTF16_INCLUDES) \
	    test/cpp/bench_materialize.cpp \
	    $(BENCH_MAT_SOURCES) \
	    $(BENCH_UTF16_LIBS) \
	    -L build/release/src -lduckdb \
	    -o build/test/bench_materialize
	@echo ""
	@echo "Running materialization microbenchmark..."
	DYLD_LIBRARY_PATH=build/release/src LD_LIBRARY_PATH=build/release/src build/test/bench_materialize


# Spec 055 D3: column staging structures + arena ownership/watermark tests.
# Pure in-memory — no SQL Server, no conversion. Links libduckdb for idx_t /
# duckdb::vector and the exception types only.
CODEC_STAGING_TEST_FLAGS := -std=c++17 -O1 -pthread -Wno-deprecated-declarations
test-column-staging: release
	@echo "Building codec::staging unit test (spec 055 D3)..."
	@mkdir -p build/test
	$(CXX) $(CODEC_STAGING_TEST_FLAGS) -I src/include -I duckdb/src/include \
	    test/cpp/codec/test_column_staging.cpp \
	    src/codec/staging/column_staging.cpp \
	    src/codec/staging/column_ops.cpp \
	    src/tds/encoding/type_converter.cpp \
	    src/tds/tds_column_metadata.cpp \
	    -L build/release/src -lduckdb \
	    -o build/test/test_column_staging
	@echo ""
	DYLD_LIBRARY_PATH=build/release/src LD_LIBRARY_PATH=build/release/src build/test/test_column_staging

# Staged-read-path C++ tests (specs 055 / 059).
#
# Both targets go through test/cpp/codec/run_staging_test.sh so the source list
# lives in exactly ONE place, shared with the CI job — a second copy is exactly
# the drift the framing test exists to catch. The vcpkg prefix is passed in
# rather than derived inside the script: make and CI each already know their own
# triplet.
#
# test-row-stager         (spec 059 D1b) — the ROW / NBCROW walks over synthetic
#                         rows, decoded and checked value by value.
# test-row-stager-framing (spec 055 T5)  — the same walks pinned against
#                         RowReader::SkipRow, which is what makes their absence
#                         of per-value bounds checks safe.
#
# ASan hangs at process init on Darwin 25.5, so on a macOS dev box the sanitized
# binary cannot run at all and SANITIZE=0 is passed there. That keeps the
# assertions; CI runs both legs sanitized (its macOS runner is older and fine),
# so the heap-over-read half is covered there.
STAGING_TEST_SANITIZE := $(shell [ "$$(uname)" = "Darwin" ] && echo 0 || echo 1)

test-row-stager: release
	@if [ -z "$(BENCH_UTF16_VCPKG_TRIPLET)" ]; then \
		echo "ERROR: $(BENCH_UTF16_VCPKG_INSTALLED) has no triplet subdir; run 'make release' first." >&2; \
		exit 1; \
	fi
	SANITIZE=$(STAGING_TEST_SANITIZE) test/cpp/codec/run_staging_test.sh \
	    $(BENCH_UTF16_VCPKG_INSTALLED)/$(BENCH_UTF16_VCPKG_TRIPLET) \
	    test/cpp/codec/test_row_stager.cpp build/release

test-row-stager-framing: release
	@if [ -z "$(BENCH_UTF16_VCPKG_TRIPLET)" ]; then \
		echo "ERROR: $(BENCH_UTF16_VCPKG_INSTALLED) has no triplet subdir; run 'make release' first." >&2; \
		exit 1; \
	fi
	SANITIZE=$(STAGING_TEST_SANITIZE) test/cpp/codec/run_staging_test.sh \
	    $(BENCH_UTF16_VCPKG_INSTALLED)/$(BENCH_UTF16_VCPKG_TRIPLET) \
	    test/cpp/codec/test_row_stager_framing.cpp build/release

# Spec 045: per-type-family codec unit tests
# Pattern target: `make test-codec-<family>` builds and runs
# test/cpp/codec/test_<family>_codec.cpp linked against src/codec/*.cpp.
# Supported family names: boolean integer float decimal money string binary
# datetime uuid. Plus `make test-literal-format` for the shared dispatcher.
#
# All codec tests need DuckDB headers (LogicalType / Value / Vector) so they
# follow the spec-043 LOGIN7 test pattern (CXX direct compile + vcpkg lib
# linkage). Each test links in ALL codec sources (so cross-family forwards
# like HUGEINT→Decimal in the Integer module resolve) plus the encoding
# helpers (utf16, datetime_encoding, decimal_encoding, guid_encoding).
#
# Files are populated as families migrate (Phase 2+). The targets exist
# from Phase 1 but will print an explanatory error if the test or family
# sources are missing.
CODEC_TEST_VCPKG_INSTALLED := build/debug/vcpkg_installed
CODEC_TEST_VCPKG_TRIPLET := $(shell ls $(CODEC_TEST_VCPKG_INSTALLED) 2>/dev/null | head -n 1)
CODEC_TEST_FLAGS := -std=c++17 -pthread -Wno-deprecated-declarations
CODEC_TEST_INCLUDES := -I src/include -I duckdb/src/include \
    -I $(CODEC_TEST_VCPKG_INSTALLED)/$(CODEC_TEST_VCPKG_TRIPLET)/include
# Link against built libduckdb.dylib (built by `make debug`) for Value/Vector/hugeint
# symbols. Tests run with DYLD_LIBRARY_PATH set so the loader can find it.
CODEC_TEST_LIBS := -L $(CODEC_TEST_VCPKG_INSTALLED)/$(CODEC_TEST_VCPKG_TRIPLET)/debug/lib -lsimdutf \
    -L build/debug/src -lduckdb
CODEC_TEST_RPATH := DYLD_LIBRARY_PATH=build/debug/src LD_LIBRARY_PATH=build/debug/src
CODEC_TEST_ENCODING_SOURCES := \
    src/tds/encoding/utf16.cpp \
    src/tds/encoding/datetime_encoding.cpp \
    src/tds/encoding/decimal_encoding.cpp \
    src/tds/encoding/guid_encoding.cpp \
    src/tds/encoding/bcp_row_encoder.cpp
# bcp_row_encoder.cpp is required: integer/decimal EncodeToBcp call
# BCPRowEncoder::EncodeDecimal, which is defined there (not in a codec source).
# Without it `make test-codec-integer` / `test-codec-decimal` fail to link.
# CODEC_TEST_FAMILY_SOURCES is appended by Phase 2 (T011) and each family
# migration phase as $(wildcard src/codec/*.cpp) once stub files exist.
CODEC_TEST_FAMILY_SOURCES := $(wildcard src/codec/*.cpp)

test-codec-%: debug
	@echo "Building codec unit test for family: $*"
	@mkdir -p build/test
	@if [ -z "$(CODEC_TEST_VCPKG_TRIPLET)" ]; then \
		echo "ERROR: $(CODEC_TEST_VCPKG_INSTALLED) has no triplet subdir; run 'make debug' first." >&2; \
		exit 1; \
	fi
	@if [ ! -f test/cpp/codec/test_$*_codec.cpp ]; then \
		echo "ERROR: test/cpp/codec/test_$*_codec.cpp does not exist yet (Phase 1 scaffolding;" >&2; \
		echo "       the test file is written when the $* family migrates in Phase 3 or later)." >&2; \
		exit 1; \
	fi
	$(CXX) $(CODEC_TEST_FLAGS) $(CODEC_TEST_INCLUDES) \
	    test/cpp/codec/test_$*_codec.cpp \
	    $(CODEC_TEST_FAMILY_SOURCES) \
	    $(CODEC_TEST_ENCODING_SOURCES) \
	    $(CODEC_TEST_LIBS) \
	    -o build/test/test_$*_codec
	@echo ""
	@echo "Running codec unit test for $*..."
	$(CODEC_TEST_RPATH) build/test/test_$*_codec

# TypeConverter VARCHAR-fallback test (issue #89 regression — spec 045 Phase 6 sub-phase 3).
# Exercises the "catalog says VARCHAR but TDS returns non-string" path that views with
# CAST/CONVERT can trigger.
test-type-converter-fallback: debug
	@echo "Building TypeConverter VARCHAR-fallback test..."
	@mkdir -p build/test
	@if [ -z "$(CODEC_TEST_VCPKG_TRIPLET)" ]; then \
		echo "ERROR: $(CODEC_TEST_VCPKG_INSTALLED) has no triplet subdir; run 'make debug' first." >&2; \
		exit 1; \
	fi
	$(CXX) $(CODEC_TEST_FLAGS) $(CODEC_TEST_INCLUDES) \
	    test/cpp/codec/test_type_converter_fallback.cpp \
	    src/tds/encoding/type_converter.cpp \
	    $(CODEC_TEST_FAMILY_SOURCES) \
	    $(CODEC_TEST_ENCODING_SOURCES) \
	    $(CODEC_TEST_LIBS) \
	    -o build/test/test_type_converter_fallback
	@echo ""
	@echo "Running TypeConverter VARCHAR-fallback test..."
	$(CODEC_TEST_RPATH) build/test/test_type_converter_fallback

# Shared literal_format dispatcher test (covers LiteralContext divergence cases)
test-literal-format: debug
	@echo "Building shared literal_format test..."
	@mkdir -p build/test
	@if [ -z "$(CODEC_TEST_VCPKG_TRIPLET)" ]; then \
		echo "ERROR: $(CODEC_TEST_VCPKG_INSTALLED) has no triplet subdir; run 'make debug' first." >&2; \
		exit 1; \
	fi
	@if [ ! -f test/cpp/test_literal_format.cpp ]; then \
		echo "ERROR: test/cpp/test_literal_format.cpp does not exist yet (Phase 1 scaffolding;" >&2; \
		echo "       written when US2 lands in Phase 4)." >&2; \
		exit 1; \
	fi
	$(CXX) $(CODEC_TEST_FLAGS) $(CODEC_TEST_INCLUDES) \
	    test/cpp/test_literal_format.cpp \
	    $(CODEC_TEST_FAMILY_SOURCES) \
	    $(CODEC_TEST_ENCODING_SOURCES) \
	    $(CODEC_TEST_LIBS) \
	    -o build/test/test_literal_format
	@echo ""
	@echo "Running shared literal_format test..."
	$(CODEC_TEST_RPATH) build/test/test_literal_format

# Spec 047 — US1 acceptance tests. Two C++ standalone binaries that link
# the debug DuckDB shared library and exercise the extension's catalog +
# pool ownership via real ATTACH / mssql_scan calls.
#
# Each binary auto-skips when MSSQL_TEST_PASS is unset (env-var gate per
# the same pattern as test_multi_connection_transactions.cpp).
#
# Targets:
#   test-multi-instance-pool-isolation  — Scenarios 1/2/3 (SC-001/002/003)
#   test-issue-96-attach-loop           — Scenario 4   (SC-009, closes #96)
#   test-spec047-us1                    — meta target: builds + runs both
SPEC047_TEST_FLAGS := -std=c++17 -pthread -Wno-deprecated-declarations
SPEC047_TEST_INCLUDES := -I duckdb/src/include
SPEC047_TEST_LIBS := -L build/debug/src -lduckdb

# On Linux, libduckdb.so is ASan/UBSan-instrumented (DuckDB CMake default for
# Debug) but our test binaries are NOT compiled with -fsanitize=* (these
# Makefile rules keep the test build platform-agnostic). glibc loader
# requires libasan come FIRST in the initial library list — otherwise:
#   ==NNN==ASan runtime does not come first in initial library list;
#   you should either link runtime to your application or manually
#   preload it with LD_PRELOAD
# Set LD_PRELOAD only on the run prefix (NOT a make-wide env var — that
# would propagate into any cmake/vcpkg sub-invocation triggered by a
# `make test-…: debug` dependency, and break vcpkg's compiler-detection
# probe). macOS dyld handles ASan-via-linked-lib transparently, so no
# preload needed there.
ifeq ($(shell uname),Linux)
SANITIZER_PRELOAD := LD_PRELOAD=$(shell gcc -print-file-name=libasan.so):$(shell gcc -print-file-name=libubsan.so)
else
SANITIZER_PRELOAD :=
endif
SPEC047_TEST_RPATH := $(SANITIZER_PRELOAD) DYLD_LIBRARY_PATH=build/debug/src LD_LIBRARY_PATH=build/debug/src

test-multi-instance-pool-isolation: debug
	@echo "Building spec 047 multi-instance pool isolation test (T023)..."
	@mkdir -p build/test
	$(CXX) $(SPEC047_TEST_FLAGS) $(SPEC047_TEST_INCLUDES) \
	    test/cpp/test_multi_instance_pool_isolation.cpp \
	    $(SPEC047_TEST_LIBS) \
	    -o build/test/test_multi_instance_pool_isolation
	@echo ""
	@echo "Running spec 047 multi-instance pool isolation test..."
	$(SPEC047_TEST_RPATH) build/test/test_multi_instance_pool_isolation

test-issue-96-attach-loop: debug
	@echo "Building spec 047 issue #96 ATTACH-loop regression test (T024)..."
	@mkdir -p build/test
	$(CXX) $(SPEC047_TEST_FLAGS) $(SPEC047_TEST_INCLUDES) \
	    test/cpp/test_issue_96_attach_loop.cpp \
	    $(SPEC047_TEST_LIBS) \
	    -o build/test/test_issue_96_attach_loop
	@echo ""
	@echo "Running spec 047 issue #96 ATTACH-loop regression test..."
	$(SPEC047_TEST_RPATH) build/test/test_issue_96_attach_loop

test-spec047-us1: test-multi-instance-pool-isolation test-issue-96-attach-loop
	@echo ""
	@echo "All spec 047 US1 acceptance tests PASSED (SC-001, SC-002, SC-003, SC-009)"

test-result-stream-registry-isolation: debug
	@echo "Building spec 047 result-stream registry isolation test (T040)..."
	@mkdir -p build/test
	$(CXX) $(SPEC047_TEST_FLAGS) $(SPEC047_TEST_INCLUDES) \
	    test/cpp/test_result_stream_registry_isolation.cpp \
	    $(SPEC047_TEST_LIBS) \
	    -o build/test/test_result_stream_registry_isolation
	@echo ""
	@echo "Running spec 047 result-stream registry isolation test..."
	$(SPEC047_TEST_RPATH) build/test/test_result_stream_registry_isolation

test-spec047-us3: test-result-stream-registry-isolation
	@echo ""
	@echo "Spec 047 US3 acceptance test PASSED (SC-006)"

# Concurrent reads stress test (dbt threads>=2 scenario reproduction).
# Mixed mssql_scan + catalog-bound SELECT across N threads sharing a single
# ATTACH; also scenario with N concurrent ATTACHes (different aliases).
test-concurrent-reads: debug
	@echo "Building concurrent-reads stress test..."
	@mkdir -p build/test
	$(CXX) $(SPEC047_TEST_FLAGS) $(SPEC047_TEST_INCLUDES) \
	    test/cpp/test_concurrent_reads.cpp \
	    $(SPEC047_TEST_LIBS) \
	    -o build/test/test_concurrent_reads
	@echo ""
	@echo "Running concurrent-reads stress test..."
	$(SPEC047_TEST_RPATH) build/test/test_concurrent_reads

# Spec 047 US-SEC: TokenCache per-DatabaseInstance namespace isolation (T046g, SC-011).
# Compiles src/azure/azure_token.cpp together with the test driver. The driver
# stubs HttpPost / ReadAzureSecret / AcquireInteractiveToken so AcquireToken's
# call graph links cleanly without dragging in httplib, OpenSSL, or the DuckDB
# Secret API. Test only exercises TokenCache::Set/Get/Has/Invalidate.
test-token-cache-isolation: debug
	@echo "Building spec 047 TokenCache isolation test (T046g)..."
	@mkdir -p build/test
	$(CXX) $(SPEC047_TEST_FLAGS) $(SPEC047_TEST_INCLUDES) -I src/include \
	    test/cpp/test_token_cache_isolation.cpp \
	    src/azure/azure_token.cpp \
	    $(SPEC047_TEST_LIBS) \
	    -o build/test/test_token_cache_isolation
	@echo ""
	@echo "Running spec 047 TokenCache isolation test..."
	$(SPEC047_TEST_RPATH) build/test/test_token_cache_isolation

test-spec047-us-sec: test-token-cache-isolation
	@echo ""
	@echo "Spec 047 US-SEC TokenCache isolation test PASSED (SC-011)"

# Show help
help:
	@echo "DuckDB MSSQL Extension Build System"
	@echo ""
	@echo "Standard CI targets (from extension-ci-tools):"
	@echo "  make release              - Build release version"
	@echo "  make debug                - Build debug version"
	@echo "  make test                 - Run unit tests"
	@echo "  make set_duckdb_version   - Set DuckDB version (use DUCKDB_GIT_VERSION=v1.x.x)"
	@echo ""
	@echo "Custom targets:"
	@echo "  make vcpkg-setup          - Bootstrap vcpkg (required for TLS support)"
	@echo "  make integration-test     - Run integration tests (requires SQL Server)"
	@echo "  make counters-test        - Run the SQL suite with MSSQL_COUNTERS=1 (exercises the counter path)"
	@echo "  make test-all             - Run all tests"
	@echo "  make test-debug           - Run tests with debug build"
	@echo "  make test-simple-query    - Run C++ simple query test"
	@echo "  make docker-up            - Start SQL Server test container"
	@echo "  make docker-down          - Stop SQL Server test container"
	@echo "  make docker-status        - Check SQL Server container status"
	@echo "  make help                 - Show this help"
	@echo ""
	@echo "Examples:"
	@echo "  make vcpkg-setup && make release"
	@echo "  DUCKDB_GIT_VERSION=v1.4.3 make set_duckdb_version"
	@echo "  make docker-up && make integration-test"
	@echo ""
	@echo "Extension will be built at:"
	@echo "  build/release/extension/mssql/mssql.duckdb_extension"

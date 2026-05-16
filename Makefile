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

.PHONY: vcpkg-setup docker-up docker-down docker-status integration-test test-all test-debug test-simple-query help

# Bootstrap vcpkg if not present
vcpkg-setup:
	@if [ ! -d "$(VCPKG_DIR)" ]; then \
		echo "Bootstrapping vcpkg..."; \
		git clone https://github.com/microsoft/vcpkg.git $(VCPKG_DIR); \
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
	build/release/test/unittest "[integration]" --force-reload
	build/release/test/unittest "[sql]" --force-reload

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
    src/tds/encoding/guid_encoding.cpp
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

# Makefile for DuckDB MSSQL Extension
# Builds extension using DuckDB's build system

.PHONY: all release debug clean configure test help docker-up docker-down docker-status integration-test vcpkg-setup

# Default generator (can be overridden: GEN=ninja make release)
GEN ?= Ninja

# Build directory
BUILD_DIR := build

# Extension config path (absolute path)
EXT_CONFIG := $(shell pwd)/extension_config.cmake

# DuckDB source directory
DUCKDB_DIR := $(shell pwd)/duckdb

# vcpkg setup
VCPKG_DIR := $(shell pwd)/vcpkg
VCPKG_TOOLCHAIN := $(VCPKG_DIR)/scripts/buildsystems/vcpkg.cmake

# Default target
all: release

# Bootstrap vcpkg if not present
vcpkg-setup:
	@if [ ! -d "$(VCPKG_DIR)" ]; then \
		echo "Bootstrapping vcpkg..."; \
		git clone https://github.com/microsoft/vcpkg.git $(VCPKG_DIR); \
		$(VCPKG_DIR)/bootstrap-vcpkg.sh; \
	fi

# Release build - uses DuckDB's CMakeLists.txt as root
release: vcpkg-setup
	mkdir -p $(BUILD_DIR)/release
	cd $(BUILD_DIR)/release && cmake -G "$(GEN)" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$(VCPKG_TOOLCHAIN)" -DVCPKG_MANIFEST_DIR="$(shell pwd)" -DDUCKDB_EXTENSION_CONFIGS="$(EXT_CONFIG)" $(DUCKDB_DIR)
	cmake --build $(BUILD_DIR)/release --config Release

# Debug build with debug symbols
debug: vcpkg-setup
	mkdir -p $(BUILD_DIR)/debug
	cd $(BUILD_DIR)/debug && cmake -G "$(GEN)" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE="$(VCPKG_TOOLCHAIN)" -DVCPKG_MANIFEST_DIR="$(shell pwd)" -DDUCKDB_EXTENSION_CONFIGS="$(EXT_CONFIG)" $(DUCKDB_DIR)
	cmake --build $(BUILD_DIR)/debug --config Debug

# Configure only (useful for IDE integration)
configure: vcpkg-setup
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake -G "$(GEN)" -DCMAKE_TOOLCHAIN_FILE="$(VCPKG_TOOLCHAIN)" -DVCPKG_MANIFEST_DIR="$(shell pwd)" -DDUCKDB_EXTENSION_CONFIGS="$(EXT_CONFIG)" $(DUCKDB_DIR)

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)

# Run tests (requires built extension) - excludes integration tests
test: release
	$(BUILD_DIR)/release/test/unittest "[sql]" --force-reload

# Show help
help:
	@echo "DuckDB MSSQL Extension Build System"
	@echo ""
	@echo "Targets:"
	@echo "  release          - Build release version (default)"
	@echo "  debug            - Build debug version with symbols"
	@echo "  clean            - Remove build artifacts"
	@echo "  test             - Run unit tests (no SQL Server required)"
	@echo "  integration-test - Run integration tests (requires SQL Server)"
	@echo "  docker-up        - Start SQL Server test container"
	@echo "  docker-down      - Stop SQL Server test container"
	@echo "  docker-status    - Check SQL Server container status"
	@echo "  help             - Show this help"
	@echo ""
	@echo "Options:"
	@echo "  GEN=<generator>  - CMake generator (default: Ninja)"
	@echo ""
	@echo "Examples:"
	@echo "  make release"
	@echo "  make debug"
	@echo "  make docker-up && make integration-test"
	@echo "  GEN='Unix Makefiles' make release"
	@echo ""
	@echo "Extension will be built at:"
	@echo "  build/release/extension/mssql/mssql.duckdb_extension"

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

# Export all test environment variables for subprocesses
export MSSQL_TEST_HOST
export MSSQL_TEST_PORT
export MSSQL_TEST_USER
export MSSQL_TEST_PASS
export MSSQL_TEST_DB
export MSSQL_TEST_DSN
export MSSQL_TEST_URI
# NOTE: MSSQL_TEST_DSN_TLS is NOT exported by default because TLS tests require
# the loadable extension (.duckdb_extension). The built-in test runner uses the
# static extension which has a TLS stub. To run TLS tests, use the loadable
# extension with standalone DuckDB and manually export MSSQL_TEST_DSN_TLS.
# export MSSQL_TEST_DSN_TLS

# Integration tests - requires SQL Server running
# NOTE: TLS tests are skipped because the test runner uses the static extension
# which has a TLS stub. To run TLS tests, use the loadable extension.
integration-test: release
	@echo "Running integration tests..."
	@echo "NOTE: SQL Server must be running (use 'make docker-up' first)"
	@echo "NOTE: TLS tests are skipped (static extension has TLS stub)"
	@echo ""
	@echo "Test environment:"
	@echo "  MSSQL_TEST_HOST=$(MSSQL_TEST_HOST)"
	@echo "  MSSQL_TEST_PORT=$(MSSQL_TEST_PORT)"
	@echo "  MSSQL_TEST_USER=$(MSSQL_TEST_USER)"
	@echo "  MSSQL_TEST_DB=$(MSSQL_TEST_DB)"
	@echo "  MSSQL_TEST_DSN=$(MSSQL_TEST_DSN)"
	@echo "  MSSQL_TEST_URI=$(MSSQL_TEST_URI)"
	@echo ""
	@if ! docker compose -f $(DOCKER_COMPOSE) ps sqlserver 2>/dev/null | grep -q "healthy"; then \
		echo "ERROR: SQL Server is not running or not healthy."; \
		echo "Run 'make docker-up' first to start the test container."; \
		exit 1; \
	fi
	$(BUILD_DIR)/release/test/unittest "[integration]" --force-reload

# Run all tests (unit + integration)
test-all: release
	@echo "Running all tests..."
	@echo ""
	@echo "Test environment:"
	@echo "  MSSQL_TEST_HOST=$(MSSQL_TEST_HOST)"
	@echo "  MSSQL_TEST_PORT=$(MSSQL_TEST_PORT)"
	@echo "  MSSQL_TEST_DSN=$(MSSQL_TEST_DSN)"
	@echo ""
	$(BUILD_DIR)/release/test/unittest "*mssql*" --force-reload

# Debug test run
test-debug: debug
	@echo "Running tests (debug build)..."
	$(BUILD_DIR)/debug/test/unittest "*mssql*" --force-reload

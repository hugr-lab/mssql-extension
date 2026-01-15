# Makefile for DuckDB MSSQL Extension
# Builds extension using DuckDB's build system

.PHONY: all release debug clean configure test help docker-up docker-down docker-status integration-test

# Default generator (can be overridden: GEN=ninja make release)
GEN ?= Ninja

# Build directory
BUILD_DIR := build

# Extension config path (absolute path)
EXT_CONFIG := $(shell pwd)/extension_config.cmake

# DuckDB source directory
DUCKDB_DIR := $(shell pwd)/duckdb

# Default target
all: release

# Release build - uses DuckDB's CMakeLists.txt as root
release:
	mkdir -p $(BUILD_DIR)/release
	cd $(BUILD_DIR)/release && cmake -G "$(GEN)" -DCMAKE_BUILD_TYPE=Release -DDUCKDB_EXTENSION_CONFIGS="$(EXT_CONFIG)" $(DUCKDB_DIR)
	cmake --build $(BUILD_DIR)/release --config Release

# Debug build with debug symbols
debug:
	mkdir -p $(BUILD_DIR)/debug
	cd $(BUILD_DIR)/debug && cmake -G "$(GEN)" -DCMAKE_BUILD_TYPE=Debug -DDUCKDB_EXTENSION_CONFIGS="$(EXT_CONFIG)" $(DUCKDB_DIR)
	cmake --build $(BUILD_DIR)/debug --config Debug

# Configure only (useful for IDE integration)
configure:
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake -G "$(GEN)" -DDUCKDB_EXTENSION_CONFIGS="$(EXT_CONFIG)" $(DUCKDB_DIR)

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
	@docker exec mssql-dev /opt/mssql-tools18/bin/sqlcmd -S localhost -U sa -P 'DevPassword123!' -C -Q "SELECT 'Connection OK' AS status" 2>/dev/null || echo "Connection failed - is the container running?"

# Integration tests - requires SQL Server running
integration-test: release
	@echo "Running integration tests..."
	@echo "NOTE: SQL Server must be running (use 'make docker-up' first)"
	@echo ""
	@if ! docker compose -f $(DOCKER_COMPOSE) ps sqlserver 2>/dev/null | grep -q "healthy"; then \
		echo "ERROR: SQL Server is not running or not healthy."; \
		echo "Run 'make docker-up' first to start the test container."; \
		exit 1; \
	fi
	$(BUILD_DIR)/release/test/unittest "[integration]" --force-reload

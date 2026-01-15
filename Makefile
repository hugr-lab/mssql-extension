# Makefile for DuckDB MSSQL Extension
# Builds extension using DuckDB's build system

.PHONY: all release debug clean configure test help

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

# Run tests (requires built extension)
test: release
	cd $(BUILD_DIR)/release && ctest --output-on-failure

# Show help
help:
	@echo "DuckDB MSSQL Extension Build System"
	@echo ""
	@echo "Targets:"
	@echo "  release  - Build release version (default)"
	@echo "  debug    - Build debug version with symbols"
	@echo "  clean    - Remove build artifacts"
	@echo "  test     - Run tests"
	@echo "  help     - Show this help"
	@echo ""
	@echo "Options:"
	@echo "  GEN=<generator>  - CMake generator (default: Ninja)"
	@echo ""
	@echo "Examples:"
	@echo "  make release"
	@echo "  make debug"
	@echo "  GEN='Unix Makefiles' make release"
	@echo ""
	@echo "Extension will be built at:"
	@echo "  build/release/extension/mssql/mssql.duckdb_extension"

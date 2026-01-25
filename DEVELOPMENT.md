# MSSQL Extension Development Guide

This document covers building, testing, and contributing to the DuckDB MSSQL extension.

## Prerequisites

- CMake 3.21+
- Ninja build system
- C++17 compatible compiler (GCC 11+, Clang 14+, MSVC 2022+)
- Git
- Docker (for SQL Server test environment and Linux CI testing)
- vcpkg (for TLS support in loadable extension)

## Quick Start

```bash
# Clone with submodules
git clone --recurse-submodules <repository-url>
cd mssql-extension

# Build release version (static extension, no TLS)
make

# Build debug version
make debug

# Run unit tests
make test
```

## Build System

### Makefile Targets

| Target         | Description                              |
| -------------- | ---------------------------------------- |
| `make`         | Build optimized release version          |
| `make debug`   | Build with debug symbols                 |
| `make clean`   | Remove all build artifacts               |
| `make test`    | Run DuckDB extension tests               |

### Build Outputs

| Output | Path | Description |
| ------ | ---- | ----------- |
| DuckDB CLI | `build/release/duckdb` | DuckDB with extension statically linked |
| Static Extension | `build/release/extension/mssql/libmssql_extension.a` | For embedding |
| Loadable Extension | `build/release/extension/mssql/mssql.duckdb_extension` | Dynamic loading |

### TLS Support

The extension uses OpenSSL for TLS support. Both static and loadable builds include full TLS support:

| Build Type | TLS Support | Use Case |
| ---------- | ----------- | -------- |
| Static     | Yes (OpenSSL via vcpkg) | CLI development, embedded DuckDB |
| Loadable   | Yes (OpenSSL via vcpkg) | Production with encrypted connections |

OpenSSL is statically linked and symbol visibility is controlled to prevent conflicts with other libraries.

### Building with TLS

TLS support is automatically included when building with vcpkg:

```bash
# Bootstrap vcpkg (if not already installed)
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh

# Build with vcpkg (using make)
make

# Or build manually with CMake
mkdir -p build/release && cd build/release
cmake -G "Ninja" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$(pwd)/../../vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_MANIFEST_DIR="$(pwd)/../.." \
  -DDUCKDB_EXTENSION_CONFIGS="$(pwd)/../../extension_config.cmake" \
  ../../duckdb
cmake --build . --config Release
```

## Testing

### Local Development Environment

Start SQL Server for integration tests:

```bash
# Start SQL Server container
docker compose -f docker/docker-compose.yml up -d

# Wait for initialization
docker compose -f docker/docker-compose.yml logs -f sqlserver

# Stop when done
docker compose -f docker/docker-compose.yml down
```

Default credentials:
- Host: `localhost`
- Port: `1433`
- User: `sa`
- Password: `TestPassword1`

### Running Tests

```bash
# Unit tests (no SQL Server required)
make test

# Integration tests require SQL Server running
# Tests are in tests/integration/
```

### Manual Testing

```bash
# Load extension in DuckDB CLI (static build)
./build/release/duckdb

# Load extension dynamically (with TLS)
duckdb --unsigned -c "LOAD 'build/release/extension/mssql/mssql.duckdb_extension';"
```

## Linux CI Build (Docker)

To test your changes against the exact CI environment (Ubuntu 22.04, GCC, DuckDB nightly):

### Build Docker Image

```bash
docker compose -f docker/docker-compose.linux-ci.yml build build
```

### Run Linux Build

```bash
docker compose -f docker/docker-compose.linux-ci.yml run --rm build
```

This will:
1. Fetch DuckDB v1.4.3 (stable release)
2. Auto-detect DuckDB API version
3. Build DuckDB CLI (static extension with TLS)
4. Build loadable extension with full TLS support
5. Build with GCC (same as GitHub Actions)

### Run Lint Check

```bash
docker compose -f docker/docker-compose.linux-ci.yml run --rm lint
```

### Run Integration Tests

```bash
# Start SQL Server first
docker compose -f docker/docker-compose.linux-ci.yml up -d sqlserver

# Wait for it to be healthy, then run tests
docker compose -f docker/docker-compose.linux-ci.yml run --rm test
```

### Clean Up

```bash
docker compose -f docker/docker-compose.linux-ci.yml down -v
```

### Build Targets

The Docker CI builds the following targets:

| Target | Description |
| ------ | ----------- |
| `duckdb` | DuckDB CLI with statically linked extension (with TLS) |
| `mssql_loadable_extension` | Loadable extension with full TLS support |

Both builds use OpenSSL for TLS support. Symbol visibility is controlled using version scripts (Linux) or exported_symbols_list (macOS) to prevent runtime conflicts when dynamically loaded.

## DuckDB API Compatibility

The extension supports both DuckDB stable (1.4.x) and nightly (main) APIs through automatic detection at CMake configure time:

| DuckDB Version | API | Detection |
| -------------- | --- | --------- |
| 1.4.x (stable) | `GetData` | Auto-detected |
| main (nightly) | `GetDataInternal` | Auto-detected |

CMake automatically detects which API to use by checking the DuckDB headers. No manual configuration is needed.

### How It Works

CMake detects the API version by checking for `GetDataInternal` in the DuckDB headers, then sets the appropriate compile definition. The compatibility is handled via `src/include/mssql_compat.hpp`:

```cpp
#ifdef MSSQL_DUCKDB_NIGHTLY
#define MSSQL_GETDATA_METHOD GetDataInternal
#else
#define MSSQL_GETDATA_METHOD GetData
#endif
```

## IDE Configuration

### VS Code

1. Open the project folder in VS Code
2. Install recommended extensions when prompted
3. Use `Cmd+Shift+B` (macOS) or `Ctrl+Shift+B` (Windows/Linux) to build
4. Use `F5` to debug with DuckDB shell

Recommended extensions:
- C/C++ (Microsoft)
- CMake Tools
- clangd (optional, for better IntelliSense)

### CLion

1. Open the project as a CMake project
2. Set CMake options: `-DCMAKE_BUILD_TYPE=Debug`
3. Select the `duckdb` target to run with the extension
4. Build and debug normally

## Code Style

- Use clang-format (version 14) for C++ formatting
- Run `find src -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i` before committing
- The CI will reject PRs that don't pass lint

## Project Structure

```
src/
├── include/           # Header files
│   ├── catalog/       # DuckDB catalog integration
│   ├── connection/    # Connection pooling, settings
│   ├── dml/           # Shared DML utilities (value serializer, statement builder)
│   ├── insert/        # INSERT implementation
│   ├── update/        # UPDATE implementation
│   ├── delete/        # DELETE implementation
│   ├── pushdown/      # Filter/projection pushdown
│   ├── query/         # Query execution
│   └── tds/           # TDS protocol implementation
├── catalog/           # Catalog implementation
├── connection/        # Connection management
├── dml/               # Shared DML components
├── insert/            # INSERT operators
├── update/            # UPDATE operators
├── delete/            # DELETE operators
├── pushdown/          # Pushdown optimization
├── query/             # Query execution
└── tds/               # TDS protocol
    ├── encoding/      # Type encoding/decoding
    └── tls/           # TLS implementation (OpenSSL)

test/sql/
├── catalog/           # Catalog integration tests
├── dml/               # UPDATE and DELETE tests
├── insert/            # INSERT tests
├── integration/       # Core integration tests
├── query/             # Query-level tests
└── tds_connection/    # TDS protocol tests

docker/
├── docker-compose.yml         # Local SQL Server for development
├── docker-compose.linux-ci.yml # Linux CI build environment
├── Dockerfile.build           # Ubuntu 22.04 build image
└── init/                      # SQL Server init scripts
```

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Run lint: `find src -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i`
5. Test locally with Docker CI: `docker compose -f docker/docker-compose.linux-ci.yml run --rm build`
6. Push and create a PR

## Debugging

### Enable Debug Output

```bash
# TDS protocol debugging
export MSSQL_DEBUG=1
./build/debug/duckdb
```

### DML Debugging

```bash
# Debug DML operations (INSERT, UPDATE, DELETE)
export MSSQL_DML_DEBUG=1
./build/debug/duckdb

# This will output:
# - Generated SQL statements
# - Batch sizes and parameter counts
# - Execution timing
# - rowid values for UPDATE/DELETE targeting
```

### TLS Debugging

```bash
# Verbose TLS handshake
export MSSQL_DEBUG=1
# Then run queries with use_encrypt=true
```

### GDB/LLDB

```bash
# Debug build
make debug

# Run with debugger
lldb ./build/debug/duckdb
# or
gdb ./build/debug/duckdb
```

### DML Component Debugging Tips

When debugging DML operations:

1. **INSERT issues**: Check `mssql_insert_executor.cpp` - the batch builder and statement generator
2. **UPDATE/DELETE issues**: Check `mssql_update.cpp` or `mssql_delete.cpp` - ensure rowid column is properly positioned
3. **Value serialization**: Check `mssql_value_serializer.cpp` for type → T-SQL literal conversion
4. **Column ordering**: Ensure INSERT columns are in statement order, not table order (see `mssql_catalog.cpp`)

## Release Process

1. Update version in relevant files
2. Create PR with changes
3. Merge to main
4. CI builds and tests on all platforms
5. Tag release creates GitHub release with artifacts

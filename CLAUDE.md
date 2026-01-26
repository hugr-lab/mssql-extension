# mssql-extension Development Guidelines

Auto-generated from all feature plans. Last updated: 2026-01-19

## Active Technologies
- C++17 (DuckDB extension standard) + DuckDB main branch (extension API) (002-duckdb-surface-api)
- N/A (connection metadata in memory, secrets via DuckDB's secret manager) (002-duckdb-surface-api)
- C++17 (DuckDB extension standard) + DuckDB main branch (extension API), POSIX sockets (TCP) (003-tds-connection-pooling)
- In-memory (connection metadata, pool state) (003-tds-connection-pooling)
- C++17 (DuckDB extension standard) + DuckDB main branch (extension API, DataChunk), existing TDS layer from spec 003 (004-streaming-select-cancel)
- In-memory (result streaming, no intermediate buffering) (004-streaming-select-cancel)
- C++17 (DuckDB extension standard) + DuckDB main branch (extension API), OpenSSL (TLS library via vcpkg) (005-tls-connection-support)
- In-memory (TLS context per connection) (005-tls-connection-support)
- C++17 (DuckDB extension standard) + DuckDB (main branch), OpenSSL (vcpkg, unified for both static and loadable) (006-split-tls-build)
- N/A (build system only) (006-split-tls-build)
- C++17 (DuckDB extension standard) + DuckDB main branch (catalog API, DataChunk), existing TDS layer (specs 001-006), OpenSSL (007-catalog-integration)
- In-memory (metadata cache with TTL), DuckDB secret manager for credentials (007-catalog-integration)
- C++17 (DuckDB extension standard) + DuckDB main branch (extension API, catalog API, DataChunk), existing TDS layer (specs 001-007), OpenSSL (via vcpkg) (008-catalog-ddl-statistics)
- C++17 (DuckDB extension standard) + DuckDB main branch (catalog API, DataChunk), existing TDS layer (specs 001-007), OpenSSL (via vcpkg) (009-dml-insert)
- In-memory (no intermediate buffering per Streaming First principle) (009-dml-insert)
- Markdown (GitHub-flavored) + N/A (documentation only) (010-extension-documentation)
- YAML (GitHub Actions workflow syntax) + Bash scripts + GitHub Actions runners, CMake, vcpkg, Ninja, Docker (Linux only) (011-ci-release-workflows)
- N/A (workflow artifacts stored in GitHub) (011-ci-release-workflows)
- C++17 (DuckDB extension standard) + DuckDB main branch (extension API, catalog API), existing TDS layer (specs 001-011) (012-docs-platform-refresh)
- In-memory (metadata cache) (012-docs-platform-refresh)
- C++17 (DuckDB extension standard) + DuckDB main branch (extension API, catalog API, DataChunk), existing TDS layer (specs 001-012), OpenSSL (via vcpkg) (013-table-scan-filter-refactor)
- Makefile (GNU Make), CMake 3.20+, Bash + duckdb/extension-ci-tools (git submodule), duckdb (existing submodule) (001-extension-ci-tools-integration)
- C++17 (DuckDB extension standard) + DuckDB (main branch), vcpkg (manifest mode) (001-project-bootstrap)
- YAML configuration + Shell/Makefile + DuckDB extension-ci-tools (Makefile-based test runner) (015-ci-test-skip)
- N/A (configuration files only) (015-ci-test-skip)
- C++17 (DuckDB extension standard) + DuckDB main branch (extension API), OpenSSL (via vcpkg), WinSock2 (Windows) (016-windows-ci-build)
- N/A (build/CI configuration only) (016-windows-ci-build)
- C++17 (DuckDB extension standard) + DuckDB main branch (catalog API, DataChunk), existing TDS layer (specs 001-012) (001-pk-rowid-semantics)
- In-memory (PK metadata cache per table entry) (001-pk-rowid-semantics)
- C++17 (DuckDB extension standard) + DuckDB main branch (catalog API, PhysicalOperator, DataChunk), existing TDS layer (specs 001-009) (002-dml-update-delete)
- C++17 (DuckDB extension standard) + DuckDB main branch (catalog API), existing TDS layer (specs 001-012) (001-attach-connection-validation)
- In-memory (connection pool, metadata cache) (001-attach-connection-validation)
- C++17 (DuckDB extension standard) + DuckDB main branch (extension API), existing TDS layer (001-code-cleanup)
- N/A (code refactoring only, no new data storage) (001-code-cleanup)
- C++17 (DuckDB extension standard) + DuckDB main branch (TransactionManager, Transaction classes), existing TDS layer (ConnectionPool, TdsConnection) (001-mssql-transactions)
- In-memory (pinned connection stored in MSSQLTransaction object) (001-mssql-transactions)

## Project Structure

```text
src/
  dml/                  # DML operations
    insert/             # INSERT implementation (spec 009)
      mssql_value_serializer.cpp   # Type → T-SQL literal conversion
      mssql_insert_statement.cpp   # SQL statement generation
      mssql_batch_builder.cpp      # Row accumulation and batching
      mssql_insert_executor.cpp    # Batch execution orchestration
      mssql_physical_insert.cpp    # DuckDB physical operator
      mssql_returning_parser.cpp   # OUTPUT INSERTED result parsing
    update/             # UPDATE implementation
    delete/             # DELETE implementation
  include/dml/          # Headers for DML modules
    insert/
    update/
    delete/
  catalog/              # DuckDB catalog integration
  table_scan/           # Table scan and filter pushdown
  tds/                  # TDS protocol implementation
  connection/           # Connection pooling and settings
  query/                # Query execution
tests/
  cpp/                  # Unit tests (no SQL Server required)
  integration/          # Integration tests (require SQL Server)
```

## Commands

```bash
# Build release (default)
make

# Build debug
make debug

# Run DuckDB tests for mssql extension
make test

# Load extension in DuckDB CLI for manual testing (the release and debug builds include TLS support via OpenSSL)
./build/release/duckdb

# Or dynamically load the mssql extension
# The duckdb cli version must be the same as the built extension (release/debug) (branch in the submodule duckdb).
duckdb --unsigned -c "INSTALL mssql FROM local_build_debug; LOAD mssql; ...."

# Extension installation sources:
# core - `http://extensions.duckdb.org` DuckDB core extensions
# core_nightly - `http://nightly-extensions.duckdb.org` Nightly builds for core
# community - `http://community-extensions.duckdb.org` DuckDB community extensions
# local_build_debug - `./build/debug/repository` Repository created when building DuckDB from source in debug mode (for development)
# local_build_release - `./build/release/repository` Repository created when building DuckDB from source in release mode (for development)

```

## Code Style

C++17 (DuckDB extension standard): Follow standard conventions

## Recent Changes
- 001-mssql-transactions: Added C++17 (DuckDB extension standard) + DuckDB main branch (TransactionManager, Transaction classes), existing TDS layer (ConnectionPool, TdsConnection)
- 001-code-cleanup: Added C++17 (DuckDB extension standard) + DuckDB main branch (extension API), existing TDS layer
- 001-attach-connection-validation: Added C++17 (DuckDB extension standard) + DuckDB main branch (catalog API), existing TDS layer (specs 001-012)

<!-- MANUAL ADDITIONS START -->

## Windows Build Support

The extension supports Windows builds with both MSVC and MinGW toolchains.

### Platform Compatibility

- **ssize_t**: Windows MSVC does not define the POSIX `ssize_t` type. The header `src/include/tds/tds_platform.hpp` provides the Windows-compatible typedef.

### CI Windows Build Testing

To test Windows builds before submitting to community-extensions:

```bash
# Trigger Windows build manually via GitHub Actions
# Go to Actions → CI → Run workflow → Check "Run Windows build jobs"
```

### Build Configurations

| Toolchain           | vcpkg Triplet              | Notes                                       |
|---------------------|----------------------------|---------------------------------------------|
| MSVC (VS2022)       | x64-windows-static-release | Primary Windows build                       |
| MinGW (Rtools 4.2)  | x64-mingw-static           | Must use Rtools 4.2, not 4.3 (linker bugs)  |

### Community Extensions CI

The community-extensions CI uses both MSVC and MinGW builds. Local CI workflow matches these configurations for accurate pre-submission testing.
<!-- MANUAL ADDITIONS END -->

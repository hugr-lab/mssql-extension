# mssql-extension Development Guidelines

Auto-generated from all feature plans. Last updated: 2026-01-15

## Active Technologies
- C++17 (DuckDB extension standard) + DuckDB main branch (extension API) (002-duckdb-surface-api)
- N/A (connection metadata in memory, secrets via DuckDB's secret manager) (002-duckdb-surface-api)
- C++17 (DuckDB extension standard) + DuckDB main branch (extension API), POSIX sockets (TCP) (003-tds-connection-pooling)
- In-memory (connection metadata, pool state) (003-tds-connection-pooling)
- C++17 (DuckDB extension standard) + DuckDB main branch (extension API, DataChunk), existing TDS layer from spec 003 (004-streaming-select-cancel)
- In-memory (result streaming, no intermediate buffering) (004-streaming-select-cancel)
- C++17 (DuckDB extension standard) + DuckDB main branch (extension API), mbedTLS 3.x (TLS library via vcpkg) (005-tls-connection-support)
- In-memory (TLS context per connection) (005-tls-connection-support)
- C++17 (DuckDB extension standard) + DuckDB (main branch), mbedTLS (vcpkg 3.6.4 for loadable, DuckDB bundled for static) (006-split-tls-build)
- N/A (build system only) (006-split-tls-build)
- C++17 (DuckDB extension standard) + DuckDB main branch (catalog API, DataChunk), existing TDS layer (specs 001-006), mbedTLS (via split TLS build) (007-catalog-integration)
- In-memory (metadata cache with TTL), DuckDB secret manager for credentials (007-catalog-integration)

- C++17 (DuckDB extension standard) + DuckDB (main branch), vcpkg (manifest mode) (001-project-bootstrap)

## Project Structure

```text
src/
tests/
```

## Commands

# Add commands for C++17 (DuckDB extension standard)

## Code Style

C++17 (DuckDB extension standard): Follow standard conventions

## Recent Changes
- 007-catalog-integration: Added C++17 (DuckDB extension standard) + DuckDB main branch (catalog API, DataChunk), existing TDS layer (specs 001-006), mbedTLS (via split TLS build)
- 006-split-tls-build: Added C++17 (DuckDB extension standard) + DuckDB (main branch), mbedTLS (vcpkg 3.6.4 for loadable, DuckDB bundled for static)
- 005-tls-connection-support: Added C++17 (DuckDB extension standard) + DuckDB main branch (extension API), mbedTLS 3.x (TLS library via vcpkg)


<!-- MANUAL ADDITIONS START -->
<!-- MANUAL ADDITIONS END -->

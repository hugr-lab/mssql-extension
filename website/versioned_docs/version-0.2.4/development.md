---
title: Development
sidebar_position: 8
---

# Contributing

We welcome contributions! Whether it's bug reports, feature requests, documentation improvements, or code contributions - your help makes this extension better for everyone.

- **Report bugs**: Open an issue with reproduction steps
- **Request features**: Describe your use case and proposed solution
- **Submit PRs**: Fork, branch, and submit a pull request
- **Test on your platform**: Help us validate on different environments

## Development

For building from source, testing, and contributing, see the [Development Guide](https://github.com/hugr-lab/mssql-extension/blob/main/DEVELOPMENT.md).

Quick start:

```bash
git clone --recurse-submodules <repository-url>
cd mssql-extension
make        # Build release
make test   # Run tests
```

## Building with DuckDB Extension CI Tools

This extension is compatible with DuckDB Community Extensions CI.

### Setup

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/hugr-lab/mssql-extension.git
cd mssql-extension

# Or initialize submodules after clone
git submodule update --init --recursive
```

### CI Build (Community Extensions compatible)

```bash
# Set DuckDB version (required by Community CI)
DUCKDB_GIT_VERSION=v1.5.5 make set_duckdb_version

# Build release
make release

# Run tests
make test
```

### Local Development Build

```bash
# Bootstrap vcpkg (required for TLS/OpenSSL support)
make vcpkg-setup

# Build
make release   # or: make debug

# Load extension in DuckDB
./build/release/duckdb
> LOAD mssql;
```

### Running Integration Tests

```bash
# Start SQL Server container
make docker-up

# Run integration tests
make integration-test

# Stop container when done
make docker-down
```

### Available Build Targets

Run `make help` to see all available targets:

| Target               | Description                                          |
| -------------------- | ---------------------------------------------------- |
| `release`            | Build release version                                |
| `debug`              | Build debug version                                  |
| `test`               | Run unit tests                                       |
| `set_duckdb_version` | Set DuckDB version (use `DUCKDB_GIT_VERSION=v1.x.x`) |
| `vcpkg-setup`        | Bootstrap vcpkg (required for TLS support)           |
| `integration-test`   | Run integration tests (requires SQL Server)          |
| `test-all`           | Run all tests                                        |
| `docker-up`          | Start SQL Server test container                      |
| `docker-down`        | Stop SQL Server test container                       |
| `docker-status`      | Check SQL Server container status                    |

## Releasing Documentation

The docs site is versioned with Docusaurus. At each extension release, before
tagging:

```bash
cd website
npm run docusaurus docs:version <X.Y.Z>   # e.g. 0.2.3
```

and commit the generated `versioned_docs/` + `versioned_sidebars/` snapshot.
The released version then serves at the site root (the default), while the
live `website/docs/` tree publishes as **Next** under `/next/` with an
"unreleased" banner — documentation for the coming release is always
reachable from the version dropdown, but never the landing default.

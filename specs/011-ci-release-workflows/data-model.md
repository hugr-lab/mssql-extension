# Data Model: CI/CD Release Workflows

**Branch**: `011-ci-release-workflows` | **Date**: 2026-01-19

## Overview

This document defines the data models and configurations for the CI/CD workflows. Since this feature is infrastructure-focused (no application code), the "data model" describes workflow matrices, environment variables, and artifact schemas.

## Build Matrix Definition

### Release Workflow Matrix

The release workflow builds all combinations of platforms and DuckDB versions:

```yaml
# Total: 4 platforms × 3 DuckDB versions = 12 builds
matrix:
  platform:
    - id: linux_amd64
      os: linux
      arch: amd64
      runner: ubuntu-22.04
      docker_available: true

    - id: linux_arm64
      os: linux
      arch: arm64
      runner: ubuntu-22.04-arm
      docker_available: true

    - id: osx_arm64
      os: osx
      arch: arm64
      runner: macos-14
      docker_available: false

    - id: windows_amd64
      os: windows
      arch: amd64
      runner: windows-latest
      docker_available: false

  duckdb_version:
    - '1.4.1'
    - '1.4.2'
    - '1.4.3'
```

### CI Workflow Matrix

The CI workflow builds a subset for PR validation:

```yaml
# Total: 2 platforms × 1 DuckDB version (nightly) = 2 builds
matrix:
  platform:
    - id: linux_amd64
      os: linux
      arch: amd64
      runner: ubuntu-22.04
      docker_available: true

    - id: osx_arm64
      os: osx
      arch: arm64
      runner: macos-14
      docker_available: false

  duckdb_version:
    - 'main'  # nightly
```

## Environment Variables Schema

### Build Environment

| Variable | Source | Description |
|----------|--------|-------------|
| `EXT_VERSION` | Git tag | Extension version (e.g., `0.1.0` from tag `v0.1.0`) |
| `DUCKDB_VERSION` | Matrix | Target DuckDB version (e.g., `1.4.1`) |
| `DUCKDB_COMMIT` | Git | Actual DuckDB commit hash used |
| `PLATFORM` | Matrix | Platform identifier (e.g., `linux_amd64`) |
| `RUNNER_OS` | GitHub | Runner operating system |
| `RUNNER_ARCH` | GitHub | Runner architecture |

### SQL Server Test Environment

| Variable | Default | Description |
|----------|---------|-------------|
| `MSSQL_TEST_HOST` | `localhost` | SQL Server hostname |
| `MSSQL_TEST_PORT` | `1433` | SQL Server port |
| `MSSQL_TEST_USER` | `sa` | SQL Server username |
| `MSSQL_TEST_PASS` | `TestPassword1` | SQL Server password |
| `MSSQL_TEST_DB` | `master` | Default database |

## Artifact Schema

### Extension Artifact

**Naming Pattern**: `mssql-{EXT_VERSION}-duckdb-{DUCKDB_VERSION}-{PLATFORM}.duckdb_extension`

**Examples**:
```
mssql-0.1.0-duckdb-1.4.1-linux_amd64.duckdb_extension
mssql-0.1.0-duckdb-1.4.1-linux_arm64.duckdb_extension
mssql-0.1.0-duckdb-1.4.1-osx_arm64.duckdb_extension
mssql-0.1.0-duckdb-1.4.1-windows_amd64.duckdb_extension
mssql-0.1.0-duckdb-1.4.2-linux_amd64.duckdb_extension
... (12 total for release)
```

**Build Output Location**: `build/release/extension/mssql/mssql.duckdb_extension`

### Checksum Artifact

**Filename**: `SHA256SUMS.txt`

**Format** (BSD-style):
```
<64-char-hex-hash>  mssql-0.1.0-duckdb-1.4.1-linux_amd64.duckdb_extension
<64-char-hex-hash>  mssql-0.1.0-duckdb-1.4.1-linux_arm64.duckdb_extension
<64-char-hex-hash>  mssql-0.1.0-duckdb-1.4.1-osx_arm64.duckdb_extension
...
```

**Verification Command**: `sha256sum -c SHA256SUMS.txt`

## Workflow Job Dependencies

### Release Workflow

```
┌─────────┐
│  lint   │
└────┬────┘
     │
     ▼
┌─────────────────────────────────────────┐
│               build (matrix)             │
│  [linux_amd64, linux_arm64,             │
│   osx_arm64, windows_amd64]             │
│  × [1.4.1, 1.4.2, 1.4.3]                │
└────┬────────────────────────────────────┘
     │
     ▼
┌─────────────────────────────────────────┐
│            smoke-test (matrix)           │
│  [Linux: full integration]              │
│  [macOS/Windows: load-only]             │
└────┬────────────────────────────────────┘
     │
     ▼
┌─────────────────────────────────────────┐
│              release                     │
│  - Collect all artifacts                │
│  - Generate SHA256SUMS.txt              │
│  - Upload to GitHub Release             │
└─────────────────────────────────────────┘
```

### CI Workflow

```
┌─────────┐
│  lint   │
└────┬────┘
     │
     ▼
┌─────────────────────────────────────────┐
│               build (matrix)             │
│  [linux_amd64, osx_arm64]               │
│  × [nightly]                            │
└────┬────────────────────────────────────┘
     │
     ▼
┌─────────────────────────────────────────┐
│            smoke-test (matrix)           │
│  [linux_amd64: full integration]        │
│  [osx_arm64: load-only]                 │
└─────────────────────────────────────────┘
```

## GitHub Release Schema

### Release Metadata

| Field | Value |
|-------|-------|
| Tag | `v{EXT_VERSION}` (e.g., `v0.1.0`) |
| Name | `v{EXT_VERSION}` (same as tag) |
| Draft | `false` |
| Prerelease | Auto-detect from tag (e.g., `v0.1.0-beta.1` → prerelease) |

### Release Assets

1. **Extension artifacts**: 12 `.duckdb_extension` files
2. **Checksum file**: `SHA256SUMS.txt`

## Docker Compose Service Schema

The existing `docker/docker-compose.yml` defines the SQL Server test container:

```yaml
services:
  sqlserver:
    image: mcr.microsoft.com/mssql/server:2022-latest
    container_name: mssql-dev
    environment:
      - ACCEPT_EULA=Y
      - MSSQL_SA_PASSWORD=${MSSQL_TEST_PASS:-TestPassword1}
    ports:
      - "${MSSQL_TEST_PORT:-1433}:1433"
    healthcheck:
      test: ["CMD-SHELL", "/opt/mssql-tools18/bin/sqlcmd ..."]
      interval: 10s
      timeout: 5s
      retries: 10
      start_period: 30s
```

## Script Interface Definitions

### fetch_duckdb.sh

**Purpose**: Fetch DuckDB sources by version tag or main branch

**Arguments**:
- `$1`: DuckDB version (e.g., `1.4.1` or `main`)

**Output**:
- Clones DuckDB to `./duckdb/`
- Prints commit hash to stdout

**Exit Codes**:
- `0`: Success
- `1`: Failed to clone or invalid version

### build_extension.sh

**Purpose**: Build extension for current platform

**Arguments**: None (uses environment)

**Environment**:
- `DUCKDB_VERSION`: Target version
- `BUILD_TYPE`: `Release` or `Debug`

**Output**:
- Built extension at `build/release/extension/mssql/mssql.duckdb_extension`

### smoke_test.sh

**Purpose**: Run load-only smoke test

**Arguments**:
- `$1`: Path to DuckDB CLI
- `$2`: Path to extension file

**Exit Codes**:
- `0`: Extension loads successfully
- `1`: Extension failed to load

### integration_test.sh

**Purpose**: Run full SQL Server integration test

**Arguments**:
- `$1`: Path to DuckDB CLI
- `$2`: Path to extension file

**Prerequisites**:
- SQL Server container running and healthy

**Exit Codes**:
- `0`: All tests pass
- `1`: Test failure

# Quickstart: CI/CD Release Workflows

**Branch**: `011-ci-release-workflows` | **Date**: 2026-01-19

## Overview

This guide explains how to test the CI/CD workflows locally and how to trigger releases.

## Local Testing

### Prerequisites

- Docker (for SQL Server integration tests)
- Git
- CMake 3.21+
- Ninja build system
- C++17 compiler

### Test the Build

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/hugr-lab/mssql-extension.git
cd mssql-extension

# Build release
make release

# Verify extension loads
./build/release/duckdb -c "SELECT mssql_version();"
```

### Test SQL Server Integration

```bash
# Start SQL Server container
make docker-up

# Wait for SQL Server to be ready (automatically waits)
# Then run integration tests
make integration-test

# Stop SQL Server when done
make docker-down
```

### Test Smoke Test Scripts

Once the scripts are implemented, test them locally:

```bash
# Test load-only smoke test
./scripts/ci/smoke_test.sh ./build/release/duckdb ./build/release/extension/mssql/mssql.duckdb_extension

# Test integration (requires SQL Server running)
make docker-up
./scripts/ci/integration_test.sh ./build/release/duckdb ./build/release/extension/mssql/mssql.duckdb_extension
make docker-down
```

## Triggering a Release

### Step 1: Ensure Code is Ready

```bash
# Make sure you're on main and up-to-date
git checkout main
git pull origin main

# Run tests locally
make test
make docker-up && make integration-test && make docker-down
```

### Step 2: Create and Push a Version Tag

```bash
# Create annotated tag
git tag -a v0.1.0 -m "Release v0.1.0"

# Push tag to trigger release workflow
git push origin v0.1.0
```

### Step 3: Monitor Release Workflow

1. Go to **Actions** tab in GitHub
2. Find the "Release" workflow run triggered by the tag
3. Monitor build progress across all 12 matrix combinations
4. Once complete, check the **Releases** page for artifacts

### Step 4: Verify Release

1. Download a platform-specific extension
2. Verify checksum: `sha256sum -c SHA256SUMS.txt`
3. Test the downloaded extension:
   ```bash
   duckdb -unsigned -c "
     INSTALL 'mssql-0.1.0-duckdb-1.4.1-linux_amd64.duckdb_extension';
     LOAD mssql;
     SELECT mssql_version();
   "
   ```

## PR Validation

When you open a PR to `main`, the CI workflow automatically:

1. Runs lint checks (clang-format)
2. Builds on linux_amd64 and osx_arm64
3. Runs smoke tests (full integration on Linux)

### Checking CI Status

- CI status appears on the PR page
- Click "Details" to see logs
- All checks must pass before merging

### Debugging CI Failures

1. Check the specific job that failed
2. Look at the build logs for error messages
3. Common issues:
   - **Lint failure**: Run `make lint` locally to fix formatting
   - **Build failure**: Check compiler errors in logs
   - **Smoke test failure**: Check SQL Server connectivity

## Workflow Files

| File | Purpose |
|------|---------|
| `.github/workflows/release.yml` | Release builds (triggered by `v*` tags) |
| `.github/workflows/ci.yml` | PR validation (triggered by PR/push) |
| `scripts/ci/fetch_duckdb.sh` | Fetch DuckDB sources by version |
| `scripts/ci/smoke_test.sh` | Load-only smoke test |
| `scripts/ci/integration_test.sh` | Full SQL Server integration test |
| `scripts/sql/smoke_test.sql` | SQL commands for smoke test |

## Troubleshooting

### "DuckDB version not found"

The specified DuckDB version tag doesn't exist. Check available tags:
```bash
git ls-remote --tags https://github.com/duckdb/duckdb.git | grep 'v1.4'
```

### "SQL Server container not healthy"

Docker startup can take time. The workflow waits up to 2 minutes. If it still fails:
1. Check Docker logs: `docker logs mssql-dev`
2. Ensure port 1433 is not in use
3. Verify SA password meets complexity requirements

### "Extension failed to load"

Check for missing dependencies or build issues:
```bash
# On Linux, check for missing libraries
ldd build/release/extension/mssql/mssql.duckdb_extension
```

### "Artifact upload failed"

GitHub Release might not exist or permissions are insufficient:
1. Verify the tag was pushed
2. Check repository has Actions permissions for releases
3. Retry the failed job

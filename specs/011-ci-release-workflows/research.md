# Research: CI/CD Release Workflows

**Branch**: `011-ci-release-workflows` | **Date**: 2026-01-19

## Research Topics

### 1. GitHub Actions Runner Selection by Platform

**Decision**: Use GitHub-hosted runners for all platforms except linux_arm64

**Rationale**:
- `ubuntu-latest` / `ubuntu-22.04`: Standard Linux x64 runner, widely available
- `ubuntu-22.04-arm`: GitHub-hosted ARM64 runner (available since late 2024)
- `macos-14`: Apple Silicon (ARM64) runner, required for osx_arm64 builds
- `windows-latest`: Standard Windows x64 runner

**Alternatives Considered**:
- Self-hosted runners: More control but requires infrastructure maintenance
- Cross-compilation: Complex and error-prone for DuckDB extensions
- Third-party ARM runners (Actuated, Buildjet): Additional cost and complexity

**Platform to Runner Mapping**:

| Platform | Runner | Architecture |
|----------|--------|--------------|
| linux_amd64 | ubuntu-22.04 | x86_64 native |
| linux_arm64 | ubuntu-22.04-arm | ARM64 native |
| osx_arm64 | macos-14 | ARM64 native |
| windows_amd64 | windows-latest | x86_64 native |

### 2. DuckDB Version Pinning Strategy

**Decision**: Clone DuckDB by tag for stable versions, by main branch for nightly

**Rationale**:
- DuckDB releases are tagged as `v{VERSION}` (e.g., `v1.4.1`)
- Cloning by tag ensures reproducible builds
- Main branch gives latest features for PR validation
- Git commit hash must be logged for traceability

**Implementation**:
```bash
# For stable version (e.g., 1.4.1)
git clone --depth 1 --branch v1.4.1 https://github.com/duckdb/duckdb.git

# For nightly
git clone --depth 1 https://github.com/duckdb/duckdb.git
COMMIT_HASH=$(git -C duckdb rev-parse HEAD)
```

**Alternatives Considered**:
- Git submodule updates: More complex, requires submodule management
- Pre-built DuckDB binaries: Not available for all platforms/versions
- duckdb/extension-ci-tools: Good but adds external dependency complexity

### 3. Build System Configuration

**Decision**: Use existing Makefile-based build with vcpkg toolchain

**Rationale**:
- Project already has a working `Makefile` with `release` target
- vcpkg provides mbedTLS for TLS support in loadable extension
- CMake + Ninja generator is already configured
- Cross-platform compatibility is established

**Build Commands**:
```bash
# Unix (Linux/macOS)
make release

# Windows (direct CMake)
mkdir -p build/release
cd build/release
cmake -G "Ninja" \
  -DCMAKE_BUILD_TYPE=Release \
  -DDUCKDB_EXTENSION_CONFIGS="$(pwd)/../../extension_config.cmake" \
  ../../duckdb
cmake --build . --config Release
```

**Alternatives Considered**:
- DuckDB extension-ci-tools workflow: Mature but complex, less control
- Pure CMake without Makefile: Requires more workflow logic
- Bazel: Overkill for this project size

### 4. Smoke Test Strategy

**Decision**: Two-tier testing: load-only (all platforms) + integration (Linux only)

**Rationale**:
- All platforms: Verify extension loads without errors
- Linux only: Full SQL Server integration via Docker
- macOS/Windows: Docker support varies, load test is reliable
- Existing docker-compose.yml can be reused for CI

**Load-Only Test** (all platforms):
```bash
./build/release/duckdb -c "
  SELECT mssql_version();
"
```

**Integration Test** (Linux only):
```bash
# Start SQL Server container
docker-compose -f docker/docker-compose.yml up -d sqlserver

# Wait for healthy state
timeout 120 bash -c 'until docker-compose ps sqlserver | grep -q healthy; do sleep 5; done'

# Run smoke test SQL
./build/release/duckdb < scripts/sql/smoke_test.sql
```

**Alternatives Considered**:
- Skip macOS/Windows tests entirely: Misses platform-specific issues
- Use Azure SQL Database: Adds external dependency, cost
- Windows Docker: Unreliable in GitHub Actions

### 5. Artifact Naming Convention

**Decision**: `mssql-{EXT_VERSION}-duckdb-{DUCKDB_VERSION}-{PLATFORM}.duckdb_extension`

**Rationale**:
- `EXT_VERSION`: From git tag (e.g., `v0.1.0` → `0.1.0`)
- `DUCKDB_VERSION`: Target DuckDB version (e.g., `1.4.1`)
- `PLATFORM`: Canonical identifier (linux_amd64, linux_arm64, osx_arm64, windows_amd64)
- Single artifact per matrix entry, not a directory or archive

**Example**:
```
mssql-0.1.0-duckdb-1.4.1-linux_amd64.duckdb_extension
mssql-0.1.0-duckdb-1.4.1-linux_arm64.duckdb_extension
mssql-0.1.0-duckdb-1.4.1-osx_arm64.duckdb_extension
mssql-0.1.0-duckdb-1.4.1-windows_amd64.duckdb_extension
... (repeat for 1.4.2, 1.4.3)
```

**Alternatives Considered**:
- Zip archives per platform: Unnecessary complexity
- Version in directory structure: Harder to download individual files
- Different naming scheme: Would deviate from DuckDB community conventions

### 6. GitHub Release Publishing

**Decision**: Use `softprops/action-gh-release` with automatic release creation

**Rationale**:
- Well-maintained action with good API
- Automatically creates release if not exists
- Supports glob patterns for artifact upload
- Handles checksums as additional assets

**Implementation**:
```yaml
- uses: softprops/action-gh-release@v2
  with:
    files: |
      build/*.duckdb_extension
      SHA256SUMS.txt
    fail_on_unmatched_files: true
```

**Alternatives Considered**:
- `gh release create`: Requires more scripting
- `actions/upload-release-asset`: Deprecated
- Manual release creation: Error-prone, defeats automation purpose

### 7. Checksum Generation

**Decision**: Generate `SHA256SUMS.txt` in standard format (BSD-style)

**Rationale**:
- SHA256 is widely supported and secure
- BSD-style format works with `shasum -c` on all platforms
- Single file containing all checksums is easier to manage

**Format**:
```
abc123def456...  mssql-0.1.0-duckdb-1.4.1-linux_amd64.duckdb_extension
789012ghi345...  mssql-0.1.0-duckdb-1.4.1-linux_arm64.duckdb_extension
...
```

**Implementation**:
```bash
# On Linux/macOS
sha256sum *.duckdb_extension > SHA256SUMS.txt

# On Windows (PowerShell)
Get-FileHash *.duckdb_extension -Algorithm SHA256 |
  ForEach-Object { "$($_.Hash.ToLower())  $($_.Path | Split-Path -Leaf)" } > SHA256SUMS.txt
```

### 8. Workflow Trigger Configuration

**Decision**: Release triggers on `v*` tags, CI triggers on PR to main

**Rationale**:
- Tag-based release ensures intentional releases
- PR validation catches issues before merge
- Concurrency controls prevent duplicate runs

**Release Trigger**:
```yaml
on:
  push:
    tags:
      - 'v*'
```

**CI Trigger**:
```yaml
on:
  pull_request:
    branches: [main]
  push:
    branches: [main]
```

### 9. DuckDB Submodule vs Fresh Clone

**Decision**: Use fresh clone instead of submodule for CI builds

**Rationale**:
- Fresh clone allows targeting specific DuckDB versions
- Submodule is pinned to a specific commit, not suitable for multi-version builds
- Clone is simpler to manage in workflow context
- Existing submodule is for local development only

**Implementation**:
```bash
# Remove existing submodule for build
rm -rf duckdb

# Clone specific version
git clone --depth 1 --branch v${DUCKDB_VERSION} https://github.com/duckdb/duckdb.git

# Log commit hash
git -C duckdb rev-parse HEAD
```

### 10. Matrix Strategy for Release Workflow

**Decision**: Full matrix with fail-fast disabled

**Rationale**:
- 4 platforms × 3 DuckDB versions = 12 builds
- `fail-fast: false` ensures all builds complete even if some fail
- Users on unaffected platforms can still get working builds

**Matrix Definition**:
```yaml
strategy:
  fail-fast: false
  matrix:
    platform:
      - {os: linux, arch: amd64, runner: ubuntu-22.04}
      - {os: linux, arch: arm64, runner: ubuntu-22.04-arm}
      - {os: osx, arch: arm64, runner: macos-14}
      - {os: windows, arch: amd64, runner: windows-latest}
    duckdb_version: ['1.4.1', '1.4.2', '1.4.3']
```

## Summary of Decisions

| Topic | Decision |
|-------|----------|
| Runner selection | GitHub-hosted runners for all platforms |
| DuckDB versioning | Clone by tag for stable, main for nightly |
| Build system | Existing Makefile with vcpkg toolchain |
| Smoke tests | Two-tier: load-only (all) + integration (Linux) |
| Artifact naming | `mssql-{EXT}-duckdb-{DB}-{PLATFORM}.duckdb_extension` |
| Release publishing | `softprops/action-gh-release` |
| Checksums | SHA256SUMS.txt in BSD format |
| Workflow triggers | Tags for release, PR/push for CI |
| DuckDB source | Fresh clone (not submodule) |
| Matrix strategy | 4×3 with fail-fast disabled |

# Feature Specification: CI/CD Release Workflows

**Feature Branch**: `011-ci-release-workflows`
**Created**: 2026-01-19
**Status**: Draft
**Input**: User description: "GitHub Actions workflows for building, testing, and releasing the mssql-extension across multiple platforms and DuckDB versions"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Release Build and Publish (Priority: P1)

As a maintainer, when I push a version tag (e.g., `v0.1.0`), the CI system automatically builds the extension for all supported platforms and DuckDB versions, runs smoke tests, and publishes all artifacts to the GitHub Release.

**Why this priority**: This is the core deliverable - without automated release builds, users cannot easily obtain pre-built binaries for their platform and DuckDB version.

**Independent Test**: Tag a release on the repository and verify all 12 artifacts (4 platforms × 3 DuckDB versions) appear on the GitHub Release with correct naming and checksums.

**Acceptance Scenarios**:

1. **Given** the main branch is ready for release, **When** a maintainer pushes tag `v0.1.0`, **Then** the workflow triggers and builds artifacts for all 12 matrix combinations (linux_amd64, linux_arm64, osx_arm64, windows_amd64 × DuckDB 1.4.1, 1.4.2, 1.4.3)
2. **Given** builds complete successfully, **When** the release job runs, **Then** all artifacts are uploaded to the GitHub Release with naming pattern `mssql-{EXT_VERSION}-duckdb-{DUCKDB_VERSION}-{PLATFORM}.duckdb_extension`
3. **Given** all artifacts are uploaded, **When** the checksum job runs, **Then** `SHA256SUMS.txt` containing checksums for all artifacts is attached to the release
4. **Given** the workflow runs, **When** any build step executes, **Then** build metadata (extension version, DuckDB version requested, DuckDB commit hash used, platform) is logged for reproducibility

---

### User Story 2 - PR Validation (Priority: P2)

As a contributor, when I open a pull request, the CI system builds the extension on representative platforms against DuckDB nightly to catch compatibility issues early.

**Why this priority**: PR validation prevents broken code from being merged, maintaining code quality. It's less critical than releases since code can be fixed before tagging.

**Independent Test**: Open a PR with a small change and verify the CI workflow runs, builds succeed on linux_amd64 and osx_arm64, and smoke tests pass.

**Acceptance Scenarios**:

1. **Given** a PR is opened or updated, **When** the CI workflow triggers, **Then** the extension builds successfully against DuckDB nightly on linux_amd64 and osx_arm64
2. **Given** the linux_amd64 build completes, **When** the smoke test runs, **Then** a SQL Server container starts and the extension can ATTACH and execute a simple query
3. **Given** any platform build completes, **When** the load test runs, **Then** DuckDB successfully loads the extension without errors

---

### User Story 3 - Linux Integration Smoke Test (Priority: P2)

As a developer, when builds complete on Linux runners, a SQL Server container starts and the extension is verified to connect, execute queries, and perform basic DDL/DML operations.

**Why this priority**: Integration tests ensure the extension actually works with SQL Server, not just that it compiles. Critical for catching runtime issues.

**Independent Test**: Run the smoke test workflow manually on Linux and verify successful SQL Server connection, SELECT, and optional DDL/DML.

**Acceptance Scenarios**:

1. **Given** the extension is built on a Linux runner, **When** the smoke test starts, **Then** a SQL Server container (via docker-compose or service container) becomes available within a reasonable time
2. **Given** the SQL Server container is running, **When** the smoke test script executes, **Then** the extension loads, ATTACHes to MSSQL, and executes a basic SELECT successfully
3. **Given** the SELECT succeeds, **When** extended smoke tests run, **Then** basic DDL (CREATE TABLE) and DML (INSERT, SELECT from created table) operations complete successfully

---

### User Story 4 - Load-Only Smoke Test on macOS/Windows (Priority: P3)

As a developer, when builds complete on macOS or Windows runners where Docker is unavailable or unreliable, the extension is still verified to load correctly in DuckDB.

**Why this priority**: While not testing full SQL Server connectivity, verifying the extension loads proves the binary is valid and linkage is correct for that platform.

**Independent Test**: Run the build workflow on osx_arm64 or windows_amd64 and verify DuckDB loads the extension without errors.

**Acceptance Scenarios**:

1. **Given** the extension is built on macOS or Windows, **When** Docker is unavailable, **Then** the workflow runs a load-only smoke test instead of full integration
2. **Given** the load test runs, **When** DuckDB attempts to load the extension, **Then** the extension loads successfully and reports its version
3. **Given** the load test passes, **When** the job completes, **Then** logs clearly indicate "Integration test skipped - Docker not available" if applicable

---

### User Story 5 - README Documentation Updates (Priority: P3)

As a user reading the README, I can see the minimum supported DuckDB version, platform support status, and license information.

**Why this priority**: Documentation helps users understand compatibility before downloading. Important but not blocking the core build/release functionality.

**Independent Test**: Read the README and verify it contains minimum DuckDB version (1.4.1), Windows caveat, and MIT license mention.

**Acceptance Scenarios**:

1. **Given** the README exists, **When** a user reads it, **Then** they see the minimum DuckDB version is 1.4.1
2. **Given** the README exists, **When** a user reads the platform support section, **Then** they see Windows is not fully tested with a note to contribute
3. **Given** the README exists, **When** a user checks the license, **Then** they see the project is MIT licensed

---

### Edge Cases

- What happens when a DuckDB version tag doesn't exist? The workflow fails with a clear error message identifying the invalid version.
- What happens when SQL Server container fails to start? The smoke test retries with timeout, then fails the job with diagnostic logs.
- What happens when GitHub Release doesn't exist for the tag? The workflow creates the release automatically if it doesn't exist.
- What happens when the build matrix contains an unsupported platform/version combination? The job is skipped with a clear message.
- What happens when artifact upload fails mid-way? The workflow retries the upload; if persistent failure, the release is marked as draft/incomplete.

## Requirements *(mandatory)*

### Functional Requirements

#### Release Workflow (`.github/workflows/release.yml`)

- **FR-001**: Workflow MUST trigger on tags matching pattern `v*`
- **FR-002**: Workflow MUST build for platforms: linux_amd64, linux_arm64, osx_arm64, windows_amd64
- **FR-003**: Workflow MUST build for DuckDB versions: 1.4.1, 1.4.2, 1.4.3
- **FR-004**: Workflow MUST use CMake in Release mode with vcpkg toolchain where applicable
- **FR-005**: Workflow MUST fetch DuckDB sources by tag (e.g., `v1.4.1`) for stable versions
- **FR-006**: Workflow MUST log the exact DuckDB git commit hash used for each build
- **FR-007**: Workflow MUST name artifacts as `mssql-{EXT_VERSION}-duckdb-{DUCKDB_VERSION}-{PLATFORM}.duckdb_extension`
- **FR-008**: Workflow MUST generate `SHA256SUMS.txt` containing checksums for all artifacts
- **FR-009**: Workflow MUST upload all artifacts and checksums to the GitHub Release for the triggering tag
- **FR-010**: Workflow MUST log build metadata: extension version, DuckDB version requested, DuckDB commit hash, platform

#### CI Workflow (`.github/workflows/ci.yml`)

- **FR-011**: Workflow MUST trigger on pull requests to the main branch
- **FR-012**: Workflow MUST build on at least linux_amd64 and osx_arm64 platforms
- **FR-013**: Workflow MUST build against DuckDB main branch (nightly)
- **FR-014**: Workflow MUST resolve and log the DuckDB commit hash for nightly builds

#### Smoke Tests

- **FR-015**: On Linux runners, workflow MUST start a SQL Server container for integration testing
- **FR-016**: Smoke test MUST load the built extension into DuckDB
- **FR-017**: Smoke test MUST ATTACH to SQL Server (with appropriate encryption settings)
- **FR-018**: Smoke test MUST execute a basic SELECT query
- **FR-019**: Smoke test SHOULD execute DDL (CREATE TABLE) and DML (INSERT) operations
- **FR-020**: On macOS and Windows, workflow MUST run a load-only smoke test when Docker is unavailable
- **FR-021**: Workflow MUST clearly log when integration tests are skipped due to Docker unavailability

#### Helper Scripts

- **FR-022**: Repository MUST include helper scripts for DuckDB source fetching (`scripts/fetch_duckdb.sh` or similar)
- **FR-023**: Repository MUST include smoke test SQL (`scripts/smoke_test.sql` or similar)
- **FR-024**: Repository MUST include smoke test runner (`scripts/run_smoke_test.sh` or similar)

#### README Updates

- **FR-025**: README MUST state minimum supported DuckDB version as 1.4.1
- **FR-026**: README MUST note that Windows environment is not fully tested and contributions are welcome
- **FR-027**: README MUST indicate the project is MIT licensed

### Key Entities

- **Build Matrix Entry**: Combination of platform (linux_amd64, linux_arm64, osx_arm64, windows_amd64) and DuckDB version (1.4.1, 1.4.2, 1.4.3)
- **Artifact**: Built extension binary with standardized naming convention
- **Checksum File**: SHA256SUMS.txt containing hashes of all artifacts
- **GitHub Release**: Target destination for all release artifacts, keyed by version tag

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Release workflow produces all 12 artifacts (4 platforms × 3 DuckDB versions) for each release tag
- **SC-002**: All release artifacts are downloadable from GitHub Releases with correct naming convention
- **SC-003**: SHA256 checksums in SHA256SUMS.txt match the actual artifact hashes when verified
- **SC-004**: PR builds complete successfully for linux_amd64 and osx_arm64 against DuckDB nightly
- **SC-005**: Linux smoke tests successfully connect to SQL Server and execute queries
- **SC-006**: Load smoke tests pass on all platforms (extension loads without errors)
- **SC-007**: Build metadata (DuckDB commit hash, version, platform) is visible in workflow logs for every build
- **SC-008**: README contains accurate compatibility and license information

## Assumptions

- GitHub Actions runners are available for all target platforms (ubuntu-latest for linux_amd64, self-hosted or alternative for linux_arm64, macos-latest for osx_arm64, windows-latest for windows_amd64)
- SQL Server container image (mcr.microsoft.com/mssql/server or similar) is available and usable on Linux runners
- The existing CMake build system and vcpkg configuration support cross-platform builds
- DuckDB source tags follow the pattern `v{VERSION}` (e.g., `v1.4.1`)
- The extension can be built as a loadable `.duckdb_extension` file
- GitHub token permissions allow creating releases and uploading assets

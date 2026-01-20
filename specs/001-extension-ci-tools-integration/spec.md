# Feature Specification: DuckDB Extension CI Tools Integration

**Feature Branch**: `001-extension-ci-tools-integration`
**Created**: 2026-01-20
**Status**: Draft
**Input**: User description: "Add DuckDB extension-ci-tools integration (community_extensions compatible)"

## Clarifications

### Session 2026-01-20

- Q: How should the existing custom Makefile functionality be preserved while integrating extension-ci-tools? → A: Include extension-ci-tools Makefile first, then add custom targets after (hybrid approach)

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Community CI Build Compatibility (Priority: P1)

As a DuckDB Community Extensions maintainer, I need the mssql-extension repository to be compatible with the standard Community Extensions CI workflow so that the extension can be built and distributed through the official DuckDB community channel.

**Why this priority**: This is the core requirement - without Community CI compatibility, the extension cannot be published through the official DuckDB community extensions infrastructure.

**Independent Test**: Can be fully tested by running `DUCKDB_GIT_VERSION=v1.4.3 make set_duckdb_version` followed by `make release` and verifying a loadable extension artifact is produced.

**Acceptance Scenarios**:

1. **Given** a fresh clone of the repository, **When** CI runs `DUCKDB_GIT_VERSION=v1.4.3 make set_duckdb_version`, **Then** DuckDB source is switched to the specified version without errors
2. **Given** DuckDB version has been set, **When** CI runs `make release`, **Then** a loadable extension artifact is produced in the expected output location
3. **Given** the repository contains a Makefile at root, **When** CI inspects available targets, **Then** standard targets (`set_duckdb_version`, `debug`, `release`, `test`) are available

---

### User Story 2 - Local Developer Build (Priority: P2)

As a developer working on the mssql-extension, I need to build the extension locally using standard DuckDB extension tooling commands so that my local workflow matches CI and I can reproduce CI builds.

**Why this priority**: Developer experience is important for maintainability but is secondary to the primary CI integration requirement.

**Independent Test**: Can be fully tested by running `make debug` and `make release` locally and loading the resulting extension in DuckDB.

**Acceptance Scenarios**:

1. **Given** a developer has cloned the repository with submodules, **When** they run `make debug`, **Then** a debug build of the extension is produced
2. **Given** a developer has cloned the repository with submodules, **When** they run `make release`, **Then** a release build of the extension is produced
3. **Given** a built extension, **When** a developer runs the DuckDB CLI and executes `LOAD mssql`, **Then** the extension loads successfully without errors

---

### User Story 3 - Test Execution in CI Environment (Priority: P3)

As a CI system, I need the test target to gracefully handle environments without SQL Server so that CI builds don't fail when integration tests cannot run.

**Why this priority**: Testing is important but the Community CI environment won't have SQL Server available, so graceful degradation is required.

**Independent Test**: Can be fully tested by running `make test` without SQL Server configured and verifying it completes without failure.

**Acceptance Scenarios**:

1. **Given** the CI environment has no SQL Server available, **When** CI runs `make test`, **Then** the test target completes successfully (skipping integration tests that require SQL Server)
2. **Given** a local environment with SQL Server configured, **When** a developer runs `make test`, **Then** integration tests execute normally

---

### Edge Cases

- What happens when the specified DuckDB version doesn't exist? The `set_duckdb_version` target should fail with a clear error message from the underlying git checkout.
- How does the system handle submodules not being initialized? Build should fail early with an informative error directing the user to initialize submodules.
- What happens if the existing Makefile conflicts with the new tooling? The hybrid approach includes extension-ci-tools first, then appends custom targets; any target name conflicts should be resolved by allowing extension-ci-tools targets to take precedence for CI compatibility.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Repository MUST include `extension-ci-tools` as a git submodule at path `./extension-ci-tools`
- **FR-002**: Repository MUST provide a root-level Makefile that exposes standard DuckDB extension targets: `set_duckdb_version`, `debug`, `release`, `test`
- **FR-003**: The `set_duckdb_version` target MUST accept a `DUCKDB_GIT_VERSION` environment variable and switch DuckDB source to the specified version/tag
- **FR-004**: DuckDB source MUST be located at `./duckdb` (either as submodule or fetched by CI tools)
- **FR-005**: The Makefile MUST use a hybrid approach: include extension-ci-tools Makefile first (providing CI-required targets), then define custom targets after (preserving Docker management, integration-test, vcpkg-setup, and other developer-oriented functionality)
- **FR-006**: Repository MUST include an `extension_config.cmake` file (if required by DuckDB extension build tooling) for proper build integration
- **FR-007**: The `test` target MUST gracefully handle environments without SQL Server by using environment variable detection to skip integration tests
- **FR-008**: Repository documentation MUST be updated with instructions for building using the new Makefile targets

### Key Entities

- **extension-ci-tools submodule**: Git submodule providing standard DuckDB extension build infrastructure
- **Root Makefile**: Entry point for CI and developers, includes rules from extension-ci-tools
- **extension_config.cmake**: Configuration file that bridges the extension's build system with DuckDB's extension build tooling
- **DuckDB submodule**: DuckDB source at `./duckdb` required for building the extension

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The command `DUCKDB_GIT_VERSION=v1.4.3 make set_duckdb_version` completes successfully on a fresh repository clone
- **SC-002**: The command `make release` produces a loadable extension file that can be verified with `LOAD mssql` in DuckDB
- **SC-003**: The command `make test` completes without failure when no SQL Server is available (skips integration tests gracefully)
- **SC-004**: All changes are contained to: adding submodule, adding Makefile, adding config file (if needed), and minimal README update
- **SC-005**: Existing maintainer workflows continue to function after the integration

## Assumptions

- The `duckdb/extension-ci-tools` repository provides the standard Makefile include at `extension-ci-tools/makefiles/duckdb_extension.Makefile`
- DuckDB source can be managed either via submodule at `./duckdb` or fetched by the CI tools
- The existing build system can be integrated with minimal configuration changes
- Environment variable detection can be used to conditionally skip SQL Server-dependent tests

## Dependencies

- External dependency: `duckdb/extension-ci-tools` repository (git submodule)
- External dependency: DuckDB source repository (submodule or CI-fetched)
- Existing dependency: vcpkg for OpenSSL and other dependencies (must remain compatible)

## Out of Scope

- Changes to the core extension functionality
- Modifications to existing test logic beyond adding skip conditions
- Changes to existing build configuration beyond minimal integration glue
- Automated publishing to DuckDB Community Extensions (separate process)

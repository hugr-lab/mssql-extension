# Feature Specification: Skip Tests in Community Extensions CI

**Feature Branch**: `015-ci-test-skip`
**Created**: 2026-01-20
**Status**: Draft
**Input**: User description: "The workflow in the community extension repo fails because the CI runs `make release_test`, and it fails because SQL Server is not set up. Skip all extension tests in the community_extensions repo."

## Problem Statement

The DuckDB community-extensions CI workflow runs `make release_test` on all submitted extensions. For the MSSQL extension, tests require a live SQL Server instance, which is not available in the community-extensions CI environment. This causes the PR workflow to fail, blocking the extension from being published.

**Relevant Links**:
- Failed workflow: https://github.com/duckdb/community-extensions/actions/runs/21183598944/job/60934910147
- PR: https://github.com/duckdb/community-extensions/pull/1104

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Community Extensions CI Success (Priority: P1)

As an extension maintainer, I want the MSSQL extension to pass the community-extensions CI workflow without requiring a live SQL Server instance, so that the extension can be published to the DuckDB community extensions repository.

**Why this priority**: This is the primary goal - without passing CI, the extension cannot be published.

**Independent Test**: Submit a PR to the community-extensions repository and verify the workflow completes successfully without SQL Server-dependent test failures.

**Acceptance Scenarios**:

1. **Given** the MSSQL extension is submitted to the community-extensions repository, **When** the CI workflow runs, **Then** the build completes successfully and tests are either skipped or pass without requiring SQL Server.

2. **Given** the extension's `description.yml` is properly configured, **When** the community-extensions CI processes the extension, **Then** no test failures occur due to missing SQL Server connectivity.

---

### User Story 2 - Local Development Testing Preserved (Priority: P2)

As a developer working on the MSSQL extension, I want to continue running integration tests locally with a SQL Server instance, so that I can verify my changes work correctly against a real database.

**Why this priority**: Developers need to be able to test their changes thoroughly before submitting PRs.

**Independent Test**: Run `make integration-test` locally with Docker SQL Server running and verify all tests execute.

**Acceptance Scenarios**:

1. **Given** a developer has SQL Server running locally (via Docker), **When** they run `make integration-test`, **Then** all SQL Server-dependent tests execute and can pass or fail based on actual functionality.

2. **Given** the `MSSQL_TEST_DSN` environment variable is set, **When** tests are run, **Then** the integration tests execute against the configured SQL Server instance.

---

### User Story 3 - Graceful Test Skip (Priority: P3)

As a developer without SQL Server access, I want tests that require SQL Server to be skipped gracefully, so that I can still run other tests and verify non-SQL Server functionality.

**Why this priority**: Supports development workflows where SQL Server is not always available.

**Independent Test**: Run tests without `MSSQL_TEST_DSN` set and verify no test failures occur (tests requiring SQL Server are skipped).

**Acceptance Scenarios**:

1. **Given** the `MSSQL_TEST_DSN` environment variable is not set, **When** the test suite runs, **Then** tests with `require-env MSSQL_TEST_DSN` are skipped (not failed).

---

### Edge Cases

- What happens when partial SQL Server configuration is provided (e.g., host but no password)?
  - Tests requiring full connectivity should be skipped; no sensitive errors should be exposed.

- How does the system handle tests that don't require SQL Server?
  - These tests (like `mssql_version.test`) should always run and pass regardless of SQL Server availability.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The extension's `description.yml` MUST be configured to skip tests in the community-extensions CI environment.

- **FR-002**: Tests that use `require-env MSSQL_TEST_DSN` MUST be skipped (not fail) when the environment variable is not set.

- **FR-003**: Tests that do not require SQL Server connectivity MUST continue to run in all environments.

- **FR-004**: The local development test workflow (`make integration-test`) MUST continue to work when SQL Server is available.

- **FR-005**: The configuration MUST use the supported mechanisms in the DuckDB community-extensions CI (i.e., `test_config` in `description.yml` or appropriate test skip configuration).

### Configuration Approach

Based on research of the community-extensions CI infrastructure:

1. **Option A - Use `test_config` with skip mechanism**: Add `test_config` to `description.yml` that sets `SKIP_TESTS=1` or uses environment variables that cause tests to be skipped.

2. **Option B - Ensure tests self-skip**: Rely on DuckDB's `require-env` directive which already skips tests when required environment variables are not set (this is the existing behavior).

**Assumption**: Option B (existing `require-env` behavior) should work automatically if the community-extensions CI does not set `MSSQL_TEST_DSN`. The issue may be that the CI sets some generic test DSN or the tests are failing for another reason. Investigation shows tests should naturally skip via `require-env MSSQL_TEST_DSN`.

### Key Entities

- **description.yml**: Extension configuration file for community-extensions that controls build and test behavior.
- **test_config**: Optional field in description.yml that provides JSON configuration passed to test runs.
- **Test files**: SQL test files using `require-env` directive to conditionally require environment variables.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The community-extensions CI workflow completes successfully (no test failures) for the MSSQL extension PR.

- **SC-002**: Local integration tests continue to run successfully when SQL Server is available and `MSSQL_TEST_DSN` is set.

- **SC-003**: Tests that don't require SQL Server (e.g., `mssql_version.test`) pass in all CI environments.

- **SC-004**: No changes required to existing test files beyond potential configuration adjustments.

## Assumptions

- The `require-env` directive in DuckDB test files causes tests to be skipped (not failed) when the specified environment variable is not set.
- The community-extensions CI does not pre-set the `MSSQL_TEST_DSN` environment variable.
- The workflow failure is specifically due to tests attempting to connect to SQL Server, not other build or configuration issues.

## Out of Scope

- Modifying the community-extensions CI infrastructure itself.
- Setting up SQL Server in the community-extensions CI (not feasible for extension-specific requirements).
- Running integration tests as part of community-extensions CI (these are meant for local development only).

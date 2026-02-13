# Feature Specification: DuckDB v1.5 Variegata Upgrade

**Feature Branch**: `034-duckdb-v15-upgrade`
**Created**: 2026-02-13
**Status**: Draft
**Input**: User description: "Upgrade DuckDB submodule to v1.5-variegata branch, build extension, run integration tests, and apply fixes as needed"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Build Extension Against DuckDB v1.5 (Priority: P1)

As a developer, I want the MSSQL extension to compile successfully against the DuckDB v1.5-variegata branch so that the extension stays compatible with the latest DuckDB release.

**Why this priority**: Without a successful build, no other functionality can be verified. This is the foundational gate for all subsequent work.

**Independent Test**: Switch the DuckDB submodule to the v1.5-variegata branch and run the build process. A successful build with zero compilation errors confirms this story is complete.

**Acceptance Scenarios**:

1. **Given** the DuckDB submodule is pointed to the v1.5-variegata branch, **When** the extension is built in release mode, **Then** the build completes with zero errors.
2. **Given** the extension is built successfully, **When** it is loaded into DuckDB, **Then** `mssql_version()` returns the current version number.

---

### User Story 2 - Pass All Existing Tests (Priority: P2)

As a developer, I want all existing unit and integration tests to pass against DuckDB v1.5 so that no regressions are introduced by the upgrade.

**Why this priority**: Passing tests confirm backward compatibility and correctness of the extension with the new DuckDB version.

**Independent Test**: Run the full test suite (unit tests and integration tests) and verify all tests pass or fail only due to known, documented API changes.

**Acceptance Scenarios**:

1. **Given** the extension is built against DuckDB v1.5, **When** unit tests are executed, **Then** all unit tests pass.
2. **Given** a SQL Server test instance is available, **When** integration tests are executed, **Then** all integration tests pass.
3. **Given** a test fails due to a DuckDB API change, **When** the failure is analyzed, **Then** the extension code is updated to use the new API while maintaining backward compatibility via the compat layer.

---

### User Story 3 - Apply Compatibility Fixes (Priority: P3)

As a developer, I want any breaking API changes in DuckDB v1.5 to be handled through the existing compatibility layer so that the extension continues to support both stable and nightly DuckDB builds.

**Why this priority**: The extension must support multiple DuckDB versions. Fixes should use the existing compat pattern rather than breaking support for older versions.

**Independent Test**: Build the extension with both the current DuckDB main branch and v1.5-variegata to confirm dual compatibility.

**Acceptance Scenarios**:

1. **Given** a DuckDB API has changed in v1.5, **When** the extension encounters the changed API, **Then** the compatibility layer handles both old and new API signatures.
2. **Given** the compatibility fixes are applied, **When** the extension is built against DuckDB main (pre-v1.5), **Then** the build still succeeds.

---

### Edge Cases

- What happens if DuckDB v1.5 removes a previously deprecated API that the extension depends on?
- How does the extension handle new required virtual method overrides introduced in DuckDB v1.5?
- What if DuckDB v1.5 changes the catalog or type system in a way that affects metadata caching?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Extension MUST compile without errors against DuckDB v1.5-variegata branch
- **FR-002**: Extension MUST pass all existing unit tests (tests not requiring SQL Server)
- **FR-003**: Extension MUST pass all existing integration tests (tests requiring SQL Server)
- **FR-004**: Extension MUST maintain compatibility with the previous DuckDB version via the existing compat layer
- **FR-005**: Extension MUST correctly load and register all functions, types, and catalog entries in DuckDB v1.5
- **FR-006**: Any API compatibility changes MUST be documented in the compat header with version detection macros

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Extension builds with zero compilation errors on the primary platform
- **SC-002**: 100% of existing unit tests pass (excluding tests skipped for environment reasons)
- **SC-003**: 100% of existing integration tests pass when a SQL Server instance is available
- **SC-004**: Extension loads successfully and `mssql_version()` returns the correct version
- **SC-005**: All core operations (ATTACH, SELECT, INSERT, UPDATE, DELETE, COPY TO) function correctly against SQL Server

## Assumptions

- The DuckDB v1.5-variegata branch is available in the DuckDB repository and is stable enough for extension development
- Breaking API changes in DuckDB v1.5 are limited in scope and can be addressed through the existing compatibility macro pattern
- The SQL Server test container (docker) is available for integration testing
- No new third-party dependencies are required for the upgrade

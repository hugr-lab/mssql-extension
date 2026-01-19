# Feature Specification: Documentation, Platform Support & Cache Refresh

**Feature Branch**: `012-docs-platform-refresh`
**Created**: 2026-01-19
**Status**: Draft
**Input**: User description: "Update README with experimental status, implement mssql_refresh_cache function, add platform support documentation, and create description.yml for community extension"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Manually Refresh Metadata Cache (Priority: P1)

A DuckDB user has an attached SQL Server database and schema changes have been made on the SQL Server side (new tables added, columns modified, etc.). The user wants to see these changes reflected in DuckDB without detaching and reattaching the database.

**Why this priority**: This is core functionality that was planned but never implemented (stub exists). It completes the catalog integration feature set and enables practical use cases where SQL Server schemas evolve.

**Independent Test**: Can be fully tested by calling `mssql_refresh_cache('catalog_name')` after attaching a SQL Server database, then verifying the cache state changed. Delivers immediate value for users working with evolving SQL Server schemas.

**Acceptance Scenarios**:

1. **Given** a DuckDB session with an attached SQL Server database named 'sqlserver', **When** the user executes `SELECT mssql_refresh_cache('sqlserver')`, **Then** the function returns `true` indicating the cache was successfully refreshed
2. **Given** a DuckDB session with no attached MSSQL databases, **When** the user executes `SELECT mssql_refresh_cache('nonexistent')`, **Then** the function returns an error indicating the catalog was not found
3. **Given** a DuckDB session with an attached SQL Server database, **When** a new table is created on SQL Server and `mssql_refresh_cache()` is called, **Then** the new table becomes visible via `duckdb_tables()` without reattaching
4. **Given** a DuckDB session with an attached SQL Server database, **When** the user executes `SELECT mssql_refresh_cache('sqlserver')` and the SQL Server is unreachable, **Then** the function returns an appropriate connection error

---

### User Story 2 - Understand Platform Support (Priority: P2)

A potential user or contributor wants to understand which platforms are supported by the extension before installing or contributing.

**Why this priority**: Clear platform documentation prevents frustration and sets correct expectations for users across different operating systems.

**Independent Test**: Can be validated by reading the README and verifying platform support information is accurate, clear, and complete.

**Acceptance Scenarios**:

1. **Given** a user reading the README, **When** they look for platform support information, **Then** they find a clear table showing macOS ARM64 as primary development platform
2. **Given** a user reading the README, **When** they check Linux support, **Then** they see Linux amd64 is CI-validated and Linux arm64 is not tested/not built in CD
3. **Given** a user reading the README, **When** they check Windows support, **Then** they see Windows amd64 is not tested/not built in CD

---

### User Story 3 - Identify Experimental Status (Priority: P2)

A potential user wants to understand the maturity level of the extension before using it in their projects.

**Why this priority**: Clear communication about experimental status helps users make informed decisions and encourages contributions.

**Independent Test**: Can be validated by reading the README header section for experimental status notice.

**Acceptance Scenarios**:

1. **Given** a user reading the README, **When** they view the introduction section, **Then** they see a clear notice that the extension is experimental and under active development
2. **Given** a potential contributor reading the README, **When** they look for contribution information, **Then** they see a welcome message encouraging testing and contributions

---

### User Story 4 - Submit to DuckDB Community Extensions (Priority: P3)

A maintainer wants to submit the extension to the DuckDB community extensions repository with proper metadata.

**Why this priority**: Community extension submission enables wider distribution but requires documentation and implementation to be complete first.

**Independent Test**: Can be validated by checking the description.yml file validates against community extension schema requirements.

**Acceptance Scenarios**:

1. **Given** the repository root, **When** a maintainer looks for the description.yml file, **Then** they find a valid YAML file with all required fields
2. **Given** the description.yml file, **When** parsed, **Then** it contains correct extension name, version, description, license, and maintainer information
3. **Given** the description.yml file, **When** checked against DuckDB community extension requirements, **Then** excluded platforms (osx_amd64, windows_arm64) are correctly specified
4. **Given** the description.yml file, **When** reviewed for documentation, **Then** it includes a working hello_world example and extended description

---

### Edge Cases

- **Empty string argument**: System throws an error indicating a valid catalog name is required
- **No arguments**: System throws an error indicating the catalog name argument is required
- **Connection pool exhausted**: System waits for available connection (respects `mssql_acquire_timeout` setting), then throws timeout error if exceeded
- **Metadata query timeout**: System propagates the connection/query timeout error to the caller

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST provide a scalar function `mssql_refresh_cache(VARCHAR) -> BOOLEAN` that manually refreshes the metadata cache for the specified attached catalog
- **FR-002**: System MUST return `true` from `mssql_refresh_cache()` when cache refresh succeeds
- **FR-003**: System MUST throw an informative error when `mssql_refresh_cache()` is called with a non-existent catalog name
- **FR-004**: System MUST register `mssql_refresh_cache` function during extension load
- **FR-005**: README MUST contain an experimental status notice in the introduction section
- **FR-006**: README MUST contain a welcome message for contributors and testers
- **FR-007**: README MUST contain accurate platform support table with: macOS ARM64 (primary development), Linux amd64 (CI-validated), Linux arm64 (not tested/not built), Windows amd64 (not tested/not built)
- **FR-008**: Repository MUST contain a `description.yml` file in the root directory conforming to DuckDB community extension specification
- **FR-009**: The `description.yml` MUST specify excluded platforms: `osx_amd64` and `windows_arm64`
- **FR-010**: The `description.yml` MUST include a working hello_world SQL example
- **FR-011**: README function reference section MUST document `mssql_refresh_cache()` with signature, description, and usage example
- **FR-012**: System MUST throw an error when `mssql_refresh_cache()` is called with an empty string or no arguments

### Key Entities

- **MSSQLRefreshCacheFunction**: Scalar function implementation that interfaces with MSSQLCatalog to trigger cache invalidation and refresh
- **description.yml**: YAML metadata file for DuckDB community extension registry

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can refresh metadata cache for an attached MSSQL database without detaching
- **SC-002**: Users can identify the extension's experimental status within 10 seconds of opening the README
- **SC-003**: Users can determine platform support status for their OS within 30 seconds of opening the README
- **SC-004**: The extension metadata file passes DuckDB community extension validation
- **SC-005**: All existing tests continue to pass after changes
- **SC-006**: New unit tests verify `mssql_refresh_cache()` function behavior (success, error cases)

## Clarifications

### Session 2026-01-19

- Q: What should `mssql_refresh_cache()` do when called with an empty string or no arguments? → A: Throw an error (argument required, empty string invalid)

## Assumptions

- The existing `MSSQLCatalog::EnsureCacheLoaded()` and `MSSQLMetadataCache::Refresh()` methods provide the underlying cache refresh capability
- The `InvalidateMetadataCache()` method in MSSQLCatalog is the correct entry point for forcing a cache refresh
- DuckDB community extension description.yml schema follows the template provided by the user
- Platform exclusions in description.yml effectively prevent builds for those platforms in the community CI

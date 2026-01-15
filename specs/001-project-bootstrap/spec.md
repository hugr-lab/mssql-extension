# Feature Specification: Project Bootstrap and Tooling

**Feature Branch**: `001-project-bootstrap`
**Created**: 2026-01-15
**Status**: Draft
**Input**: Spec 01 — Project Bootstrap and Tooling

## Clarifications

### Session 2026-01-15

- Q: Which DuckDB version should the extension target? → A: Track DuckDB main branch (bleeding edge)
- Q: What primary key structure for test tables? → A: Both single-column PK and composite PK tables

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Build Extension from Source (Priority: P1)

A developer clones the repository and builds the DuckDB extension from source on
their local machine (Linux or Windows) using a single command.

**Why this priority**: Without a working build, no other development can proceed.
This is the foundational capability that enables all subsequent work.

**Independent Test**: Can be fully tested by running the build command on a fresh
clone and verifying the extension binary is produced.

**Acceptance Scenarios**:

1. **Given** a fresh clone of the repository on Linux, **When** the developer runs
   the build command, **Then** the extension binary is produced without errors.

2. **Given** a fresh clone of the repository on Windows, **When** the developer runs
   the build command, **Then** the extension binary is produced without errors.

3. **Given** build dependencies are not pre-installed, **When** the developer runs
   the build command, **Then** vcpkg automatically fetches required dependencies.

---

### User Story 2 - Load Extension in DuckDB (Priority: P1)

A developer loads the built extension into a running DuckDB instance to verify
it initializes correctly.

**Why this priority**: The extension must load successfully before any functionality
can be tested. This validates the build output is a valid DuckDB extension.

**Independent Test**: Can be fully tested by starting DuckDB and loading the
extension, observing successful initialization.

**Acceptance Scenarios**:

1. **Given** a successfully built extension, **When** the developer loads it into
   DuckDB, **Then** DuckDB reports the extension loaded successfully.

2. **Given** a successfully built extension, **When** the developer queries
   extension metadata, **Then** the extension name and version are displayed.

---

### User Story 3 - Debug Extension in VSCode (Priority: P2)

A developer uses VSCode to build the extension, set breakpoints, and step through
code during debugging sessions.

**Why this priority**: Debugging capability accelerates development but is not
required for the extension to function. Developers can use other tools initially.

**Independent Test**: Can be fully tested by opening the project in VSCode, setting
a breakpoint, and starting a debug session.

**Acceptance Scenarios**:

1. **Given** the project is open in VSCode, **When** the developer triggers the
   build task, **Then** the extension compiles with debug symbols.

2. **Given** a debug build, **When** the developer starts a debug session, **Then**
   breakpoints are hit and variables can be inspected.

---

### User Story 4 - Start SQL Server Development Environment (Priority: P2)

A developer starts a local SQL Server instance with sample data for testing
extension connectivity in future milestones.

**Why this priority**: The SQL Server environment is needed for integration testing
but is not required for the build/load smoke test. It prepares the foundation for
protocol development.

**Independent Test**: Can be fully tested by starting docker-compose and connecting
to the SQL Server instance with standard tools.

**Acceptance Scenarios**:

1. **Given** Docker is installed, **When** the developer runs docker-compose up,
   **Then** SQL Server starts and becomes available for connections.

2. **Given** SQL Server is running, **When** the developer connects to it, **Then**
   the test database, schema, and both test tables exist with sample data.

3. **Given** SQL Server is running, **When** the developer queries the test tables,
   **Then** rows with primary key values are returned from both simple and composite
   PK tables.

---

### Edge Cases

- What happens when Docker is not installed? Build should still succeed; only
  docker-compose will fail with a clear error message.
- What happens when vcpkg cache is corrupted? Build should detect and re-fetch
  dependencies automatically.
- What happens when VSCode extensions are missing? Build task should still work;
  debug launch may prompt for required extensions.
- What happens when SQL Server container port is already in use? docker-compose
  should fail with a clear port conflict message.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Build system MUST compile the extension on Linux (x86_64).
- **FR-002**: Build system MUST compile the extension on Windows (x64).
- **FR-003**: Build system MUST use vcpkg in manifest mode for dependency management.
- **FR-004**: Build output MUST be a valid DuckDB extension that can be loaded.
- **FR-005**: VSCode MUST provide a build task that compiles the extension.
- **FR-006**: VSCode MUST provide a debug launch configuration for the extension.
- **FR-007**: docker-compose MUST start SQL Server Developer Edition container.
- **FR-008**: SQL Server initialization MUST create a test database.
- **FR-009**: SQL Server initialization MUST create at least one schema.
- **FR-010**: SQL Server initialization MUST create a table with a single-column
  primary key and sample data (validates scalar rowid mapping).
- **FR-011**: SQL Server initialization MUST create a table with a composite
  primary key and sample data (validates STRUCT rowid mapping).
- **FR-012**: Extension skeleton MUST register with DuckDB and report its version.

### Key Entities

- **Extension**: The DuckDB loadable module; has name, version, and load state.
- **Test Database**: SQL Server database for development; contains schemas and tables.
- **Simple PK Table**: Sample table with single-column primary key; validates scalar
  rowid mapping in future integration tests.
- **Composite PK Table**: Sample table with multi-column primary key; validates
  STRUCT rowid mapping per constitution's Row Identity Model.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A new developer can build the extension from a fresh clone in under
  10 minutes (excluding initial vcpkg cache population).
- **SC-002**: The extension loads into DuckDB without errors on both Linux and
  Windows.
- **SC-003**: VSCode build task completes successfully and produces a loadable
  extension.
- **SC-004**: VSCode debug session can hit breakpoints in extension code.
- **SC-005**: docker-compose starts SQL Server and the test database is queryable
  within 60 seconds of container start.
- **SC-006**: Both test tables (simple PK and composite PK) contain sample data
  that can be queried.

## Assumptions

- Developers have standard build tools installed (CMake, C++ compiler).
- Developers have Docker installed for SQL Server testing (not required for build).
- VSCode is the primary IDE; other IDEs may work but are not explicitly supported.
- SQL Server Developer Edition is free for development use and acceptable for this
  project.
- vcpkg manifest mode handles all C++ dependencies without manual intervention.
- DuckDB extension tracks the main branch; builds may require periodic updates to
  match upstream API changes.

## Non-Goals

- No TDS protocol implementation in this milestone.
- No actual SQL Server connectivity from the extension.
- No macOS build support in this milestone (Linux and Windows only per requirements).
- No CI/CD pipeline setup (future milestone).
- No production deployment configuration.

# Feature Specification: Extension Documentation

**Feature Branch**: `010-extension-documentation`
**Created**: 2026-01-19
**Status**: Draft
**Input**: User description: "Create and update documentation for the extension including function descriptions, connection string formats, secret management, catalog integration, supported SQL features, IDE configuration, build instructions, and testing"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Quick Start Guide (Priority: P1)

A new user discovers the mssql-extension and wants to quickly understand how to install, connect to SQL Server, and run their first query without reading extensive documentation.

**Why this priority**: First impressions matter. Users who cannot get started quickly will abandon the extension. A clear quick start path reduces friction to adoption.

**Independent Test**: Can be tested by following the quick start section alone and successfully executing a SELECT query against SQL Server.

**Acceptance Scenarios**:

1. **Given** a user has DuckDB installed, **When** they follow the Quick Start section, **Then** they can load the extension and connect to SQL Server within 5 minutes of reading.
2. **Given** a user has no prior mssql-extension experience, **When** they read the Quick Start, **Then** they can execute a basic SELECT query and see results.
3. **Given** a user encounters a connection error, **When** they check the troubleshooting section, **Then** they find guidance for common issues (wrong credentials, network, TLS).

---

### User Story 2 - Function Reference Lookup (Priority: P1)

A developer using the extension needs to understand what functions are available and their exact signatures to write queries.

**Why this priority**: Function documentation is essential for daily use. Developers need accurate signatures and examples to write correct queries.

**Independent Test**: Can be tested by looking up any extension function and finding complete signature, parameters, return type, and working example.

**Acceptance Scenarios**:

1. **Given** a developer needs to execute a raw SQL statement, **When** they search for execute functions, **Then** they find mssql_execute() with signature, parameters, and example.
2. **Given** a developer wants to scan a table, **When** they look up mssql_scan(), **Then** they find complete documentation including streaming behavior.
3. **Given** a developer wants diagnostic functions, **When** they browse the function reference, **Then** they find mssql_open, mssql_close, mssql_ping, and mssql_pool_stats documented.

---

### User Story 3 - Connection Configuration (Priority: P1)

A user needs to configure connections using secrets or connection strings with various formats (ADO.NET, URI) and options (TLS, ports).

**Why this priority**: Connection configuration is required for any usage. Users must understand the connection options to integrate with their SQL Server infrastructure.

**Independent Test**: Can be tested by configuring a connection using either secret or connection string method and successfully attaching.

**Acceptance Scenarios**:

1. **Given** a user prefers ADO.NET connection strings, **When** they read connection documentation, **Then** they find the exact format with all supported parameters.
2. **Given** a user prefers URI-style connections, **When** they read connection documentation, **Then** they find mssql:// format with query parameters.
3. **Given** a user needs TLS encryption, **When** they configure the connection, **Then** documentation explains the encrypt option for both connection string and secret methods.
4. **Given** a user wants to store credentials securely, **When** they read secret management, **Then** they can create and use DuckDB secrets for mssql connections.

---

### User Story 4 - Catalog Integration Usage (Priority: P2)

A user has attached a SQL Server database and needs to understand how to browse schemas, discover tables, and query using catalog integration.

**Why this priority**: Catalog integration is a key feature that differentiates this extension. Users need to understand the three-part naming and schema browsing.

**Independent Test**: Can be tested by attaching a database and using SHOW SCHEMAS, SHOW TABLES, and three-part naming queries.

**Acceptance Scenarios**:

1. **Given** a user has attached a SQL Server database, **When** they want to list schemas, **Then** documentation shows SHOW SCHEMAS FROM syntax.
2. **Given** a user wants to query a specific table, **When** they read catalog docs, **Then** they understand context.schema.table naming.
3. **Given** a user wants to join remote and local data, **When** they read examples, **Then** they find working examples of cross-catalog joins.

---

### User Story 5 - INSERT and DML Operations (Priority: P2)

A user needs to insert data into SQL Server tables and wants to understand batching, RETURNING clause, and configuration options.

**Why this priority**: Data modification is a common use case. Users need to understand INSERT behavior, especially batching and RETURNING.

**Independent Test**: Can be tested by following INSERT examples and successfully inserting data with and without RETURNING clause.

**Acceptance Scenarios**:

1. **Given** a user needs to insert rows, **When** they read INSERT documentation, **Then** they find syntax for single, multiple, and SELECT-based inserts.
2. **Given** a user wants inserted IDs back, **When** they use RETURNING clause, **Then** documentation explains OUTPUT INSERTED behavior.
3. **Given** a user needs to tune batch sizes, **When** they read configuration, **Then** they find mssql_insert_batch_size and related settings.

---

### User Story 6 - Type Mapping Reference (Priority: P2)

A user needs to understand how SQL Server types map to DuckDB types to design queries and handle data correctly.

**Why this priority**: Type mapping affects data integrity and query correctness. Users need a reference for all supported types.

**Independent Test**: Can be tested by looking up any SQL Server type and finding its DuckDB equivalent with any conversion notes.

**Acceptance Scenarios**:

1. **Given** a user queries a table with DATETIME2, **When** they check type mapping, **Then** they find it maps to TIMESTAMP with precision notes.
2. **Given** a user has MONEY columns, **When** they check type mapping, **Then** they find it maps to DECIMAL(19,4).
3. **Given** a user has UNIQUEIDENTIFIER, **When** they check type mapping, **Then** they find it maps to UUID.

---

### User Story 7 - Building from Source (Priority: P3)

A developer wants to build the extension from source, either for contribution or custom deployment.

**Why this priority**: Build documentation enables contributions and custom deployments but is not needed for regular usage.

**Independent Test**: Can be tested by following build instructions on a fresh development environment and producing a working extension.

**Acceptance Scenarios**:

1. **Given** a developer clones the repository, **When** they follow build instructions, **Then** they can build release and debug versions.
2. **Given** a developer wants TLS support, **When** they read build docs, **Then** they understand the split TLS build approach.
3. **Given** a developer runs make test, **When** tests fail, **Then** documentation explains test requirements (Docker, SQL Server).

---

### User Story 8 - IDE and Development Configuration (Priority: P3)

A developer wants to configure their IDE for extension development with proper IntelliSense and debugging.

**Why this priority**: IDE configuration improves development experience but is only needed for contributors.

**Independent Test**: Can be tested by configuring VS Code or CLion with provided settings and getting code completion.

**Acceptance Scenarios**:

1. **Given** a developer uses VS Code, **When** they apply recommended settings, **Then** they get C++ IntelliSense for the codebase.
2. **Given** a developer uses CLion, **When** they import the CMake project, **Then** they can build and debug.
3. **Given** a developer wants to run tests, **When** they read test configuration, **Then** they can run unit and integration tests from IDE.

---

### User Story 9 - Configuration Reference (Priority: P2)

A user needs to tune connection pooling, caching, or other settings for production use.

**Why this priority**: Configuration tuning is important for production deployments and troubleshooting.

**Independent Test**: Can be tested by looking up any setting and finding its purpose, default value, and valid range.

**Acceptance Scenarios**:

1. **Given** a user needs to adjust pool size, **When** they search for pool settings, **Then** they find mssql_connection_limit with default and range.
2. **Given** a user wants to tune cache TTL, **When** they search for cache settings, **Then** they find mssql_catalog_cache_ttl.
3. **Given** a user needs all settings listed, **When** they read configuration reference, **Then** they find a complete table of all settings.

---

### Edge Cases

- What happens when documentation references features not yet implemented? Documentation must clearly mark unsupported features.
- How does documentation handle platform-specific differences (macOS, Linux, Windows)? Platform notes must be included where behavior differs.
- What happens when examples use SQL Server syntax not supported? Examples must be tested against actual SQL Server.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Documentation MUST include a Quick Start section enabling users to run first query within 5 minutes of reading
- **FR-002**: Documentation MUST provide complete function reference for all public functions (mssql_version, mssql_execute, mssql_scan, mssql_open, mssql_close, mssql_ping, mssql_pool_stats)
- **FR-003**: Documentation MUST explain both connection string formats (ADO.NET style and URI style) with all parameters
- **FR-004**: Documentation MUST explain secret management including CREATE SECRET syntax and all secret fields
- **FR-005**: Documentation MUST describe catalog integration including ATTACH syntax, schema browsing, and three-part naming
- **FR-006**: Documentation MUST include complete type mapping table between SQL Server and DuckDB types
- **FR-007**: Documentation MUST explain INSERT operations including batching, RETURNING clause, and configuration settings
- **FR-008**: Documentation MUST include build instructions for release, debug, and TLS-enabled builds
- **FR-009**: Documentation MUST list all configurable settings with defaults, ranges, and descriptions
- **FR-010**: Documentation MUST include troubleshooting section for common connection and query issues
- **FR-011**: Documentation MUST explain TLS/SSL configuration for encrypted connections
- **FR-012**: Documentation MUST include working code examples that can be copy-pasted
- **FR-013**: Documentation MUST clearly indicate features that are not yet implemented or partially supported

### Key Entities

- **README.md**: Primary user-facing documentation file in repository root
- **Function Reference**: Section documenting each extension function
- **Configuration Reference**: Section documenting all DuckDB settings
- **Type Mapping Table**: Reference table of SQL Server to DuckDB type conversions
- **Examples**: Runnable code snippets demonstrating features

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can install and run first query by following Quick Start without external assistance
- **SC-002**: All public functions are documented with signature, parameters, return type, and example
- **SC-003**: Both connection string formats have complete parameter documentation
- **SC-004**: Type mapping table covers all 20+ supported SQL Server types
- **SC-005**: Build instructions work on fresh development environment (macOS, Linux)
- **SC-006**: All configuration settings are documented with defaults and valid ranges
- **SC-007**: Documentation passes review for accuracy against current implementation

## Assumptions

- Documentation will be written primarily in README.md for discoverability
- Users have basic familiarity with DuckDB and SQL
- SQL Server 2019+ is the target for examples
- Docker examples use the official Microsoft SQL Server container image
- Code examples are tested against actual SQL Server before inclusion

# Implementation Plan: Project Bootstrap and Tooling

**Branch**: `001-project-bootstrap` | **Date**: 2026-01-15 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/001-project-bootstrap/spec.md`

## Summary

Establish a reproducible development environment and working DuckDB extension skeleton
that builds on Linux and Windows, loads into DuckDB, and can be debugged in VSCode.
Includes docker-compose SQL Server environment with test tables demonstrating both
simple and composite primary key patterns for future Row Identity Model validation.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB (main branch), vcpkg (manifest mode)
**Build System**: CMake 3.21+
**Storage**: N/A (extension skeleton only; SQL Server via docker-compose for dev)
**Testing**: DuckDB extension load test, SQL Server connectivity via sqlcmd/SSMS
**Target Platform**: Linux x86_64, Windows x64
**Project Type**: Single project (DuckDB extension)
**Performance Goals**: Build completes in <10 minutes; docker-compose ready in <60s
**Constraints**: No ODBC/JDBC/FreeTDS dependencies; vcpkg manifest mode required
**Scale/Scope**: Extension skeleton + dev environment setup

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Applies to This Feature? | Status |
|-----------|-------------------------|--------|
| I. Native and Open | Partially - no connectivity yet, but no prohibited deps | PASS |
| II. Streaming First | No - no data flow in this milestone | N/A |
| III. Correctness over Convenience | Partially - test tables use PK patterns | PASS |
| IV. Explicit State Machines | No - no protocol in this milestone | N/A |
| V. DuckDB-Native UX | No - no catalog integration yet | N/A |
| VI. Incremental Delivery | Yes - this is the foundation milestone | PASS |

**Row Identity Model Compliance**: Test tables include both single-column PK and
composite PK to validate future rowid mapping patterns.

**Version Baseline Compliance**: docker-compose targets SQL Server 2019+.

**Gate Result**: PASS - No violations. Proceed to Phase 0.

## Project Structure

### Documentation (this feature)

```text
specs/001-project-bootstrap/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output (test table schemas)
├── quickstart.md        # Phase 1 output (developer onboarding)
├── contracts/           # Phase 1 output (N/A for this feature - no APIs)
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
src/
├── mssql_extension.cpp  # Extension entry point and registration
└── include/
    └── mssql_extension.hpp

.vscode/
├── tasks.json           # Build task configuration
├── launch.json          # Debug launch configuration
└── settings.json        # Recommended workspace settings

docker/
├── docker-compose.yml   # SQL Server container definition
└── init/
    └── init.sql         # Database, schema, and table creation

CMakeLists.txt           # Root CMake configuration
vcpkg.json               # vcpkg manifest
Makefile                 # Convenience wrapper for build commands
```

**Structure Decision**: Single project structure following DuckDB extension conventions.
The `src/` directory contains extension source code. Docker and VSCode configurations
are at repository root level for discoverability.

## Complexity Tracking

> No constitution violations requiring justification.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| (none)    | —          | —                                   |

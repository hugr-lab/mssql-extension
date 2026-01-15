# Research: Project Bootstrap and Tooling

**Feature**: 001-project-bootstrap
**Date**: 2026-01-15

## 1. DuckDB Extension Template Structure

**Decision**: Use the official DuckDB extension-template repository structure.

**Rationale**: The extension-template provides pre-configured CI/CD, build infrastructure,
and follows established patterns that DuckDB maintainers expect. This reduces friction
for future contributions and ensures compatibility with DuckDB's evolving build system.

**Alternatives Considered**:
- Custom CMake setup: Rejected due to maintenance burden and risk of incompatibility
- Header-only integration: Not applicable for extensions requiring registration

**Structure**:
```
src/
├── mssql_extension.cpp       # Extension entry point
└── include/
    └── mssql_extension.hpp   # Header declarations

test/
└── sql/                      # SQL-based tests (DuckDB convention)

CMakeLists.txt                # Root CMake configuration
extension_config.cmake        # Extension registration with DuckDB build system
vcpkg.json                    # Manifest mode dependencies
Makefile                      # Convenience wrapper
```

## 2. DuckDB Dependency Management

**Decision**: Add DuckDB as a Git submodule tracking the main branch.

**Rationale**: Submodules lock to a specific commit, enabling reproducible builds while
tracking main. Each checkout pins to a known DuckDB state. The clarification session
confirmed tracking main branch (bleeding edge) rather than stable releases.

**Alternatives Considered**:
- vcpkg package: DuckDB not available in vcpkg; would require custom port
- FetchContent: Less control over version pinning; harder to debug
- System-installed DuckDB: Breaks reproducibility across developer machines

**Implementation**:
```bash
git submodule add --branch main https://github.com/duckdb/duckdb.git duckdb
```

## 3. vcpkg Manifest Mode Configuration

**Decision**: Use vcpkg manifest mode with empty dependencies for skeleton.

**Rationale**: Manifest mode isolates dependencies per-project in `vcpkg_installed/`.
The skeleton has no external dependencies beyond DuckDB (which is a submodule).
Future milestones will add dependencies (e.g., OpenSSL for TLS) via the manifest.

**Alternatives Considered**:
- Classic vcpkg mode: Rejected due to global state conflicts
- Conan: Less native CMake integration than vcpkg

**vcpkg.json** (skeleton):
```json
{
  "name": "mssql-extension",
  "version": "0.0.1",
  "dependencies": []
}
```

## 4. Extension Registration and Version Reporting

**Decision**: Use `DUCKDB_CPP_EXTENSION_ENTRY` macro with DuckDB commit hash as version.

**Rationale**: This is the modern standard pattern for DuckDB extensions. Using the
DuckDB commit hash as the extension version provides traceability when tracking main.

**Alternatives Considered**:
- Semantic version string: Doesn't indicate which DuckDB commit is targeted
- Build timestamp: Less useful for debugging compatibility issues

**Implementation Pattern**:
```cpp
extern "C" {
    DUCKDB_CPP_EXTENSION_ENTRY(mssql, loader) {
        duckdb::LoadInternal(loader);
    }
}

std::string GetExtensionVersion() {
#ifdef EXT_VERSION_MSSQL
    return EXT_VERSION_MSSQL;
#else
    return "";
#endif
}
```

CMake injects version at build time:
```cmake
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/duckdb
    OUTPUT_VARIABLE DUCKDB_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
target_compile_definitions(mssql PRIVATE EXT_VERSION_MSSQL="${DUCKDB_COMMIT_HASH}")
```

## 5. Building and Loading Unsigned Extensions

**Decision**: Use `allow_unsigned_extensions` setting for development; produce both
static and loadable extension targets.

**Rationale**: Unsigned extensions are appropriate for local development. The setting
must be configured at database startup and cannot be changed afterward (security
measure). Both build targets support different use cases: static for linked builds,
loadable for runtime loading.

**Alternatives Considered**:
- Extension signing: Unnecessary complexity for development phase
- Only static target: Would prevent runtime loading during development

**Load Pattern**:
```sql
SET allow_unsigned_extensions = true;
LOAD '/path/to/mssql.duckdb_extension';
```

## 6. VSCode Debug Configuration

**Decision**: CMake-based build tasks with GDB/LLDB debug configurations for both
Linux and Windows.

**Rationale**: CMake is already required for the build system. Using CMake tasks
integrates naturally with the build process. GDB on Linux and GDB/MSVC on Windows
cover the target platforms specified in the requirements.

**Alternatives Considered**:
- CLion: Excellent but not universally available; VSCode is more accessible
- Command-line debugging: Less productive for iterative development

**Key Configuration Points**:
- Program: `${workspaceFolder}/build/debug/duckdb` (shell with extension)
- Args: `:memory:` for in-process DB, `-c "SET allow_unsigned_extensions = true;"`
- preLaunchTask: Ensures build completes before debug session

## 7. SQL Server Docker Configuration

**Decision**: Use Microsoft's official SQL Server 2022 Developer Edition image with
init script for database/schema/table creation.

**Rationale**: Developer Edition is free for development use and matches the
constitution's Version Baseline (SQL Server 2019+). SQL Server 2022 is the latest
and includes all features needed for TDS protocol development.

**Alternatives Considered**:
- SQL Server 2019: Older; 2022 is more recent and equally free for dev
- Azure SQL Edge: ARM-only; doesn't cover x86_64 Linux target
- Express Edition: Feature limitations unnecessary for local dev

**Image**: `mcr.microsoft.com/mssql/server:2022-latest`

## 8. Test Table Design

**Decision**: Two tables per clarification session - one with single-column PK, one
with composite PK.

**Rationale**: The constitution's Row Identity Model requires PK-based rowid mapping.
Testing both patterns (scalar and STRUCT rowid) from the start ensures the dev
environment validates future integration work.

**Tables**:
1. `TestSimplePK` - Integer primary key (scalar rowid)
2. `TestCompositePK` - Two-column primary key (STRUCT rowid)

## Summary of Decisions

| Area                    | Decision                                        |
|-------------------------|-------------------------------------------------|
| Template Structure      | Official extension-template pattern             |
| DuckDB Dependency       | Git submodule tracking main branch              |
| Dependency Management   | vcpkg manifest mode (empty for skeleton)        |
| Extension Entry         | DUCKDB_CPP_EXTENSION_ENTRY macro                |
| Version Tracking        | DuckDB commit hash injected at build time       |
| Build Targets           | Both static and loadable extension              |
| Dev Loading             | allow_unsigned_extensions setting               |
| IDE Support             | VSCode with CMake tasks + GDB debug             |
| SQL Server              | Docker with 2022 Developer Edition              |
| Test Tables             | Single PK + Composite PK per Row Identity Model |

# Quickstart: Project Bootstrap and Tooling

**Feature**: 001-project-bootstrap
**Date**: 2026-01-15

## Prerequisites

Before starting, ensure you have:

- **Git** (with submodule support)
- **CMake** 3.21 or newer
- **C++ compiler**: GCC 10+ (Linux) or MSVC 2019+ (Windows)
- **Ninja** (recommended) or Make
- **Docker** and **Docker Compose** (for SQL Server testing)
- **VSCode** (optional, for IDE integration)

## 1. Clone and Initialize

```bash
git clone https://github.com/hugr-lab/mssql-extension.git
cd mssql-extension
git submodule update --init --recursive
```

## 2. Build the Extension

### Linux

```bash
make release
```

Or with CMake directly:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build --config Release
```

### Windows (PowerShell)

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build --config Release
```

**Build Output**: `build/release/extension/mssql/mssql.duckdb_extension`

## 3. Load the Extension in DuckDB

```bash
./build/release/duckdb
```

In the DuckDB shell:

```sql
SET allow_unsigned_extensions = true;
LOAD 'build/release/extension/mssql/mssql.duckdb_extension';

-- Verify extension loaded
SELECT * FROM duckdb_extensions() WHERE extension_name = 'mssql';
```

Expected output shows `loaded = true` and the extension version (DuckDB commit hash).

## 4. Start SQL Server Development Environment

```bash
docker-compose -f docker/docker-compose.yml up -d
```

Wait for SQL Server to initialize (approximately 30-60 seconds on first run).

### Verify SQL Server is Ready

```bash
docker-compose -f docker/docker-compose.yml logs mssql
```

Look for: `SQL Server is now ready for client connections.`

### Connect to SQL Server

Using sqlcmd (if installed):

```bash
sqlcmd -S localhost,1433 -U sa -P 'YourStrong@Password' -d mssql_test
```

Or use Azure Data Studio, SSMS, or DBeaver with:

- **Host**: localhost
- **Port**: 1433
- **User**: sa
- **Password**: YourStrong@Password
- **Database**: mssql_test

### Verify Test Tables

```sql
SELECT * FROM TestSimplePK;
SELECT * FROM TestCompositePK;
```

Both tables should return sample data rows.

## 5. VSCode Development Setup

### Open Project

```bash
code .
```

### Install Recommended Extensions

VSCode will prompt to install recommended extensions. Accept to install:

- C/C++ (Microsoft)
- CMake Tools (Microsoft)

### Build from VSCode

Press `Ctrl+Shift+B` (or `Cmd+Shift+B` on macOS) to run the default build task.

### Debug from VSCode

1. Set a breakpoint in `src/mssql_extension.cpp`
2. Press `F5` to start debugging
3. The DuckDB shell launches with the extension pre-loaded
4. Type SQL commands to trigger breakpoints

## 6. Clean Up

### Stop SQL Server

```bash
docker-compose -f docker/docker-compose.yml down
```

### Clean Build Artifacts

```bash
make clean
```

Or:

```bash
rm -rf build/
```

## Troubleshooting

### Build fails with "DuckDB not found"

Ensure submodules are initialized:

```bash
git submodule update --init --recursive
```

### Extension fails to load with "unsigned extension" error

The `allow_unsigned_extensions` setting must be set before loading:

```sql
SET allow_unsigned_extensions = true;
LOAD '...';
```

### SQL Server container fails to start

Check if port 1433 is already in use:

```bash
lsof -i :1433  # Linux/macOS
netstat -an | findstr 1433  # Windows
```

Stop conflicting services or change the port in `docker-compose.yml`.

### vcpkg dependencies fail to download

Clear the vcpkg cache and retry:

```bash
rm -rf vcpkg_installed/
make release
```

## Next Steps

Once the bootstrap is complete:

1. Run `/speckit.tasks` to generate implementation tasks
2. Implement the extension skeleton code
3. Verify all success criteria pass
4. Proceed to Spec 02 (TDS protocol foundation)

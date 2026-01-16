# DuckDB MSSQL Extension

A DuckDB extension for connecting to Microsoft SQL Server databases using native TDS protocol.

## Status

This extension provides native TDS protocol connectivity to Microsoft SQL Server with the following features:
- Connection string parsing (ADO.NET and URI formats)
- TLS/SSL encrypted connections
- Connection pooling
- Streaming query execution
- DuckDB secret management integration

## Prerequisites

- CMake 3.21+
- Ninja build system
- C++ compiler with C++11 support
- Git (for submodule management)
- Docker (for SQL Server development environment)

## Quick Start

### Clone and Initialize

```bash
git clone --recurse-submodules <repository-url>
cd mssql-extension
```

If you already cloned without submodules:

```bash
git submodule update --init --recursive
```

### Build

Build the release version:

```bash
make release
```

Build the debug version:

```bash
make debug
```

### Verify Extension

After building, the extension is statically linked into DuckDB. Test it:

```bash
./build/release/duckdb -c "SELECT mssql_version();"
```

Expected output shows the DuckDB commit hash the extension was built against.

### Connect to SQL Server

Use the ATTACH command with a connection string:

```sql
-- Connection string format
ATTACH 'Server=localhost,1433;Database=master;User Id=sa;Password=YourPassword' AS db (TYPE mssql);

-- Query SQL Server tables
SELECT * FROM mssql_scan('db', 'SELECT * FROM your_table') LIMIT 100;
```

Connection string parameters:
- `Server` - hostname and port (format: `host,port`)
- `Database` - database name
- `User Id` - SQL Server login username
- `Password` - SQL Server login password

### TLS/Encrypted Connections

For secure connections with TLS encryption, you can use any of these methods:

**Method 1: Secret with use_encrypt**
```sql
CREATE SECRET mssql_secure (
    TYPE mssql,
    host 'sql-server.example.com',
    port 1433,
    database 'MyDatabase',
    user 'sa',
    password 'MyPassword123!',
    use_encrypt true
);
ATTACH '' AS db (TYPE mssql, SECRET mssql_secure);
```

**Method 2: ADO.NET connection string with Encrypt**
```sql
ATTACH 'Server=sql-server.example.com,1433;Database=MyDatabase;User Id=sa;Password=MyPassword123!;Encrypt=yes' AS db (TYPE mssql);
```

**Method 3: URI format with encrypt query parameter**
```sql
ATTACH 'mssql://sa:MyPassword123!@sql-server.example.com:1433/MyDatabase?encrypt=true' AS db (TYPE mssql);
```

Key points about TLS support:
- `use_encrypt` / `Encrypt` defaults to `false` for backward compatibility
- Uses "trust server certificate" mode (accepts self-signed certificates)
- TLS 1.2 is the minimum supported version (SQL Server 2016+ requirement)
- Works with both static and loadable extensions
- mbedTLS is used as the TLS library (via vcpkg)

Set `MSSQL_DEBUG=1` environment variable to see TLS handshake details in debug output.

### Load as Dynamic Extension

The loadable extension is built at:
- `build/release/extension/mssql/mssql.duckdb_extension`

## Development

### Project Structure

```
mssql-extension/
├── src/                          # Extension source code
│   ├── include/                  # Header files
│   │   └── mssql_extension.hpp   # Main extension header
│   └── mssql_extension.cpp       # Extension implementation
├── duckdb/                       # DuckDB submodule (main branch)
├── docker/                       # Docker configuration
│   ├── docker-compose.yml        # SQL Server container setup
│   └── init/                     # Database initialization scripts
│       └── init.sql              # Test tables and data
├── .vscode/                      # VSCode configuration
│   ├── tasks.json                # Build tasks
│   ├── launch.json               # Debug configurations
│   └── settings.json             # Workspace settings
├── CMakeLists.txt                # Extension CMake configuration
├── extension_config.cmake        # DuckDB extension registration
├── Makefile                      # Build convenience wrapper
└── vcpkg.json                    # Dependency manifest
```

### VSCode Development

1. Open the project folder in VSCode
2. Install recommended extensions when prompted
3. Use `Cmd+Shift+B` to build (defaults to debug)
4. Use `F5` to debug with DuckDB shell

### SQL Server Development Environment

Start the SQL Server container:

```bash
cd docker
docker-compose up -d
```

Wait for initialization to complete, then connect:

```bash
docker exec -it mssql-dev /opt/mssql-tools18/bin/sqlcmd \
  -S localhost -U sa -P 'DevPassword123!' -C \
  -d TestDB -Q "SELECT * FROM TestSimplePK"
```

Stop the environment:

```bash
docker-compose down
```

### Test Tables

The init script creates two test tables:

1. **TestSimplePK** - Single-column primary key (scalar rowid)
   - `id` (INT, PK), `name`, `value`, `created_at`

2. **TestCompositePK** - Composite primary key (STRUCT rowid)
   - `region_id` + `product_id` (composite PK), `quantity`, `unit_price`, `order_date`

## Build Targets

| Target | Description |
|--------|-------------|
| `make release` | Build optimized release version |
| `make debug` | Build with debug symbols |
| `make clean` | Remove all build artifacts |
| `make test` | Run tests |
| `make help` | Show available targets |

## Architecture

### DuckDB Version

This extension tracks DuckDB's `main` branch. The extension version reported by `mssql_version()` is the DuckDB commit hash.

### Row Identity Model

The extension implements PK-based row identity:
- **Single-column PK**: Scalar rowid
- **Composite PK**: STRUCT-based rowid with named fields

## License

[TBD]

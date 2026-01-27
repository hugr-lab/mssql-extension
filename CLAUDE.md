# mssql-extension Development Guidelines

DuckDB extension for SQL Server via a custom TDS protocol implementation (no FreeTDS/ODBC).

## Technology

- **Language**: C++17 (DuckDB extension standard)
- **DuckDB**: main branch, supports both stable (1.4.x) and nightly APIs via `src/include/mssql_compat.hpp`
- **TLS**: OpenSSL via vcpkg (statically linked, symbol visibility controlled)
- **Platforms**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW/Rtools 4.2)

## Project Structure

```text
src/
  catalog/              # DuckDB catalog integration + transaction manager
  connection/           # Connection pooling, provider, settings
  dml/                  # DML operations
    insert/             # INSERT (batched VALUES, OUTPUT INSERTED for RETURNING)
    update/             # UPDATE (rowid-based, VALUES JOIN pattern)
    delete/             # DELETE (rowid-based, VALUES JOIN pattern)
  include/              # Headers (mirrors src/ layout)
  query/                # Query execution and result streaming
  table_scan/           # Table scan with filter/projection pushdown
  tds/                  # TDS protocol implementation
    encoding/           # Type encoding (datetime, decimal, GUID, UTF-16)
    tls/                # TLS via OpenSSL with custom BIO callbacks
test/
  sql/                  # SQLLogicTest files (require SQL Server for most)
    attach/             # ATTACH/DETACH tests
    catalog/            # Catalog, DDL, filter pushdown, statistics
    dml/                # UPDATE and DELETE tests
    insert/             # INSERT tests
    integration/        # Core integration (pool, TLS, large data)
    query/              # Query-level tests
    rowid/              # Rowid pseudo-column tests
    tds_connection/     # TDS protocol tests
    transaction/        # Transaction management tests
  cpp/                  # C++ unit tests (no SQL Server required)
docs/                   # Architecture documentation (see docs/architecture.md)
docker/                 # SQL Server container and Linux CI build
```

## Commands

```bash
# Build
make                    # Release build (with TLS via vcpkg)
make debug              # Debug build
make clean              # Remove build artifacts

# Test
make test               # Unit tests (no SQL Server required)
make docker-up          # Start SQL Server container
make integration-test   # Integration tests (requires SQL Server)
make test-all           # All tests
make test-debug         # Tests with debug build

# Docker
make docker-up          # Start SQL Server test container
make docker-down        # Stop container
make docker-status      # Check container health

# Load extension in DuckDB CLI
./build/release/duckdb
# Or dynamically:
duckdb --unsigned -c "INSTALL mssql FROM local_build_debug; LOAD mssql;"
```

## Code Style

- C++17, follow DuckDB extension conventions
- Use clang-format (version 14): `find src -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i`

## Naming Conventions

### Files
- Source files: `mssql_<component>.cpp` for extension code, `tds_<component>.cpp` for TDS protocol
- Headers mirror source layout in `src/include/`
- Test files: `test_<component>.cpp` (C++), `<feature>_<scenario>.test` (SQL)

### Namespace Prefix Rule

**Critical**: The `MSSQL` or `Tds` prefix depends on namespace placement:

| Namespace | Prefix | Rationale | Examples |
|-----------|--------|-----------|----------|
| `duckdb` (common) | **Required** (`MSSQL`, `Tds`) | Avoid name collisions in shared namespace | `MSSQLCatalog`, `MSSQLTransaction`, `TdsConnection`, `TdsSocket` |
| `duckdb::mssql` | **No prefix** | Already scoped by namespace | `ConnectionProvider`, `PoolStatistics`, `InsertConfig` |
| `duckdb::tds` | **No prefix** | Already scoped by namespace | `Connection`, `Socket`, `PacketType` |
| `duckdb::tds::tls` | **No prefix** | Already scoped by namespace | `TlsContext`, `TlsBio` |
| `duckdb::encoding` | **No prefix** | Already scoped by namespace | `TypeConverter`, `DatetimeEncoding` |

### Classes and Structs
- **PascalCase** always. Prefix per namespace rule above.
- Info/metadata structs: `PrimaryKeyInfo`, `ColumnInfo`, `TableMetadata`, `PoolStatistics`
- Config structs: `InsertConfig`, `DMLConfig`, `PoolConfig`

### Methods
- **PascalCase** (DuckDB convention): `GetConnection()`, `SetPinnedConnection()`, `ExecuteBatch()`
- Getters: `Get<Property>()` — Setters: `Set<Property>()`
- Boolean queries: `Is<State>()`, `Has<Property>()` (e.g., `IsAlive()`, `HasPinnedConnection()`)

### Variables
- **Member variables**: `snake_case` with trailing underscore: `connection_pool_`, `schema_mutex_`, `transaction_descriptor_[8]`
- **Local variables**: `snake_case` without underscore: `row_count`, `schema_name`
- **Constants**: `constexpr` with UPPER_SNAKE_CASE: `TDS_VERSION_7_4`, `TDS_HEADER_SIZE`, `DEFAULT_IDLE_TIMEOUT`

### Enums
- `enum class` with PascalCase values: `ConnectionState::Idle`, `PacketType::SQL_BATCH`, `MSSQLCacheState::LOADED`
- All-caps for protocol constants: `DONE_FINAL`, `ENCRYPT_ON`

### Namespaces
- `duckdb` — main extension entry points and DuckDB API overrides (prefixed)
- `duckdb::tds` — TDS protocol layer (no prefix)
- `duckdb::tds::tls` — TLS implementation (no prefix)
- `duckdb::mssql` — MSSQL-specific utilities (no prefix)
- `duckdb::encoding` — Type encoding/decoding (no prefix)

### Test Naming
- SQL test files: `# group: [sql]`, `[mssql]`, `[integration]`, `[transaction]`, `[dml]`
- SQL test context names: unique per file, prefixed by operation (e.g., `txtest`, `mssql_upd_scalar`)
- Test tables: PascalCase (`TestSimplePK`, `TxTestOrders`) or snake_case for simple ones (`tx_test`)

## Key Architecture Concepts

- **Custom TDS implementation**: No external TDS/ODBC library. TDS v7.4 protocol with PRELOGIN, LOGIN7, SQL_BATCH, ATTENTION packet types.
- **Connection pool**: Thread-safe pool per attached database with idle timeout, background cleanup, configurable limits.
- **Transaction support**: Connection pinning maps DuckDB transactions to SQL Server transactions via 8-byte ENVCHANGE descriptors. See `docs/transactions.md`.
- **Catalog integration**: DuckDB Catalog/Schema/Table APIs with metadata cache (TTL-based), primary key discovery for rowid support.
- **DML**: INSERT uses batched VALUES; UPDATE/DELETE use rowid-based VALUES JOIN pattern with deferred execution in transactions.
- **Filter pushdown**: DuckDB filter expressions translated to T-SQL WHERE clauses. Function mapping for common string/date/arithmetic operations.
- **DuckDB API compat**: Auto-detected at CMake time. `MSSQL_GETDATA_METHOD` macro handles GetData vs GetDataInternal.

## Windows Build Support

- **ssize_t**: `src/include/tds/tds_platform.hpp` provides Windows-compatible typedef
- **MSVC**: `x64-windows-static-release` vcpkg triplet
- **MinGW**: `x64-mingw-static` triplet (Rtools 4.2, not 4.3 due to linker bugs)
- **CI**: Trigger Windows builds via Actions -> CI -> Run workflow -> Check "Run Windows build jobs"

## Debug Environment Variables

| Variable | Description |
|----------|-------------|
| `MSSQL_DEBUG=1..3` | TDS protocol debug level (1=basic, 3=trace) |
| `MSSQL_DML_DEBUG=1` | DML operation debugging (generated SQL, batch sizes, rowid values) |

## Extension Settings (SET in DuckDB)

| Setting | Default | Description |
|---------|---------|-------------|
| `mssql_connection_limit` | 10 | Max connections per context |
| `mssql_connection_timeout` | 30 | TCP timeout (seconds) |
| `mssql_idle_timeout` | 300 | Idle connection timeout (seconds) |
| `mssql_min_connections` | 1 | Min connections to maintain |
| `mssql_acquire_timeout` | 10 | Connection acquire timeout (seconds) |
| `mssql_connection_cache` | true | Enable connection pooling |
| `mssql_catalog_cache_ttl` | 0 | Metadata cache TTL (0 = manual via `mssql_refresh_cache()`) |
| `mssql_insert_batch_size` | 500 | Rows per INSERT statement |
| `mssql_insert_max_rows_per_statement` | 1000 | Hard cap per INSERT |
| `mssql_insert_max_sql_bytes` | 1MB | INSERT SQL size limit |
| `mssql_insert_use_returning_output` | true | Use OUTPUT INSERTED for RETURNING |
| `mssql_dml_batch_size` | 500 | Rows per UPDATE/DELETE batch |
| `mssql_dml_max_parameters` | 2000 | Max parameters per UPDATE/DELETE statement |
| `mssql_dml_use_prepared` | true | Use prepared statements for DML |
| `mssql_enable_statistics` | true | Enable statistics collection |
| `mssql_statistics_cache_ttl_seconds` | 3600 | Statistics cache TTL |

## Extension Functions

| Function | Type | Description |
|----------|------|-------------|
| `mssql_scan(context, query)` | Table | Execute raw T-SQL, stream results |
| `mssql_exec(context, sql)` | Scalar | Execute T-SQL, return affected row count |
| `mssql_pool_stats([context])` | Table | View connection pool statistics |
| `mssql_refresh_cache(context)` | Scalar | Refresh metadata cache |
| `mssql_open(conn_string)` | Scalar | Open standalone diagnostic connection |
| `mssql_close(handle)` | Scalar | Close diagnostic connection |
| `mssql_ping(handle)` | Scalar | Test connection liveness |

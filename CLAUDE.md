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
  azure/                # Azure AD authentication infrastructure
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
    auth/               # Authentication strategies (SQL auth, FEDAUTH)
    encoding/           # Type encoding (datetime, decimal, GUID, UTF-16)
    tls/                # TLS via OpenSSL with custom BIO callbacks
test/
  sql/                  # SQLLogicTest files (require SQL Server for most)
    attach/             # ATTACH/DETACH tests
    azure/              # Azure AD authentication tests (no SQL Server required)
    catalog/            # Catalog, DDL, filter pushdown, statistics
    copy/               # COPY TO MSSQL (BulkLoadBCP) tests
    ctas/               # CREATE TABLE AS SELECT tests
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

## Versioning

- The extension version is defined in `CMakeLists.txt` as `MSSQL_EXTENSION_VERSION` (e.g., `set(MSSQL_EXTENSION_VERSION "0.1.10")`)
- This is passed to C++ code via the `MSSQL_VERSION` compile definition and returned by `mssql_version()`
- **When releasing a new version**: update `MSSQL_EXTENSION_VERSION` in `CMakeLists.txt` and `version` in `vcpkg.json`

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
- **Catalog integration**: DuckDB Catalog/Schema/Table APIs with incremental metadata cache (lazy loading, TTL-based expiration, point invalidation), primary key discovery for rowid support.
- **DML**: INSERT uses batched VALUES; UPDATE/DELETE use rowid-based VALUES JOIN pattern with deferred execution in transactions.
- **DDL**: `CREATE TABLE IF NOT EXISTS` silently succeeds when table exists; `CREATE OR REPLACE TABLE` drops and recreates. Auto-TABLOCK enabled for new table creation (CTAS/COPY TO).
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
| `mssql_connection_limit` | 64 | Max connections per context |
| `mssql_connection_timeout` | 30 | TCP timeout (seconds) |
| `mssql_idle_timeout` | 300 | Idle connection timeout (seconds) |
| `mssql_min_connections` | 0 | Min connections to maintain |
| `mssql_acquire_timeout` | 30 | Connection acquire timeout (seconds) |
| `mssql_connection_cache` | true | Enable connection pooling |
| `mssql_metadata_timeout` | 300 | Metadata query timeout in seconds (0 = no timeout) |
| `mssql_catalog_cache_ttl` | 0 | Metadata cache TTL (0 = manual via `mssql_refresh_cache()`) |
| `mssql_insert_batch_size` | 1000 | Rows per INSERT statement |
| `mssql_insert_max_rows_per_statement` | 1000 | Hard cap per INSERT |
| `mssql_insert_max_sql_bytes` | 8MB (8388608) | INSERT SQL size limit |
| `mssql_insert_use_returning_output` | true | Use OUTPUT INSERTED for RETURNING |
| `mssql_dml_batch_size` | 500 | Rows per UPDATE/DELETE batch |
| `mssql_dml_max_parameters` | 2000 | Max parameters per UPDATE/DELETE statement |
| `mssql_dml_use_prepared` | true | Use prepared statements for DML |
| `mssql_enable_statistics` | true | Enable statistics collection |
| `mssql_statistics_cache_ttl_seconds` | 300 | Statistics cache TTL |
| `mssql_copy_flush_rows` | 100000 | Rows before flushing to SQL Server during COPY |
| `mssql_copy_tablock` | auto | Use TABLOCK hint for COPY/BCP (15-30% faster, blocks concurrent access). Auto-enabled for new tables when not explicitly set. |
| `mssql_ctas_use_bcp` | true | Use BCP protocol for CTAS data transfer (2-10x faster than INSERT) |
| `mssql_convert_varchar_max` | true | Convert VARCHAR(MAX) to NVARCHAR(MAX) in catalog queries for UTF-8 compatibility |

## ATTACH Options & Secret Parameters (Catalog Filters)

| Parameter | Type | Description |
|-----------|------|-------------|
| `schema_filter` | VARCHAR | Regex pattern to filter visible schemas (case-insensitive, partial match via `regex_search`) |
| `table_filter` | VARCHAR | Regex pattern to filter visible tables/views (case-insensitive, partial match via `regex_search`) |

Available in: ATTACH options, ADO.NET connection strings (`SchemaFilter`/`TableFilter`), URI query parameters, and MSSQL secrets. ATTACH options override secret/connection string values.

## Extension Functions

| Function | Type | Description |
|----------|------|-------------|
| `mssql_scan(context, query)` | Table | Execute raw T-SQL, stream results |
| `mssql_exec(context, sql)` | Scalar | Execute T-SQL, return affected row count |
| `mssql_pool_stats([context])` | Table | View connection pool statistics |
| `mssql_refresh_cache(context)` | Scalar | Refresh metadata cache |
| `mssql_preload_catalog(context [, schema])` | Scalar | Bulk-load all metadata in one round trip |
| `mssql_open(conn_string)` | Scalar | Open standalone diagnostic connection |
| `mssql_close(handle)` | Scalar | Close diagnostic connection |
| `mssql_ping(handle)` | Scalar | Test connection liveness |
| `mssql_azure_auth_test(secret, tenant?)` | Scalar | Test Azure AD token acquisition |

## Active Technologies
- C++17 (DuckDB extension standard) + DuckDB (main branch), OpenSSL (vcpkg), Winsock2 (Windows system library) (019-fix-winsock-init)
- C++17 (DuckDB extension standard) + DuckDB (main branch), existing TDS layer (specs 001-019) (020-multi-statement-scan)
- In-memory (result streaming, connection pool state) (020-multi-statement-scan)
- C++17 (DuckDB extension standard) + DuckDB (main branch), OpenSSL (vcpkg), existing TDS protocol layer (022-mssql-ctas)
- SQL Server 2019+ (remote), in-memory (result streaming, connection pool state) (022-mssql-ctas)
- C++17 (DuckDB extension standard) + DuckDB (main branch), OpenSSL (via vcpkg for TLS) (023-pool-stats-validation)
- In-memory (connection pool state, metadata cache) (023-pool-stats-validation)
- C++17 (DuckDB extension standard) + DuckDB (main branch), TDS BulkLoadBCP protocol (0x07), OpenSSL (via vcpkg for TLS) (024-mssql-copy-bcp)
- SQL Server 2019+ (remote target), in-memory (batch buffering, connection pool state) (024-mssql-copy-bcp)
- SQL Server 2019+ (remote target), in-memory (connection pool state) (025-bcp-improvements)
- C++17 (DuckDB extension standard) + DuckDB (main branch), existing TDS protocol layer, OpenSSL (vcpkg) (026-varchar-nvarchar-conversion)
- SQL Server 2019+ (remote target), in-memory (batch buffering) (027-ctas-bcp-integration)
- C++17 (DuckDB extension standard) + DuckDB (main branch), OpenSSL (vcpkg for TLS), libcurl (vcpkg for OAuth2 HTTP), DuckDB Azure extension (runtime, for Azure secret management) (001-azure-token-infrastructure)
- In-memory (token cache, no persistence required) (001-azure-token-infrastructure)
- C++17 (DuckDB extension standard) + DuckDB (main branch), OpenSSL (vcpkg), libcurl (vcpkg for Azure OAuth2) (031-connection-fedauth-refactor)
- In-memory (connection pool state, token cache) (031-connection-fedauth-refactor)
- C++17 (DuckDB extension standard, but C++11 compatible for ODR) + libcurl (OAuth2 HTTP), OpenSSL (TLS), DuckDB Azure extension (secret management) (032-fedauth-token-provider)
- In-memory token cache (TokenCache singleton) (032-fedauth-token-provider)
- C++17 (C++11-compatible for ODR with DuckDB) + DuckDB (main branch), OpenSSL (vcpkg), TDS protocol layer (033-fix-catalog-scan)
- In-memory metadata cache (`MSSQLMetadataCache`) (033-fix-catalog-scan)

## Azure AD Authentication

The extension supports Azure AD authentication for Azure SQL Database and Microsoft Fabric. Authentication is implemented using **libcurl** for OAuth2 token acquisition (no Azure SDK dependency for simpler builds).

**Supported methods:**
- **Service Principal**: Client credentials flow with tenant_id, client_id, client_secret
- **Azure CLI**: Uses `az account get-access-token` for developers with `az login`
- **Device Code Flow**: Interactive authentication for MFA-enabled accounts

**Implementation files:**
- `src/azure/azure_token.cpp` - OAuth2 token acquisition via libcurl
- `src/azure/azure_device_code.cpp` - RFC 8628 device code flow
- `src/azure/azure_secret_reader.cpp` - Reads Azure secrets from DuckDB Azure extension
- `src/azure/azure_test_function.cpp` - `mssql_azure_auth_test()` function

See `AZURE.md` for user documentation.

## Build Troubleshooting

### ODR (One Definition Rule) Errors on Linux

**Symptom:** Linux builds fail with "multiple definition of `duckdb::LogicalType::BIGINT`" and similar errors for constexpr static members.

**Root Cause:** DuckDB defaults to C++11. If the extension uses `target_compile_features(... cxx_std_17)`, it gets compiled with C++17 while DuckDB remains C++11. The different handling of `constexpr static` members (external linkage in C++11 vs inline in C++17) causes ODR violations when linking.

**What NOT to do in CMakeLists.txt:**
```cmake
# DO NOT USE - causes ODR errors on Linux when DuckDB is C++11:
set(CMAKE_CXX_STANDARD 17 CACHE STRING "..." FORCE)
target_compile_features(${EXTENSION_NAME} PRIVATE cxx_std_17)
```

**Correct Solution:** Don't force C++17 for the extension. Use DuckDB's default C++ standard (C++11) and avoid C++17-only features like structured bindings. The extension code should be compatible with C++11.

**Note:** This issue only manifests on GCC/Linux, not on Clang/macOS, because Clang is more lenient with ODR for constexpr static members.

## Recent Changes
- 033-fix-catalog-scan: Added C++17 (C++11-compatible for ODR with DuckDB) + DuckDB (main branch), OpenSSL (vcpkg), TDS protocol layer
- 032-fedauth-token-provider: Added C++17 (DuckDB extension standard, but C++11 compatible for ODR) + libcurl (OAuth2 HTTP), OpenSSL (TLS), DuckDB Azure extension (secret management)
- 031-connection-fedauth-refactor: Added C++17 (DuckDB extension standard) + DuckDB (main branch), OpenSSL (vcpkg), libcurl (vcpkg for Azure OAuth2)


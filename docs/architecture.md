# MSSQL Extension Architecture

## Overview

The MSSQL extension is a DuckDB extension that enables transparent access to Microsoft SQL Server databases. It implements the TDS (Tabular Data Stream) protocol from scratch in C++17, without relying on external driver libraries like FreeTDS or ODBC.

## Entry Point

The extension entry point is in `src/mssql_extension.cpp`. DuckDB loads the extension via the C entry point macro:

```cpp
DUCKDB_CPP_EXTENSION_ENTRY(mssql, loader) {
    duckdb::LoadInternal(loader);
}
```

`LoadInternal()` registers all components in this order:

1. **Secret type** (`RegisterMSSQLSecretType`) — `CREATE SECRET` support for credentials
2. **Storage extension** (`RegisterMSSQLStorageExtension`) — `ATTACH ... TYPE mssql` support
3. **Table functions** (`RegisterMSSQLFunctions`) — `mssql_scan` for raw SQL queries
4. **Scalar functions** (`RegisterMSSQLExecFunction`) — `mssql_exec` for DDL/DML execution
5. **Settings** (`RegisterMSSQLSettings`) — connection pool, statistics, DML, COPY tuning
6. **Diagnostic functions** (`RegisterMSSQLDiagnosticFunctions`) — `mssql_open`, `mssql_close`, `mssql_ping`, `mssql_pool_stats`
7. **Cache refresh** (`RegisterMSSQLRefreshCacheFunction`) — `mssql_refresh_cache`
8. **Catalog preload** (`RegisterMSSQLPreloadCatalogFunction`) — `mssql_preload_catalog`
9. **COPY functions** (`RegisterMSSQLCopyFunctions`) — `bcp` format for COPY TO
10. **Version function** — `mssql_version()`

## High-Level Component Diagram

```
┌──────────────────────────────────────────────────────────────────────┐
│                          DuckDB Engine                               │
│  ┌────────────┐  ┌─────────────┐  ┌──────────────┐  ┌───────────┐  │
│  │ SQL Parser  │  │  Optimizer  │  │  Execution   │  │ Catalog   │  │
│  └──────┬─────┘  └──────┬──────┘  └──────┬───────┘  └─────┬─────┘  │
└─────────┼───────────────┼────────────────┼─────────────────┼────────┘
          │               │                │                 │
          ▼               ▼                ▼                 ▼
┌──────────────────────────────────────────────────────────────────────┐
│                       MSSQL Extension                                │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐    │
│  │  Catalog Integration (MSSQLCatalog, Schema, Table entries)   │    │
│  │  - Incremental cache with lazy loading and TTL               │    │
│  │  - Point invalidation for DDL operations                     │    │
│  │  - Statistics provider                                       │    │
│  │  - Primary key / rowid support                               │    │
│  └─────────────────────────┬────────────────────────────────────┘    │
│                            │                                         │
│  ┌─────────────┐  ┌───────┴──────┐  ┌──────────────────────────┐    │
│  │ Table Scan   │  │ DML Layer    │  │ Transaction Management   │    │
│  │ - Filter     │  │ - INSERT     │  │ - BEGIN/COMMIT/ROLLBACK  │    │
│  │   pushdown   │  │ - UPDATE     │  │ - Connection pinning     │    │
│  │ - Projection │  │ - DELETE     │  │ - Transaction descriptor │    │
│  │   pushdown   │  │ - CTAS       │  │                          │    │
│  │              │  │ - COPY (BCP) │  │                          │    │
│  └──────┬──────┘  └──────┬───────┘  └────────────┬─────────────┘    │
│         │                │                        │                  │
│  ┌──────┴────────────────┴────────────────────────┴─────────────┐    │
│  │  Connection Management                                        │    │
│  │  - ConnectionProvider (transaction-aware acquisition)          │    │
│  │  - ConnectionPool (thread-safe, background cleanup)            │    │
│  │  - MssqlPoolManager (per-database singleton)                   │    │
│  └──────────────────────────┬───────────────────────────────────┘    │
│                             │                                        │
│  ┌──────────────────────────┴───────────────────────────────────┐    │
│  │  TDS Protocol Layer                                           │    │
│  │  - TdsConnection (state machine, authentication)              │    │
│  │  - TdsSocket (TCP + TLS via OpenSSL)                          │    │
│  │  - TdsPacket / TdsProtocol (packet construction/parsing)      │    │
│  │  - TokenParser (incremental token stream processing)          │    │
│  │  - RowReader (row value extraction)                           │    │
│  │  - Encoding subsystem (type conversion, UTF-16, datetime)     │    │
│  └──────────────────────────────────────────────────────────────┘    │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
                    ┌──────────────────┐
                    │  SQL Server      │
                    │  (TDS over TCP)  │
                    └──────────────────┘
```

## Source Directory Structure

```
src/
├── mssql_extension.cpp           # Entry point, LoadInternal()
├── mssql_functions.cpp           # mssql_scan (table function), mssql_exec (scalar)
├── mssql_secret.cpp              # Secret type registration and validation
├── mssql_storage.cpp             # ATTACH mechanism, context manager
│
├── azure/                        # Azure AD authentication infrastructure
│   ├── azure_token.cpp           # Token acquisition and caching
│   ├── azure_device_code.cpp     # Interactive device code flow (RFC 8628)
│   └── azure_secret_reader.cpp   # Azure secret parsing from DuckDB SecretManager
│
├── catalog/                      # DuckDB catalog integration
│   ├── mssql_catalog.cpp         # Catalog implementation (extends duckdb::Catalog)
│   ├── mssql_schema_entry.cpp    # Schema entries (extends SchemaCatalogEntry)
│   ├── mssql_table_entry.cpp     # Table entries (extends TableCatalogEntry)
│   ├── mssql_table_set.cpp       # Lazy-loaded table collection per schema
│   ├── mssql_metadata_cache.cpp  # In-memory metadata cache with TTL
│   ├── mssql_column_info.cpp     # Column metadata with collation info
│   ├── mssql_primary_key.cpp     # PK discovery and rowid type computation
│   ├── mssql_statistics.cpp      # Row count statistics provider
│   ├── mssql_transaction.cpp     # MSSQLTransaction and MSSQLTransactionManager
│   ├── mssql_ddl_translator.cpp  # DDL statement translation
│   ├── mssql_table_function.cpp  # Table scan function bindings
│   ├── mssql_refresh_function.cpp # mssql_refresh_cache() implementation
│   ├── mssql_preload_catalog.cpp # mssql_preload_catalog() bulk preload
│   └── mssql_catalog_filter.cpp  # Regex-based schema/table visibility filter
│
├── connection/                   # Connection pooling and settings
│   ├── mssql_pool_manager.cpp    # Global pool registry (singleton)
│   ├── mssql_connection_provider.cpp # Transaction-aware connection acquisition
│   ├── mssql_settings.cpp        # Extension settings registration
│   └── mssql_diagnostic.cpp      # mssql_open/close/ping/pool_stats
│
├── tds/                          # TDS protocol implementation
│   ├── tds_connection.cpp        # TCP connection, authentication, state machine
│   ├── tds_protocol.cpp          # Packet building (PRELOGIN, LOGIN7, SQL_BATCH)
│   ├── tds_packet.cpp            # Packet serialization/deserialization
│   ├── tds_token_parser.cpp      # Incremental token stream parser
│   ├── tds_socket.cpp            # Cross-platform socket I/O
│   ├── tds_row_reader.cpp        # Row/NBC row value extraction
│   ├── tds_column_metadata.cpp   # Column metadata parsing
│   ├── tds_connection_pool.cpp   # Thread-safe connection pool
│   ├── tds_types.cpp             # TDS type definitions
│   ├── encoding/                 # Type encoding/decoding
│   │   ├── type_converter.cpp    # SQL Server ↔ DuckDB type mapping
│   │   ├── datetime_encoding.cpp # Date/time wire format conversions
│   │   ├── decimal_encoding.cpp  # DECIMAL/MONEY conversions
│   │   ├── guid_encoding.cpp     # UNIQUEIDENTIFIER mixed-endian handling
│   │   ├── utf16.cpp             # UTF-16LE ↔ UTF-8 conversion (optimized ASCII path)
│   │   └── bcp_row_encoder.cpp   # BCP row encoding for COPY operations
│   └── tls/                      # TLS encryption
│       ├── tds_tls_context.cpp   # OpenSSL context wrapper
│       └── tds_tls_impl.cpp      # TLS handshake with custom BIO callbacks
│
├── query/                        # Query execution
│   ├── mssql_query_executor.cpp  # Execute queries with schema detection
│   ├── mssql_result_stream.cpp   # Streaming result handling
│   └── mssql_simple_query.cpp    # Simple query execution for mssql_exec
│
├── table_scan/                   # Table scan and filter pushdown
│   ├── table_scan.cpp            # Main table scan operator
│   ├── table_scan_bind.cpp       # Bind phase (schema determination)
│   ├── table_scan_execute.cpp    # Execution phase
│   ├── table_scan_state.cpp      # Scan state management
│   ├── filter_encoder.cpp        # DuckDB expressions → T-SQL WHERE
│   └── function_mapping.cpp      # DuckDB functions → SQL Server functions
│
├── dml/                          # Data Modification Language
│   ├── mssql_rowid_extractor.cpp # PK extraction from rowid values
│   ├── mssql_dml_config.cpp      # DML configuration
│   ├── insert/                   # INSERT implementation
│   │   ├── mssql_physical_insert.cpp   # DuckDB PhysicalOperator
│   │   ├── mssql_insert_executor.cpp   # Batch execution orchestration
│   │   ├── mssql_batch_builder.cpp     # Row accumulation and batching
│   │   ├── mssql_insert_statement.cpp  # SQL statement generation
│   │   ├── mssql_value_serializer.cpp  # DuckDB Value → T-SQL literal
│   │   └── mssql_returning_parser.cpp  # OUTPUT INSERTED result parsing
│   ├── update/                   # UPDATE implementation (rowid-based)
│   │   ├── mssql_physical_update.cpp
│   │   ├── mssql_update_executor.cpp
│   │   └── mssql_update_statement.cpp
│   ├── delete/                   # DELETE implementation (rowid-based)
│   │   ├── mssql_physical_delete.cpp
│   │   ├── mssql_delete_executor.cpp
│   │   └── mssql_delete_statement.cpp
│   └── ctas/                     # CREATE TABLE AS SELECT
│       ├── mssql_ctas_planner.cpp      # CTAS planning and type mapping
│       ├── mssql_ctas_executor.cpp     # Two-phase execution (DDL + BCP/INSERT)
│       └── mssql_physical_ctas.cpp     # DuckDB PhysicalOperator (Sink)
│
├── copy/                         # COPY TO via BulkLoadBCP
│   ├── copy_function.cpp         # DuckDB COPY function callbacks (bind, sink, finalize)
│   ├── bcp_writer.cpp            # BCP protocol writer (packet construction, streaming)
│   ├── bcp_config.cpp            # Configuration loading from settings
│   └── target_resolver.cpp       # Target URL/catalog parsing and table creation
│
└── include/                      # Headers (mirrors src/ structure)
```

## DuckDB Base Class Overrides

| DuckDB Base Class | Extension Class | Purpose |
|---|---|---|
| `Catalog` | `MSSQLCatalog` | Database-level catalog for attached MSSQL databases |
| `SchemaCatalogEntry` | `MSSQLSchemaEntry` | Schema-level metadata (tables, DDL operations) |
| `TableCatalogEntry` | `MSSQLTableEntry` | Table-level metadata (columns, PK, scan function) |
| `TransactionManager` | `MSSQLTransactionManager` | Transaction lifecycle (start/commit/rollback) |
| `Transaction` | `MSSQLTransaction` | Per-transaction state (pinned connection, descriptor) |
| `PhysicalOperator` | `MSSQLPhysicalInsert` | INSERT execution operator |
| `PhysicalOperator` | `MSSQLPhysicalUpdate` | UPDATE execution operator |
| `PhysicalOperator` | `MSSQLPhysicalDelete` | DELETE execution operator |
| `PhysicalOperator` | `MSSQLPhysicalCreateTableAs` | CTAS execution operator (Sink) |

## Registered Functions

| Function | Type | Signature | Purpose |
|---|---|---|---|
| `mssql_scan` | Table | `(context VARCHAR, query VARCHAR)` | Execute raw T-SQL, stream results |
| `mssql_exec` | Scalar | `(context VARCHAR, sql VARCHAR) → BIGINT` | Execute DDL/DML, return affected rows |
| `mssql_open` | Scalar | `(conn_string VARCHAR) → BIGINT` | Open diagnostic connection |
| `mssql_close` | Scalar | `(handle BIGINT) → BOOLEAN` | Close diagnostic connection |
| `mssql_ping` | Scalar | `(handle BIGINT) → BOOLEAN` | Test connection liveness |
| `mssql_pool_stats` | Table | `(context VARCHAR?)` | Pool statistics |
| `mssql_refresh_cache` | Scalar | `(catalog VARCHAR) → BOOLEAN` | Refresh metadata cache |
| `mssql_preload_catalog` | Scalar | `(catalog VARCHAR, schema? VARCHAR) → VARCHAR` | Bulk-load all metadata per-schema |
| `mssql_azure_auth_test` | Scalar | `(secret VARCHAR, tenant? VARCHAR) → VARCHAR` | Test Azure AD token acquisition |
| `mssql_version` | Scalar | `() → VARCHAR` | Extension version |

## Extension Settings

### Connection Pool
| Setting | Default | Description |
|---|---|---|
| `mssql_connection_limit` | 64 | Max connections per context |
| `mssql_connection_cache` | true | Enable idle connection caching |
| `mssql_connection_timeout` | 30 | TCP connect timeout (seconds) |
| `mssql_idle_timeout` | 300 | Idle connection eviction (seconds) |
| `mssql_min_connections` | 0 | Minimum pool size |
| `mssql_acquire_timeout` | 30 | Pool acquire timeout (seconds) |
| `mssql_query_timeout` | 30 | Query execution timeout (seconds, 0=infinite) |

### Catalog Cache
| Setting | Default | Description |
|---|---|---|
| `mssql_catalog_cache_ttl` | 0 | Metadata TTL in seconds (0 = manual refresh) |
| `mssql_metadata_timeout` | 300 | Metadata query timeout in seconds (0 = no timeout) |

### Statistics
| Setting | Default | Description |
|---|---|---|
| `mssql_enable_statistics` | true | Expose row count to optimizer |
| `mssql_statistics_level` | 0 | 0=rowcount, 1=histogram, 2=NDV |
| `mssql_statistics_use_dbcc` | false | Use DBCC SHOW_STATISTICS (requires permissions) |
| `mssql_statistics_cache_ttl_seconds` | 300 | Statistics cache TTL |

### INSERT Tuning
| Setting | Default | Description |
|---|---|---|
| `mssql_insert_batch_size` | 1000 | Rows per INSERT statement |
| `mssql_insert_max_sql_bytes` | 8MB | Max SQL statement size |
| `mssql_insert_use_returning_output` | true | Use OUTPUT INSERTED for RETURNING |

### UPDATE/DELETE Tuning
| Setting | Default | Description |
|---|---|---|
| `mssql_dml_batch_size` | 500 | Rows per UPDATE/DELETE batch |
| `mssql_dml_max_parameters` | 2000 | Max SQL parameters per statement |
| `mssql_dml_use_prepared` | true | Use prepared statements for DML |

### CTAS (CREATE TABLE AS SELECT)
| Setting | Default | Description |
|---|---|---|
| `mssql_ctas_use_bcp` | true | Use BCP protocol for data transfer (2-10x faster than INSERT) |
| `mssql_ctas_text_type` | NVARCHAR | Text column type (NVARCHAR or VARCHAR) |
| `mssql_ctas_drop_on_failure` | false | Drop table if data transfer phase fails |

### COPY (BulkLoadBCP)
| Setting | Default | Description |
|---|---|---|
| `mssql_copy_flush_rows` | 100000 | Rows before flushing to SQL Server (bounded memory) |
| `mssql_copy_tablock` | false | Use TABLOCK hint for 15-30% faster bulk load (blocks concurrent access) |

## COPY Function Options

The `bcp` format for COPY TO supports these options:

| Option | Type | Default | Description |
|---|---|---|---|
| `CREATE_TABLE` | BOOLEAN | true | Auto-create target table if it doesn't exist |
| `REPLACE` | BOOLEAN | false | Drop and recreate table (replaces existing data) |
| `FLUSH_ROWS` | BIGINT | 100000 | Rows before flushing (overrides setting) |
| `TABLOCK` | BOOLEAN | true | Use TABLOCK hint (overrides setting) |

**Note**: The `REPLACE` option was chosen instead of `OVERWRITE` because DuckDB intercepts `OVERWRITE` as a built-in file operation option.

## Column Mapping (COPY TO Existing Tables)

When copying to an existing table with `CREATE_TABLE false`, the extension uses **name-based column mapping** instead of position-based matching:

### Behavior

- **Case-insensitive matching**: Source column `id` matches target column `ID` or `Id`
- **Column reordering**: Source columns can be in any order; they're mapped by name to target columns
- **Subset of columns**: Source can have fewer columns than target; unmapped target columns receive NULL
- **Extra source columns**: Source columns not found in target are silently ignored
- **At least one match required**: Error if no source columns match any target columns

### Example

```sql
-- Target table: (id INT, name VARCHAR(50), value FLOAT)
-- Source query: SELECT 1.5 AS value, 1 AS id

COPY source TO 'mssql://db/dbo/target' (FORMAT 'bcp', CREATE_TABLE false);
-- Result: id=1, name=NULL, value=1.5 (mapped by name, not position)
```

### Column Mapping Array

Internally, a mapping array translates target positions to source positions:

```
Target: [id, name, value]  (3 columns)
Source: [value, id]        (2 columns)

mapping = [1, -1, 0]  // target[0]->source[1], target[1]->NULL, target[2]->source[0]
```

This mapping is computed in `TargetResolver::BuildColumnMapping()` and passed to `BCPRowEncoder::EncodeRow()` for value extraction during BCP packet construction.

## ANSI Connection Initialization

All connections to SQL Server are initialized with ANSI-compliant session options to ensure DDL commands and queries against indexed views work correctly.

### fODBC Flag (LOGIN7)

The LOGIN7 packet includes the `fODBC` flag (bit 1 of OptionFlags2 = `0x02`) which signals to SQL Server that the client is an ODBC-compatible application. This automatically enables:

- `CONCAT_NULL_YIELDS_NULL ON` - String concatenation with NULL yields NULL
- `ANSI_WARNINGS ON` - Warnings on NULL in aggregate functions and divide-by-zero
- `ANSI_NULLS ON` - ANSI-compliant NULL comparison semantics
- `ANSI_PADDING ON` - Trailing spaces preserved in character columns
- `QUOTED_IDENTIFIER ON` - Double quotes for identifiers

These options are required for:
- DDL commands (`ALTER DATABASE`, `DBCC`, `BACKUP LOG`)
- Queries against indexed views
- Operations on computed column indexes
- XML data type methods
- Filtered index operations

### Connection Pool Reset Handling

When a connection is returned to the pool and later reused with the `RESET_CONNECTION` flag, session state is reset. However, SQL Server remembers the connection's ODBC mode (set via fODBC during LOGIN7) and automatically restores ANSI-compliant session options.

## Authentication Strategy Pattern

The extension uses a Strategy pattern for authentication, supporting both SQL Server auth and Azure AD FEDAUTH:

```
src/tds/auth/
├── auth_strategy.hpp            # Abstract strategy interface
├── auth_strategy_factory.hpp    # Factory for strategy creation
├── sql_auth_strategy.hpp/.cpp   # SQL Server username/password auth
└── fedauth_strategy.hpp/.cpp    # Azure AD FEDAUTH authentication
```

### Strategy Interface

`AuthenticationStrategy` (abstract base) defines:

| Method | Purpose |
|--------|---------|
| `RequiresFedAuth()` | Returns true for Azure AD auth |
| `GetPreloginOptions()` | PRELOGIN configuration (encryption, FEDAUTHREQUIRED) |
| `GetLogin7Options()` | LOGIN7 configuration (credentials, database) |
| `GetFedAuthToken()` | Token acquisition (FEDAUTH only) |
| `InvalidateToken()` | Force token refresh (FEDAUTH only) |
| `IsTokenExpired()` | Check token validity (FEDAUTH only) |
| `GetName()` | Returns strategy name for debugging |

### Strategy Implementations

| Strategy | File | Purpose |
|----------|------|---------|
| `SqlAuthStrategy` | `sql_auth_strategy.cpp` | SQL Server username/password auth |
| `FedAuthStrategy` | `fedauth_strategy.cpp` | Azure AD via Azure secret (service principal, CLI, interactive) |
| `ManualTokenAuthStrategy` | `manual_token_strategy.cpp` | Pre-provided Azure AD access token (Spec 032) |

### Configuration Structs

**PreloginOptions**:
- `use_encrypt` — Request TLS encryption
- `request_fedauth` — Include FEDAUTHREQUIRED option
- `sni_hostname` — SNI hostname for Azure routing

**Login7Options**:
- `database`, `username`, `password` — Connection parameters
- `fedauth_token_utf16le` — Pre-acquired token (for validation)
- `include_fedauth_ext` — Include FEDAUTH extension in LOGIN7

### Factory Pattern

`AuthStrategyFactory::Create()` selects strategy based on `MSSQLConnectionInfo`:

```cpp
if (info.use_azure_auth) {
    return CreateFedAuth(context, info.azure_secret_name, info.database, info.host, info.azure_tenant_id);
} else {
    return CreateSqlAuth(info.username, info.password, info.database, info.use_encrypt);
}
```

## Azure AD Authentication Infrastructure

The extension includes Azure AD token acquisition infrastructure in `src/azure/`:

```
src/azure/
├── azure_token.cpp           # Token acquisition and caching
├── azure_device_code.cpp     # Interactive device code flow (RFC 8628)
├── azure_secret_reader.cpp   # Azure secret parsing from DuckDB SecretManager
├── azure_fedauth.cpp         # FEDAUTH data structures and endpoint detection
├── jwt_parser.cpp            # JWT token validation (Spec 032)
└── include/azure/
    ├── azure_token.hpp       # TokenCache, TokenResult, acquisition functions
    ├── azure_device_code.hpp # Device code flow constants and functions
    ├── azure_secret_reader.hpp # AzureSecretInfo struct
    ├── azure_fedauth.hpp     # FedAuthData, FedAuthLibrary, endpoint detection
    └── jwt_parser.hpp        # JWT claims parsing and validation
```

### JWT Token Validation (Spec 032)

The `jwt_parser.cpp` module provides JWT validation for manually-provided access tokens:

| Function | Purpose |
|----------|---------|
| `ParseJwtClaims()` | Extract claims from JWT payload (base64url decoding) |
| `IsTokenExpired()` | Check if token is expired with configurable margin |
| `FormatTimestamp()` | Format Unix timestamp for error messages |

**JwtClaims struct:**
- `exp` — Expiration timestamp (Unix seconds)
- `aud` — Audience (resource URL, validated against `https://database.windows.net/`)
- `oid` — Object ID (user/service principal)
- `tid` — Tenant ID
- `valid` — Parse success flag
- `error` — Error message if parsing failed

### FEDAUTH Data Structures

**FedAuthLibrary** enum:
- `SSPI` (0x01) — Windows integrated auth (not implemented)
- `MSAL` (0x02) — ADAL/MSAL workflow
- `SECURITY_TOKEN` (0x03) — Direct token embedding (deprecated)

**FedAuthData** struct:
- `library` — FedAuthLibrary enum value
- `token_utf16le` — UTF-16LE encoded access token

### Authentication Methods

| Method | Provider | Description |
|--------|----------|-------------|
| Service Principal | `service_principal` | Client credentials flow with client_id/client_secret |
| Environment | `credential_chain` (chain=`env`) | Uses AZURE_TENANT_ID, AZURE_CLIENT_ID, AZURE_CLIENT_SECRET env vars (Spec 032) |
| Azure CLI | `credential_chain` (chain=`cli`) | Uses `az account get-access-token` |
| Interactive | `credential_chain` (chain=`interactive`) | Device code flow with browser authentication |
| Manual Token | `ACCESS_TOKEN` option | Pre-provided JWT token in ATTACH or MSSQL secret (Spec 032) |

**Chain priority order** (matches Azure SDK DefaultAzureCredential): `env` > `cli` > `interactive`

### Token Sizes and Fragmentation

Token sizes vary by authentication method:
- **Service Principal**: ~1632 chars → ~3264 bytes UTF-16LE (fits in single 4096-byte TDS packet)
- **Azure CLI**: ~2091 chars → ~4182 bytes UTF-16LE (requires packet fragmentation)

When tokens exceed TDS packet size, `BuildFedAuthTokenMultiPacket()` splits them across multiple packets. See [TDS Protocol - FEDAUTH Token Packet Fragmentation](tds-protocol.md#fedauth-token-packet-fragmentation).

### Token Caching

Tokens are cached globally in `TokenCache` (singleton) with:
- Thread-safe access via mutex
- 5-minute refresh margin before expiration (`TOKEN_REFRESH_MARGIN_SECONDS`)
- Cache key includes secret name and optional tenant override

### Endpoint Detection

Functions in `azure_fedauth.cpp` detect Azure endpoint types:
- `IsAzureEndpoint()` — `*.database.windows.net`
- `IsFabricEndpoint()` — `*.datawarehouse.fabric.microsoft.com`
- `IsSynapseEndpoint()` — `*.sql.azuresynapse.net`
- `RequiresHostnameVerification()` — Returns true for all Azure endpoints

### Test Function

`mssql_azure_auth_test(secret_name, tenant_id?)` validates Azure AD token acquisition without connecting to SQL Server. Useful for testing Azure credentials in isolation.

See [AZURE.md](../AZURE.md) for complete usage documentation.

## Cross-References

- [TDS Protocol Layer](tds-protocol.md)
- [Connection Management](connection-management.md)
- [Catalog Integration](catalog-integration.md)
- [Type Mapping](type-mapping.md)
- [Query Execution & DML](query-execution.md)
- [Transaction Management](transactions.md)
- [Azure AD Authentication](../AZURE.md)

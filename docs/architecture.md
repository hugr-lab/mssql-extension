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
5. **Settings** (`RegisterMSSQLSettings`) — connection pool, statistics, DML tuning
6. **Diagnostic functions** (`RegisterMSSQLDiagnosticFunctions`) — `mssql_open`, `mssql_close`, `mssql_ping`, `mssql_pool_stats`
7. **Cache refresh** (`RegisterMSSQLRefreshCacheFunction`) — `mssql_refresh_cache`
8. **Version function** — `mssql_version()`

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
│  │  - Metadata cache with TTL                                   │    │
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
│   └── mssql_refresh_function.cpp # mssql_refresh_cache() implementation
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
│   │   └── utf16.cpp             # UTF-16LE ↔ UTF-8 conversion
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
│       ├── mssql_ctas_executor.cpp     # Two-phase execution (DDL + INSERT)
│       └── mssql_physical_ctas.cpp     # DuckDB PhysicalOperator (Sink)
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

### Statistics
| Setting | Default | Description |
|---|---|---|
| `mssql_enable_statistics` | true | Expose row count to optimizer |
| `mssql_statistics_level` | 0 | 0=rowcount, 1=histogram, 2=NDV |
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

### CTAS (CREATE TABLE AS SELECT)
| Setting | Default | Description |
|---|---|---|
| `mssql_ctas_text_type` | NVARCHAR | Text column type (NVARCHAR or VARCHAR) |
| `mssql_ctas_drop_on_failure` | false | Drop table if INSERT phase fails |

## Cross-References

- [TDS Protocol Layer](tds-protocol.md)
- [Connection Management](connection-management.md)
- [Catalog Integration](catalog-integration.md)
- [Type Mapping](type-mapping.md)
- [Query Execution & DML](query-execution.md)
- [Transaction Management](transactions.md)

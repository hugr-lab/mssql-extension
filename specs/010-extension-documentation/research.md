# Research: Extension Documentation

**Date**: 2026-01-19
**Feature**: 010-extension-documentation

## 1. Function Signatures (Verified)

### Table Functions

| Function | Signature | Returns | Description |
| -------- | --------- | ------- | ----------- |
| `mssql_execute` | `(context VARCHAR, sql VARCHAR)` | `TABLE(success BOOLEAN, affected_rows BIGINT, message VARCHAR)` | Execute raw SQL statement |
| `mssql_scan` | `(context VARCHAR, query VARCHAR)` | `TABLE(...)` dynamic schema | Stream SELECT query results |
| `mssql_pool_stats` | `(context? VARCHAR)` | `TABLE(context_name, total_connections, idle_connections, active_connections, connections_created, connections_closed, acquire_count, acquire_timeout_count, acquire_wait_total_ms)` | Connection pool statistics |

### Scalar Functions

| Function | Signature | Returns | Description |
| -------- | --------- | ------- | ----------- |
| `mssql_version` | `()` | `VARCHAR` | Extension version (DuckDB commit hash) |
| `mssql_open` | `(secret VARCHAR)` | `BIGINT` | Open diagnostic connection, returns handle |
| `mssql_close` | `(handle BIGINT)` | `BOOLEAN` | Close diagnostic connection |
| `mssql_ping` | `(handle BIGINT)` | `BOOLEAN` | Test connection liveness |
| `mssql_exec` | `(secret VARCHAR, sql VARCHAR)` | `BIGINT` | Execute SQL, return affected rows |

**Decision**: Document all 8 functions with complete signatures
**Rationale**: Users need accurate function reference for daily use
**Alternatives**: None - these are the public APIs

## 2. Configuration Settings (Verified)

### Connection Pool Settings

| Setting | Type | Default | Range | Description |
| ------- | ---- | ------- | ----- | ----------- |
| `mssql_connection_limit` | BIGINT | 10 | ≥1 | Max connections per attached database |
| `mssql_connection_cache` | BOOLEAN | true | - | Enable connection pooling |
| `mssql_connection_timeout` | BIGINT | 30 | ≥0 | TCP connection timeout (seconds) |
| `mssql_idle_timeout` | BIGINT | 300 | ≥0 | Idle connection timeout (seconds, 0=none) |
| `mssql_min_connections` | BIGINT | 2 | ≥0 | Minimum pooled connections |
| `mssql_acquire_timeout` | BIGINT | 30 | ≥0 | Connection acquire timeout (seconds) |
| `mssql_catalog_cache_ttl` | BIGINT | 0 | ≥0 | Metadata cache TTL (seconds, 0=manual) |

### Statistics Settings

| Setting | Type | Default | Range | Description |
| ------- | ---- | ------- | ----- | ----------- |
| `mssql_enable_statistics` | BOOLEAN | true | - | Enable statistics collection |
| `mssql_statistics_level` | BIGINT | 0 | ≥0 | Detail: 0=rowcount, 1=+histogram, 2=+NDV |
| `mssql_statistics_use_dbcc` | BOOLEAN | false | - | Use DBCC SHOW_STATISTICS |
| `mssql_statistics_cache_ttl_seconds` | BIGINT | 300 | ≥0 | Statistics cache TTL (seconds) |

### INSERT Settings

| Setting | Type | Default | Range | Description |
| ------- | ---- | ------- | ----- | ----------- |
| `mssql_insert_batch_size` | BIGINT | 1000 | ≥1 | Rows per INSERT (SQL Server limit: 1000) |
| `mssql_insert_max_rows_per_statement` | BIGINT | 1000 | ≥1 | Hard cap on rows per INSERT |
| `mssql_insert_max_sql_bytes` | BIGINT | 8388608 | ≥1024 | Max SQL statement size (8MB) |
| `mssql_insert_use_returning_output` | BOOLEAN | true | - | Use OUTPUT INSERTED for RETURNING |

**Decision**: Document all 15 settings with defaults and valid ranges
**Rationale**: Production users need tuning reference
**Alternatives**: None - these are all registered settings

## 3. Type Mappings (Verified)

| SQL Server Type | DuckDB Type | Notes |
| --------------- | ----------- | ----- |
| TINYINT | UTINYINT | Unsigned 0-255 |
| SMALLINT | SMALLINT | - |
| INT | INTEGER | - |
| BIGINT | BIGINT | - |
| BIT | BOOLEAN | - |
| REAL | FLOAT | 32-bit |
| FLOAT | DOUBLE | 64-bit |
| DECIMAL/NUMERIC | DECIMAL(p,s) | Preserves precision/scale |
| MONEY | DECIMAL(19,4) | - |
| SMALLMONEY | DECIMAL(10,4) | - |
| CHAR/VARCHAR | VARCHAR | - |
| NCHAR/NVARCHAR | VARCHAR | UTF-16LE decoded |
| BINARY/VARBINARY | BLOB | - |
| DATE | DATE | - |
| TIME | TIME | Up to 100ns precision |
| DATETIME | TIMESTAMP | 3.33ms precision |
| SMALLDATETIME | TIMESTAMP | 1min precision |
| DATETIME2 | TIMESTAMP | Up to 100ns precision |
| DATETIMEOFFSET | TIMESTAMP_TZ | Timezone-aware |
| UNIQUEIDENTIFIER | UUID | - |

**Unsupported**: XML, UDT, SQL_VARIANT, IMAGE, TEXT, NTEXT

**Decision**: Document all 20 supported types plus unsupported list
**Rationale**: Type mapping is critical for data integrity
**Alternatives**: None - this is the implemented mapping

## 4. Connection String Formats (Verified)

### ADO.NET Format

```text
Server=host,port;Database=db;User Id=user;Password=pass;Encrypt=yes
```

**Key Aliases**:

- `Server` / `Data Source`
- `Database` / `Initial Catalog`
- `User Id` / `Uid` / `User`
- `Password` / `Pwd`
- `Encrypt` / `Use Encryption for Data`

### URI Format

```text
mssql://user:password@host:port/database?encrypt=true
```

**Features**:

- URL-encoded components supported
- Query parameters: `encrypt`, `ssl`, `use_ssl`
- Default port: 1433

**Decision**: Document both formats with all aliases and examples
**Rationale**: Users need flexibility for their infrastructure
**Alternatives**: None - both formats are implemented

## 5. Secret Fields (Verified)

| Field | Type | Required | Redacted | Description |
| ----- | ---- | -------- | -------- | ----------- |
| `host` | VARCHAR | Yes | No | SQL Server hostname |
| `port` | INTEGER | Yes | No | TCP port (1-65535) |
| `database` | VARCHAR | Yes | No | Database name |
| `user` | VARCHAR | Yes | No | Username |
| `password` | VARCHAR | Yes | **Yes** | Password (hidden) |
| `use_encrypt` | BOOLEAN | No | No | Enable TLS (default: false) |

**Decision**: Document all 6 fields with types and requirements
**Rationale**: Secret creation is essential for secure connections
**Alternatives**: None - these are the implemented fields

## 6. TLS Build Differences (Verified)

| Build Type | TLS Support | Use Case |
| ---------- | ----------- | -------- |
| Static (`make`) | No (stub) | CLI development, no external deps |
| Loadable (`make loadable`) | Yes (mbedTLS 3.6.4) | Production with encrypted connections |

**Decision**: Document TLS availability per build type
**Rationale**: Users need to know which build supports encryption
**Alternatives**: None - this is the split TLS architecture

## 7. DuckDB Extension README Best Practices

Based on analysis of existing DuckDB extensions (postgres_scanner, sqlite_scanner):

**Structure**:

1. Title + Brief description
2. Installation (INSTALL/LOAD)
3. Quick Start with minimal example
4. Connection/Configuration
5. Usage examples (increasing complexity)
6. Function reference
7. Type mapping
8. Configuration settings
9. Building from source
10. Limitations/Known issues

**Decision**: Follow DuckDB extension documentation conventions
**Rationale**: Consistency with ecosystem improves discoverability
**Alternatives**: Custom structure (rejected for consistency)

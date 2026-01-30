# Data Model: COPY TO MSSQL via TDS BulkLoadBCP

**Feature**: 024-mssql-copy-bcp
**Date**: 2026-01-29

## Entities

### 1. BCPCopyTarget

Represents the resolved destination for a COPY operation.

**Attributes:**
| Field | Type | Description |
|-------|------|-------------|
| `catalog_name` | string | Name of attached MSSQL catalog |
| `schema_name` | string | SQL Server schema (e.g., "dbo") |
| `table_name` | string | Target table name |
| `is_temp_table` | bool | True if starts with `#` |
| `is_global_temp` | bool | True if starts with `##` |
| `fully_qualified` | string | `[schema].[table]` for DDL |

**Validation Rules:**
- `catalog_name` must reference an attached MSSQL database
- `table_name` must not be empty
- `schema_name` defaults to "dbo" if not specified
- If `is_temp_table`, schema is implicitly "tempdb"

**State Transitions:** N/A (immutable after resolution)

---

### 2. BCPCopyConfig

Configuration for COPY operation behavior.

**Attributes:**
| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `create_table` | bool | true | Create table if missing |
| `overwrite` | bool | false | Drop and recreate if exists |
| `batch_rows` | idx_t | 10000 | Rows per BCP batch |
| `max_batch_bytes` | idx_t | 32MB | Byte limit per batch |

**Validation Rules:**
- `batch_rows` must be > 0
- `max_batch_bytes` must be >= 1MB
- `create_table` and `overwrite` cannot both be false when table doesn't exist

**State Transitions:** N/A (immutable after bind)

---

### 3. BCPColumnMetadata

Column definition for BulkLoadBCP COLMETADATA token.

**Attributes:**
| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Column name (UTF-16LE in wire format) |
| `duckdb_type` | LogicalType | Source DuckDB type |
| `tds_type_token` | uint8_t | TDS type identifier (e.g., 0x26 for INTNTYPE) |
| `max_length` | uint16_t | Maximum data length |
| `precision` | uint8_t | For DECIMAL/NUMERIC |
| `scale` | uint8_t | For DECIMAL/NUMERIC/TIME |
| `nullable` | bool | Whether column accepts NULL |
| `collation` | array<uint8_t, 5> | For character types |

**Validation Rules:**
- `tds_type_token` must be valid for the DuckDB type
- `precision` in range 1-38 for DECIMAL
- `scale` <= `precision` for DECIMAL
- `scale` in range 0-7 for TIME/DATETIME2

**Relationships:**
- Many BCPColumnMetadata belong to one BCPCopyGlobalState

---

### 4. BCPCopyGlobalState

Shared state across all Sink operations for a COPY.

**Attributes:**
| Field | Type | Description |
|-------|------|-------------|
| `connection` | shared_ptr<TdsConnection> | Pinned SQL Server connection |
| `target` | BCPCopyTarget | Resolved destination |
| `columns` | vector<BCPColumnMetadata> | Column definitions |
| `rows_sent` | atomic<idx_t> | Rows successfully sent |
| `bytes_sent` | atomic<idx_t> | Bytes sent (for progress) |
| `colmetadata_sent` | bool | Whether COLMETADATA already sent |
| `write_mutex` | mutex | Guards connection writes |

**Validation Rules:**
- `connection` must be non-null and in Idle state
- `columns` must have at least one entry
- `rows_sent` is monotonically increasing

**State Transitions:**
```
Created → ColmetadataSent → Streaming → Finalized
                              ↓
                           Error
```

---

### 5. BCPCopyLocalState

Per-thread buffering state for parallel-safe accumulation.

**Attributes:**
| Field | Type | Description |
|-------|------|-------------|
| `buffer` | ColumnDataCollection | Accumulated rows |
| `append_state` | AppendState | DuckDB append state |
| `local_row_count` | idx_t | Rows in local buffer |
| `local_byte_count` | idx_t | Estimated bytes in buffer |

**Validation Rules:**
- `buffer` types must match global column types
- `local_row_count` <= config batch_rows triggers flush

**State Transitions:**
```
Empty → Accumulating → Flushing → Empty
```

---

### 6. BCPPacketBuilder

Constructs TDS BULK_LOAD packets (internal, not persisted).

**Attributes:**
| Field | Type | Description |
|-------|------|-------------|
| `packet_type` | uint8_t | Always 0x07 (BULK_LOAD) |
| `payload` | vector<uint8_t> | Accumulated token data |
| `max_packet_size` | size_t | TDS packet size limit |

**Operations:**
- `WriteColmetadata(columns)` → Adds 0x81 token
- `WriteRow(chunk, row_idx, columns)` → Adds 0xD1 token
- `WriteDone(row_count)` → Adds 0xFD token
- `Serialize()` → Returns complete packet(s)

---

## Type Mapping Table

| DuckDB Type | TDS Token | Wire Size | Notes |
|-------------|-----------|-----------|-------|
| BOOLEAN | 0x68 (BITNTYPE) | 1 | 0x00 or 0x01 |
| TINYINT | 0x26 (INTNTYPE) | 1 | Unsigned |
| SMALLINT | 0x26 (INTNTYPE) | 2 | Little-endian |
| INTEGER | 0x26 (INTNTYPE) | 4 | Little-endian |
| BIGINT | 0x26 (INTNTYPE) | 8 | Little-endian |
| FLOAT | 0x6D (FLTNTYPE) | 4 | IEEE 754 |
| DOUBLE | 0x6D (FLTNTYPE) | 8 | IEEE 754 |
| DECIMAL(p,s) | 0x6A (DECIMALNTYPE) | 5-17 | Sign + mantissa |
| VARCHAR | 0xE7 (NVARCHARTYPE) | 2+n | Length + UTF-16LE |
| UUID | 0x24 (GUIDTYPE) | 16 | Mixed-endian |
| BLOB | 0xA5 (BIGVARBINARYTYPE) | 2+n | Length + bytes |
| DATE | 0x28 (DATENTYPE) | 3 | Days since 0001-01-01 |
| TIME | 0x29 (TIMENTYPE) | 3-5 | Scale-dependent |
| TIMESTAMP | 0x2A (DATETIME2NTYPE) | 6-8 | Time + date |
| TIMESTAMP_TZ | 0x2B (DATETIMEOFFSETNTYPE) | 8-10 | DT2 + offset |

---

## Error States

| Error | Condition | Recovery |
|-------|-----------|----------|
| `ConnectionBusy` | Connection is Executing | Abort, user must close scan |
| `TargetNotFound` | Table doesn't exist, CREATE_TABLE=false | Abort with clear message |
| `ViewTarget` | Target is a VIEW | Abort, views not supported |
| `SchemaMismatch` | Column count/types incompatible | Abort with details |
| `TransactionConflict` | Source includes mssql_scan in txn | Abort with error message |
| `NetworkError` | Connection lost mid-stream | Abort, rollback if in txn |
| `ServerError` | SQL Server returns error token | Abort with server message |

---

## Relationships Diagram

```
BCPCopyConfig (options)
       │
       ▼
BCPCopyTarget ◄────────┐
       │               │
       ▼               │
BCPCopyGlobalState ────┤
       │               │
       │   ┌───────────┘
       │   │
       ▼   ▼
BCPColumnMetadata[] ◄── derived from DuckDB source types
       │
       ▼
BCPPacketBuilder
       │
       ▼
TdsConnection (existing)
       │
       ▼
SQL Server
```

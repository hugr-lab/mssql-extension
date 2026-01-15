# Data Model: Streaming SELECT and Query Cancellation

**Feature**: 004-streaming-select-cancel
**Date**: 2026-01-15

## Entity Overview

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                            Query Execution Flow                                  │
├─────────────────────────────────────────────────────────────────────────────────┤
│                                                                                  │
│  MSSQLQueryExecutor                                                             │
│       │                                                                          │
│       ├── acquires → ConnectionPool → TdsConnection                             │
│       │                                                                          │
│       ├── builds → SqlBatch (UTF-16LE encoded SQL)                              │
│       │                                                                          │
│       └── creates → QueryResult                                                 │
│                        │                                                         │
│                        ├── uses → TokenParser                                   │
│                        │              │                                          │
│                        │              ├── yields → ColumnMetadata[]             │
│                        │              ├── yields → RowData                      │
│                        │              ├── yields → TdsError                     │
│                        │              └── yields → TdsInfo                      │
│                        │                                                         │
│                        ├── uses → TypeConverter                                 │
│                        │                                                         │
│                        └── produces → DataChunk (to DuckDB)                     │
│                                                                                  │
└─────────────────────────────────────────────────────────────────────────────────┘
```

## Entities

### SqlBatch

Represents a SQL_BATCH TDS message for executing ad-hoc SQL queries.

**Attributes:**

| Attribute | Type | Description |
|-----------|------|-------------|
| sql_text | std::string | Original SQL query (UTF-8) |
| encoded_text | std::vector<uint8_t> | UTF-16LE encoded query |
| packets | std::vector<TdsPacket> | Serialized TDS packets |

**Behavior:**
- Encodes SQL text to UTF-16LE
- Splits into multiple packets if query exceeds packet size
- Sets appropriate continuation/EOM status flags

**Relationships:**
- Created by `MSSQLQueryExecutor`
- Sent via `TdsConnection`

---

### ColumnMetadata

Describes a single result column parsed from COLMETADATA token.

**Attributes:**

| Attribute | Type | Description |
|-----------|------|-------------|
| name | std::string | Column name |
| type_id | uint8_t | TDS type identifier |
| type_name | std::string | Human-readable type name |
| max_length | uint16_t | Maximum length for variable types |
| precision | uint8_t | Precision for DECIMAL/NUMERIC |
| scale | uint8_t | Scale for DECIMAL/NUMERIC |
| collation | uint32_t | Collation ID for string types |
| is_nullable | bool | Whether column allows NULL |
| is_identity | bool | Whether column is IDENTITY |

**Derived:**
- `duckdb_type`: LogicalType | Corresponding DuckDB type

**Relationships:**
- Parsed by `TokenParser`
- Used by `RowReader` for value extraction
- Used by `TypeConverter` for DuckDB mapping

---

### RowData

Represents raw row data extracted from a ROW token.

**Attributes:**

| Attribute | Type | Description |
|-----------|------|-------------|
| values | std::vector<ColumnValue> | Column values in order |
| null_mask | std::vector<bool> | NULL indicators per column |

**ColumnValue** (variant type):

| Variant | Storage | Used For |
|---------|---------|----------|
| int8_val | int8_t | TINYINT |
| int16_val | int16_t | SMALLINT |
| int32_val | int32_t | INT |
| int64_val | int64_t | BIGINT |
| float_val | float | REAL |
| double_val | double | FLOAT |
| decimal_val | hugeint_t + scale | DECIMAL/NUMERIC |
| string_val | std::string | CHAR/VARCHAR/NCHAR/NVARCHAR |
| binary_val | std::vector<uint8_t> | BINARY/VARBINARY |
| date_val | date_t | DATE |
| time_val | dtime_t | TIME |
| timestamp_val | timestamp_t | DATETIME/DATETIME2 |
| uuid_val | hugeint_t | UNIQUEIDENTIFIER |

**Relationships:**
- Extracted by `RowReader`
- Converted by `TypeConverter`

---

### TdsError

Error information from ERROR token.

**Attributes:**

| Attribute | Type | Description |
|-----------|------|-------------|
| number | uint32_t | SQL Server error number |
| state | uint8_t | Error state |
| severity | uint8_t | Error severity (0-25) |
| message | std::string | Error message text |
| server_name | std::string | Server name |
| proc_name | std::string | Procedure name (if applicable) |
| line_number | uint32_t | Line number in batch |

**Derived:**
- `is_fatal`: bool | severity >= 20
- `is_user_error`: bool | severity >= 11 && severity <= 16

**Relationships:**
- Parsed by `TokenParser`
- Accumulated by `QueryResult`
- Thrown as DuckDB exception

---

### TdsInfo

Informational message from INFO token.

**Attributes:**

| Attribute | Type | Description |
|-----------|------|-------------|
| number | uint32_t | Message number |
| state | uint8_t | Message state |
| severity | uint8_t | Message class |
| message | std::string | Message text |
| server_name | std::string | Server name |
| proc_name | std::string | Procedure name (if applicable) |
| line_number | uint32_t | Line number in batch |

**Relationships:**
- Parsed by `TokenParser`
- Surfaced via DuckDB warning mechanism

---

### TokenParser

Incremental parser for TDS token stream.

**Attributes:**

| Attribute | Type | Description |
|-----------|------|-------------|
| buffer | std::vector<uint8_t> | Pending data buffer |
| buffer_pos | size_t | Current parse position |
| column_metadata | std::vector<ColumnMetadata> | Columns from COLMETADATA |
| state | ParserState | Current parser state |

**ParserState:**

| State | Description |
|-------|-------------|
| WaitingForToken | Expecting token type byte |
| ParsingColMetadata | Reading COLMETADATA |
| ParsingRow | Reading ROW data |
| ParsingDone | Reading DONE token |
| ParsingError | Reading ERROR token |
| ParsingInfo | Reading INFO token |
| Complete | Final DONE received |

**Operations:**
- `Feed(data: uint8_t[], length: size_t)`: Add data to buffer
- `TryParseNext()`: Attempt to parse next token
- `GetColumnMetadata()`: Get parsed column definitions
- `GetRow()`: Get parsed row data
- `GetError()`: Get parsed error
- `GetInfo()`: Get parsed info message
- `IsComplete()`: Check if final DONE received

**Relationships:**
- Receives data from `TdsConnection`
- Yields to `QueryResult`

---

### RowReader

Extracts typed values from ROW token data.

**Attributes:**

| Attribute | Type | Description |
|-----------|------|-------------|
| columns | const std::vector<ColumnMetadata>& | Column definitions |
| data | const uint8_t* | Row data pointer |
| data_length | size_t | Available data length |
| current_offset | size_t | Current read position |

**Operations:**
- `ReadRow()`: Extract all column values
- `ReadValue(col_idx)`: Extract single column value
- `IsNull(col_idx)`: Check if column is NULL

**Relationships:**
- Uses `ColumnMetadata` for type info
- Produces `RowData`

---

### TypeConverter

Converts SQL Server types to DuckDB types. Located in `src/encoding/` alongside UTF-16 converter.

**Operations:**

| Operation | Description |
|-----------|-------------|
| `GetDuckDBType(ColumnMetadata)` | Map SQL Server type to DuckDB LogicalType |
| `ConvertValue(ColumnValue, ColumnMetadata, Vector&, idx_t)` | Write value to DuckDB Vector |
| `IsSupported(type_id)` | Check if type is supported |

**Specialized Encoders** (in `src/encoding/`):
- `DecimalEncoding`: DECIMAL/NUMERIC sign+magnitude format
- `DateTimeEncoding`: DATE, TIME, DATETIME, DATETIME2 wire formats
- `GuidEncoding`: UNIQUEIDENTIFIER mixed-endian byte reordering

**Type Mapping Table:**

| SQL Server Type ID | DuckDB LogicalType |
|--------------------|-------------------|
| 0x30 (TINYINT) | TINYINT |
| 0x34 (SMALLINT) | SMALLINT |
| 0x38 (INT) | INTEGER |
| 0x7F (BIGINT) | BIGINT |
| 0x32 (BIT) | BOOLEAN |
| 0x3B (REAL) | FLOAT |
| 0x3E (FLOAT) | DOUBLE |
| 0x6A, 0x6C (DECIMAL/NUMERIC) | DECIMAL(p,s) |
| 0x3C (MONEY) | DECIMAL(19,4) |
| 0x7A (SMALLMONEY) | DECIMAL(10,4) |
| 0xAF, 0xA7 (CHAR/VARCHAR) | VARCHAR |
| 0xEF, 0xE7 (NCHAR/NVARCHAR) | VARCHAR |
| 0x28 (DATE) | DATE |
| 0x29 (TIME) | TIME |
| 0x3D (DATETIME) | TIMESTAMP |
| 0x2A (DATETIME2) | TIMESTAMP |
| 0x24 (UNIQUEIDENTIFIER) | UUID |
| 0xAD, 0xA5 (BINARY/VARBINARY) | BLOB |

**Relationships:**
- Used by `QueryResult`
- References `ColumnMetadata`

---

### QueryResult

Streaming result iterator that yields DataChunks.

**Attributes:**

| Attribute | Type | Description |
|-----------|------|-------------|
| connection | TdsConnection* | Borrowed connection |
| parser | TokenParser | Token stream parser |
| converter | TypeConverter | Type conversion |
| columns | std::vector<ColumnMetadata> | Result columns |
| errors | std::vector<TdsError> | Accumulated errors |
| is_cancelled | std::atomic<bool> | Cancellation flag |
| rows_read | uint64_t | Total rows processed |

**Operations:**
- `GetColumnTypes()`: Return DuckDB types for bind
- `GetColumnNames()`: Return column names for bind
- `FillChunk(DataChunk&)`: Fill next chunk with rows
- `Cancel()`: Initiate query cancellation
- `IsComplete()`: Check if all rows consumed

**States:**

| State | Description |
|-------|-------------|
| Initializing | Waiting for COLMETADATA |
| Streaming | Yielding ROW tokens |
| Draining | Cancellation in progress |
| Complete | Final DONE received |
| Error | Fatal error occurred |

**Relationships:**
- Manages `TokenParser`
- Uses `TypeConverter`
- Yields to DuckDB via DataChunk

---

### MSSQLQueryExecutor

Orchestrates query execution with pool integration.

**Attributes:**

| Attribute | Type | Description |
|-----------|------|-------------|
| pool | ConnectionPool& | Connection pool reference |
| context_name | std::string | Attached database name |
| acquire_timeout_ms | int | Pool acquire timeout |

**Operations:**
- `Execute(sql: string)`: Execute query, return QueryResult
- `ValidateContext()`: Ensure context exists

**Relationships:**
- Acquires from `ConnectionPool`
- Creates `SqlBatch`
- Returns `QueryResult`

---

## State Transitions

### Connection State (Extended from Spec 003)

```
                    ┌─────────────────┐
                    │  Disconnected   │
                    └────────┬────────┘
                             │ Connect()
                             ▼
                    ┌─────────────────┐
                    │ Authenticating  │
                    └────────┬────────┘
                             │ Authenticate()
                             ▼
           Cancel()  ┌─────────────────┐  ExecuteBatch()
          ┌─────────│      Idle       │◄──────────┐
          │         └────────┬────────┘           │
          │                  │ ExecuteBatch()     │ Complete
          │                  ▼                    │
          │         ┌─────────────────┐           │
          │         │   Executing     │───────────┘
          │         └────────┬────────┘
          │                  │ SendAttention()
          │                  ▼
          │         ┌─────────────────┐
          └────────►│   Cancelling    │
                    └────────┬────────┘
                             │ DONE+ATTN received
                             ▼
                    ┌─────────────────┐
                    │      Idle       │
                    └─────────────────┘
```

### QueryResult State

```
                    ┌─────────────────┐
                    │  Initializing   │
                    └────────┬────────┘
                             │ COLMETADATA received
                             ▼
                    ┌─────────────────┐  Cancel()  ┌──────────────┐
                    │   Streaming     │───────────►│   Draining   │
                    └────────┬────────┘            └──────┬───────┘
                             │ DONE received              │ DONE+ATTN
                             ▼                            ▼
                    ┌─────────────────┐            ┌──────────────┐
                    │    Complete     │            │   Complete   │
                    └─────────────────┘            └──────────────┘
                             │
                             │ Fatal ERROR
                             ▼
                    ┌─────────────────┐
                    │     Error       │
                    └─────────────────┘
```

## Validation Rules

### ColumnMetadata Validation
- `type_id` must be in supported types list
- `precision` for DECIMAL must be 1-38
- `scale` for DECIMAL must be 0-precision
- `max_length` for variable types must be > 0 or 0xFFFF (MAX)

### RowData Validation
- Column count must match ColumnMetadata count
- NULL values only allowed where `is_nullable = true`
- Values must fit within declared precision/scale

### Query Execution Validation
- Context must exist in MSSQLContextManager
- Connection must transition from Idle to Executing
- SQL text must not be empty

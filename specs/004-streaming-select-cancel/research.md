# Research: Streaming SELECT and Query Cancellation

**Feature**: 004-streaming-select-cancel
**Date**: 2026-01-15

## 1. TDS SQL_BATCH Message Format

### Decision
Use TDS SQL_BATCH packet type (0x01) with UTF-16LE encoded SQL text.

### Rationale
SQL_BATCH is the standard TDS mechanism for sending ad-hoc SQL queries. It's simpler than RPC_REQUEST (which requires parameter metadata) and sufficient for this feature's scope.

### Format

```
SQL_BATCH Message:
┌─────────────────────────────────────────────────┐
│ TDS Header (8 bytes)                           │
│   Type: 0x01 (SQL_BATCH)                       │
│   Status: 0x01 (EOM) or 0x00 (continuation)    │
│   Length: uint16_t big-endian                  │
│   SPID: uint16_t (0 for client packets)        │
│   PacketID: uint8_t (sequence 1-255)           │
│   Window: uint8_t (always 0)                   │
├─────────────────────────────────────────────────┤
│ SQL Text (UTF-16LE encoded, no length prefix)  │
│   Query string converted to UTF-16LE           │
│   No null terminator required                  │
└─────────────────────────────────────────────────┘
```

### Packet Splitting
- Maximum TDS packet size: 32767 bytes (negotiated, default 4096)
- For queries > (packet_size - 8), split into multiple packets
- Continuation packets: Status = 0x00
- Final packet: Status = 0x01 (EOM)

### Alternatives Considered
- **RPC_REQUEST (0x03)**: More complex, requires parameter metadata. Useful for parameterized queries but overkill for this feature.
- **Prepared statements**: SQL Server doesn't have traditional prepared statements; uses sp_prepare/sp_execute which are RPC calls. Deferred to future feature.

## 2. TDS Token Stream Format

### Decision
Implement incremental token parser that handles tokens spanning TDS packet boundaries.

### Rationale
TDS responses are token streams that may span multiple packets. A buffer-based parser with token continuations handles all edge cases reliably.

### Token Formats

#### COLMETADATA (0x81)
```
COLMETADATA Token:
┌─────────────────────────────────────────────────┐
│ Token Type: 0x81 (1 byte)                      │
│ Column Count: uint16_t LE                       │
├─────────────────────────────────────────────────┤
│ For each column:                               │
│   UserType: uint32_t LE (legacy, ignore)       │
│   Flags: uint16_t LE (nullable, identity, etc) │
│   TYPE_INFO: variable (type-specific)          │
│   ColName: B_VARCHAR (1-byte length + UTF-16LE)│
└─────────────────────────────────────────────────┘
```

**TYPE_INFO Structure** (varies by type):
- Fixed-length types (INT, BIGINT, etc.): type_id only
- Variable-length types (VARCHAR): type_id + max_length
- Precision types (DECIMAL): type_id + length + precision + scale
- Collation types (CHAR/VARCHAR): type_id + collation_info (5 bytes)

#### ROW (0xD1)
```
ROW Token:
┌─────────────────────────────────────────────────┐
│ Token Type: 0xD1 (1 byte)                      │
├─────────────────────────────────────────────────┤
│ For each column (order matches COLMETADATA):   │
│   Value: type-dependent encoding               │
│   NULL: 0x00 length prefix for variable types  │
│         or special NULL indicator for fixed    │
└─────────────────────────────────────────────────┘
```

#### DONE/DONEPROC/DONEINPROC (0xFD/0xFE/0xFF)
```
DONE Token:
┌─────────────────────────────────────────────────┐
│ Token Type: 0xFD/0xFE/0xFF (1 byte)            │
│ Status: uint16_t LE                             │
│   0x0000 = DONE_FINAL                          │
│   0x0001 = DONE_MORE (more results)            │
│   0x0002 = DONE_ERROR                          │
│   0x0010 = DONE_COUNT (row count valid)        │
│   0x0020 = DONE_ATTN (attention ack)           │
│ CurCmd: uint16_t LE (current command)          │
│ RowCount: uint64_t LE (if DONE_COUNT set)      │
└─────────────────────────────────────────────────┘
```

#### ERROR (0xAA)
```
ERROR Token:
┌─────────────────────────────────────────────────┐
│ Token Type: 0xAA (1 byte)                      │
│ Length: uint16_t LE                             │
│ Number: uint32_t LE (error number)             │
│ State: uint8_t                                  │
│ Class: uint8_t (severity 0-25)                 │
│ MsgText: US_VARCHAR (uint16 len + UTF-16LE)    │
│ ServerName: B_VARCHAR                          │
│ ProcName: B_VARCHAR                            │
│ LineNumber: uint32_t LE                        │
└─────────────────────────────────────────────────┘
```

#### INFO (0xAB)
Same structure as ERROR token.

### Alternatives Considered
- **Full packet buffering**: Simpler but violates streaming principle. Would require buffering entire response before parsing.
- **State machine per token type**: More complex. Single unified parser with type dispatch is cleaner.

## 3. SQL Server Type IDs and Wire Formats

### Decision
Support common types with explicit type ID → DuckDB type mapping. Fail explicitly for unsupported types.

### Rationale
Complete type coverage is impractical for initial implementation. Focus on types covering 95%+ of real-world usage. Explicit failure prevents silent data corruption.

### Type Mappings

#### Fixed-Length Integer Types
| SQL Server | Type ID | Wire Size | DuckDB | Wire Format |
|------------|---------|-----------|--------|-------------|
| TINYINT | 0x30 | 1 byte | TINYINT | uint8_t |
| SMALLINT | 0x34 | 2 bytes | SMALLINT | int16_t LE |
| INT | 0x38 | 4 bytes | INTEGER | int32_t LE |
| BIGINT | 0x7F | 8 bytes | BIGINT | int64_t LE |
| BIT | 0x32 | 1 byte | BOOLEAN | 0x00/0x01 |

#### Floating-Point Types
| SQL Server | Type ID | Wire Size | DuckDB | Wire Format |
|------------|---------|-----------|--------|-------------|
| REAL | 0x3B | 4 bytes | FLOAT | IEEE 754 single |
| FLOAT | 0x3E | 8 bytes | DOUBLE | IEEE 754 double |

#### Decimal Types
| SQL Server | Type ID | Wire Format | DuckDB |
|------------|---------|-------------|--------|
| DECIMAL | 0x6A | sign(1) + magnitude(4-16) | DECIMAL(p,s) |
| NUMERIC | 0x6C | same as DECIMAL | DECIMAL(p,s) |
| MONEY | 0x3C | int64_t LE (÷10000) | DECIMAL(19,4) |
| SMALLMONEY | 0x7A | int32_t LE (÷10000) | DECIMAL(10,4) |

#### String Types
| SQL Server | Type ID | Wire Format | DuckDB |
|------------|---------|-------------|--------|
| CHAR | 0xAF | fixed-length bytes | VARCHAR |
| VARCHAR | 0xA7 | uint16 len + bytes | VARCHAR |
| NCHAR | 0xEF | fixed-length UTF-16LE | VARCHAR (UTF-8) |
| NVARCHAR | 0xE7 | uint16 len + UTF-16LE | VARCHAR (UTF-8) |

#### Date/Time Types
| SQL Server | Type ID | Wire Format | DuckDB |
|------------|---------|-------------|--------|
| DATE | 0x28 | 3 bytes (days since 0001-01-01) | DATE |
| TIME | 0x29 | 3-5 bytes (scale-dependent) | TIME |
| DATETIME | 0x3D | 4+4 bytes (days + ticks) | TIMESTAMP |
| DATETIME2 | 0x2A | 6-8 bytes (scale-dependent) | TIMESTAMP |

#### Binary Types
| SQL Server | Type ID | Wire Format | DuckDB |
|------------|---------|-------------|--------|
| BINARY | 0xAD | fixed-length bytes | BLOB |
| VARBINARY | 0xA5 | uint16 len + bytes | BLOB |
| UNIQUEIDENTIFIER | 0x24 | 16 bytes (mixed-endian GUID) | UUID |

### NULL Handling
- Fixed-length types: Use NULLABLE type variants (0x26 for INTN, etc.) with length prefix
  - Length 0 = NULL
  - Length N = N bytes of data
- Variable-length types: Length 0xFFFF = NULL

### Alternatives Considered
- **Generic VARIANT type**: Would require runtime type inspection. Explicit mapping is more predictable and testable.
- **Support all types**: Impractical scope. IMAGE/TEXT/NTEXT are deprecated. XML/GEOGRAPHY/GEOMETRY need specialized handling.

## 4. DuckDB DataChunk Streaming API

### Decision
Use DuckDB's `DataChunk` and `Vector` APIs for direct result population without intermediate buffers.

### Rationale
DuckDB's columnar format matches well with TDS row-by-row parsing if we buffer rows into chunks before emitting. DataChunk size (2048 rows default) provides natural batching boundary.

### API Usage Pattern

```cpp
// In table function scan
void MSSQLScanFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    auto &state = data.local_state->Cast<MSSQLScanLocalState>();

    idx_t row_count = 0;
    while (row_count < STANDARD_VECTOR_SIZE && state.HasMoreRows()) {
        auto row = state.ReadNextRow();

        for (idx_t col = 0; col < output.ColumnCount(); col++) {
            auto &vector = output.data[col];

            if (row.IsNull(col)) {
                FlatVector::SetNull(vector, row_count, true);
            } else {
                // Type-specific value assignment
                switch (state.GetColumnType(col)) {
                    case LogicalTypeId::INTEGER:
                        FlatVector::GetData<int32_t>(vector)[row_count] = row.GetInt32(col);
                        break;
                    case LogicalTypeId::VARCHAR:
                        FlatVector::GetData<string_t>(vector)[row_count] =
                            StringVector::AddString(vector, row.GetString(col));
                        break;
                    // ... other types
                }
            }
        }
        row_count++;
    }

    output.SetCardinality(row_count);
}
```

### Key APIs
- `DataChunk::SetCardinality(idx_t count)`: Set number of rows in chunk
- `FlatVector::GetData<T>(Vector &vec)`: Get typed data pointer
- `FlatVector::SetNull(Vector &vec, idx_t idx, bool is_null)`: Set NULL mask
- `StringVector::AddString(Vector &vec, string_t str)`: Add string with ownership
- `FlatVector::Validity(Vector &vec)`: Get validity mask for bulk NULL operations

### Alternatives Considered
- **Returning single rows**: Inefficient, DuckDB optimized for vectorized processing.
- **Full result buffering**: Violates streaming principle, memory unbounded.
- **Custom result format**: Unnecessary, DataChunk is the standard interface.

## 5. Query Cancellation Protocol

### Decision
Implement TDS Attention packet for cancellation, with proper token drain sequence.

### Rationale
TDS Attention is the standard mechanism for query cancellation. Proper implementation allows connection reuse after cancellation.

### Attention Packet Format
```
Attention Packet:
┌─────────────────────────────────────────────────┐
│ TDS Header (8 bytes)                           │
│   Type: 0x06 (ATTENTION)                       │
│   Status: 0x01 (EOM)                           │
│   Length: 0x0008 (header only, no payload)     │
│   SPID: 0x0000                                 │
│   PacketID: next_sequence                      │
│   Window: 0x00                                 │
└─────────────────────────────────────────────────┘
```

### Cancellation Sequence
1. **Send Attention**: Immediately send Attention packet
2. **Continue reading**: Continue reading response tokens
3. **Detect acknowledgment**: Look for DONE token with DONE_ATTN flag (0x0020)
4. **Drain remaining**: Consume any tokens after DONE+ATTN until socket empty
5. **Transition state**: Connection state → `idle`

### Edge Cases
- **Query completes before attention processed**: Results delivered normally, Attention ignored
- **Attention timeout**: If no DONE+ATTN within timeout (5s default), close connection
- **Multiple result sets**: Attention cancels all pending results

### Implementation Notes
- Attention can be sent at any time during query execution
- Server may send partial results before acknowledging attention
- Must drain all tokens to ensure connection is clean

### Alternatives Considered
- **Connection close**: Simpler but wastes pooled connection. Cancellation is more efficient.
- **KILL command**: Server-side cancellation, but requires separate connection. Attention is cleaner.

## 6. Error Handling Strategy

### Decision
Accumulate errors during token parsing, throw DuckDB exception at appropriate point based on severity.

### Rationale
SQL Server can return multiple errors in a batch. Accumulation allows complete error reporting. Severity-based handling matches SQL Server semantics.

### Severity Levels
| Range | Meaning | Action |
|-------|---------|--------|
| 0-10 | Informational | Log as warning, continue |
| 11-16 | User error | Accumulate, throw after result |
| 17-19 | Server error | Accumulate, may continue |
| 20-25 | Fatal | Throw immediately, close connection |

### Error Propagation
```cpp
void ThrowSqlServerError(const TdsError& error) {
    throw InvalidInputException(
        "SQL Server Error %d (Severity %d, State %d): %s\n"
        "Server: %s, Procedure: %s, Line: %d",
        error.number, error.severity, error.state, error.message.c_str(),
        error.server_name.c_str(), error.proc_name.c_str(), error.line_number);
}
```

### Alternatives Considered
- **First error only**: Loses context in multi-error scenarios.
- **All errors as warnings**: Doesn't match SQL Server severity semantics.
- **Custom exception types**: Overkill, DuckDB's InvalidInputException is sufficient.

## 7. DuckDB Warning and Logging Integration

### Decision
Use DuckDB's standard warning mechanism for INFO messages; debug logging via `enable_logging`.

### Rationale
Aligns with DuckDB patterns. Users can enable debug logging when needed without code changes.

### Warning API
```cpp
// Add warning to client context
context.AddWarning("SQL Server: " + info.message);
```

### Debug Logging
```cpp
// Via DuckDB's logging infrastructure
// User enables: CALL enable_logging(level = 'debug');
// Then extension logs are visible
Logger::Debug("TDS: Received COLMETADATA with %d columns", column_count);
Logger::Debug("TDS: Row %llu parsed, %d bytes", row_number, row_size);
```

### Alternatives Considered
- **Custom logging**: Inconsistent with DuckDB patterns.
- **Stderr output**: Not controllable, pollutes output.
- **Silent operation**: Loses valuable debugging information.

## 8. Connection Pool Integration

### Decision
Acquire connection from pool at query start, return on completion or cancellation.

### Rationale
Integrates with existing pool from spec 003. Connection reuse is critical for performance.

### Integration Points
```cpp
// Acquire connection for query
auto conn = pool.Acquire(timeout_ms);
if (!conn) {
    throw InvalidInputException("Failed to acquire connection from pool");
}

// State transition for query
if (!conn->TransitionState(ConnectionState::Idle, ConnectionState::Executing)) {
    pool.Release(std::move(conn));
    throw InvalidInputException("Connection not in idle state");
}

// On success: return to pool
conn->TransitionState(ConnectionState::Executing, ConnectionState::Idle);
pool.Release(std::move(conn));

// On cancellation: handle attention sequence, then return
// On error: close if fatal, otherwise return
```

### Alternatives Considered
- **New connection per query**: Defeats pooling purpose.
- **Connection held for session**: Limits concurrency, wastes resources.

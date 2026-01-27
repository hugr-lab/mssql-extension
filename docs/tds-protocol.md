# TDS Protocol Layer

The TDS (Tabular Data Stream) protocol layer implements Microsoft's proprietary wire protocol for SQL Server communication. The implementation covers TDS 7.4 (SQL Server 2019+) and is located in `src/tds/` and `src/include/tds/`.

## TdsConnection State Machine

`TdsConnection` (`src/tds/tds_connection.cpp`) manages a single TDS connection with an atomic state machine.

```
Disconnected
    │ Connect(host, port, timeout)
    ▼
Authenticating
    │ DoPrelogin() → DoLogin7()
    │   ├─ PRELOGIN exchange
    │   ├─ [Optional] TLS handshake via EnableTls()
    │   └─ LOGIN7 exchange
    ▼
Idle (ready for queries)
    │ ExecuteBatch(sql)
    ▼
Executing (query in progress)
    │                 │
    │ DONE token      │ SendAttention()
    ▼                 ▼
Idle              Cancelling
                    │ WaitForAttentionAck() → DONE(ATTN)
                    ▼
                  Idle
    │ Close()
    ▼
Disconnected
```

**State enum** (`ConnectionState` in `tds_connection.hpp`):
- `Disconnected` (0) — no TCP connection
- `Authenticating` (1) — PRELOGIN/LOGIN7 in progress
- `Idle` (2) — connected, ready for queries
- `Executing` (3) — query in progress, receiving response
- `Cancelling` (4) — ATTENTION sent, awaiting ACK

State transitions use `std::atomic<ConnectionState>` with acquire/release ordering for lock-free thread safety.

## TdsSocket

`TdsSocket` (`src/tds/tds_socket.cpp`) provides cross-platform TCP and TLS socket operations.

### Platform Abstraction

| Feature | Windows | POSIX |
|---|---|---|
| Socket close | `closesocket()` | `close()` |
| Error code | `WSAGetLastError()` | `errno` |
| Poll | `WSAPoll()` | `poll()` |
| Buffer type | `char*` | `void*` |

### TLS Support

TLS is implemented via OpenSSL (through `TlsTdsContext` in `src/tds/tls/`). The TLS handshake uses custom BIO callbacks because SQL Server wraps the TLS handshake inside PRELOGIN packets.

**TlsTdsContext** (`src/tds/tls/tds_tls_impl.cpp`):
- `Initialize()` — create OpenSSL SSL_CTX
- `SetBioCallbacks(send_cb, recv_cb)` — custom I/O for TDS-wrapped TLS
- `Handshake(timeout_ms)` — perform TLS handshake
- `Send()` / `Receive()` — encrypted I/O
- `Close()` — graceful close_notify

**TLS error codes** (`TlsErrorCode`):
- `HANDSHAKE_FAILED`, `HANDSHAKE_TIMEOUT`, `SERVER_NO_ENCRYPT`, `TLS_NOT_AVAILABLE`

## TDS Packet Types

`TdsPacket` (`src/tds/tds_packet.cpp`) handles packet construction and parsing.

### Packet Header Format (8 bytes)

```
Byte 0:    Type (PacketType enum)
Byte 1:    Status (PacketStatus flags)
Bytes 2-3: Length (big-endian uint16, includes header)
Bytes 4-5: SPID (big-endian uint16)
Byte 6:    Packet ID (sequential)
Byte 7:    Window (reserved, always 0)
```

### Packet Types (`PacketType` enum)

| Type | Value | Direction | Purpose |
|---|---|---|---|
| `SQL_BATCH` | 0x01 | Client → Server | Execute SQL statement |
| `PRELOGIN` | 0x12 | Both | Pre-authentication negotiation |
| `TABULAR_RESULT` | 0x04 | Server → Client | Query results |
| `ATTENTION` | 0x06 | Client → Server | Cancel running query |
| `LOGIN7` | 0x10 | Client → Server | Authentication |
| `TRANSACTION_MANAGER_REQUEST` | 0x0E | Client → Server | Transaction operations |

### Packet Status Flags (`PacketStatus`)

| Flag | Value | Meaning |
|---|---|---|
| `NORMAL` | 0x00 | More packets follow |
| `END_OF_MESSAGE` | 0x01 | Last packet in message |

### SQL_BATCH with ALL_HEADERS

When a transaction descriptor is active, SQL_BATCH packets include an ALL_HEADERS structure:

```
[TDS Header - 8 bytes]
[ALL_HEADERS]
  TotalLength:              4 bytes LE (22)
  HeaderLength:             4 bytes LE (18)
  HeaderType:               2 bytes LE (0x0002 = Transaction Descriptor)
  TransactionDescriptor:    8 bytes (from ENVCHANGE or zeros)
  OutstandingRequestCount:  4 bytes LE (1)
[SQL text in UTF-16LE]
```

## PRELOGIN Handshake

**Client → Server** (PRELOGIN request):
- `VERSION` option (0x00): TDS version info
- `ENCRYPTION` option (0x01): Encryption preference
- `TERMINATOR` (0xFF): End of options

**Server → Client** (PRELOGIN response):
- Server indicates encryption capability
- If both sides support encryption: TLS handshake follows (wrapped in PRELOGIN packets)

**Encryption options** (`EncryptionOption`):
- `ENCRYPT_OFF` (0x00), `ENCRYPT_ON` (0x01), `ENCRYPT_NOT_SUP` (0x02), `ENCRYPT_REQ` (0x03)

## LOGIN7 Authentication

**Client → Server** (LOGIN7 packet):
- Hostname, username, obfuscated password, database, app name
- TDS version, client process ID, interface version

**Server → Client** response tokens:
- `LOGINACK` — login succeeded (contains SPID, TDS version)
- `ENVCHANGE` — environment changes (packet size, database, language, collation)
- `ERROR` — login failed

## Token Parsing

`TokenParser` (`src/tds/tds_token_parser.cpp`) implements incremental token stream parsing. Data is fed in chunks and tokens are parsed as they become available.

### Token Types

| Token | ID | Content |
|---|---|---|
| `COLMETADATA` | 0x81 | Column definitions (types, names, flags) |
| `ROW` | 0xD1 | Standard row data |
| `NBCROW` | 0xD2 | Null Bitmap Compressed row (SQL Server 2012+) |
| `DONE` | 0xFD | Statement complete |
| `DONEPROC` | 0xFE | Procedure complete |
| `DONEINPROC` | 0xFF | Done inside procedure, more follows |
| `ERROR` | 0xAA | Error from server |
| `INFO` | 0xAB | Informational message |
| `ENVCHANGE` | 0xE3 | Environment change (database, packet size, transaction) |
| `LOGINACK` | 0xAD | Login succeeded |
| `ORDER` | 0xA9 | ORDER BY column list |
| `RETURNSTATUS` | 0x79 | Stored procedure return value |

### COLMETADATA Token (0x81)

```
Token ID:     1 byte (0x81)
Column Count: 2 bytes LE

Per column:
  Type ID:      1 byte
  [Type-specific metadata]
    Fixed types:      none
    Variable types:   max_length (2 bytes LE)
    DECIMAL/NUMERIC:  precision (1), scale (1)
    String types:     collation (4 bytes)
  Flags:        2 bytes LE (nullable, identity, computed)
  Column Name:  B_VARCHAR (1-byte length + UTF-16LE name)
```

### ROW Token (0xD1)

Per column (in COLMETADATA order):
- **Fixed types**: raw bytes (1/2/4/8 depending on type)
- **Nullable fixed** (INTN, FLOATN): 1-byte length prefix (0 = NULL)
- **Variable-length** (VARCHAR, VARBINARY): 2-byte length LE (0xFFFF = NULL)
- **DECIMAL**: 1-byte length + sign byte + magnitude
- **PLP types** (MAX): 8-byte total length + chunked data

### NBCROW Token (0xD2) — Null Bitmap Compressed

```
Token ID:    1 byte (0xD2)
Null Bitmap: ceil(column_count / 8) bytes
             1 bit per column (1 = NULL)

For non-NULL columns only:
  [Value data without length prefix for nullable types]
```

### DONE Token (0xFD / 0xFE / 0xFF)

```
Token ID:        1 byte
Status:          2 bytes LE (DoneStatus flags)
Current Command: 2 bytes LE
Row Count:       8 bytes LE (valid if DONE_COUNT set)
```

**Status flags**:
- `DONE_MORE` (0x0001) — more results follow
- `DONE_ERROR` (0x0002) — error occurred
- `DONE_INXACT` (0x0004) — inside transaction
- `DONE_COUNT` (0x0010) — row count is valid
- `DONE_ATTN` (0x0020) — ATTENTION acknowledgment

### ERROR Token (0xAA)

```
Token ID:           1 byte (0xAA)
Length:             2 bytes LE
Error Number:      4 bytes LE
State:             1 byte
Severity:          1 byte (>=20 is fatal)
Message Length:    2 bytes LE
Message:           UTF-16LE text
Server Name:       B_VARCHAR
Procedure Name:    B_VARCHAR
Line Number:       4 bytes LE
```

### ENVCHANGE Token (0xE3)

```
Token ID: 1 byte (0xE3)
Length:   2 bytes LE
Type:     1 byte
  1 = DATABASE, 2 = LANGUAGE, 3 = CHARSET
  4 = PACKET_SIZE, 5 = COLLATION
  8 = BEGIN_TRANS (8-byte transaction descriptor)
  9 = COMMIT_TRANS, 10 = ROLLBACK_TRANS
```

For `BEGIN_TRANS` (type 8): the 8-byte transaction descriptor is stored and included in subsequent SQL_BATCH ALL_HEADERS.

## RowReader

`RowReader` (`src/tds/tds_row_reader.cpp`) extracts column values from ROW and NBCROW tokens.

### Type-Specific Readers

| Method | Types Handled |
|---|---|
| `ReadFixedType` | TINYINT, BIT, SMALLINT, INT, BIGINT, REAL, FLOAT, MONEY, DATETIME |
| `ReadNullableFixedType` | INTN, BITN, FLOATN, MONEYN, DATETIMEN |
| `ReadVariableLengthType` | VARCHAR, NVARCHAR, VARBINARY (2-byte length prefix) |
| `ReadDecimalType` | DECIMAL, NUMERIC (sign byte + magnitude) |
| `ReadDateType` | DATE (3 bytes) |
| `ReadTimeType` | TIME (3-5 bytes, scale-dependent) |
| `ReadDateTime2Type` | DATETIME2 (variable time + 3-byte date) |
| `ReadDateTimeOffsetType` | DATETIMEOFFSET (time + date + 2-byte offset) |
| `ReadGuidType` | UNIQUEIDENTIFIER (16 bytes, mixed-endian) |
| `ReadPLPType` | VARCHAR(MAX), NVARCHAR(MAX), VARBINARY(MAX) |

### PLP Format (for MAX types)

```
8 bytes: Total length (LE) or 0xFFFFFFFFFFFFFFFF for NULL
Chunks:
  4 bytes: Chunk length (LE), 0 = end
  [chunk data]
  ... repeat until chunk length = 0
```

## Encoding Subsystem

Located in `src/tds/encoding/`:

### TypeConverter (`type_converter.cpp`)

Maps SQL Server TDS types to DuckDB `LogicalType` and converts wire format values. See [Type Mapping](type-mapping.md) for the complete mapping table.

### UTF-16 (`utf16.cpp`)

- `Utf16LEEncode(string)` — UTF-8 to UTF-16LE bytes
- `Utf16LEDecode(data, byte_length)` — UTF-16LE bytes to UTF-8 string
- Used for all string data in TDS (SQL text, column names, error messages)

### DateTimeEncoding (`datetime_encoding.cpp`)

- `ConvertDate(data)` — 3-byte days since 0001-01-01
- `ConvertTime(data, scale)` — 3-5 bytes, 100ns ticks since midnight
- `ConvertDatetime(data)` — 8 bytes (4-byte days since 1900 + 4-byte 1/300s ticks)
- `ConvertDatetime2(data, scale)` — variable time + 3-byte date
- `ConvertSmallDatetime(data)` — 4 bytes (2-byte days + 2-byte minutes)
- `ConvertDatetimeOffset(data, scale)` — datetime2 + 2-byte offset in minutes

### DecimalEncoding (`decimal_encoding.cpp`)

- `ConvertDecimal(data, length)` — sign byte + little-endian magnitude
- `ConvertMoney(data)` — 8-byte value × 10000 → DECIMAL(19,4)
- `ConvertSmallMoney(data)` — 4-byte value × 10000 → DECIMAL(10,4)

### GuidEncoding (`guid_encoding.cpp`)

SQL Server GUIDs use mixed-endian format:
```
Bytes 0-3:  Data1 (little-endian uint32)
Bytes 4-5:  Data2 (little-endian uint16)
Bytes 6-7:  Data3 (little-endian uint16)
Bytes 8-15: Data4 (big-endian, as-is)
```

`ReorderGuidBytes()` converts to standard big-endian, then `ConvertGuid()` produces a DuckDB `hugeint_t` UUID.

## Timeout Constants

Defined in `src/include/tds/tds_types.hpp`:

| Constant | Value | Purpose |
|---|---|---|
| `DEFAULT_CONNECTION_TIMEOUT` | 30s | TCP connect timeout |
| `DEFAULT_IDLE_TIMEOUT` | 300s | Idle connection eviction |
| `DEFAULT_ACQUIRE_TIMEOUT` | 30s | Pool acquire timeout |
| `DEFAULT_QUERY_TIMEOUT` | 30s | Query execution timeout |
| `CANCELLATION_TIMEOUT` | 5s | ATTENTION ACK timeout |
| `LONG_IDLE_THRESHOLD` | 60s | Ping validation threshold |

## Platform Support

`tds_platform.hpp` provides:
- Windows MSVC `ssize_t` typedef (POSIX type not available on Windows)
- Cross-platform socket includes and function mappings

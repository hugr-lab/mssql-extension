# Data Model: TDS Connection, Authentication, and Pooling

**Branch**: `003-tds-connection-pooling`
**Date**: 2026-01-15

## Entities

### TdsPacket

Represents a TDS protocol packet with 8-byte header and variable payload.

| Field | Type | Size | Description |
|-------|------|------|-------------|
| type | uint8_t | 1 | Packet type (PRELOGIN=18, LOGIN7=16, SQLBATCH=1, ATTENTION=6, TABULAR=4) |
| status | uint8_t | 1 | Status flags (EOM=0x01) |
| length | uint16_t | 2 | Total packet length (big-endian) |
| spid | uint16_t | 2 | Server Process ID (big-endian) |
| packet_id | uint8_t | 1 | Sequence number (1-255, wraps) |
| window | uint8_t | 1 | Reserved, always 0 |
| payload | vector<uint8_t> | variable | Packet data |

**Constraints**:
- length ≥ 8 (header only) and ≤ 32767
- packet_id increments per packet, wraps at 255

### ConnectionState

Enumeration of valid connection states.

| Value | Name | Description |
|-------|------|-------------|
| 0 | Disconnected | No TCP connection |
| 1 | Authenticating | PRELOGIN/LOGIN7 in progress |
| 2 | Idle | Connected, ready for queries |
| 3 | Executing | Query in progress |
| 4 | Cancelling | ATTENTION sent, awaiting ACK |

**State Transition Rules**:
```
disconnected → authenticating (on open)
authenticating → idle (on auth success)
authenticating → disconnected (on auth failure)
idle → executing (on query start)
idle → disconnected (on close)
executing → idle (on query complete)
executing → cancelling (on cancel)
cancelling → idle (on ATTENTION_ACK)
cancelling → disconnected (on cancel timeout)
```

### TdsSocket

Low-level TCP socket wrapper. Pure C++ with no DuckDB dependencies.

| Field | Type | Description |
|-------|------|-------------|
| fd | int | Socket file descriptor (-1 if closed) |
| host | string | Remote hostname |
| port | uint16_t | Remote port |
| connected | bool | Connection status |

**Methods**:
- `Connect(host, port, timeout_seconds)` → bool
- `Send(data, length)` → ssize_t
- `Receive(buffer, max_length, timeout_ms)` → ssize_t
- `Close()` → void
- `IsConnected()` → bool

### TdsConnection

Represents a single TDS connection to SQL Server.

| Field | Type | Description |
|-------|------|-------------|
| socket | unique_ptr<TdsSocket> | TCP socket wrapper |
| state | atomic<ConnectionState> | Current connection state |
| spid | uint16_t | Server Process ID (assigned by server) |
| packet_id | uint8_t | Next packet sequence number |
| database | string | Current database |
| created_at | time_point | Connection creation timestamp |
| last_used_at | time_point | Last activity timestamp |
| error_message | string | Last error (empty if no error) |

**Invariants**:
- Only one active request per connection (FR-011)
- State transitions must follow valid sequences (FR-010)
- socket_fd = -1 when state = Disconnected

**Methods**:
- `connect(timeout_seconds)` → bool
- `authenticate(username, password)` → bool
- `ping()` → bool (sends minimal TDS packet)
- `send_attention()` → bool
- `close()` → void
- `is_alive()` → bool (quick state check)
- `validate_with_ping()` → bool (I/O-based check)

### ConnectionMetadata

Wrapper for tracking idle connection timestamps in pool.

| Field | Type | Description |
|-------|------|-------------|
| connection | shared_ptr<TdsConnection> | The connection |
| connection_id | uint64_t | Unique ID within pool |
| last_released | time_point | When returned to idle |

### ConnectionPool

Manages connections for a single database context.

| Field | Type | Description |
|-------|------|-------------|
| context_name | string | Attached database context name |
| secret_name | string | Associated mssql secret |
| idle_connections | queue<ConnectionMetadata> | Available connections |
| active_connections | map<uint64_t, shared_ptr<TdsConnection>> | In-use connections |
| pool_mutex | mutex | Protects all pool operations |
| available_cv | condition_variable | Signals connection availability |
| next_connection_id | uint64_t | ID counter |
| config | PoolConfig | Pool configuration |
| stats | PoolStatistics | Runtime statistics |
| cleanup_thread | thread | Background idle cleanup |
| shutdown_flag | atomic<bool> | Shutdown signal |

**Thread Safety**:
- All public methods are thread-safe (protected by pool_mutex)
- acquire() releases mutex during connection creation (blocking I/O)
- Background cleanup thread runs every 1 second

**Methods**:
- `acquire(timeout_ms)` → shared_ptr<TdsConnection>
- `release(connection)` → void
- `get_stats()` → PoolStatistics
- `shutdown()` → void

### PoolConfig

Configuration for connection pool behavior.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| connection_limit | size_t | 64 | Maximum connections per context |
| connection_cache | bool | true | Enable connection reuse |
| connection_timeout | seconds | 30 | TCP connect timeout |
| idle_timeout | seconds | 300 | Idle connection lifetime |
| min_connections | size_t | 0 | Minimum connections to maintain |
| acquire_timeout | seconds | 30 | Wait time when pool exhausted |

**Validation**:
- connection_limit ≥ 1
- idle_timeout ≥ 0 (0 = no timeout)
- acquire_timeout ≥ 0 (0 = fail immediately if exhausted)

### PoolStatistics

Runtime statistics for monitoring pool health.

| Field | Type | Description |
|-------|------|-------------|
| total_connections | size_t | Current total (idle + active) |
| idle_connections | size_t | Current idle count |
| active_connections | size_t | Current active count |
| connections_created | size_t | Lifetime created count |
| connections_closed | size_t | Lifetime closed count |
| acquire_count | size_t | Total acquire() calls |
| acquire_timeout_count | size_t | Acquire timeouts |
| acquire_wait_total_ms | uint64_t | Total time spent waiting |

## Relationships

```
                     Connection Layer (src/connection/)
┌─────────────────────────────────────────────────────────────────┐
│                                                                 │
│  ┌─────────────────┐     ┌─────────────────┐                    │
│  │  MssqlSettings  │     │ MssqlDiagnostic │                    │
│  │  (DuckDB vars)  │     │ (scalar/table)  │                    │
│  └────────┬────────┘     └────────┬────────┘                    │
│           │                       │                             │
│           ▼                       ▼                             │
│  ┌─────────────────────────────────────────┐                    │
│  │          MssqlPoolManager               │                    │
│  │   (pool lifecycle per ATTACH context)   │                    │
│  └────────────────────┬────────────────────┘                    │
│                       │                                         │
└───────────────────────│─────────────────────────────────────────┘
                        │ uses
                        ▼
                        TDS Layer (src/tds/) - No DuckDB deps
┌───────────────────────────────────────────────────────────────────┐
│                                                                   │
│  ┌─────────────────┐         ┌─────────────────┐                  │
│  │ ConnectionPool  │─────────│   PoolConfig    │                  │
│  │  (thread-safe)  │         │  (pure C++)     │                  │
│  └────────┬────────┘         └─────────────────┘                  │
│           │ manages                                               │
│           ▼                                                       │
│  ┌─────────────────┐         ┌─────────────────┐                  │
│  │ TdsConnection   │─────────│ConnectionState  │                  │
│  │ (state machine) │         │    (enum)       │                  │
│  └────────┬────────┘         └─────────────────┘                  │
│           │ uses                                                  │
│           ▼                                                       │
│  ┌─────────────────┐         ┌─────────────────┐                  │
│  │   TdsSocket     │         │  TdsProtocol    │                  │
│  │ (TCP wrapper)   │         │ (PRELOGIN/etc)  │                  │
│  └────────┬────────┘         └────────┬────────┘                  │
│           │                           │                           │
│           └───────────┬───────────────┘                           │
│                       ▼                                           │
│              ┌─────────────────┐                                  │
│              │   TdsPacket     │                                  │
│              │  (framing)      │                                  │
│              └─────────────────┘                                  │
│                                                                   │
└───────────────────────────────────────────────────────────────────┘
```

## DuckDB Integration

### MssqlPoolManager

Manages pool lifecycle tied to DuckDB ATTACH/DETACH operations. Located in `src/connection/`.

| Field | Type | Description |
|-------|------|-------------|
| pools | map<string, unique_ptr<ConnectionPool>> | Pools keyed by context name |
| manager_mutex | mutex | Protects pool map operations |

**Methods**:
- `GetOrCreatePool(context_name, config)` → ConnectionPool*
- `RemovePool(context_name)` → void
- `GetPool(context_name)` → ConnectionPool* (nullptr if not found)
- `GetPoolStats(context_name)` → PoolStatistics

### Diagnostic Functions

| Function | Type | Parameters | Returns | Description |
|----------|------|------------|---------|-------------|
| `mssql_open` | Scalar | secret_name VARCHAR | BIGINT | Opens connection, returns handle |
| `mssql_close` | Scalar | handle BIGINT | BOOLEAN | Closes connection |
| `mssql_ping` | Scalar | handle BIGINT | BOOLEAN | Tests connection health |
| `mssql_pool_stats` | Table | context_name VARCHAR | TABLE | Returns pool statistics |

### DuckDB Settings

Registered via `DBConfig::AddExtensionOption()`:

| Setting | LogicalType | Scope | Callback |
|---------|-------------|-------|----------|
| mssql_connection_limit | BIGINT | GLOBAL | Validate ≥ 1 |
| mssql_connection_cache | BOOLEAN | GLOBAL | None |
| mssql_connection_timeout | BIGINT | GLOBAL | Validate ≥ 0 |
| mssql_idle_timeout | BIGINT | GLOBAL | Validate ≥ 0 |
| mssql_min_connections | BIGINT | GLOBAL | Validate ≥ 0 |
| mssql_acquire_timeout | BIGINT | GLOBAL | Validate ≥ 0 |

## Protocol Data Structures

### PRELOGIN Options

| Type | Value | Length | Content |
|------|-------|--------|---------|
| VERSION | 0 | 6 | major(1), minor(1), build(2), suite(1) |
| ENCRYPTION | 1 | 1 | 0x00 (off), 0x01 (on), 0x02 (not supported) |
| TERMINATOR | 0xFF | 0 | End marker |

### LOGIN7 Structure

Fixed header (94 bytes, all offsets from start):

| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0 | Length | uint32_t | Total packet length |
| 4 | TDSVersion | uint32_t | 0x74000004 for TDS 7.4 |
| 8 | PacketSize | uint32_t | 4096-32767 |
| 12 | ClientProgVer | uint32_t | Client version |
| 16 | ClientPID | uint32_t | Process ID |
| 20 | ConnectionID | uint32_t | 0 for new |
| 24 | OptionFlags1 | uint8_t | Byte order, USE_DB |
| 25 | OptionFlags2 | uint8_t | ODBC, integrated sec |
| 26 | TypeFlags | uint8_t | SQL Server type |
| 27 | OptionFlags3 | uint8_t | UTF-8 flag |
| 28 | ClientTimeZone | int32_t | Minutes from UTC |
| 32 | ClientLCID | uint32_t | Locale ID |
| 36-93 | Offset/Length pairs | uint16_t × 2 | For variable fields |

Variable fields (UTF-16LE): hostname, username, password, appname, servername, database

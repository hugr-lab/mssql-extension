# Research: TDS Connection, Authentication, and Pooling

**Branch**: `003-tds-connection-pooling`
**Date**: 2026-01-15

## 1. TDS 7.4 Protocol Implementation

### Decision
Implement TDS 7.4 protocol natively in C++ targeting SQL Server 2019+.

### Rationale
- TDS 7.4 is the standard protocol for SQL Server 2012+ with full SQL Server 2019+ support
- Native implementation avoids FreeTDS/ODBC dependencies (per constitution principle I)
- Well-documented protocol with stable byte layouts

### Key Protocol Details

#### TDS Packet Header (8 bytes, all multi-byte values big-endian)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | Type | Packet type (PRELOGIN=18, LOGIN7=16, SQLBATCH=1, ATTENTION=6) |
| 1 | 1 | Status | Flags (0x01=EOM end of message) |
| 2 | 2 | Length | Total packet length including header |
| 4 | 2 | SPID | Server Process ID (client sends 0) |
| 6 | 1 | PacketID | Sequence number (wraps at 255) |
| 7 | 1 | Window | Reserved, always 0 |

#### PRELOGIN Handshake

Client sends PRELOGIN (type 0x12) with:
- VERSION option: 6 bytes (major, minor, build[2], suite)
- ENCRYPTION option: 1 byte (0x00 for unencrypted)
- TERMINATOR: 0xFF

Server responds with negotiated version and encryption agreement.

#### LOGIN7 Packet Structure

Fixed header (94 bytes) followed by variable-length UTF-16LE strings:
- TDS version: 0x74000004 (TDS 7.4)
- Packet size: 4096-32767 bytes
- Option flags for SQL Server authentication
- Offsets/lengths for: hostname, username, password, appname, servername, database

**Password Encoding**: XOR each byte with 0xA5, then rotate left 4 bits. Result stored as UTF-16LE.

#### TDS-Level Ping

Send empty SQLBATCH packet (type 0x01, status 0x01 EOM, length 8). Server responds with DONE token if connection healthy.

#### Attention Signal (Cancellation)

Send ATTENTION packet (type 0x06, status 0x01 EOM, 1-byte payload 0xFF). Server acknowledges with ATTENTION_ACK bit in DONE token.

### Alternatives Considered
- **FreeTDS**: Rejected due to external dependency and constitution compliance
- **ODBC/JDBC**: Rejected for same reasons
- **TDS 7.1**: Rejected; targeting only SQL Server 2019+ per clarification

---

## 2. DuckDB Extension Configuration API

### Decision
Use `DBConfig::AddExtensionOption()` API to register pool settings with appropriate scopes.

### Rationale
- Standard DuckDB pattern used by parquet, icu, and other extensions
- Supports both GLOBAL and SESSION scopes
- Integrates with `SET` command and PRAGMA system

### Implementation Pattern

```cpp
// In extension Load() function
auto &config = DBConfig::GetConfig(db_instance);

config.AddExtensionOption(
    "mssql_connection_limit",      // Name
    "Maximum connections per context", // Description
    LogicalType::BIGINT,           // Type
    Value::BIGINT(64),             // Default
    nullptr,                       // Validation callback (optional)
    SetScope::GLOBAL               // Scope
);
```

### Reading Settings

```cpp
Value setting_value;
if (context.TryGetCurrentSetting("mssql_connection_limit", setting_value)) {
    int64_t limit = setting_value.GetValue<int64_t>();
}
```

### Settings to Register

| Setting | Type | Default | Scope | Description |
|---------|------|---------|-------|-------------|
| `mssql_connection_limit` | BIGINT | 64 | GLOBAL | Max connections per context |
| `mssql_connection_cache` | BOOLEAN | true | GLOBAL | Enable connection reuse |
| `mssql_connection_timeout` | BIGINT | 30 | GLOBAL | TCP connect timeout (seconds) |
| `mssql_idle_timeout` | BIGINT | 300 | GLOBAL | Idle connection timeout (seconds) |
| `mssql_min_connections` | BIGINT | 0 | GLOBAL | Minimum connections to maintain |
| `mssql_acquire_timeout` | BIGINT | 30 | GLOBAL | Pool exhaustion wait timeout (seconds) |

### Alternatives Considered
- Custom storage mechanism: Rejected; DuckDB API provides standard integration
- Session-only settings: GLOBAL chosen for pool settings to ensure consistency

---

## 3. Thread-Safe Connection Pool Design

### Decision
Implement pool using `std::mutex` + `std::condition_variable` with separate idle/active tracking.

### Rationale
- C++17 standard library provides sufficient primitives
- Condition variables with predicates prevent spurious wakeups
- Lock-free alternatives (lock-free queues) add complexity without significant benefit for expected connection counts

### Core Data Structures

```cpp
struct ConnectionMetadata {
    std::shared_ptr<TdsConnection> conn;
    std::chrono::steady_clock::time_point last_released;
    uint64_t connection_id;
};

class ConnectionPool {
    std::queue<ConnectionMetadata> idle_connections_;
    std::unordered_map<uint64_t, std::shared_ptr<TdsConnection>> active_connections_;
    mutable std::mutex pool_mutex_;
    std::condition_variable available_cv_;
};
```

### Acquire Pattern

1. Lock mutex
2. If idle connection available: validate, move to active, return
3. If under limit: unlock, create connection, re-lock, add to active, return
4. If at limit: wait on condition variable with timeout
5. On timeout: throw acquire timeout exception

**Critical**: Release mutex before blocking I/O (connection creation).

### Release Pattern

1. Lock mutex
2. Remove from active set
3. If connection healthy: add to idle with timestamp
4. Notify one waiting thread
5. Unlock

### Idle Cleanup

Background thread checks every 1 second:
1. Lock mutex
2. Remove idle connections past timeout (preserve min_connections)
3. Unlock

Use `std::atomic<bool>` for clean shutdown signal.

### Connection Validation

Tiered approach:
1. Quick state check (no I/O): Always performed
2. TDS ping (I/O): Only for connections idle > 60 seconds

### Alternatives Considered
- Lock-free queue: Rejected; adds complexity, overkill for typical pool sizes (< 100)
- Single queue with state flag: Rejected; O(n) lookup for active connections
- boost::asio timers: Rejected; simple sleep loop sufficient, avoids dependency

---

## 4. Connection State Machine

### Decision
Implement explicit state machine with 5 states per FR-009.

### States

```
disconnected → authenticating → idle ⇄ executing
                                  ↑        ↓
                                  ← cancelling ←
```

### State Transitions

| From | To | Trigger |
|------|-----|---------|
| disconnected | authenticating | `mssql_open()` called |
| authenticating | idle | LOGIN7 success |
| authenticating | disconnected | LOGIN7 failure |
| idle | executing | Query starts |
| idle | disconnected | `mssql_close()` or timeout |
| executing | idle | Query completes |
| executing | cancelling | Cancel requested |
| cancelling | idle | ATTENTION_ACK received |
| cancelling | disconnected | Cancel timeout (5s) |

### Implementation

```cpp
enum class ConnectionState {
    Disconnected,
    Authenticating,
    Idle,
    Executing,
    Cancelling
};

class TdsConnection {
    std::atomic<ConnectionState> state_{ConnectionState::Disconnected};

    bool transition(ConnectionState from, ConnectionState to) {
        return state_.compare_exchange_strong(from, to);
    }
};
```

---

## 5. File Structure Plan

### New Files

```
src/
├── include/
│   ├── tds/
│   │   ├── tds_packet.hpp       # TDS packet framing
│   │   ├── tds_connection.hpp   # Single connection with state machine
│   │   └── tds_protocol.hpp     # PRELOGIN, LOGIN7, etc.
│   ├── pool/
│   │   ├── connection_pool.hpp  # Thread-safe pool
│   │   └── pool_config.hpp      # Pool configuration
│   └── mssql_diagnostic.hpp     # mssql_open/close/ping functions
├── tds/
│   ├── tds_packet.cpp
│   ├── tds_connection.cpp
│   └── tds_protocol.cpp
├── pool/
│   ├── connection_pool.cpp
│   └── pool_config.cpp
└── mssql_diagnostic.cpp
```

### Integration Points

- `mssql_extension.cpp`: Register settings, diagnostic functions
- `mssql_storage.cpp`: Use pool for connection management in ATTACH

---

## References

- Microsoft TDS Protocol Documentation (MS-TDS)
- DuckDB Extension API: `duckdb/src/include/duckdb/main/config.hpp`
- ICU Extension: Example of AddExtensionOption usage
- C++17 Standard: `<mutex>`, `<condition_variable>`, `<atomic>`, `<chrono>`

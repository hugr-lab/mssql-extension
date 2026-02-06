# Data Model: Connection & FEDAUTH Refactoring

**Branch**: `031-connection-fedauth-refactor` | **Date**: 2026-02-06

## Connection State Machine

### Current State Enum

```cpp
enum class ConnectionState : uint8_t {
    Disconnected,   // Not connected
    Prelogin,       // PRELOGIN sent, awaiting response
    LoggedIn,       // LOGIN7 complete, not yet ready for queries
    Streaming,      // Receiving result data
    Idle            // Ready for new queries
    // NOTE: "Executing" is implied when not Idle but processing
};
```

### Proposed State Machine (Phase 4)

```text
                     ┌──────────────────┐
                     │   Disconnected   │◄─────────────────────────────┐
                     └────────┬─────────┘                              │
                              │ Connect()                              │
                              ▼                                        │
                     ┌──────────────────┐                              │
                     │   Connecting     │                              │
                     └────────┬─────────┘                              │
                              │ TCP established                        │
                              ▼                                        │
                     ┌──────────────────┐                              │
                     │    Prelogin      │                              │
                     └────────┬─────────┘                              │
                              │ PRELOGIN response                      │
                              ▼                                        │
                     ┌──────────────────┐     ROUTING ENVCHANGE        │
                     │  Authenticating  │─────────────────────────────►│
                     │  (LOGIN7/FEDAUTH)│     (close & reconnect       │
                     └────────┬─────────┘      to routed server)       │
                              │                                        │
                              │ Auth success (no routing)              │
                              ▼                                        │
              ┌───────┬──────────────────┬───────┐                     │
              │       │      Idle        │       │                     │
              │       └────────┬─────────┘       │                     │
              │                │                 │                     │
        Ping()│      Execute() │         Error   │Close()              │
              │                ▼                 │                     │
              │       ┌──────────────────┐       │                     │
              │       │    Executing     │       │                     │
              │       └────────┬─────────┘       │                     │
              │                │                 │                     │
              │      Complete  │   Error         │                     │
              │                ▼                 │                     │
              └───────────────►Idle◄─────────────┘                     │
                               │                                       │
                               │ Close()                               │
                               ▼                                       │
                     ┌──────────────────┐                              │
                     │   Disconnected   │◄─────────────────────────────┘
                     └──────────────────┘
```

### Azure Routing Flow (FEDAUTH)

When connecting to Azure SQL or Fabric via FEDAUTH, the gateway may redirect:

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                        ROUTING FLOW (up to 5 hops)                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Client              Gateway                    Routed Server               │
│    │                   │                             │                      │
│    │──PRELOGIN────────►│                             │                      │
│    │◄─PRELOGIN RESP────│                             │                      │
│    │──LOGIN7+FEDAUTH──►│                             │                      │
│    │◄─FEDAUTHINFO──────│                             │                      │
│    │──FEDAUTH_TOKEN───►│                             │                      │
│    │◄─LOGINACK─────────│                             │                      │
│    │◄─ROUTING ENVCHANGE│  (new server:port)         │                      │
│    │◄─DONE─────────────│                             │                      │
│    │                   │                             │                      │
│    │   [close TCP]     │                             │                      │
│    │                                                 │                      │
│    │──PRELOGIN───────────────────────────────────────►                      │
│    │◄─PRELOGIN RESP──────────────────────────────────│                      │
│    │──LOGIN7+FEDAUTH─────────────────────────────────►                      │
│    │◄─FEDAUTHINFO────────────────────────────────────│                      │
│    │──FEDAUTH_TOKEN──────────────────────────────────►                      │
│    │◄─LOGINACK───────────────────────────────────────│  (SUCCESS)          │
│    │                                                 │                      │
└─────────────────────────────────────────────────────────────────────────────┘
```

**Key Points:**
- Gateway sends ROUTING after LOGINACK (partial success)
- Client closes connection to gateway
- Client reconnects to routed server (e.g., `*.pbidedicated.windows.net`)
- Full auth flow restarts on routed server
- Up to 5 routing hops allowed (prevents infinite loops)

### Valid State Transitions

| From | To | Trigger |
|------|-----|---------|
| Disconnected | Connecting | `Connect()` |
| Connecting | Prelogin | TCP established |
| Connecting | Disconnected | TCP error |
| Prelogin | Authenticating | PRELOGIN response received |
| Prelogin | Disconnected | PRELOGIN error |
| Authenticating | Idle | Auth success (no routing) |
| Authenticating | Disconnected | Auth failure OR routing redirect |
| Idle | Executing | `ExecuteBatch()`, `ExecuteBulkLoad()` |
| Idle | Disconnected | `Close()` |
| Executing | Idle | Query complete |
| Executing | Disconnected | Error (connection closed) |

**Note on Routing:** When ROUTING ENVCHANGE is received, the connection transitions to Disconnected, then the entire flow restarts with the new server address. This is not a separate state but a "reconnect with new target" operation.

---

## Token Cache Model

### Current Structure

```cpp
struct CachedToken {
    std::string access_token;
    std::chrono::system_clock::time_point expires_at;

    bool IsValid() const {
        auto margin = std::chrono::seconds(TOKEN_REFRESH_MARGIN_SECONDS);  // 5 min
        return std::chrono::system_clock::now() < (expires_at - margin);
    }
};

class TokenCache {
    std::mutex mutex_;
    std::unordered_map<std::string, CachedToken> cache_;  // Key: secret_name
};
```

### Token Lifecycle

```text
1. ATTACH with Azure secret
   └─► AcquireToken(secret_name)
       ├─► Cache hit (valid) → Use cached token
       └─► Cache miss/expired → Fetch new token → Cache it

2. Token used in pool factory closure
   └─► Connection created with pre-encoded UTF-16LE token

3. Token expires (~1 hour)
   └─► Connections using old token fail on reconnect
   └─► NO automatic refresh (current bug)

4. DETACH
   └─► Pool destroyed, but token cache NOT invalidated (current bug)
```

### Proposed Token Refresh Flow

```text
1. Connection auth fails with "token expired" error
2. Invalidate cached token: TokenCache::Invalidate(secret_name)
3. Acquire fresh token: AcquireToken(secret_name)
4. Retry connection with new token
5. If retry fails, propagate error to user
```

---

## Metadata Cache Model

### Cache Hierarchy

```text
MSSQLMetadataCache
├── schemas_load_state_: CacheLoadState  (catalog level)
├── schemas_last_refresh_: time_point
└── schemas_: map<string, MSSQLSchemaMetadata>
    ├── schema_name: string
    ├── tables_load_state: CacheLoadState  (schema level)
    ├── tables_last_refresh: time_point
    └── tables: map<string, MSSQLTableMetadata>
        ├── name: string
        ├── type: TableType (BASE_TABLE, VIEW)
        ├── columns_load_state: CacheLoadState  (table level)
        ├── columns_last_refresh: time_point
        └── columns: vector<MSSQLColumnMetadata>
```

### Cache Load States

```cpp
enum class CacheLoadState {
    NOT_LOADED,  // Never loaded or invalidated
    LOADING,     // Currently being loaded
    LOADED       // Loaded and valid
};
```

### Current vs Proposed Cache Access Pattern

**Current (Eager Acquisition)**:
```cpp
auto connection = pool->Acquire();  // Always acquires
EnsureSchemasLoaded(*connection);   // May not use it
pool->Release(connection);          // Wasted round-trip
```

**Proposed (Lazy Acquisition)**:
```cpp
// Check cache first WITHOUT connection
if (cache->IsSchemasLoaded() && !cache->IsTTLExpired()) {
    return cache->GetSchemaEntry(name);  // No connection needed
}
// Only acquire if cache miss
auto connection = pool->Acquire();
EnsureSchemasLoaded(*connection);
```

---

## Connection Pool Model

### Pool Statistics

```cpp
struct PoolStatistics {
    size_t total_connections;     // Active + Idle
    size_t idle_connections;      // In idle queue
    size_t active_connections;    // Checked out
    size_t connections_created;   // Total ever created
    size_t connections_closed;    // Total ever closed
    size_t acquire_count;         // Total acquire calls
    size_t acquire_timeout_count; // Acquires that timed out
    size_t pinned_connections;    // Transaction-pinned
};
```

### Pool Release Validation (Proposed)

```cpp
void ConnectionPool::Release(std::shared_ptr<TdsConnection> conn) {
    // ... existing code ...

    // NEW: Validate connection state before returning to pool
    if (conn->GetState() != ConnectionState::Idle) {
        MSSQL_POOL_DEBUG_LOG(1, "Closing connection in non-Idle state: %d",
                             static_cast<int>(conn->GetState()));
        conn->Close();
        stats_.connections_closed++;
        stats_.total_connections--;
        available_cv_.notify_one();
        return;
    }

    // ... return to idle pool ...
}
```

---

## Error Model (Phase 3)

### Proposed Structured Error Type

```cpp
struct TdsError {
    enum class Code {
        Ok,
        ConnectionFailed,
        TlsNegotiationFailed,
        AuthenticationFailed,
        TokenExpired,          // FEDAUTH specific
        RoutingFailed,         // Azure routing failure
        ProtocolError,
        StateError,            // Invalid state transition
        Timeout,
        PoolExhausted
    };

    Code code;
    std::string message;
    std::string context;         // e.g., "server:port", "schema.table"
    std::optional<TdsError> cause;  // For error chaining

    bool IsOk() const { return code == Code::Ok; }
    bool IsRetryable() const {
        return code == Code::TokenExpired ||
               code == Code::Timeout ||
               code == Code::PoolExhausted;
    }
};
```

---

## Authentication Strategy Model (Phase 1)

### Strategy Interface

```cpp
class AuthenticationStrategy {
public:
    virtual ~AuthenticationStrategy() = default;

    // Does this strategy use FEDAUTH?
    virtual bool RequiresFedAuth() const = 0;

    // Get token for FEDAUTH (only called if RequiresFedAuth() == true)
    virtual std::vector<uint8_t> GetFedAuthToken(const FedAuthInfo& info) = 0;

    // PRELOGIN options
    virtual PreloginOptions GetPreloginOptions() const = 0;

    // LOGIN7 options
    virtual Login7Options GetLogin7Options() const = 0;
};
```

### Implementations

| Strategy | RequiresFedAuth | Token Source |
|----------|-----------------|--------------|
| SqlServerAuth | false | N/A (username/password in LOGIN7) |
| AzureServicePrincipalAuth | true | Client credentials flow |
| AzureCliAuth | true | `az account get-access-token` |
| AzureDeviceCodeAuth | true | RFC 8628 device code flow |

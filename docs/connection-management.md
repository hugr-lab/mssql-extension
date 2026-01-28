# Connection Management

Connection management is organized in four layers: TDS Connection, Connection Pool, Pool Manager, and Connection Provider.

## Architecture

```
┌────────────────────────────────────────────────────────┐
│           DuckDB Client Context / Transaction          │
├────────────────────────────────────────────────────────┤
│  ConnectionProvider::GetConnection()                   │
│  - Checks IsInTransaction() / IsAutoCommit()           │
│  - Transaction mode: returns pinned connection          │
│  - Autocommit mode: acquires from pool                 │
├────────────────────────────────────────────────────────┤
│  MssqlPoolManager (global singleton)                   │
│  - pools_: Map<context_name, ConnectionPool>           │
├────────────────────────────────────────────────────────┤
│  ConnectionPool (per attached database)                │
│  - idle_connections_: Queue<ConnectionMetadata>        │
│  - active_connections_: Map<id, TdsConnection>         │
│  - cleanup_thread_ (background idle eviction)          │
├────────────────────────────────────────────────────────┤
│  TdsConnection (single TDS connection)                 │
│  - TdsSocket (TCP + optional TLS)                      │
│  - atomic<ConnectionState> state machine               │
│  - transaction_descriptor_[8]                          │
├────────────────────────────────────────────────────────┤
│  SQL Server (TDS over TCP/TLS)                         │
└────────────────────────────────────────────────────────┘
```

## ConnectionPool

**Files**: `src/tds/tds_connection_pool.cpp`, `src/include/tds/tds_connection_pool.hpp`

Thread-safe connection pool with background cleanup. One pool exists per attached MSSQL database.

### Configuration

```cpp
struct PoolConfiguration {
    size_t connection_limit;   // Default: 64
    bool connection_cache;     // Default: true
    int connection_timeout;    // Default: 30 seconds
    int idle_timeout;          // Default: 300 seconds (5 minutes)
    size_t min_connections;    // Default: 0
    int acquire_timeout;       // Default: 30 seconds
    int query_timeout;         // Default: 30 seconds (0 = infinite)
};
```

### Acquire Flow

```
Acquire(timeout_ms)
  ├─ Check shutdown flag
  ├─ Lock pool_mutex_
  └─ Loop until timeout:
       ├─ TryAcquireIdle()
       │   ├─ Short idle: IsAlive() check (no I/O)
       │   └─ Long idle (>60s): Ping validation (empty SQL_BATCH → DONE)
       ├─ If under connection_limit: CreateNewConnection() (unlock during I/O)
       └─ If at limit: Wait on available_cv_ (condition variable)
```

### Release Flow

```
Release(connection)
  ├─ Lock pool_mutex_
  ├─ Remove from active_connections_
  ├─ If !connection_cache OR !IsAlive(): Close
  └─ Otherwise: Push to idle_connections_ queue, notify available_cv_
```

### Connection Reset on Pool Return

When a connection is returned to the pool (autocommit mode or after transaction commit/rollback), it is flagged for reset via `TdsConnection::SetNeedsReset(true)`. On the next `ExecuteBatch()` call, the TDS `RESET_CONNECTION` flag (0x08) is OR'd into the first packet's Status byte. This causes SQL Server to clean up session state (temp tables, session variables, SET options) before executing the next batch — the same mechanism used by ADO.NET, JDBC, and ODBC drivers.

### Background Cleanup Thread

Runs every 1 second:
1. Check idle connections against `idle_timeout`
2. Close expired connections (preserves `min_connections`)
3. Validate long-idle connections via ping
4. Exits cleanly when `shutdown_flag_` is set

### Thread Safety

| Element | Type | Purpose |
|---|---|---|
| `pool_mutex_` | `std::mutex` | Protects queues, maps, stats |
| `available_cv_` | `std::condition_variable` | Acquire/Release handoff |
| `shutdown_flag_` | `std::atomic<bool>` | Shutdown coordination |
| `cleanup_thread_` | `std::thread` | Background cleanup |

### Statistics

```cpp
struct PoolStatistics {
    size_t total_connections;       // Created - Closed
    size_t idle_connections;
    size_t active_connections;
    size_t connections_created;
    size_t connections_closed;
    size_t acquire_count;           // Total acquisition attempts
    size_t acquire_timeout_count;   // Failed acquisitions
    size_t pinned_connections;      // Connections pinned to transactions
};
```

Exposed via `mssql_pool_stats()` table function.

## MssqlPoolManager

**Files**: `src/connection/mssql_pool_manager.cpp`, `src/include/connection/mssql_pool_manager.hpp`

Global singleton registry managing one pool per attached database context.

### Key Methods

| Method | Purpose |
|---|---|
| `Instance()` | Static singleton accessor (Meyer's pattern) |
| `GetOrCreatePool(name, config, conn_info)` | Get existing or create new pool |
| `GetPool(name)` | Get existing pool (nullptr if not found) |
| `RemovePool(name)` | Remove and shutdown pool (called on DETACH) |
| `HasPool(name)` | Check if pool exists |

### Pool Creation

`GetOrCreatePool()` creates a connection factory lambda that captures host, port, credentials, and TLS settings. The factory:
1. Creates a `TdsConnection`
2. Calls `Connect(host, port)` for TCP
3. Calls `Authenticate(user, pass, db, use_encrypt)` for PRELOGIN + LOGIN7
4. Returns the authenticated connection (or nullptr on failure)

## ConnectionProvider

**Files**: `src/connection/mssql_connection_provider.cpp`, `src/include/connection/mssql_connection_provider.hpp`

Transaction-aware static interface for acquiring and releasing connections.

### GetConnection Flow

```
GetConnection(context, catalog, timeout_ms)
  │
  ├─ Is autocommit? ─── YES ──→ Acquire from pool, return
  │
  └─ NO (explicit transaction)
       │
       ├─ Get MSSQLTransaction from context
       │
       ├─ Already has pinned connection? ─── YES ──→ Return pinned
       │
       └─ NO (first access in transaction)
            ├─ Acquire connection from pool
            ├─ Pin to transaction: txn->SetPinnedConnection(conn)
            ├─ Execute "BEGIN TRANSACTION"
            ├─ Receive TDS response
            ├─ Parse ENVCHANGE token for BEGIN_TRANS (type 0x08)
            ├─ Extract 8-byte transaction descriptor
            ├─ Store in txn->SetTransactionDescriptor()
            ├─ Store in conn->SetTransactionDescriptor()
            ├─ Mark txn->SetSqlServerTransactionActive(true)
            └─ Return pinned connection
```

### ReleaseConnection Flow

```
ReleaseConnection(context, catalog, conn)
  │
  ├─ Autocommit? ─── YES ──→ Flag for reset, return to pool
  │
  └─ In transaction? ─── YES ──→ No-op (stays pinned)
                         NO  ──→ Flag for reset, return to pool
```

In autocommit mode, `ReleaseConnection()` calls `conn->SetNeedsReset(true)` before returning the connection to the pool. The `RESET_CONNECTION` flag will be set on the TDS header of the next `SQL_BATCH`, resetting session state (temp tables, variables, SET options).

### Error Handling

If `BEGIN TRANSACTION` fails:
1. Clear pinned connection from transaction
2. Return connection to pool
3. Throw `IOException`

## Secret Management

**Files**: `src/mssql_secret.cpp`, `src/include/mssql_secret.hpp`

Integrates with DuckDB's secret manager for credential storage.

### Secret Fields

| Field | Type | Required | Notes |
|---|---|---|---|
| `host` | VARCHAR | Yes | Server hostname |
| `port` | INTEGER | Yes | TCP port (1-65535) |
| `database` | VARCHAR | Yes | Database name |
| `user` | VARCHAR | Yes | Login username |
| `password` | VARCHAR | Yes | Redacted in duckdb_secrets() |
| `use_encrypt` | BOOLEAN | No | TLS encryption (default: true) |
| `catalog` | BOOLEAN | No | Catalog integration (default: true). Set false for serverless/restricted databases |

### Usage

```sql
CREATE SECRET mssql_cred (
    TYPE mssql,
    HOST 'myserver.com',
    PORT 1433,
    DATABASE 'MyDB',
    USER 'sa',
    PASSWORD 'password',
    USE_ENCRYPT true
);

ATTACH '' AS mydb (TYPE mssql, SECRET mssql_cred);
```

## ATTACH/DETACH Lifecycle

### ATTACH Flow

```
ATTACH '' AS mydb (TYPE mssql, SECRET mssql_cred)
  │
  ├─ MSSQLAttach() in mssql_storage.cpp
  ├─ Extract SECRET parameter
  ├─ Parse connection info from secret (including catalog_enabled flag)
  ├─ Validate connection:
  │   ├─ TCP connection
  │   ├─ LOGIN7 authentication
  │   └─ TLS validation (SELECT 1 query if use_encrypt=true)
  ├─ Register context in MSSQLContextManager
  ├─ Create/get connection pool via MssqlPoolManager
  ├─ Create MSSQLCatalog (with catalog_enabled flag)
  └─ Initialize catalog (query database collation) - skipped if catalog_enabled=false
```

### DETACH Flow

```
DETACH DATABASE mydb
  │
  ├─ MSSQLCatalog::OnDetach()
  ├─ MssqlPoolManager::RemovePool(context_name)
  ├─ ConnectionPool::Shutdown()
  │   ├─ Set shutdown_flag_
  │   ├─ Join cleanup_thread_
  │   ├─ Close idle connections
  │   └─ Close active connections
  └─ Resources freed
```

## Diagnostic Functions

| Function | Purpose |
|---|---|
| `mssql_open(conn_string)` | Open standalone connection (URI, ADO.NET, or secret name) |
| `mssql_close(handle)` | Close connection by handle |
| `mssql_ping(handle)` | Test connection liveness via TDS ping |
| `mssql_pool_stats([context])` | View pool statistics (all pools or specific) |

`MSSQLConnectionHandleManager` is a thread-safe singleton that maps handles to diagnostic connections, separate from the pool system.

## Debug Logging

Controlled by `MSSQL_DEBUG` environment variable (integer level). Level >= 1 enables:
- Connection acquisition details
- Pool statistics before/after operations
- Transaction state transitions
- SPID logging

# Data Model: Pool Stats and Connection Validation

**Feature**: 023-pool-stats-validation
**Date**: 2026-01-28

## Entity Changes

### 1. PoolStatistics (Extended)

**File**: `src/include/tds/tds_connection_pool.hpp`

```cpp
struct PoolStatistics {
    // Existing fields
    size_t total_connections = 0;      // Total connections ever created
    size_t idle_connections = 0;       // Connections in idle queue
    size_t active_connections = 0;     // Connections currently borrowed
    size_t connections_created = 0;    // Lifetime created count
    size_t connections_closed = 0;     // Lifetime closed count
    size_t acquire_count = 0;          // Total acquire attempts
    size_t acquire_timeout_count = 0;  // Acquire timeout failures
    uint64_t acquire_wait_total_ms = 0; // Cumulative acquire wait time

    // NEW: Pinned connection tracking
    size_t pinned_connections = 0;     // Connections pinned to transactions
};
```

**Validation Rules**:
- `pinned_connections <= active_connections` (pinned is subset of active)
- `active_connections + idle_connections <= total_connections`

---

### 2. MSSQLPoolConfig (Extended)

**File**: `src/include/connection/mssql_settings.hpp`

```cpp
struct MSSQLPoolConfig {
    // Existing fields
    size_t connection_limit = tds::DEFAULT_CONNECTION_LIMIT;
    bool connection_cache = tds::DEFAULT_CONNECTION_CACHE;
    int connection_timeout = tds::DEFAULT_CONNECTION_TIMEOUT;
    int idle_timeout = tds::DEFAULT_IDLE_TIMEOUT;
    size_t min_connections = tds::DEFAULT_MIN_CONNECTIONS;
    int acquire_timeout = tds::DEFAULT_ACQUIRE_TIMEOUT;

    // NEW: Query timeout
    int query_timeout = tds::DEFAULT_QUERY_TIMEOUT;  // Default: 30 seconds
};
```

**Validation Rules**:
- `query_timeout >= 0` (0 = infinite timeout)
- No upper bound enforced (per clarification)

---

### 3. MSSQLConnectionInfo (Extended)

**File**: `src/include/mssql_storage.hpp`

```cpp
struct MSSQLConnectionInfo {
    // Existing fields
    string host;
    uint16_t port = 1433;
    string database;
    string user;
    string password;
    bool use_encrypt = true;    // CHANGED: Default from false to true
    bool connected = false;

    // NEW: Catalog mode
    bool catalog_enabled = true;  // Default: catalog integration enabled
};
```

**Validation Rules**:
- `catalog_enabled` can be `true` or `false`
- When `catalog_enabled = false`, DuckDB catalog queries must fail with clear error

---

### 4. MssqlPoolManager (Extended Interface)

**File**: `src/include/connection/mssql_pool_manager.hpp`

```cpp
class MssqlPoolManager {
public:
    // Existing methods...

    // NEW: Pinned connection tracking
    void IncrementPinnedCount(const std::string &context_name);
    void DecrementPinnedCount(const std::string &context_name);

private:
    // NEW: Per-pool pinned counts
    std::unordered_map<std::string, std::atomic<size_t>> pinned_counts_;
};
```

---

## New DuckDB Settings

### mssql_query_timeout

| Property | Value |
|----------|-------|
| Name | `mssql_query_timeout` |
| Type | `BIGINT` |
| Default | `30` |
| Min | `0` (infinite) |
| Max | None |
| Scope | Session |
| Description | Query execution timeout in seconds. 0 = no timeout. |

**Registration** (in `mssql_settings.cpp`):
```cpp
ExtensionOption("mssql_query_timeout",
                "Query execution timeout in seconds (0 = no timeout)",
                LogicalType::BIGINT,
                Value::BIGINT(DEFAULT_QUERY_TIMEOUT))
```

---

## Secret Field Changes

### New Field: catalog

**File**: `src/include/mssql_secret.hpp`

```cpp
constexpr const char *MSSQL_SECRET_CATALOG = "catalog";  // Optional, defaults to true
```

**Parsing** (in `mssql_secret.cpp`):
```cpp
// Read optional catalog field (defaults to true)
auto catalog_val = kv_secret.TryGetValue("catalog");
if (!catalog_val.IsNull()) {
    result->catalog_enabled = catalog_val.GetValue<bool>();
} else {
    result->catalog_enabled = true;
}
```

---

## Connection String Parameter Changes

### New Parameter: Catalog

| Parameter | Aliases | Values | Default |
|-----------|---------|--------|---------|
| `Catalog` | `catalog`, `use_catalog` | `yes`/`true`/`1` or `no`/`false`/`0` | `yes` |

**Parsing** (in `mssql_storage.cpp`):
```cpp
// Parse Catalog parameter
if (lower_key == "catalog" || lower_key == "use_catalog") {
    auto val = StringUtil::Lower(value);
    result["catalog"] = (val == "yes" || val == "true" || val == "1") ? "true" : "false";
}
```

---

## mssql_pool_stats() Output Schema

### Updated Columns

| Column | Type | Description |
|--------|------|-------------|
| `db` | VARCHAR | Database context name |
| `total_connections` | BIGINT | Total connections in pool |
| `idle_connections` | BIGINT | Connections available in idle queue |
| `active_connections` | BIGINT | Connections currently borrowed |
| `pinned_connections` | BIGINT | **NEW**: Connections pinned to transactions |
| `connections_created` | BIGINT | Lifetime connection creation count |
| `connections_closed` | BIGINT | Lifetime connection close count |
| `acquire_count` | BIGINT | Total acquire operation count |
| `acquire_timeout_count` | BIGINT | Acquire operations that timed out |

---

## State Transitions

### Connection Pinning Lifecycle

```
┌─────────────┐
│   IDLE      │  (in idle_connections queue)
└──────┬──────┘
       │ Acquire()
       ▼
┌─────────────┐
│   ACTIVE    │  (in active_connections map)
└──────┬──────┘
       │ SetPinnedConnection(conn)
       │ IncrementPinnedCount()
       ▼
┌─────────────┐
│   PINNED    │  (still in active_connections, pinned_count++)
└──────┬──────┘
       │ SetPinnedConnection(nullptr)
       │ DecrementPinnedCount()
       ▼
┌─────────────┐
│   ACTIVE    │  (still borrowed, not pinned)
└──────┬──────┘
       │ Release()
       ▼
┌─────────────┐
│   IDLE      │  (back in idle queue)
└─────────────┘
```

### Catalog Mode Decision

```
ATTACH with catalog_enabled=true:
  → Initialize catalog
  → Query collation
  → Lazy load schemas/tables on access

ATTACH with catalog_enabled=false:
  → Skip catalog initialization
  → mssql_scan() works
  → mssql_exec() works
  → SELECT * FROM ctx.schema.table → ERROR: "Catalog disabled"
```

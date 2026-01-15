# Contract: DuckDB Settings

**Branch**: `003-tds-connection-pooling`
**Date**: 2026-01-15

## Overview

DuckDB configuration variables for controlling mssql connection pool behavior. All settings are GLOBAL scope and affect all attached mssql databases.

---

## mssql_connection_limit

Maximum number of connections per attached database context.

| Property | Value |
|----------|-------|
| Type | BIGINT |
| Default | 64 |
| Scope | GLOBAL |
| Range | ≥ 1 |

### Usage

```sql
SET mssql_connection_limit = 10;
SELECT current_setting('mssql_connection_limit');
```

### Behavior

- Applies to newly created connections
- Existing connections are not affected
- When limit reached, acquire() waits or fails

---

## mssql_connection_cache

Enable or disable connection pooling/reuse.

| Property | Value |
|----------|-------|
| Type | BOOLEAN |
| Default | true |
| Scope | GLOBAL |

### Usage

```sql
SET mssql_connection_cache = false;  -- Disable pooling
SET mssql_connection_cache = true;   -- Enable pooling (default)
```

### Behavior

- `true`: Connections returned to pool for reuse
- `false`: Connections closed immediately after use

---

## mssql_connection_timeout

TCP connection timeout in seconds.

| Property | Value |
|----------|-------|
| Type | BIGINT |
| Default | 30 |
| Scope | GLOBAL |
| Range | ≥ 0 (0 = no timeout, not recommended) |

### Usage

```sql
SET mssql_connection_timeout = 60;  -- 60 second timeout
```

### Behavior

- Applied when establishing new TCP connection
- Includes TCP connect + PRELOGIN + LOGIN7 handshake
- On timeout: connection attempt fails with IOException

---

## mssql_idle_timeout

How long idle connections are kept in pool (seconds).

| Property | Value |
|----------|-------|
| Type | BIGINT |
| Default | 300 (5 minutes) |
| Scope | GLOBAL |
| Range | ≥ 0 (0 = no idle timeout) |

### Usage

```sql
SET mssql_idle_timeout = 600;  -- Keep idle connections for 10 minutes
SET mssql_idle_timeout = 0;    -- Never close idle connections
```

### Behavior

- Background thread checks idle connections every 1 second
- Connections idle longer than timeout are closed
- `min_connections` are always preserved regardless of timeout

---

## mssql_min_connections

Minimum connections to maintain per context.

| Property | Value |
|----------|-------|
| Type | BIGINT |
| Default | 0 |
| Scope | GLOBAL |
| Range | ≥ 0 |

### Usage

```sql
SET mssql_min_connections = 2;  -- Always keep 2 connections ready
```

### Behavior

- Pool maintains at least this many connections
- Protected from idle timeout cleanup
- Connections created lazily (not pre-populated on attach)

---

## mssql_acquire_timeout

How long to wait for a connection when pool is exhausted (seconds).

| Property | Value |
|----------|-------|
| Type | BIGINT |
| Default | 30 |
| Scope | GLOBAL |
| Range | ≥ 0 (0 = fail immediately) |

### Usage

```sql
SET mssql_acquire_timeout = 60;  -- Wait up to 60 seconds
SET mssql_acquire_timeout = 0;   -- Fail immediately if no connection available
```

### Behavior

- When pool at `connection_limit` and all active
- Caller waits up to timeout for connection to be released
- On timeout: InvalidInputException with "Connection pool exhausted"

---

## Registration Implementation

```cpp
void RegisterMSSQLSettings(ExtensionLoader &loader) {
    auto &config = DBConfig::GetConfig(loader.GetDatabase());

    // mssql_connection_limit
    config.AddExtensionOption(
        "mssql_connection_limit",
        "Maximum connections per attached mssql database",
        LogicalType::BIGINT,
        Value::BIGINT(64),
        ValidatePositive,  // Custom validator: value >= 1
        SetScope::GLOBAL
    );

    // mssql_connection_cache
    config.AddExtensionOption(
        "mssql_connection_cache",
        "Enable connection pooling and reuse",
        LogicalType::BOOLEAN,
        Value::BOOLEAN(true),
        nullptr,
        SetScope::GLOBAL
    );

    // mssql_connection_timeout
    config.AddExtensionOption(
        "mssql_connection_timeout",
        "TCP connection timeout in seconds",
        LogicalType::BIGINT,
        Value::BIGINT(30),
        ValidateNonNegative,
        SetScope::GLOBAL
    );

    // mssql_idle_timeout
    config.AddExtensionOption(
        "mssql_idle_timeout",
        "Idle connection timeout in seconds (0 = no timeout)",
        LogicalType::BIGINT,
        Value::BIGINT(300),
        ValidateNonNegative,
        SetScope::GLOBAL
    );

    // mssql_min_connections
    config.AddExtensionOption(
        "mssql_min_connections",
        "Minimum connections to maintain per context",
        LogicalType::BIGINT,
        Value::BIGINT(0),
        ValidateNonNegative,
        SetScope::GLOBAL
    );

    // mssql_acquire_timeout
    config.AddExtensionOption(
        "mssql_acquire_timeout",
        "Connection acquire timeout in seconds (0 = fail immediately)",
        LogicalType::BIGINT,
        Value::BIGINT(30),
        ValidateNonNegative,
        SetScope::GLOBAL
    );
}

// Validators
static void ValidatePositive(ClientContext &context, SetScope scope, Value &parameter) {
    auto val = parameter.GetValue<int64_t>();
    if (val < 1) {
        throw InvalidInputException("Value must be >= 1");
    }
}

static void ValidateNonNegative(ClientContext &context, SetScope scope, Value &parameter) {
    auto val = parameter.GetValue<int64_t>();
    if (val < 0) {
        throw InvalidInputException("Value must be >= 0");
    }
}
```

---

## Reading Settings at Runtime

```cpp
PoolConfig LoadPoolConfig(ClientContext &context) {
    PoolConfig config;
    Value val;

    if (context.TryGetCurrentSetting("mssql_connection_limit", val)) {
        config.connection_limit = val.GetValue<int64_t>();
    }

    if (context.TryGetCurrentSetting("mssql_connection_cache", val)) {
        config.connection_cache = val.GetValue<bool>();
    }

    if (context.TryGetCurrentSetting("mssql_connection_timeout", val)) {
        config.connection_timeout = std::chrono::seconds(val.GetValue<int64_t>());
    }

    if (context.TryGetCurrentSetting("mssql_idle_timeout", val)) {
        config.idle_timeout = std::chrono::seconds(val.GetValue<int64_t>());
    }

    if (context.TryGetCurrentSetting("mssql_min_connections", val)) {
        config.min_connections = val.GetValue<int64_t>();
    }

    if (context.TryGetCurrentSetting("mssql_acquire_timeout", val)) {
        config.acquire_timeout = std::chrono::seconds(val.GetValue<int64_t>());
    }

    return config;
}
```

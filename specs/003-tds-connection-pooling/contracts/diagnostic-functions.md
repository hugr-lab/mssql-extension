# Contract: Diagnostic Functions

**Branch**: `003-tds-connection-pooling`
**Date**: 2026-01-15

## Overview

Diagnostic functions for testing and debugging TDS connections. Scalar functions are used for single-value operations (open, close, ping), while table functions are used for multi-column results (pool_stats).

---

## mssql_open

Opens a TDS connection using credentials from a named secret.

**Type**: Scalar function

**Signature**:

```sql
mssql_open(secret_name VARCHAR) → BIGINT
```

**Parameters**:

| Name        | Type    | Required | Description                                 |
|-------------|---------|----------|---------------------------------------------|
| secret_name | VARCHAR | Yes      | Name of mssql secret containing credentials |

**Returns**: `BIGINT` - Connection handle for use with other diagnostic functions

**Behavior**:

1. Resolves secret by name
2. Establishes TCP connection to host:port
3. Sends PRELOGIN packet, receives response
4. Sends LOGIN7 packet with SQL Server authentication
5. On success: returns handle, connection state = `idle`
6. On failure: throws exception with error message

**Errors**:

| Condition             | Exception                                |
|-----------------------|------------------------------------------|
| Secret not found      | InvalidInputException                    |
| Connection timeout    | IOException                              |
| Authentication failed | InvalidInputException (with server message) |
| Host unreachable      | IOException                              |

**Example**:

```sql
-- Open a connection
SELECT mssql_open('my_secret');
-- Returns: 1

-- Use in variable assignment
SELECT mssql_open('my_secret') AS handle;

-- Error case
SELECT mssql_open('nonexistent');
-- Error: Secret 'nonexistent' not found
```

---

## mssql_close

Closes a TDS connection and releases resources.

**Type**: Scalar function

**Signature**:

```sql
mssql_close(handle BIGINT) → BOOLEAN
```

**Parameters**:

| Name   | Type   | Required | Description                      |
|--------|--------|----------|----------------------------------|
| handle | BIGINT | Yes      | Connection handle from mssql_open |

**Returns**: `BOOLEAN` - true if closed successfully

**Behavior**:

1. Looks up connection by handle
2. If connection in `executing` state: sends ATTENTION, waits for ACK
3. Closes TCP socket
4. Invalidates handle
5. Returns true

Idempotent: closing already-closed handle returns true without error.

**Errors**: None (idempotent operation).

**Example**:

```sql
-- Close a connection
SELECT mssql_close(1);
-- Returns: true

-- Closing again is safe
SELECT mssql_close(1);
-- Returns: true

-- Chain with open
SELECT mssql_close(mssql_open('my_secret'));
-- Returns: true
```

---

## mssql_ping

Tests if a connection is alive by sending a minimal TDS packet.

**Type**: Scalar function

**Signature**:

```sql
mssql_ping(handle BIGINT) → BOOLEAN
```

**Parameters**:

| Name   | Type   | Required | Description                      |
|--------|--------|----------|----------------------------------|
| handle | BIGINT | Yes      | Connection handle from mssql_open |

**Returns**: `BOOLEAN` - true if connection responds, false otherwise

**Behavior**:

1. Looks up connection by handle
2. If state is `disconnected`: returns false
3. Sends empty SQLBATCH packet (TDS ping)
4. Waits for DONE token response (5 second timeout)
5. On response: returns true
6. On timeout/error: returns false, marks connection as `disconnected`

**Errors**:

| Condition      | Exception               |
|----------------|-------------------------|
| Invalid handle | InvalidInputException   |

**Example**:

```sql
-- Check if connection is alive
SELECT mssql_ping(1);
-- Returns: true

-- After server disconnect
SELECT mssql_ping(1);
-- Returns: false

-- Conditional usage
SELECT CASE WHEN mssql_ping(1) THEN 'alive' ELSE 'dead' END;
```

---

## mssql_pool_stats

Returns statistics for a connection pool associated with an attached database.

**Type**: Table function

**Signature**:

```sql
mssql_pool_stats(context_name VARCHAR) → TABLE
```

**Parameters**:

| Name         | Type    | Required | Description                            |
|--------------|---------|----------|----------------------------------------|
| context_name | VARCHAR | Yes      | Name of attached mssql database context |

**Result Columns**:

| Column                | Type   | Description                            |
|-----------------------|--------|----------------------------------------|
| total_connections     | BIGINT | Current pool size (idle + active)      |
| idle_connections      | BIGINT | Connections available for use          |
| active_connections    | BIGINT | Connections currently in use           |
| connections_created   | BIGINT | Total connections created since attach |
| connections_closed    | BIGINT | Total connections closed since attach  |
| acquire_count         | BIGINT | Total acquire() calls                  |
| acquire_timeout_count | BIGINT | Acquires that timed out                |

**Behavior**:

1. Looks up pool by context name
2. Returns current statistics snapshot
3. Statistics are thread-safe (protected by mutex)

**Errors**:

| Condition         | Exception               |
|-------------------|-------------------------|
| Context not found | InvalidInputException   |

**Example**:

```sql
-- After attaching and running some queries
SELECT * FROM mssql_pool_stats('mydb');

-- Result:
-- total_connections: 3
-- idle_connections: 2
-- active_connections: 1
-- connections_created: 5
-- connections_closed: 2
-- acquire_count: 100
-- acquire_timeout_count: 0
```

---

## DuckDB Function Registration

```cpp
// In mssql_extension.cpp Load()

// Scalar functions
ScalarFunctionSet open_func("mssql_open");
open_func.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::BIGINT, MssqlOpenFunction));
loader.RegisterFunction(open_func);

ScalarFunctionSet close_func("mssql_close");
close_func.AddFunction(ScalarFunction({LogicalType::BIGINT}, LogicalType::BOOLEAN, MssqlCloseFunction));
loader.RegisterFunction(close_func);

ScalarFunctionSet ping_func("mssql_ping");
ping_func.AddFunction(ScalarFunction({LogicalType::BIGINT}, LogicalType::BOOLEAN, MssqlPingFunction));
loader.RegisterFunction(ping_func);

// Table function (multi-column result)
CreateTableFunctionInfo stats_info(MssqlPoolStatsFunction::GetFunction());
loader.RegisterFunction(stats_info);
```

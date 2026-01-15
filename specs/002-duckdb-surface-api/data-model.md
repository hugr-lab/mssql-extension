# Data Model: DuckDB Surface API

**Feature**: 002-duckdb-surface-api
**Date**: 2026-01-15

## Overview

This document defines the internal data structures for the MSSQL extension's public interface. These are C++ classes and structures, not database tables.

---

## Entities

### 1. MSSQLSecret (via KeyValueSecret)

Stores SQL Server connection credentials using DuckDB's built-in secret storage.

**Storage**: DuckDB SecretManager (KeyValueSecret)

| Field | Type | Constraints | Description |
|-------|------|-------------|-------------|
| host | VARCHAR | Required, non-empty | SQL Server hostname or IP |
| port | INTEGER | Required, 1-65535 | TCP port number |
| database | VARCHAR | Required, non-empty | Target database name |
| user | VARCHAR | Required, non-empty | Authentication username |
| password | VARCHAR | Required, redacted | Authentication password |

**Lifecycle**:
- Created via `CREATE SECRET name (TYPE mssql, ...)`
- Immutable after creation (DuckDB enforced)
- Deleted via `DROP SECRET name`
- Password redacted in `duckdb_secrets()` output

**Validation Rules**:
- All fields must be provided at creation time
- Port must be valid TCP port (1-65535)
- No field may be empty string
- Name must be unique across all secrets

---

### 2. MSSQLConnectionInfo

Internal representation of connection parameters extracted from a secret.

**Storage**: In-memory (MSSQLContextManager)

| Field | Type | Description |
|-------|------|-------------|
| host | string | Resolved from secret |
| port | uint16_t | Resolved from secret |
| database | string | Resolved from secret |
| user | string | Resolved from secret |
| password | string | Resolved from secret (sensitive) |
| connected | bool | Whether network connection is established |

**Lifecycle**:
- Created during ATTACH (extracted from referenced secret)
- Updated when lazy connection is established
- Destroyed during DETACH

**Notes**:
- Password stored in memory per constitution (delegated to DuckDB)
- `connected` flag tracks lazy initialization state

---

### 3. MSSQLContext

Represents an attached MSSQL database context.

**Storage**: MSSQLContextManager (keyed by context name)

| Field | Type | Description |
|-------|------|-------------|
| name | string | Context name (from ATTACH ... AS name) |
| secret_name | string | Name of associated secret |
| connection_info | shared_ptr<MSSQLConnectionInfo> | Resolved connection parameters |
| attached_db | optional_ptr<AttachedDatabase> | Reference to DuckDB attached database |

**Lifecycle**:
- Created during `ATTACH '' AS {name} TYPE mssql (SECRET {secret})`
- Registered in MSSQLContextManager
- Unregistered and destroyed during `DETACH {name}`

**State Transitions**:
```
[Created] --> [Attached] --> [Connected] --> [Detached]
                  |                              ^
                  +------------------------------+
                  (detach before any query)
```

---

### 4. ExecuteResult

Return value structure for `mssql_execute` function.

**Storage**: Transient (query result)

| Field | Type | Description |
|-------|------|-------------|
| success | bool | Whether execution succeeded |
| affected_rows | int64_t | Number of rows affected (or -1 if N/A) |
| message | string | Status message or error description |

**Usage**:
- Single row returned per `mssql_execute` call
- `success=false` includes error message from SQL Server
- `affected_rows=-1` for statements without row count (e.g., DDL)

---

### 5. ScanBindData

Bind-time data for `mssql_scan` table function.

**Storage**: FunctionData (query execution lifetime)

| Field | Type | Description |
|-------|------|-------------|
| context_name | string | Target MSSQL context |
| query | string | SQL query to execute |
| return_types | vector<LogicalType> | Output column types |
| column_names | vector<string> | Output column names |

**Lifecycle**:
- Created during query binding
- Immutable during execution
- Destroyed after query completes

---

### 6. ScanGlobalState

Global execution state for `mssql_scan` (shared across threads).

**Storage**: GlobalTableFunctionState (query execution lifetime)

| Field | Type | Description |
|-------|------|-------------|
| context_name | string | Target context (copied from bind) |
| query | string | SQL query (copied from bind) |
| total_rows | idx_t | Total rows to return (stub: 3) |
| rows_returned | atomic<idx_t> | Rows already returned |

**Thread Safety**: `rows_returned` is atomic for potential future parallel execution.

---

### 7. ScanLocalState

Per-thread execution state for `mssql_scan`.

**Storage**: LocalTableFunctionState (query execution lifetime)

| Field | Type | Description |
|-------|------|-------------|
| current_row | idx_t | Next row index to produce |

**Notes**: For stub implementation, single-threaded execution only.

---

## Relationships

```
┌─────────────────┐     references     ┌──────────────────┐
│  MSSQLContext   │───────────────────▶│   MSSQLSecret    │
│                 │                    │  (KeyValueSecret) │
└────────┬────────┘                    └──────────────────┘
         │
         │ contains
         ▼
┌─────────────────────┐
│ MSSQLConnectionInfo │
└─────────────────────┘

┌─────────────────┐     lookup     ┌─────────────────┐
│  mssql_scan     │───────────────▶│  MSSQLContext   │
│  mssql_execute  │                │                 │
└─────────────────┘                └─────────────────┘
```

---

## Context Manager

Central registry for attached MSSQL databases.

```cpp
class MSSQLContextManager {
    // Singleton per DatabaseInstance
    static MSSQLContextManager& Get(DatabaseInstance &db);

    // Context operations
    void RegisterContext(const string &name, shared_ptr<MSSQLContext> ctx);
    void UnregisterContext(const string &name);
    shared_ptr<MSSQLContext> GetContext(const string &name);
    bool HasContext(const string &name);
    vector<string> ListContexts();

private:
    mutex lock;
    unordered_map<string, shared_ptr<MSSQLContext>> contexts;
};
```

**Thread Safety**: All operations protected by mutex.

---

## Validation Summary

| Entity | Validation Point | Rules |
|--------|------------------|-------|
| MSSQLSecret | Creation | All fields required, port 1-65535, no empty strings |
| MSSQLContext | Attach | Secret must exist, name must be unique |
| mssql_execute | Bind | Context must exist |
| mssql_scan | Bind | Context must exist |

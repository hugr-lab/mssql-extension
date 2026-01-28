# Data Model: CTAS for MSSQL

**Branch**: `022-mssql-ctas` | **Date**: 2026-01-28

## Entity Overview

CTAS operates on three primary entities: the source query result schema, the target table definition, and the execution state.

---

## Entities

### CTASTarget

Represents the target table to be created in SQL Server.

```cpp
struct CTASTarget {
    string catalog_name;      // Attached database name (e.g., "mssql")
    string schema_name;       // SQL Server schema (e.g., "dbo")
    string table_name;        // Table name (e.g., "new_orders")
    bool or_replace;          // CREATE OR REPLACE TABLE mode
    OnCreateConflict on_conflict;  // Error handling mode (from DuckDB)
};
```

**Validation Rules**:
- `schema_name` must exist in SQL Server (checked before DDL)
- `table_name` must not exist unless `or_replace = true`
- All names must be valid SQL Server identifiers (bracket-escaped in DDL)

---

### CTASColumnDef

Represents a column definition derived from the source query's output schema.

```cpp
struct CTASColumnDef {
    string name;              // Column name (from SELECT alias or generated)
    LogicalType duckdb_type;  // DuckDB type from source query
    string mssql_type;        // Translated SQL Server type
    bool nullable;            // True unless source column is NOT NULL
};
```

**Type Mapping** (from spec):

| DuckDB Type    | SQL Server Type   | Notes                               |
|----------------|-------------------|-------------------------------------|
| BOOLEAN        | bit               |                                     |
| TINYINT        | tinyint           |                                     |
| SMALLINT       | smallint          |                                     |
| INTEGER        | int               |                                     |
| BIGINT         | bigint            |                                     |
| FLOAT          | real              |                                     |
| DOUBLE         | float             |                                     |
| DECIMAL(p,s)   | decimal(p,s)      | Clamp p,s to max 38                 |
| VARCHAR        | nvarchar(max)     | Or varchar(max) via setting         |
| UUID           | uniqueidentifier  |                                     |
| BLOB           | varbinary(max)    |                                     |
| DATE           | date              |                                     |
| TIME           | time(7)           |                                     |
| TIMESTAMP      | datetime2(7)      |                                     |
| TIMESTAMP_TZ   | datetimeoffset(7) |                                     |

**Unsupported Types** (fail with error):
- HUGEINT, UHUGEINT
- UTINYINT, USMALLINT, UINTEGER, UBIGINT
- INTERVAL
- LIST, STRUCT, MAP, UNION, ARRAY

---

### CTASConfig

Configuration for CTAS execution, derived from DuckDB settings.

```cpp
struct CTASConfig {
    // From mssql_ctas_text_type setting
    enum class TextType { NVARCHAR, VARCHAR };
    TextType text_type = TextType::NVARCHAR;

    // From mssql_ctas_drop_on_failure setting
    bool drop_on_failure = false;

    // Inherited from INSERT settings
    idx_t batch_size = 1000;           // mssql_insert_batch_size
    idx_t max_sql_bytes = 8 * 1024 * 1024;  // mssql_insert_max_sql_bytes

    // Loaded from context
    static CTASConfig Load(ClientContext &context);
};
```

---

### CTASExecutionState

Global sink state for CTAS physical operator execution.

```cpp
struct CTASExecutionState {
    // Target info
    CTASTarget target;
    vector<CTASColumnDef> columns;
    CTASConfig config;

    // Execution state
    enum class Phase { PENDING, DDL_EXECUTING, DDL_DONE, INSERT_EXECUTING, COMPLETE, FAILED };
    Phase phase = Phase::PENDING;

    // DDL state
    string ddl_sql;           // Generated CREATE TABLE statement
    idx_t ddl_bytes = 0;      // Size of DDL statement
    int64_t ddl_time_ms = 0;  // Time to execute DDL

    // INSERT state (wraps existing executor)
    unique_ptr<MSSQLInsertExecutor> insert_executor;
    idx_t rows_produced = 0;  // Rows received from source
    idx_t rows_inserted = 0;  // Rows successfully inserted
    int64_t insert_time_ms = 0;

    // Connection (pinned for duration)
    shared_ptr<tds::TdsConnection> connection;

    // Error tracking
    string error_message;
    string cleanup_error;     // Secondary error from DROP on failure
};
```

**State Transitions**:
```
PENDING → DDL_EXECUTING → DDL_DONE → INSERT_EXECUTING → COMPLETE
                ↓              ↓              ↓
              FAILED        FAILED         FAILED
                              ↓
                    (if drop_on_failure: attempt DROP)
```

---

### CTASObservability

Debug output structure for logging.

```cpp
struct CTASObservability {
    // Phase identification
    string target_table;      // "[schema].[table]"
    bool or_replace;

    // DDL phase metrics
    idx_t ddl_bytes;
    int64_t ddl_time_ms;

    // INSERT phase metrics
    idx_t rows_produced;
    idx_t rows_inserted;
    idx_t batches_executed;
    int64_t insert_time_ms;

    // Outcome
    bool success;
    string failure_phase;     // "DDL" or "INSERT" or "CLEANUP"
    string error_message;

    // Emit to debug log
    void Log(int level) const;
};
```

---

## Relationships

```
CTASTarget 1──────* CTASColumnDef
    │                    │
    │                    │ derived from
    │                    ▼
    │              DuckDB LogicalType
    │                    │
    │                    │ mapped via
    │                    ▼
    │              MSSQLDDLTranslator
    │
    │ configured by
    ▼
CTASConfig ◄──────── DuckDB Settings
    │
    │ drives
    ▼
CTASExecutionState
    │
    │ wraps
    ▼
MSSQLInsertExecutor (existing)
```

---

## DDL Generation

The CREATE TABLE statement is generated deterministically from `CTASColumnDef` list:

```sql
-- Standard CTAS
CREATE TABLE [schema].[table] (
    [column1] type1 NULL,
    [column2] type2 NOT NULL,
    ...
);

-- OR REPLACE (precedes CREATE)
DROP TABLE [schema].[table];
CREATE TABLE [schema].[table] (...);
```

**Escaping Rules**:
- Identifiers: `]` → `]]`, wrapped in `[...]`
- No additional constraints (no PK, no indexes)
- Nullability only constraint preserved

---

## Settings Schema

| Setting | Type | Default | Validation |
|---------|------|---------|------------|
| `mssql_ctas_drop_on_failure` | BOOLEAN | `false` | None |
| `mssql_ctas_text_type` | VARCHAR | `"NVARCHAR"` | Must be "NVARCHAR" or "VARCHAR" (case-insensitive) |

Settings are loaded once at CTAS planning time and immutable during execution.

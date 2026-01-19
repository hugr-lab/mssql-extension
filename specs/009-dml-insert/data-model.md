# Data Model: High-Performance DML INSERT

**Feature**: 009-dml-insert
**Date**: 2026-01-19

## Entities

### MSSQLInsertConfig

Configuration for insert operations, derived from DuckDB settings.

```cpp
struct MSSQLInsertConfig {
    size_t batch_size;              // mssql_insert_batch_size (default: 2000)
    size_t max_rows_per_statement;  // mssql_insert_max_rows_per_statement (default: 2000)
    size_t max_sql_bytes;           // mssql_insert_max_sql_bytes (default: 8MB)
    bool use_returning_output;      // mssql_insert_use_returning_output (default: true)

    // Derived
    size_t EffectiveRowsPerStatement() const {
        return std::min(batch_size, max_rows_per_statement);
    }
};
```

**Validation Rules**:
- `batch_size` must be >= 1
- `max_rows_per_statement` must be >= 1
- `max_sql_bytes` must be >= 1024 (minimum 1KB)

### MSSQLInsertTarget

Metadata about the target SQL Server table for INSERT.

```cpp
struct MSSQLInsertTarget {
    std::string catalog_name;       // Database/catalog name
    std::string schema_name;        // Schema name (e.g., "dbo")
    std::string table_name;         // Table name

    std::vector<MSSQLInsertColumn> columns;     // All table columns
    std::vector<size_t> insert_column_indices;  // Columns being inserted (excludes identity if not provided)
    std::vector<size_t> returning_column_indices; // Columns for OUTPUT INSERTED

    bool has_identity_column;
    size_t identity_column_index;   // Valid if has_identity_column
};
```

### MSSQLInsertColumn

Column metadata for insert operations.

```cpp
struct MSSQLInsertColumn {
    std::string name;               // Column name
    LogicalType duckdb_type;        // DuckDB logical type
    std::string mssql_type;         // SQL Server type name
    bool is_identity;               // IDENTITY column flag
    bool is_nullable;               // NULL allowed
    bool has_default;               // Has DEFAULT constraint
    std::string collation;          // Collation name (for text types)

    // For DECIMAL types
    uint8_t precision;
    uint8_t scale;
};
```

**Relationships**:

- `MSSQLInsertTarget` contains multiple `MSSQLInsertColumn` entries
- `insert_column_indices` references positions in `columns` vector

### MSSQLInsertBatch

A batch of rows to be inserted as a single SQL statement.

```cpp
struct MSSQLInsertBatch {
    size_t row_offset_start;        // First row index in overall operation
    size_t row_offset_end;          // Last row index (exclusive)
    size_t row_count;               // Number of rows in batch
    std::string sql_statement;      // Generated SQL
    size_t sql_bytes;               // Size of sql_statement

    // State
    enum class State {
        BUILDING,       // Accumulating rows
        READY,          // SQL generated, ready to execute
        EXECUTING,      // Sent to SQL Server
        COMPLETED,      // Successfully executed
        FAILED          // Execution failed
    };
    State state;
};
```

**State Transitions**:
```
BUILDING → READY → EXECUTING → COMPLETED
                            ↘ FAILED
```

### MSSQLInsertResult

Result of a single batch execution.

```cpp
struct MSSQLInsertResult {
    bool success;
    size_t rows_affected;           // From TDS DONE token

    // For RETURNING mode
    std::vector<DataChunk> returned_chunks;

    // For error cases
    MSSQLInsertError error;
};
```

### MSSQLInsertError

Error context when an insert batch fails.

```cpp
struct MSSQLInsertError {
    size_t statement_index;         // Batch number (0-based)
    size_t row_offset_start;        // First row in failed batch
    size_t row_offset_end;          // Last row in failed batch
    int32_t sql_error_number;       // SQL Server error number
    std::string sql_error_message;  // SQL Server error text
    std::string sql_state;          // SQLSTATE code if available

    std::string FormatMessage() const {
        return StringUtil::Format(
            "INSERT failed at statement %d (rows %d-%d): [%d] %s",
            statement_index, row_offset_start, row_offset_end - 1,
            sql_error_number, sql_error_message
        );
    }
};
```

### MSSQLInsertOperationState

Overall state of an insert operation spanning multiple batches.

```cpp
struct MSSQLInsertOperationState {
    MSSQLInsertTarget target;
    MSSQLInsertConfig config;

    // Progress tracking
    size_t total_rows_inserted;
    size_t total_batches_executed;
    size_t current_row_offset;

    // Timing (optional observability)
    std::chrono::steady_clock::time_point start_time;
    std::chrono::microseconds total_execution_time;

    // Mode
    bool returning_enabled;
    std::vector<idx_t> returning_column_ids;  // DuckDB column IDs for RETURNING
};
```

## Type Mapping Reference

### DuckDB → T-SQL Literal Encoding

| DuckDB Type | T-SQL Literal | Example |
|-------------|---------------|---------|
| BOOLEAN | `0` / `1` | `1` |
| TINYINT | Integer | `42` |
| SMALLINT | Integer | `-1000` |
| INTEGER | Integer | `123456` |
| BIGINT | Integer | `9223372036854775807` |
| UTINYINT | Integer | `255` |
| USMALLINT | Integer | `65535` |
| UINTEGER | Integer | `4294967295` |
| UBIGINT | CAST | `CAST(18446744073709551615 AS DECIMAL(20,0))` |
| HUGEINT | CAST | `CAST(...  AS DECIMAL(38,0))` |
| FLOAT | Decimal/E-notation | `3.14`, `1.23E+10` |
| DOUBLE | Decimal/E-notation | `3.141592653589793` |
| DECIMAL(p,s) | Decimal | `123.45` |
| VARCHAR | N-string | `N'Hello ''World'''` |
| BLOB | Hex | `0x48454C4C4F` |
| UUID | String | `'550e8400-e29b-41d4-a716-446655440000'` |
| DATE | ISO date | `'2026-01-19'` |
| TIME | ISO time | `'14:30:00.1234567'` |
| TIMESTAMP | ISO datetime | `CAST('2026-01-19T14:30:00.1234567' AS DATETIME2(7))` |
| TIMESTAMP_TZ | ISO datetimeoffset | `CAST('2026-01-19T14:30:00.1234567+05:30' AS DATETIMEOFFSET(7))` |
| NULL (any) | NULL | `NULL` |

### Special Value Handling

| Value | Handling |
|-------|----------|
| NaN (float/double) | ERROR: "NaN values not supported for SQL Server INSERT" |
| +Infinity | ERROR: "Infinity values not supported for SQL Server INSERT" |
| -Infinity | ERROR: "Infinity values not supported for SQL Server INSERT" |
| Empty string | `N''` |
| Empty BLOB | `0x` |
| Very large DECIMAL | ERROR if exceeds SQL Server DECIMAL(38,s) limits |

## SQL Generation Templates

### Bulk Insert (Mode A)

```sql
INSERT INTO [schema].[table] ([col1], [col2], [col3])
VALUES
  (<lit1_1>, <lit1_2>, <lit1_3>),
  (<lit2_1>, <lit2_2>, <lit2_3>),
  (<lit3_1>, <lit3_2>, <lit3_3>);
```

### Insert with RETURNING (Mode B)

```sql
INSERT INTO [schema].[table] ([col1], [col2])
OUTPUT INSERTED.[col1], INSERTED.[col2], INSERTED.[identity_col]
VALUES
  (<lit1_1>, <lit1_2>),
  (<lit2_1>, <lit2_2>);
```

### Identifier Escaping

```
name           → [name]
na]me          → [na]]me]
[name]         → [[name]]]
schema.table   → [schema].[table]
```

## Class Relationships

```
┌─────────────────────┐
│   MSSQLCatalog      │
│   (PlanInsert)      │
└─────────┬───────────┘
          │ creates
          ▼
┌─────────────────────┐
│ MSSQLInsertExecutor │──────────┐
│                     │          │ uses
└─────────┬───────────┘          ▼
          │              ┌───────────────────┐
          │              │ MSSQLPoolManager  │
          │              │ (acquire/release) │
          │              └───────────────────┘
          │ uses
          ▼
┌─────────────────────┐      ┌─────────────────────┐
│  MSSQLBatchBuilder  │◄────►│MSSQLValueSerializer │
│  (row accumulation) │      │  (type encoding)    │
└─────────┬───────────┘      └─────────────────────┘
          │ produces
          ▼
┌─────────────────────┐
│MSSQLInsertStatement │
│  (SQL generation)   │
└─────────┬───────────┘
          │ executed via
          ▼
┌─────────────────────┐
│   TdsConnection     │
│   (ExecuteBatch)    │
└─────────┬───────────┘
          │ results parsed by
          ▼
┌─────────────────────┐
│MSSQLReturningParser │
│ (OUTPUT INSERTED)   │
└─────────────────────┘
```

## Validation Rules Summary

1. **Column Mapping**: All columns in INSERT must exist in target table
2. **Type Compatibility**: DuckDB types must be convertible to target column types
3. **Identity Columns**: Cannot provide explicit values for identity columns (MVP)
4. **NOT NULL**: Non-nullable columns without defaults must have values provided
5. **Size Limits**: Single row cannot exceed `max_sql_bytes` limit
6. **Batch Limits**: Batch cannot exceed `effective_rows_per_statement`
7. **Float Values**: NaN and Infinity are rejected
8. **Decimal Precision**: Must fit within SQL Server DECIMAL(38,s) limits

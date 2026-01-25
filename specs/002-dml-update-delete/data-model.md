# Data Model: DML UPDATE/DELETE

**Feature**: 002-dml-update-delete
**Date**: 2026-01-25

## Overview

This document defines the data structures used in the UPDATE/DELETE implementation. Entities follow patterns established in the INSERT implementation (Spec 009).

---

## Entities

### 1. MSSQLDMLConfig

Shared configuration for UPDATE/DELETE operations (extends INSERT config pattern).

```cpp
struct MSSQLDMLConfig {
    // Batch control
    idx_t batch_size;           // Default: 500, max rows per SQL statement
    idx_t max_parameters;       // Default: 2000, SQL Server limit safety

    // Execution mode
    bool use_prepared;          // Default: true, use prepared statements

    // Computed at execution time based on table structure
    idx_t effective_batch_size; // min(batch_size, max_parameters / params_per_row)
};
```

**Settings mapping:**
| Setting | Config Field |
|---------|--------------|
| `mssql_dml_batch_size` | `batch_size` |
| `mssql_dml_max_parameters` | `max_parameters` |
| `mssql_dml_use_prepared` | `use_prepared` |

---

### 2. MSSQLUpdateTarget

Metadata describing the target table for UPDATE operations.

```cpp
struct MSSQLUpdateTarget {
    // Table identity
    string catalog_name;        // DuckDB catalog (MSSQL attachment name)
    string schema_name;         // SQL Server schema
    string table_name;          // SQL Server table name

    // Primary key info (for rowid → PK mapping)
    PrimaryKeyInfo pk_info;     // From MSSQLTableEntry

    // Columns being updated
    vector<MSSQLUpdateColumn> update_columns;

    // All table columns (for type information)
    vector<MSSQLColumnInfo> table_columns;

    // Helper methods
    string GetFullyQualifiedName() const;  // [schema].[table]
    idx_t GetParamsPerRow() const;         // pk_cols + update_cols
    bool IsScalarPK() const;
    bool IsCompositePK() const;
};
```

---

### 3. MSSQLUpdateColumn

Metadata for a single column in an UPDATE SET clause.

```cpp
struct MSSQLUpdateColumn {
    string name;                // Column name
    idx_t column_index;         // Index in table_columns
    LogicalType duckdb_type;    // DuckDB type for value serialization
    string mssql_type;          // SQL Server type name
    string collation;           // For string columns
    uint8_t precision;          // For decimal/numeric
    uint8_t scale;              // For decimal/numeric
    bool is_nullable;           // Allow NULL values

    // Position in input DataChunk (after rowid)
    idx_t chunk_index;          // Index in Sink() input chunk
};
```

---

### 4. MSSQLDeleteTarget

Metadata describing the target table for DELETE operations.

```cpp
struct MSSQLDeleteTarget {
    // Table identity
    string catalog_name;        // DuckDB catalog
    string schema_name;         // SQL Server schema
    string table_name;          // SQL Server table name

    // Primary key info (for rowid → PK mapping)
    PrimaryKeyInfo pk_info;     // From MSSQLTableEntry

    // All table columns (for reference)
    vector<MSSQLColumnInfo> table_columns;

    // Helper methods
    string GetFullyQualifiedName() const;
    idx_t GetParamsPerRow() const;  // pk_cols only
    bool IsScalarPK() const;
    bool IsCompositePK() const;
};
```

---

### 5. MSSQLDMLBatch

Represents a batch of rows ready for execution.

```cpp
struct MSSQLDMLBatch {
    // Batch metadata
    idx_t batch_number;         // Sequential batch ID for error reporting
    idx_t row_count;            // Number of rows in this batch

    // Generated SQL
    string sql;                 // Complete parameterized SQL statement

    // Parameter values (ordered for binding)
    vector<Value> parameters;   // Flattened: [pk1_1, pk2_1, v1_1, pk1_2, pk2_2, v1_2, ...]

    // Parameter type info (for TDS binding)
    vector<LogicalType> parameter_types;
};
```

---

### 6. MSSQLDMLResult

Result from executing a DML batch.

```cpp
struct MSSQLDMLResult {
    idx_t rows_affected;        // @@ROWCOUNT from SQL Server
    bool success;               // Execution succeeded
    string error_message;       // If !success, SQL Server error
    int error_code;             // SQL Server error number
    idx_t batch_number;         // Which batch failed (if any)
};
```

---

### 7. MSSQLUpdateGlobalSinkState

Global state for UPDATE physical operator execution.

```cpp
struct MSSQLUpdateGlobalSinkState : public GlobalSinkState {
    // Executor
    unique_ptr<MSSQLUpdateExecutor> executor;

    // Statistics
    idx_t total_rows_updated;   // Accumulated across all batches
    idx_t batch_count;          // Number of batches executed

    // Result tracking
    bool finalized;             // Has Finalize() been called?
    bool returned;              // Has GetData() returned result?

    // Thread safety
    mutable std::mutex mutex;
};
```

---

### 8. MSSQLDeleteGlobalSinkState

Global state for DELETE physical operator execution.

```cpp
struct MSSQLDeleteGlobalSinkState : public GlobalSinkState {
    // Executor
    unique_ptr<MSSQLDeleteExecutor> executor;

    // Statistics
    idx_t total_rows_deleted;   // Accumulated across all batches
    idx_t batch_count;          // Number of batches executed

    // Result tracking
    bool finalized;
    bool returned;

    // Thread safety
    mutable std::mutex mutex;
};
```

---

## Relationships

```
┌─────────────────────┐
│  MSSQLCatalog       │
│  (PlanUpdate/       │
│   PlanDelete)       │
└─────────┬───────────┘
          │ creates
          ▼
┌─────────────────────┐     ┌─────────────────────┐
│ MSSQLPhysicalUpdate │     │ MSSQLPhysicalDelete │
│                     │     │                     │
│ - target: Update    │     │ - target: Delete    │
│ - config: DMLConfig │     │ - config: DMLConfig │
└─────────┬───────────┘     └─────────┬───────────┘
          │ owns                      │ owns
          ▼                           ▼
┌─────────────────────┐     ┌─────────────────────┐
│ UpdateGlobalSink    │     │ DeleteGlobalSink    │
│ State               │     │ State               │
│                     │     │                     │
│ - executor          │     │ - executor          │
└─────────┬───────────┘     └─────────┬───────────┘
          │ contains                  │ contains
          ▼                           ▼
┌─────────────────────┐     ┌─────────────────────┐
│ MSSQLUpdateExecutor │     │ MSSQLDeleteExecutor │
│                     │     │                     │
│ - Sink() rows       │     │ - Sink() rowids     │
│ - Build batches     │     │ - Build batches     │
│ - Execute SQL       │     │ - Execute SQL       │
└─────────────────────┘     └─────────────────────┘
```

---

## State Transitions

### UPDATE/DELETE Execution Flow

```
                    ┌──────────────────┐
                    │     CREATED      │
                    │ (after planning) │
                    └────────┬─────────┘
                             │ GetGlobalSinkState()
                             ▼
                    ┌──────────────────┐
          ┌────────│    ACCEPTING     │◄───────┐
          │        │   (Sink calls)   │        │
          │        └────────┬─────────┘        │
          │                 │                  │
          │   batch full    │    more rows     │
          │                 │                  │
          ▼                 │                  │
    ┌───────────┐           │                  │
    │  EXECUTE  │───────────┴──────────────────┘
    │  (batch)  │
    └─────┬─────┘
          │
          │ no more input
          ▼
    ┌───────────────┐
    │   FINALIZE    │
    │ (flush batch) │
    └───────┬───────┘
            │
            │ GetData()
            ▼
    ┌───────────────┐
    │   COMPLETE    │
    │ (return count)│
    └───────────────┘
```

---

## Validation Rules

### MSSQLUpdateTarget
- `pk_info.exists` must be true (FR-002)
- `update_columns` must not be empty
- `update_columns` must not contain PK columns (FR-014)
- All column types must be supported by `MSSQLValueSerializer`

### MSSQLDeleteTarget
- `pk_info.exists` must be true (FR-002)
- At least one rowid must be provided for batch

### MSSQLDMLBatch
- `row_count > 0`
- `parameters.size() == row_count * params_per_row`
- `sql` is valid parameterized T-SQL

---

## Type Mappings

Rowid types from `MSSQLTableEntry::GetRowIdType()`:

| PK Structure | DuckDB rowid Type | Example |
|--------------|-------------------|---------|
| Scalar INT | INTEGER | `42` |
| Scalar BIGINT | BIGINT | `9223372036854775807` |
| Scalar VARCHAR | VARCHAR | `'abc123'` |
| Composite (INT, INT) | STRUCT(a INTEGER, b INTEGER) | `{'a': 1, 'b': 2}` |
| Composite (INT, VARCHAR) | STRUCT(id INTEGER, code VARCHAR) | `{'id': 1, 'code': 'A'}` |

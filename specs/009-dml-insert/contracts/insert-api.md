# INSERT API Contract

**Feature**: 009-dml-insert
**Date**: 2026-01-19

## Overview

This document defines the internal C++ API contracts for the INSERT implementation. Since this is a DuckDB extension (not a web service), contracts describe class interfaces rather than HTTP/REST APIs.

## MSSQLInsertExecutor

Main orchestrator for INSERT operations.

### Interface

```cpp
class MSSQLInsertExecutor {
public:
    // Constructor
    MSSQLInsertExecutor(
        ClientContext &context,
        const MSSQLInsertTarget &target,
        const MSSQLInsertConfig &config
    );

    // Execute INSERT for a batch of rows
    // Returns: number of rows successfully inserted
    // Throws: MSSQLInsertException on failure
    idx_t Execute(DataChunk &input_chunk);

    // Execute INSERT with RETURNING
    // Returns: DataChunk containing OUTPUT INSERTED results
    // Throws: MSSQLInsertException on failure
    unique_ptr<DataChunk> ExecuteWithReturning(
        DataChunk &input_chunk,
        const vector<idx_t> &returning_column_ids
    );

    // Finalize operation (flush any pending batches)
    void Finalize();

    // Get execution statistics
    MSSQLInsertStatistics GetStatistics() const;
};
```

### Preconditions

- `context` must have valid connection to SQL Server via attached catalog
- `target` must reference valid table with all columns resolved
- `input_chunk` column types must match `target.insert_column_indices` types

### Postconditions

- On success: rows inserted into SQL Server, row count returned
- On failure: exception thrown with `MSSQLInsertError` details
- Connection returned to pool in all cases

### Error Conditions

| Error | Condition | Message Pattern |
|-------|-----------|-----------------|
| `MSSQLInsertException` | SQL Server constraint violation | "INSERT failed at statement N (rows X-Y): [error_num] message" |
| `MSSQLInsertException` | Single row exceeds size limit | "Row at offset N exceeds maximum SQL size (M bytes)" |
| `MSSQLInsertException` | Identity column value provided | "Cannot insert explicit value for identity column 'name'" |
| `InvalidInputException` | Type conversion failure | "Cannot convert DuckDB type T to SQL Server column 'name'" |
| `ConnectionException` | Network/TDS failure | "Connection lost during INSERT: message" |

## MSSQLValueSerializer

Converts DuckDB values to T-SQL literal strings.

### Interface

```cpp
class MSSQLValueSerializer {
public:
    // Serialize a single value to T-SQL literal
    // Returns: literal string (e.g., "N'hello'", "123", "NULL")
    static std::string Serialize(const Value &value, const LogicalType &target_type);

    // Serialize a value from a Vector at given index
    static std::string SerializeFromVector(
        Vector &vector,
        idx_t index,
        const LogicalType &target_type
    );

    // Estimate serialized size (for batch sizing)
    static size_t EstimateSerializedSize(const Value &value, const LogicalType &type);

    // Escape identifier for T-SQL
    static std::string EscapeIdentifier(const std::string &name);

    // Escape string value for T-SQL (without N'' wrapper)
    static std::string EscapeString(const std::string &value);
};
```

### Type-Specific Methods

```cpp
// Numeric types
static std::string SerializeBoolean(bool value);
static std::string SerializeInteger(int64_t value);
static std::string SerializeUBigInt(uint64_t value);
static std::string SerializeFloat(float value);
static std::string SerializeDouble(double value);
static std::string SerializeDecimal(const hugeint_t &value, uint8_t scale);

// String/Binary types
static std::string SerializeString(const string_t &value);
static std::string SerializeBlob(const string_t &value);
static std::string SerializeUUID(const hugeint_t &value);

// Temporal types
static std::string SerializeDate(date_t value);
static std::string SerializeTime(dtime_t value);
static std::string SerializeTimestamp(timestamp_t value);
static std::string SerializeTimestampTZ(timestamp_t value, int32_t offset_seconds);
```

### Error Handling

| Error | Condition | Behavior |
|-------|-----------|----------|
| NaN | Float/Double is NaN | Throw `InvalidInputException` |
| Infinity | Float/Double is +/-Inf | Throw `InvalidInputException` |
| Overflow | UBIGINT > DECIMAL(20,0) max | Throw `InvalidInputException` |

## MSSQLBatchBuilder

Accumulates rows and produces batched INSERT statements.

### Interface

```cpp
class MSSQLBatchBuilder {
public:
    MSSQLBatchBuilder(
        const MSSQLInsertTarget &target,
        const MSSQLInsertConfig &config,
        bool include_output_clause
    );

    // Add a row to current batch
    // Returns: true if row added, false if batch is full
    bool AddRow(DataChunk &chunk, idx_t row_index);

    // Check if batch has rows
    bool HasPendingRows() const;

    // Get current batch row count
    idx_t GetPendingRowCount() const;

    // Generate SQL for current batch and reset
    MSSQLInsertBatch FlushBatch();

    // Get cumulative row offset (for error reporting)
    idx_t GetCurrentRowOffset() const;
};
```

### Batch Sizing Logic

```cpp
// Pseudocode for AddRow
bool AddRow(DataChunk &chunk, idx_t row_index) {
    size_t row_sql_size = EstimateRowSQLSize(chunk, row_index);

    // Check if single row exceeds limit
    if (row_sql_size > config.max_sql_bytes) {
        throw InvalidInputException("Row exceeds max SQL size");
    }

    // Check byte limit
    if (current_sql_bytes + row_sql_size > config.max_sql_bytes) {
        return false; // Batch full, caller should flush
    }

    // Check row count limit
    if (pending_row_count >= config.EffectiveRowsPerStatement()) {
        return false; // Batch full
    }

    // Add row
    AppendRow(chunk, row_index);
    current_sql_bytes += row_sql_size;
    pending_row_count++;
    return true;
}
```

## MSSQLInsertStatement

Generates SQL INSERT statements.

### Interface

```cpp
class MSSQLInsertStatement {
public:
    MSSQLInsertStatement(const MSSQLInsertTarget &target, bool include_output);

    // Build INSERT statement from accumulated values
    std::string Build(const std::vector<std::vector<std::string>> &row_literals);

    // Get column list for INSERT
    std::string GetColumnList() const;

    // Get OUTPUT clause (empty if not enabled)
    std::string GetOutputClause() const;
};
```

### SQL Template

```sql
-- Without OUTPUT
INSERT INTO [schema].[table] ([col1], [col2])
VALUES
  (lit1, lit2),
  (lit3, lit4);

-- With OUTPUT
INSERT INTO [schema].[table] ([col1], [col2])
OUTPUT INSERTED.[col1], INSERTED.[col2], INSERTED.[id]
VALUES
  (lit1, lit2),
  (lit3, lit4);
```

## MSSQLReturningParser

Parses OUTPUT INSERTED results into DuckDB DataChunk.

### Interface

```cpp
class MSSQLReturningParser {
public:
    MSSQLReturningParser(
        const MSSQLInsertTarget &target,
        const vector<idx_t> &returning_column_ids
    );

    // Parse TDS result stream into DataChunk
    unique_ptr<DataChunk> Parse(MSSQLResultStream &stream);
};
```

### Behavior

- Uses existing `TypeConverter` for TDS → DuckDB conversion
- Column order matches `returning_column_ids` specification
- Returns `nullptr` if result set is empty (shouldn't happen for INSERT)

## Configuration Settings Contract

### Setting Registration

```cpp
// In MSSQLSettings::Register()
void RegisterInsertSettings(DatabaseInstance &db) {
    // Batch size
    db.config.AddExtensionOption(
        "mssql_insert_batch_size",
        "Maximum rows per INSERT statement",
        LogicalType::BIGINT,
        Value::BIGINT(2000)
    );

    // Max rows per statement
    db.config.AddExtensionOption(
        "mssql_insert_max_rows_per_statement",
        "Hard cap on rows per INSERT statement",
        LogicalType::BIGINT,
        Value::BIGINT(2000)
    );

    // Max SQL bytes
    db.config.AddExtensionOption(
        "mssql_insert_max_sql_bytes",
        "Maximum SQL statement size in bytes",
        LogicalType::BIGINT,
        Value::BIGINT(8388608)  // 8MB
    );

    // Use OUTPUT for RETURNING
    db.config.AddExtensionOption(
        "mssql_insert_use_returning_output",
        "Use OUTPUT INSERTED for RETURNING clause",
        LogicalType::BOOLEAN,
        Value::BOOLEAN(true)
    );
}
```

### Setting Retrieval

```cpp
MSSQLInsertConfig GetInsertConfig(ClientContext &context) {
    MSSQLInsertConfig config;
    config.batch_size = context.db->config.GetOptionValue("mssql_insert_batch_size")
                            .GetValue<int64_t>();
    config.max_rows_per_statement = context.db->config
        .GetOptionValue("mssql_insert_max_rows_per_statement").GetValue<int64_t>();
    config.max_sql_bytes = context.db->config
        .GetOptionValue("mssql_insert_max_sql_bytes").GetValue<int64_t>();
    config.use_returning_output = context.db->config
        .GetOptionValue("mssql_insert_use_returning_output").GetValue<bool>();
    return config;
}
```

## Catalog Integration Contract

### MSSQLCatalog::PlanInsert

```cpp
// In mssql_catalog.cpp
unique_ptr<PhysicalOperator> MSSQLCatalog::PlanInsert(
    ClientContext &context,
    LogicalInsert &insert,
    unique_ptr<PhysicalOperator> plan
) {
    // 1. Resolve target table
    auto &table_entry = insert.table.Cast<MSSQLTableEntry>();

    // 2. Build MSSQLInsertTarget from table metadata
    MSSQLInsertTarget target = BuildInsertTarget(table_entry, insert.column_index_map);

    // 3. Check for identity column conflicts
    ValidateNoExplicitIdentity(target, insert);

    // 4. Get configuration
    MSSQLInsertConfig config = GetInsertConfig(context);

    // 5. Create physical operator
    return make_uniq<MSSQLPhysicalInsert>(
        context,
        std::move(target),
        std::move(config),
        insert.return_chunk,  // true if RETURNING requested
        insert.returning_index_map,
        std::move(plan)
    );
}
```

### Access Mode Update

```cpp
// MSSQLCatalog must be attached with READ_WRITE mode for INSERT
if (access_mode == AccessMode::READ_ONLY) {
    throw PermissionException(
        "Cannot INSERT into read-only MSSQL catalog. "
        "Use ATTACH ... (READ_WRITE) to enable writes."
    );
}
```

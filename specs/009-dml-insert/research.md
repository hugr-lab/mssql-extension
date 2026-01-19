# Research: High-Performance DML INSERT

**Feature**: 009-dml-insert
**Date**: 2026-01-19

## Research Topics

### R1: DuckDB Catalog Write Hooks Integration

**Decision**: Implement `MSSQLCatalog::PlanInsert()` to return a `PhysicalInsert` operator that dispatches to `MSSQLInsertExecutor`

**Rationale**:
- DuckDB's catalog API provides `Catalog::PlanInsert()` as the extension point for write operations
- The existing `MSSQLCatalog` class already has the method stub (throws "not supported")
- Following the pattern from `PlanTableScan()` which returns `MSSQLTableFunction`

**Alternatives Considered**:
- Custom table function: Would bypass DuckDB's standard INSERT syntax
- Direct SQL execution: Would not integrate with DuckDB's transaction model

**Implementation Notes**:
- `PlanInsert()` receives `InsertStatement` with target table and column mappings
- Returns `PhysicalOperator` that executes INSERT via `MSSQLInsertExecutor`
- Need to handle both `INSERT VALUES` and `INSERT SELECT` patterns
- DuckDB provides input as `DataChunk` stream regardless of source

### R2: SQL Batch vs Prepared Statements

**Decision**: Use SQL Batch (plain text SQL) for MVP, not TDS RPC/prepared statements

**Rationale**:
- SQL Batch is simpler and already proven in existing query execution
- TDS RPC (sp_executesql) requires parameter metadata negotiation not yet implemented
- MVP goal is functional correctness and acceptable performance
- SQL Batch can achieve good throughput with proper batching (multi-row VALUES)
- Prepared statements add complexity for parameter type inference

**Alternatives Considered**:
- `sp_executesql` with parameters: Better for repeated queries with varying values; more complex implementation
- BCP/Bulk Insert: Highest throughput but no RETURNING support; deferred to future spec

**Implementation Notes**:
- Generate `INSERT INTO [schema].[table] ([cols]) VALUES (lit1), (lit2), ...`
- Escape all identifiers with `[name]` and `]` → `]]`
- Convert all values to T-SQL literals with proper escaping

### R3: T-SQL Literal Encoding for DuckDB Types

**Decision**: Implement `MSSQLValueSerializer` class with type-specific encoding methods

**Rationale**:
- Each DuckDB type requires specific T-SQL literal format
- Unicode strings must use N'...' prefix for server-side collation
- Binary data uses 0x... hex format
- Temporal types require ISO format with optional CAST for datetime2/datetimeoffset

**Type Mapping** (DuckDB → T-SQL Literal):

| DuckDB Type | T-SQL Literal Format | Notes |
|-------------|---------------------|-------|
| BOOLEAN | `0` or `1` | SQL Server BIT |
| TINYINT | Integer literal | Direct |
| SMALLINT | Integer literal | Direct |
| INTEGER | Integer literal | Direct |
| BIGINT | Integer literal | Direct |
| UTINYINT | Integer literal | Range fits SQL Server TINYINT |
| USMALLINT | Integer literal | Use CAST if target is SMALLINT |
| UINTEGER | Integer literal | Use CAST if target is INT |
| UBIGINT | `CAST(... AS DECIMAL(20,0))` | Exceeds BIGINT range |
| FLOAT | Decimal literal | E-notation for extreme values |
| DOUBLE | Decimal literal | E-notation for extreme values |
| DECIMAL(p,s) | Decimal literal | Preserve exact scale |
| VARCHAR | `N'...'` | Unicode literal; escape `'` as `''` |
| UUID | `'xxxxxxxx-xxxx-...'` | Standard GUID format |
| BLOB | `0x...` | Hex encoding |
| DATE | `'YYYY-MM-DD'` | ISO format |
| TIME | `'HH:MM:SS.fffffff'` | Up to 7 fractional digits |
| TIMESTAMP | `'YYYY-MM-DDTHH:MM:SS.fffffff'` | Cast to datetime2(7) |
| TIMESTAMP_TZ | `'YYYY-MM-DDTHH:MM:SS.fffffff+HH:MM'` | Cast to datetimeoffset(7) |
| NULL | `NULL` | All types |

**Alternatives Considered**:
- Parameterized queries: Would avoid escaping but requires RPC implementation
- Binary protocol: Not supported by SQL Batch execution

### R4: Batching Strategy

**Decision**: Dynamic batching based on row count AND byte size limits

**Rationale**:
- Large VALUES lists are expensive for SQL Server to parse/compile
- 2000 rows is reasonable default balancing compilation cost vs round-trip overhead
- 8MB SQL size limit prevents memory issues in TDS packet handling
- Dynamic splitting handles variable row sizes gracefully

**Algorithm**:
```
effective_rows = min(batch_size_setting, max_rows_per_statement_setting)
for each input chunk:
    while rows remain in chunk:
        estimate_bytes = calculate_row_sql_size(next_row)
        if current_batch_bytes + estimate_bytes > max_sql_bytes:
            flush_batch()
        if current_batch_rows >= effective_rows:
            flush_batch()
        add_row_to_batch()
    if not_returning:
        flush_batch()  # Flush per chunk for streaming
flush_final_batch()
```

**Alternatives Considered**:
- Fixed row batches only: Fails for rows with large text/binary values
- Full buffering: Violates Streaming First principle
- Per-row execution: Poor performance due to round-trip overhead

### R5: OUTPUT INSERTED for RETURNING

**Decision**: Use SQL Server's `OUTPUT INSERTED.*` clause for RETURNING semantics

**Rationale**:
- `OUTPUT INSERTED` is SQL Server's native mechanism for returning inserted data
- Returns actual column values including identity and defaults
- Works with multi-row INSERT
- Supported by SQL Batch execution (no RPC required)

**SQL Pattern**:
```sql
INSERT INTO [schema].[table] ([col1], [col2])
OUTPUT INSERTED.[col1], INSERTED.[col2], INSERTED.[identity_col]
VALUES
    (val1_1, val1_2),
    (val2_1, val2_2);
```

**Implementation Notes**:
- Parse TDS result set from OUTPUT clause same as SELECT
- Use existing `TypeConverter` for TDS → DuckDB conversion
- Column order in OUTPUT must match DuckDB RETURNING expectation
- Handle RETURNING * → output all columns in table order

**Alternatives Considered**:
- Separate SELECT after INSERT: Extra round-trip; race condition risk
- SCOPE_IDENTITY(): Only returns single identity value; no full row data

### R6: Identity Column Handling

**Decision**: Omit identity columns from INSERT column list; fail if user provides explicit values

**Rationale**:
- SQL Server auto-generates identity values by default
- `SET IDENTITY_INSERT ON` requires special permissions and transaction scope
- MVP simplifies by disallowing explicit identity values
- Identity values returned via OUTPUT INSERTED for RETURNING

**Implementation Notes**:
- Query column metadata to detect `is_identity` flag
- Filter identity columns from INSERT column list generation
- If DuckDB provides value for identity column → throw error with clear message
- RETURNING * includes identity column in OUTPUT clause

**Alternatives Considered**:
- `SET IDENTITY_INSERT ON`: Adds complexity; requires session-level state
- Silent ignore: Violates "Correctness over Convenience" principle

### R7: Error Handling and Atomicity

**Decision**: Statement-level atomicity with detailed error context

**Rationale**:
- SQL Server treats each INSERT statement as atomic
- If any row in a batch fails, entire statement fails (no partial insert)
- Error messages must include context for debugging large batches

**Error Context Structure**:
```cpp
struct InsertError {
    size_t statement_index;      // Which batch failed (0-based)
    size_t row_offset_start;     // First row in failed batch
    size_t row_offset_end;       // Last row in failed batch
    int32_t sql_error_number;    // SQL Server error number
    std::string sql_error_message; // SQL Server error text
};
```

**Implementation Notes**:
- Track cumulative row offset across batches
- Parse TDS ERROR token for SQL Server error details
- Format user-facing error: "INSERT failed at rows [N-M]: {SQL Server message}"
- Do not attempt partial retry in MVP

**Alternatives Considered**:
- Per-row error reporting: Requires individual statements; poor performance
- Retry with binary search: Complex; deferred to future enhancement
- Transaction wrapping: Adds round-trips; MVP uses implicit statement transactions

### R8: String Escaping and SQL Injection Prevention

**Decision**: Strict escaping of all identifiers and values; no raw SQL input

**Rationale**:
- SQL Batch execution passes SQL as text; injection is possible if not careful
- DuckDB bound pipeline provides catalog-verified identifiers (not user strings)
- Value escaping follows T-SQL rules

**Escaping Rules**:
- Identifiers: `[name]` with `]` → `]]`
- String values: `N'value'` with `'` → `''`
- Never interpolate user-provided SQL fragments

**Implementation Notes**:
```cpp
std::string EscapeIdentifier(const std::string &name) {
    std::string escaped = name;
    // Replace ] with ]]
    size_t pos = 0;
    while ((pos = escaped.find(']', pos)) != std::string::npos) {
        escaped.replace(pos, 1, "]]");
        pos += 2;
    }
    return "[" + escaped + "]";
}

std::string EscapeString(const std::string &value) {
    std::string escaped = value;
    // Replace ' with ''
    size_t pos = 0;
    while ((pos = escaped.find('\'', pos)) != std::string::npos) {
        escaped.replace(pos, 1, "''");
        pos += 2;
    }
    return "N'" + escaped + "'";
}
```

**Alternatives Considered**:
- Parameterized queries: Inherently safe but requires RPC implementation

### R9: Configuration Settings

**Decision**: Four new DuckDB configuration settings for insert behavior

**Settings**:

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `mssql_insert_batch_size` | INT | 2000 | Maximum rows per INSERT statement |
| `mssql_insert_max_rows_per_statement` | INT | 2000 | Hard cap on rows per statement |
| `mssql_insert_max_sql_bytes` | INT | 8388608 | Maximum SQL statement size (8MB) |
| `mssql_insert_use_returning_output` | BOOL | true | Use OUTPUT INSERTED for RETURNING |

**Rationale**:
- `batch_size` and `max_rows_per_statement` may differ based on use case
- `max_sql_bytes` prevents OOM on large rows; aligns with TDS packet constraints
- `use_returning_output` allows disabling OUTPUT for edge cases (triggers, etc.)

**Implementation Notes**:
- Register via `MSSQLSettings::Register()` alongside existing pool settings
- Effective rows per statement = `min(batch_size, max_rows_per_statement)`

### R10: Memory Management

**Decision**: Streaming batch execution with bounded memory per batch

**Rationale**:
- Constitution principle "Streaming First" requires bounded memory
- Pre-allocate SQL string buffer for batch; reuse across batches
- Do not buffer entire input dataset

**Implementation Notes**:
- `MSSQLBatchBuilder` owns single `std::string` buffer
- Estimate row size before adding to prevent reallocation
- Reserve capacity based on `max_sql_bytes` setting
- For RETURNING mode, parse and forward results before next batch

**Memory Bounds**:
- SQL buffer: `max_sql_bytes` (default 8MB)
- Row data: Single DataChunk at a time from DuckDB pipeline
- OUTPUT results: Single batch result set at a time

## Summary

All technical decisions align with constitution principles:
- Native TDS implementation (no ODBC/JDBC)
- Streaming execution with bounded memory
- Statement-level atomicity with clear errors
- DuckDB-native INSERT syntax via catalog hooks
- Incremental delivery (writes after reads)

No NEEDS CLARIFICATION items remain. Ready for Phase 1 design.

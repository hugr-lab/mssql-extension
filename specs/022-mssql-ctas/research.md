# Research: CTAS for MSSQL

**Branch**: `022-mssql-ctas` | **Date**: 2026-01-28

## Research Summary

All technical unknowns have been resolved through codebase exploration. No external research required as CTAS integrates entirely with existing infrastructure.

---

## R1: DuckDB CTAS Planning Hook

**Decision**: Override `MSSQLCatalog::PlanCreateTableAs()` to intercept CTAS operations.

**Rationale**: DuckDB's `Catalog` base class provides a virtual `PlanCreateTableAs()` method that external catalogs can override. The current implementation throws `NotImplementedException`. This is the standard extension point for catalog-driven CTAS.

**Alternatives Considered**:
- Custom function (`mssql_create_table_as()`) — rejected because it breaks DuckDB-Native UX principle
- Parser hook — rejected because DuckDB already routes CTAS to catalog

**Integration Point**: `src/catalog/mssql_catalog.cpp` line ~450 (near existing `PlanInsert()`)

---

## R2: Type Mapping Strategy

**Decision**: Extend `MSSQLDDLTranslator::MapTypeToSQLServer()` with CTAS-specific type inference from DuckDB `LogicalType`.

**Rationale**: Existing type mapper handles DuckDB-to-SQL-Server translation for DDL. CTAS needs the same mapping applied to the source query's output schema. The mapper already handles:
- Integer family (TINYINT → tinyint, etc.)
- Floating point (FLOAT → real, DOUBLE → float)
- DECIMAL with precision/scale clamping
- VARCHAR → nvarchar(max) (or varchar(max) via setting)
- Temporal types with appropriate precision

**Additions Needed**:
- HUGEINT, UTINYINT, USMALLINT, UINTEGER, UBIGINT → error (unsupported)
- INTERVAL → error (unsupported)
- LIST, STRUCT, MAP, UNION → error (unsupported)

**File**: `src/catalog/mssql_ddl_translator.cpp`

---

## R3: INSERT Path Reuse

**Decision**: Reuse `MSSQLInsertExecutor` infrastructure for the data load phase.

**Rationale**: The existing insert path (`src/dml/insert/`) provides:
- Batched INSERT generation (`MSSQLBatchBuilder`)
- Configurable batch size and SQL size limits
- Connection management via `ConnectionProvider`
- Transaction-aware execution
- Error handling with partial commit detection

**Mode A (Bulk Insert)**: CTAS will use Mode A exclusively — batched VALUES inserts without OUTPUT/RETURNING clause. This matches FR-002 and FR-005.

**Reuse Pattern**: Create `CTASInsertState` that wraps `MSSQLInsertExecutor` with CTAS-specific configuration (no RETURNING, potentially different batch size).

---

## R4: OR REPLACE Implementation

**Decision**: Implement OR REPLACE as explicit DROP + CREATE sequence with documented non-atomicity.

**Rationale**: SQL Server does not support atomic CREATE OR REPLACE TABLE. The extension must:
1. Check if table exists via catalog metadata cache
2. If exists and OR REPLACE: execute `DROP TABLE [schema].[table]`
3. Execute `CREATE TABLE` DDL
4. Execute batched INSERT

**Non-Atomicity**: If step 3 fails after step 2 succeeds, the original table is lost. This is documented behavior (FR-020) and cannot be avoided without SQL Server-side stored procedures.

**Table Existence Check**: Use `MSSQLMetadataCache::TableExists()` or query `sys.objects`.

---

## R5: Settings Registration

**Decision**: Add two new settings via existing `RegisterMSSQLSettings()` infrastructure.

**Settings**:
```cpp
// mssql_ctas_drop_on_failure (BOOL, default: false)
config.AddExtensionOption(
    "mssql_ctas_drop_on_failure",
    "Drop table if CTAS insert phase fails",
    LogicalType::BOOLEAN,
    Value::BOOLEAN(false)
);

// mssql_ctas_text_type (STRING, default: "NVARCHAR")
config.AddExtensionOption(
    "mssql_ctas_text_type",
    "Text column type for CTAS: NVARCHAR or VARCHAR",
    LogicalType::VARCHAR,
    Value("NVARCHAR"),
    ValidateTextType  // Custom validator
);
```

**File**: `src/connection/mssql_settings.cpp`

---

## R6: Observability Integration

**Decision**: Use existing `MSSQL_DEBUG` / `MSSQL_DML_DEBUG` infrastructure for debug logging.

**Rationale**: The codebase already has debug macros and `GetDebugLevel()` pattern. CTAS will emit:
- Level 1: Phase transitions (DDL start/end, INSERT start/end), row counts, errors
- Level 2: Generated DDL SQL, batch statistics
- Level 3: Per-batch timing, connection state

**Pattern**:
```cpp
if (GetDebugLevel() >= 1) {
    fprintf(stderr, "[MSSQL CTAS] DDL phase: %zu bytes, %lld ms\n", ddl_size, ddl_time_ms);
    fprintf(stderr, "[MSSQL CTAS] INSERT phase: %zu rows, %lld ms\n", row_count, insert_time_ms);
}
```

---

## R7: Transaction Semantics

**Decision**: DDL executes in autocommit mode; INSERT respects DuckDB transaction boundaries.

**Rationale**: SQL Server CREATE TABLE is implicitly committed. This cannot be changed without wrapping in explicit transaction, which would require:
1. BEGIN TRANSACTION before CREATE TABLE
2. Holding transaction open during entire INSERT phase
3. COMMIT or ROLLBACK at end

This approach has significant drawbacks:
- Long-running transactions on SQL Server
- Lock escalation risks
- Connection pinning for entire CTAS duration

**Current Behavior**: CREATE TABLE commits immediately. INSERT phase uses transaction pinning if inside DuckDB transaction. On rollback, table exists but data is rolled back.

---

## R8: Error Message Format

**Decision**: Follow existing error message patterns in the codebase.

**Examples**:
```cpp
// Unsupported type
throw InvalidInputException("CTAS: Unsupported DuckDB type '%s' for column '%s'",
                            type.ToString(), column_name);

// Table exists (no OR REPLACE)
throw CatalogException("Table '%s.%s' already exists. Use CREATE OR REPLACE TABLE to overwrite.",
                       schema_name, table_name);

// Schema not found
throw CatalogException("Schema '%s' does not exist in database '%s'",
                       schema_name, database_name);

// Insert failure with cleanup error
throw IOException("CTAS INSERT failed: %s. Cleanup DROP TABLE also failed: %s",
                  insert_error, drop_error);
```

---

## R9: Catalog Cache Invalidation

**Decision**: Call `MSSQLCatalog::InvalidateMetadataCache()` after successful CREATE TABLE.

**Rationale**: Existing DDL operations (CREATE TABLE, DROP TABLE, ALTER TABLE) already call this method. CTAS follows the same pattern to ensure the new table appears in catalog queries.

**Timing**: Invalidate after CREATE TABLE succeeds, before INSERT phase begins. This allows the INSERT phase to resolve the target table from cache if needed.

---

## R10: Physical Operator Design

**Decision**: Create `MSSQLPhysicalCreateTableAs` as a sink operator that consumes source data.

**Pattern** (following `MSSQLPhysicalInsert`):
```cpp
class MSSQLPhysicalCreateTableAs : public PhysicalOperator {
public:
    // Constructor receives target table info and source plan types
    MSSQLPhysicalCreateTableAs(CTASTarget target, vector<LogicalType> source_types);

    // Sink interface - receives chunks from source query
    SinkResultType Sink(ExecutionContext &context, DataChunk &chunk,
                        OperatorSinkInput &input) const override;

    // Finalize - executes DDL, then flushes remaining INSERT batches
    SinkFinalizeType Finalize(Pipeline &pipeline, Event &event,
                              ClientContext &context,
                              OperatorSinkFinalizeInput &input) const override;

    // Global state holds executor, connection, batch builder
    unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
};
```

**Execution Flow**:
1. `GetGlobalSinkState()`: Validate types, generate DDL, prepare executor
2. First `Sink()` call: Execute CREATE TABLE DDL, then start accumulating rows
3. Subsequent `Sink()` calls: Accumulate rows, flush batches as needed
4. `Finalize()`: Flush remaining batches, invalidate cache, return row count

---

## Resolved Unknowns

| Unknown | Resolution |
|---------|------------|
| CTAS planning hook | `MSSQLCatalog::PlanCreateTableAs()` override |
| Type mapping source | Extend `MSSQLDDLTranslator::MapTypeToSQLServer()` |
| INSERT reuse | Wrap `MSSQLInsertExecutor` with CTAS config |
| OR REPLACE atomicity | Non-atomic DROP + CREATE, documented |
| Settings infrastructure | `RegisterMSSQLSettings()` with validators |
| Debug logging | Existing `MSSQL_DEBUG` macros |
| Transaction semantics | DDL autocommit, INSERT transaction-aware |
| Cache invalidation | `InvalidateMetadataCache()` after CREATE |

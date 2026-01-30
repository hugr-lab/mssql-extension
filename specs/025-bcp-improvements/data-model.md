# Data Model: BCP Improvements

**Feature**: 025-bcp-improvements
**Date**: 2026-01-30

## Entities

### BCPCopyTarget (Modified)

Represents the target table for COPY operations. Extended to handle empty schema notation.

| Field | Type | Description |
|-------|------|-------------|
| catalog_name | string | Attached MSSQL catalog name |
| schema_name | string | Schema name (may be empty for temp tables) |
| table_name | string | Table name (with `#` prefix for temp tables) |
| is_temp_table | bool | True if local temp table (`#table`) |
| is_global_temp | bool | True if global temp table (`##table`) |

**Validation Rules**:
- `catalog_name` MUST NOT be empty
- `schema_name` MAY be empty ONLY if `is_temp_table` or `is_global_temp` is true
- `table_name` MUST NOT be empty
- If `table_name` starts with `##`, set `is_global_temp = true`
- If `table_name` starts with `#` (but not `##`), set `is_temp_table = true`

**New Behavior**:
- `GetFullyQualifiedName()` returns `[schema].[table]` if schema is non-empty
- `GetFullyQualifiedName()` returns `[table]` if schema is empty (temp tables)

### BCPCopyConfig (Modified)

Configuration for COPY operations. Extended to support INSERT method.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| create_table | bool | true | Auto-create target table |
| replace | bool | false | Drop and recreate table |
| flush_rows | int64 | 100000 | Rows before flushing |
| tablock | bool | true | Use TABLOCK hint |
| **method** | enum | BCP | Copy method: BCP or INSERT (new) |

**Method Enum**:
```text
CopyMethod {
  BCP,      // Default: TDS BulkLoadBCP protocol
  INSERT    // Alternative: Batched INSERT statements
}
```

### BCPColumnMetadata (Unchanged)

Column metadata for BCP operations. No changes needed.

## State Transitions

### Connection State During COPY

```text
[Idle] --acquire--> [Executing]
                         |
                    COPY operation
                         |
         +---------------+---------------+
         |               |               |
      success          error         cancel
         |               |               |
    [Executing]    [Executing]     [Executing]
         |               |               |
    finalize()     cleanup()      attention()
         |               |               |
      [Idle]         [Idle]         [Idle]
         |               |               |
      release        release        release
```

**Key Transitions**:
1. **Success path**: DONE token → Finalize → Idle → Release to pool
2. **Error path**: Cleanup (may send DONE) → Idle → Release to pool
3. **Cancel path**: ATTENTION → Drain → Idle → Release to pool

### Copy Method Selection

```text
[Parse Options]
      |
      v
  METHOD specified?
      |
  +---+---+
  |       |
  no     yes
  |       |
  v       v
 BCP    INSERT
  |       |
  v       v
[BCPWriter] [InsertExecutor]
```

## Relationships

```text
BCPCopyBind
    |
    +-- BCPCopyTarget (parsed from URL/catalog)
    |       |
    |       +-- references: MSSQLCatalog (validates catalog exists)
    |
    +-- BCPCopyConfig (parsed from options)
            |
            +-- method: determines execution path

BCPCopyGlobalState
    |
    +-- connection: TdsConnection (from pool)
    |
    +-- writer: BCPWriter (if method = BCP)
    |
    +-- inserter: InsertExecutor (if method = INSERT)
```

## Data Flow

### BCP Method (existing)
```text
DuckDB rows → BCPRowEncoder → TDS BulkLoad packets → SQL Server
```

### INSERT Method (new)
```text
DuckDB rows → Batch accumulator → INSERT VALUES → SQL Server
                                       |
                                 (batched per mssql_insert_batch_size)
```

## Error Handling Model

### Type Mismatch Detection

| Stage | Check | Error Format |
|-------|-------|--------------|
| ValidateTarget | Source vs Target types | "Column '{name}' type mismatch: target expects {sqltype}, source provides {ducktype}" |
| BCPCopySink | Encoding failure | "Failed to encode value for column '{name}': {detail}" |
| SQL Server | Runtime error | Passthrough SQL Server error with column context |

### Connection Cleanup Matrix

| Phase | Error Type | Action |
|-------|------------|--------|
| Bind | Target not found | No connection acquired, no cleanup |
| Bind | Catalog error | No connection acquired, no cleanup |
| InitGlobal | Connection error | Connection not acquired, no cleanup |
| Sink | Encoding error | Send DONE, finalize, release |
| Sink | Network error | Attempt finalize, release (may fail) |
| Finalize | SQL Server error | Release connection (already finalized) |

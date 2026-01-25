# Research: DML UPDATE/DELETE using PK-based rowid

**Feature**: 002-dml-update-delete
**Date**: 2026-01-25

## Overview

This document captures research findings for implementing UPDATE and DELETE operations on MSSQL tables through DuckDB. All "NEEDS CLARIFICATION" items from technical context have been resolved.

---

## R1: DuckDB Physical Operator Interface for UPDATE/DELETE

### Decision
Use the Sink operator pattern with `IsSink() = true`, following the existing `MSSQLPhysicalInsert` implementation.

### Rationale
- DuckDB's `LogicalUpdate` and `LogicalDelete` produce a child operator that outputs rows to be modified
- For UPDATE: Child produces rowid + new values for each column
- For DELETE: Child produces rowid values only
- Sink pattern accumulates rows into batches, then executes against MSSQL

### Alternatives Considered
1. **Source-only operator**: Rejected - doesn't align with DuckDB's DML execution model
2. **Direct SQL passthrough**: Rejected - can't support DuckDB-side predicate evaluation or complex expressions

### Key Findings from DuckDB Source Analysis
- `LogicalDelete` contains `table` reference and creates child plan that outputs rowid
- `LogicalUpdate` contains `table`, `columns`, `expressions` (SET clause expressions)
- Physical operators receive DataChunk with rowid column (index 0) + expression results

---

## R2: Rowid Column Access in Physical Operators

### Decision
Access rowid via `DataChunk.data[0]` in the input chunk from DuckDB's execution pipeline.

### Rationale
DuckDB's UPDATE/DELETE planning places rowid as the first column in the child operator's output. For UPDATE, subsequent columns contain the evaluated SET expressions.

### Implementation Pattern
```cpp
// In Sink():
Vector &rowid_vector = chunk.data[0];

// For scalar PK:
auto rowid_value = FlatVector::GetData<T>(rowid_vector)[row_idx];

// For composite PK (STRUCT):
auto &struct_entries = StructVector::GetEntries(rowid_vector);
// Extract each PK field from struct_entries
```

### Verification
- Confirmed by examining DuckDB's `PhysicalDelete::Sink()` and `PhysicalUpdate::Sink()` implementations
- The rowid column type matches `TableCatalogEntry::GetRowIdType()` which we control via `MSSQLTableEntry::GetRowIdType()`

---

## R3: SQL Server Batched UPDATE/DELETE Patterns

### Decision
Use VALUES join pattern for both UPDATE and DELETE operations.

### Rationale
- Single statement handles multiple rows (not row-by-row)
- Works with parameterized queries (prevents SQL injection)
- Supports both scalar and composite PKs
- Same pattern for UPDATE and DELETE (just different SET clause handling)

### UPDATE Pattern (Composite PK example)
```sql
UPDATE t
SET t.[col1] = v.[col1], t.[col2] = v.[col2]
FROM [schema].[table] AS t
JOIN (VALUES
  (@pk1_1, @pk2_1, @v1_1, @v2_1),
  (@pk1_2, @pk2_2, @v1_2, @v2_2),
  (@pk1_3, @pk2_3, @v1_3, @v2_3)
) AS v([pk1], [pk2], [col1], [col2])
ON t.[pk1] = v.[pk1] AND t.[pk2] = v.[pk2]
```

### DELETE Pattern (Composite PK example)
```sql
DELETE t
FROM [schema].[table] AS t
JOIN (VALUES
  (@pk1_1, @pk2_1),
  (@pk1_2, @pk2_2),
  (@pk1_3, @pk2_3)
) AS v([pk1], [pk2])
ON t.[pk1] = v.[pk1] AND t.[pk2] = v.[pk2]
```

### Alternatives Considered
1. **IN clause**: `DELETE FROM t WHERE pk IN (@p1, @p2, ...)` - Rejected for composite PK complexity
2. **MERGE statement**: Rejected - overkill for simple DELETE, and MERGE has different semantics
3. **Row-by-row execution**: Rejected - poor performance

---

## R4: SQL Server Parameter Limits

### Decision
Default batch size of 500 rows with max_parameters of 2000.

### Rationale
- SQL Server has a hard limit of 2100 parameters per statement
- For UPDATE with N columns and M PK columns: params = batch_size × (N + M)
- 500 rows × 4 columns = 2000 parameters (safe margin)
- Settings are configurable for edge cases

### Calculation Formula
```
max_rows_per_batch = min(
    mssql_dml_batch_size,
    floor(mssql_dml_max_parameters / (pk_column_count + update_column_count))
)
```

---

## R5: PK Column Update Detection

### Decision
Check column indices in `LogicalUpdate::columns` against PK column indices before execution.

### Rationale
- PK column modification would break row identity assumptions
- Detection must happen at planning time, not execution time
- Clear error message guides users to alternative approaches

### Implementation
```cpp
// In MSSQLCatalog::PlanUpdate():
auto &pk_info = table_entry.GetPrimaryKeyInfo(context);
for (auto &pk_col : pk_info.columns) {
    for (auto &update_col_idx : op.columns) {
        if (update_col_idx == pk_col.column_id - 1) {  // column_id is 1-based
            throw BinderException("MSSQL: updating primary key columns is not supported");
        }
    }
}
```

---

## R6: STRUCT Rowid Field Extraction

### Decision
Use `StructVector::GetEntries()` to extract PK components in order.

### Rationale
- Composite PK produces STRUCT rowid type with fields named after PK columns
- Fields are ordered by `key_ordinal` from PK discovery
- Each field's type matches the corresponding PK column's DuckDB type

### Implementation Pattern
```cpp
void ExtractPKFromRowid(Vector &rowid_vector, idx_t row_idx,
                        const PrimaryKeyInfo &pk_info,
                        vector<Value> &pk_values) {
    if (pk_info.IsScalar()) {
        // Direct extraction
        pk_values.push_back(rowid_vector.GetValue(row_idx));
    } else {
        // STRUCT extraction
        auto &entries = StructVector::GetEntries(rowid_vector);
        for (idx_t i = 0; i < pk_info.columns.size(); i++) {
            pk_values.push_back(entries[i]->GetValue(row_idx));
        }
    }
}
```

---

## R7: Reusing INSERT Value Serialization

### Decision
Reuse `MSSQLValueSerializer` for both PK values and UPDATE SET values.

### Rationale
- Same type conversion rules apply (DuckDB types → T-SQL literals)
- Unicode string handling (N'...') already implemented
- NULL handling already implemented
- Edge cases (NaN, Infinity) already handled

### Usage
```cpp
// For PK values:
string pk_literal = MSSQLValueSerializer::Serialize(pk_value, pk_type);

// For UPDATE values:
string value_literal = MSSQLValueSerializer::Serialize(new_value, column_type);
```

---

## R8: Transaction Semantics

### Decision
No cross-batch atomicity; each batch is an independent statement.

### Rationale
- Consistent with spec requirement FR-016
- Matches SQL Server default transaction behavior
- Allows for partial success reporting
- Users wanting full atomicity can wrap in explicit transactions

### Error Handling
- On batch failure: Record failed batch number, SQL Server error
- Return partial success count if applicable
- Allow DuckDB transaction rollback for pinned connections (FR-017)

---

## R9: Write Access Validation

### Decision
Reuse existing `CheckWriteAccess()` method from INSERT implementation.

### Rationale
- Consistent authorization model across DML operations
- Already handles read-only mode detection
- Already throws appropriate exception

### Location
`MSSQLCatalog::CheckWriteAccess(const string &operation)` in `src/catalog/mssql_catalog.cpp`

---

## Summary

All technical unknowns have been resolved. The implementation can proceed with:

1. **Physical operators**: Sink pattern matching INSERT
2. **SQL generation**: VALUES join pattern for batched execution
3. **PK handling**: Direct scalar or STRUCT extraction from rowid
4. **Value serialization**: Reuse from INSERT
5. **Configuration**: Extend INSERT settings with appropriate defaults
6. **Error handling**: Clear messages for PK-less tables and PK updates

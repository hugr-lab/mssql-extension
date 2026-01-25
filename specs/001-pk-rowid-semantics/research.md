# Research: MSSQL rowid Semantics

**Feature**: 001-pk-rowid-semantics
**Date**: 2026-01-25

## 1. Primary Key Discovery from SQL Server

### Decision: Use sys.index_columns with sys.key_constraints

**Rationale**: This combination provides:
- Correct ordinal position via `key_ordinal` (not `column_id`)
- Constraint type filtering (`type = 'PK'`)
- Direct join to `sys.columns` for type information

**Alternatives considered**:
| Approach | Pros | Cons | Verdict |
|----------|------|------|---------|
| `sys.key_constraints` + `sys.index_columns` | Accurate ordinal, explicit PK constraint | Two-table join | ✅ Selected |
| `INFORMATION_SCHEMA.KEY_COLUMN_USAGE` | Standard SQL, simpler | Slower on large catalogs | ❌ |
| `sys.indexes WHERE is_primary_key = 1` | Direct, single table | Still needs `sys.index_columns` for columns | Partial |

### SQL Query for PK Discovery

```sql
SELECT
    c.name AS column_name,
    c.column_id,
    ic.key_ordinal,           -- PK ordinal position (1-based)
    t.name AS type_name,
    c.max_length,
    c.precision,
    c.scale,
    c.is_nullable,
    ISNULL(c.collation_name, '') AS collation_name
FROM sys.key_constraints kc
JOIN sys.indexes i ON kc.parent_object_id = i.object_id
    AND kc.unique_index_id = i.index_id
JOIN sys.index_columns ic ON i.object_id = ic.object_id
    AND i.index_id = ic.index_id
JOIN sys.columns c ON ic.object_id = c.object_id
    AND ic.column_id = c.column_id
JOIN sys.types t ON c.user_type_id = t.user_type_id
WHERE kc.type = 'PK'
    AND kc.parent_object_id = OBJECT_ID('[schema].[table]')
ORDER BY ic.key_ordinal
```

### Error Handling
- **No rows returned**: Table has no PK → `pk_exists = false`
- **Query fails with permission error**: Throw "MSSQL: cannot access primary key metadata for [table]"

---

## 2. DuckDB rowid Handling Pattern

### Decision: Handle COLUMN_IDENTIFIER_ROW_ID in TableScanInitGlobal

**Rationale**: The existing table scan already checks for virtual column IDs (line 69-82 in `mssql_table_scan.cpp`). We extend this to:
1. Detect `COLUMN_IDENTIFIER_ROW_ID` in `column_ids`
2. If detected, include PK columns in the SELECT (if not already present)
3. During `FillChunk`, construct rowid values from PK column data

**DuckDB Constants** (from `duckdb/common/constants.hpp`):
```cpp
DUCKDB_API extern const column_t COLUMN_IDENTIFIER_ROW_ID;  // Special marker
DUCKDB_API bool IsRowIdColumnId(column_t column_id);        // Helper function
```

### Scan Flow with rowid

```
1. GetScanFunction() - bind_data includes PK info (lazy-loaded)
2. TableScanInitGlobal():
   - Check if COLUMN_IDENTIFIER_ROW_ID in column_ids
   - If yes: ensure PK columns in SELECT, mark rowid_requested = true
3. TableScanExecute():
   - FillChunk fills data columns
   - If rowid_requested: construct rowid from PK columns into output
```

---

## 3. DuckDB STRUCT Type for Composite PKs

### Decision: Use LogicalType::STRUCT with child types

**Rationale**: DuckDB's `LogicalType::STRUCT` is the standard way to represent composite values. It's used throughout DuckDB for:
- Arrow integration
- Nested data types
- Composite key representation

### Building STRUCT Type

```cpp
// For composite PK (tenant_id INT, id BIGINT)
child_list_t<LogicalType> struct_children;
struct_children.push_back(make_pair("tenant_id", LogicalType::INTEGER));
struct_children.push_back(make_pair("id", LogicalType::BIGINT));
LogicalType rowid_type = LogicalType::STRUCT(std::move(struct_children));
```

### Building STRUCT Value in Vector

```cpp
// In FillChunk, for composite PK:
auto &struct_vector = output.data[rowid_output_idx];
auto &children = StructVector::GetEntries(struct_vector);

// Fill each child vector from PK column data
for (idx_t pk_idx = 0; pk_idx < pk_columns.size(); pk_idx++) {
    auto &child = *children[pk_idx];
    // Copy values from source PK column to child vector
}
```

---

## 4. Lazy Loading Pattern for PK Cache

### Decision: Load on first rowid access via GetRowIdType()

**Rationale**:
- Most queries don't need rowid
- PK discovery adds latency
- Consistent with existing lazy loading in MSSQLTableSet

### Cache Location
Store in `MSSQLTableEntry`:
```cpp
struct PrimaryKeyInfo {
    bool loaded = false;
    bool exists = false;
    vector<PKColumnInfo> columns;  // Ordered by key_ordinal
    LogicalType rowid_type;
};
mutable PrimaryKeyInfo pk_info_;  // mutable for lazy loading
```

### Thread Safety
Use existing catalog mutex pattern - `MSSQLTableEntry` access is already protected by `MSSQLTableSet::entry_mutex_`.

---

## 5. View Detection

### Decision: Check object_type field in MSSQLTableEntry

**Rationale**: `MSSQLTableEntry` already stores `object_type_` as `MSSQLObjectType::TABLE` or `MSSQLObjectType::VIEW`. Reuse this.

### Implementation
```cpp
if (object_type_ == MSSQLObjectType::VIEW && rowid_requested) {
    throw BinderException("MSSQL: rowid not supported for views");
}
```

---

## 6. Integration with Existing Cache Invalidation

### Decision: PK info invalidated with table metadata

**Rationale**: PK info is part of table structure. When table DDL changes:
- `MSSQLMetadataCache::Invalidate()` marks cache stale
- On next access, entire table metadata (including PK) is reloaded
- `mssql_exec()` already triggers invalidation for DDL statements

### No Additional Invalidation Needed
The `pk_info_.loaded` flag is reset when `MSSQLTableEntry` is recreated during cache refresh.

---

## 7. Type Mapping for PK Columns

### Decision: Reuse existing MSSQLColumnInfo type mapping

**Rationale**: PK columns are regular columns. The existing `MSSQLColumnInfo` class already:
- Maps SQL Server types to DuckDB LogicalType
- Handles collation parsing
- Supports all PK-eligible types

### PK-Eligible SQL Server Types
Per SQL Server documentation, these types can be primary keys:
- Numeric: `int`, `bigint`, `smallint`, `tinyint`, `decimal`, `numeric`
- Character: `char`, `varchar`, `nchar`, `nvarchar` (with length limit)
- Date/Time: `date`, `datetime`, `datetime2`, `smalldatetime`, `time`
- Binary: `binary`, `varbinary` (with length limit)
- Other: `uniqueidentifier`

**Not allowed as PK**: `text`, `ntext`, `image`, `xml`, `varchar(max)`, `nvarchar(max)`, `varbinary(max)`

All PK-eligible types are already mapped in `MSSQLColumnInfo::MapSQLServerTypeToDuckDB()`.

---

## Summary of Decisions

| Topic | Decision | Key Reason |
|-------|----------|------------|
| PK Discovery SQL | `sys.key_constraints` + `sys.index_columns` | Correct ordinal via `key_ordinal` |
| rowid Detection | Check `COLUMN_IDENTIFIER_ROW_ID` in column_ids | DuckDB standard pattern |
| Composite PK Type | DuckDB `LogicalType::STRUCT` | Native DuckDB composite type |
| Lazy Loading | Load PK on first rowid access | Avoid overhead for non-rowid queries |
| View Detection | Check existing `object_type_` field | Reuse existing metadata |
| Cache Invalidation | Integrated with existing mechanism | No additional complexity |
| Type Mapping | Reuse `MSSQLColumnInfo` | All PK types already supported |

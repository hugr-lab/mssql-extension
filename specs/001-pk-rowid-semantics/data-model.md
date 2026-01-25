# Data Model: MSSQL rowid Semantics

**Feature**: 001-pk-rowid-semantics
**Date**: 2026-01-25

## Entities

### PKColumnInfo

Represents a single column in a primary key constraint.

```cpp
namespace duckdb {
namespace mssql {

struct PKColumnInfo {
    string name;              // Column name
    int32_t column_id;        // SQL Server column_id (1-based)
    int32_t key_ordinal;      // Position in PK (1-based, from sys.index_columns)
    LogicalType duckdb_type;  // Mapped DuckDB type
    string collation_name;    // For string columns (may affect DML predicates)
};

}  // namespace mssql
}  // namespace duckdb
```

**Constraints**:
- `key_ordinal` is unique within a PK (1, 2, 3, ...)
- `duckdb_type` is never NULL (PK columns cannot be nullable)
- `collation_name` empty for non-string types

### PrimaryKeyInfo

Cached primary key metadata for an MSSQL table.

```cpp
namespace duckdb {
namespace mssql {

struct PrimaryKeyInfo {
    // Loading state
    bool loaded = false;      // Has PK discovery been attempted?

    // PK existence
    bool exists = false;      // Does table have a PK?

    // PK structure (only valid if exists == true)
    vector<PKColumnInfo> columns;  // Ordered by key_ordinal

    // Computed rowid type
    LogicalType rowid_type;   // Scalar (single col) or STRUCT (composite)

    // Helper methods
    bool IsScalar() const { return exists && columns.size() == 1; }
    bool IsComposite() const { return exists && columns.size() > 1; }

    // Get column names for SELECT clause
    vector<string> GetColumnNames() const;

    // Build rowid type from columns
    void ComputeRowIdType();
};

}  // namespace mssql
}  // namespace duckdb
```

**State Transitions**:
```
Initial:     loaded=false, exists=false
After load:  loaded=true,  exists=true/false (depending on PK)
After invalidate: Entry is deleted, new one created with loaded=false
```

### MSSQLTableEntry (Extended)

Add PK cache to existing table entry.

```cpp
// In mssql_table_entry.hpp, add to private section:

private:
    // ... existing members ...

    // Primary key cache (lazy loaded)
    mutable PrimaryKeyInfo pk_info_;

    // Ensure PK info is loaded (thread-safe via catalog mutex)
    void EnsurePKLoaded(ClientContext &context) const;

public:
    // Get rowid type for this table (loads PK if needed)
    // Returns empty LogicalType if no PK exists
    LogicalType GetRowIdType(ClientContext &context);

    // Check if table has a primary key (loads PK if needed)
    bool HasPrimaryKey(ClientContext &context);

    // Get PK column info (loads PK if needed)
    const PrimaryKeyInfo& GetPrimaryKeyInfo(ClientContext &context);
```

## Relationships

```
MSSQLCatalog
    └── MSSQLSchemaEntry
            └── MSSQLTableSet
                    └── MSSQLTableEntry
                            ├── vector<MSSQLColumnInfo>  (all columns)
                            ├── MSSQLObjectType          (TABLE or VIEW)
                            └── PrimaryKeyInfo           (PK cache) ← NEW
                                    └── vector<PKColumnInfo>
```

## SQL Server Metadata Query

### PK Discovery Query

```sql
-- Parameterized by [schema].[table]
SELECT
    c.name AS column_name,
    c.column_id,
    ic.key_ordinal,
    t.name AS type_name,
    c.max_length,
    c.precision,
    c.scale,
    ISNULL(c.collation_name, '') AS collation_name
FROM sys.key_constraints kc
JOIN sys.indexes i
    ON kc.parent_object_id = i.object_id
    AND kc.unique_index_id = i.index_id
JOIN sys.index_columns ic
    ON i.object_id = ic.object_id
    AND i.index_id = ic.index_id
JOIN sys.columns c
    ON ic.object_id = c.object_id
    AND ic.column_id = c.column_id
JOIN sys.types t
    ON c.user_type_id = t.user_type_id
WHERE kc.type = 'PK'
    AND kc.parent_object_id = OBJECT_ID('%s')
ORDER BY ic.key_ordinal
```

**Result columns**:
| Column | Type | Description |
|--------|------|-------------|
| column_name | nvarchar | PK column name |
| column_id | int | SQL Server column ordinal |
| key_ordinal | int | Position in PK (1, 2, 3...) |
| type_name | nvarchar | SQL Server type name |
| max_length | smallint | Max bytes (-1 for MAX) |
| precision | tinyint | Numeric precision |
| scale | tinyint | Numeric scale |
| collation_name | nvarchar | Column collation (empty if N/A) |

**Empty result**: Table has no PK constraint.

## Rowid Type Construction

### Scalar PK (single column)

```cpp
// PK: id INT
pk_info.rowid_type = LogicalType::INTEGER;  // Direct mapping
```

### Composite PK (multiple columns)

```cpp
// PK: (tenant_id INT, id BIGINT)
child_list_t<LogicalType> children;
children.push_back({"tenant_id", LogicalType::INTEGER});
children.push_back({"id", LogicalType::BIGINT});
pk_info.rowid_type = LogicalType::STRUCT(std::move(children));
```

### No PK

```cpp
// Table has no primary key
pk_info.exists = false;
pk_info.rowid_type = LogicalType();  // Invalid/empty type
```

## Scan Bind Data Extension

Add rowid-related fields to `MSSQLCatalogScanBindData`:

```cpp
// In mssql_functions.hpp or table_scan_bind.hpp

struct MSSQLCatalogScanBindData : public FunctionData {
    // ... existing fields ...

    // Rowid support
    bool rowid_requested = false;     // Is rowid in projection?
    PrimaryKeyInfo pk_info;           // Cached PK info (copied from entry)
    vector<idx_t> pk_column_indices;  // Indices of PK columns in result set
};
```

## Vector Population

### During TableScanExecute

```cpp
// If rowid requested and scalar PK:
// - PK value already in result set
// - Copy to rowid output column

// If rowid requested and composite PK:
// - Build STRUCT from PK columns
auto &rowid_vector = output.data[rowid_output_idx];
auto &entries = StructVector::GetEntries(rowid_vector);
for (idx_t pk_idx = 0; pk_idx < pk_columns.size(); pk_idx++) {
    // Copy from PK column to struct child
    VectorOperations::Copy(
        source_columns[pk_column_indices[pk_idx]],
        *entries[pk_idx],
        count,
        0,  // source offset
        0   // target offset
    );
}
```

## Error Conditions

| Condition | Error Message | When Thrown |
|-----------|---------------|-------------|
| rowid on VIEW | "MSSQL: rowid not supported for views" | GetRowIdType() or scan bind |
| rowid on no-PK table | "MSSQL: rowid requires a primary key" | GetRowIdType() or scan bind |
| NULL PK value | "MSSQL: invalid NULL primary key value in rowid mapping" | During FillChunk |
| Metadata access failed | "MSSQL: cannot access primary key metadata for [table]" | During PK discovery |

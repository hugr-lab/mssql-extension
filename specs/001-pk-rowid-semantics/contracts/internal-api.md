# Internal API Contract: MSSQL rowid Semantics

**Feature**: 001-pk-rowid-semantics
**Date**: 2026-01-25

This document defines the internal C++ API contracts for rowid support.

## PrimaryKeyInfo API

### Header: `src/include/catalog/mssql_primary_key.hpp`

```cpp
namespace duckdb {
namespace mssql {

//===----------------------------------------------------------------------===//
// PKColumnInfo - Single PK column metadata
//===----------------------------------------------------------------------===//

struct PKColumnInfo {
    string name;
    int32_t column_id;
    int32_t key_ordinal;
    LogicalType duckdb_type;
    string collation_name;

    // Construct from discovery query result
    static PKColumnInfo FromMetadata(
        const string& name,
        int32_t column_id,
        int32_t key_ordinal,
        const string& type_name,
        int16_t max_length,
        uint8_t precision,
        uint8_t scale,
        const string& collation_name,
        const string& database_collation
    );
};

//===----------------------------------------------------------------------===//
// PrimaryKeyInfo - Complete PK metadata for a table
//===----------------------------------------------------------------------===//

struct PrimaryKeyInfo {
    bool loaded = false;
    bool exists = false;
    vector<PKColumnInfo> columns;
    LogicalType rowid_type;

    // Predicates
    bool IsScalar() const;
    bool IsComposite() const;

    // Column access
    vector<string> GetColumnNames() const;

    // Type computation
    void ComputeRowIdType();

    // Factory method - discovers PK from SQL Server
    static PrimaryKeyInfo Discover(
        tds::TdsConnection& connection,
        const string& schema_name,
        const string& table_name,
        const string& database_collation
    );
};

}  // namespace mssql
}  // namespace duckdb
```

## MSSQLTableEntry Extensions

### Header: `src/include/catalog/mssql_table_entry.hpp`

```cpp
class MSSQLTableEntry : public TableCatalogEntry {
public:
    // ... existing methods ...

    //===----------------------------------------------------------------------===//
    // Primary Key / RowId Support (NEW)
    //===----------------------------------------------------------------------===//

    // Get the rowid type for this table
    // - Scalar PK: returns PK column type (e.g., INTEGER)
    // - Composite PK: returns STRUCT type
    // - No PK: throws BinderException
    // - VIEW: throws BinderException
    LogicalType GetRowIdType(ClientContext& context);

    // Check if table has a primary key (lazy loads PK info)
    bool HasPrimaryKey(ClientContext& context);

    // Get full PK metadata (lazy loads if needed)
    const mssql::PrimaryKeyInfo& GetPrimaryKeyInfo(ClientContext& context);

private:
    // Lazy-loaded PK cache
    mutable mssql::PrimaryKeyInfo pk_info_;

    // Ensure PK info is loaded
    void EnsurePKLoaded(ClientContext& context) const;
};
```

## Scan Function Contract

### File: `src/table_scan/mssql_table_scan.cpp`

```cpp
// TableScanInitGlobal behavior with rowid:

// Input: input.column_ids may contain COLUMN_IDENTIFIER_ROW_ID

// Algorithm:
// 1. Check if COLUMN_IDENTIFIER_ROW_ID is in column_ids
// 2. If yes:
//    a. Get PK info from bind_data (already populated in GetScanFunction)
//    b. If no PK: throw BinderException("MSSQL: rowid requires a primary key")
//    c. If VIEW: throw BinderException("MSSQL: rowid not supported for views")
//    d. Add PK columns to SELECT if not already projected
//    e. Track pk_column_indices for FillChunk
// 3. Generate SELECT query with necessary columns
// 4. Set rowid_requested flag in global state

// TableScanExecute behavior with rowid:

// If rowid_requested:
// 1. For scalar PK:
//    - Copy PK column value to rowid output vector
// 2. For composite PK:
//    - Create STRUCT in rowid output vector
//    - Fill child vectors from PK column values
// 3. Check for NULL values (should not occur, but throw if found)
```

## Error Messages

| Error ID | Message | Condition |
|----------|---------|-----------|
| `ROWID_NO_PK` | "MSSQL: rowid requires a primary key" | rowid requested on table without PK |
| `ROWID_VIEW` | "MSSQL: rowid not supported for views" | rowid requested on VIEW |
| `ROWID_NULL_PK` | "MSSQL: invalid NULL primary key value in rowid mapping" | NULL in PK column during scan |
| `PK_METADATA_ACCESS` | "MSSQL: cannot access primary key metadata for [table]" | PK discovery query failed |

## Thread Safety

- `PrimaryKeyInfo` discovery is protected by `MSSQLTableSet::entry_mutex_`
- Once loaded, `pk_info_` is read-only (safe for concurrent reads)
- `mutable` keyword allows lazy loading through const methods

# Contract: View Entry

**Feature**: 008-catalog-ddl-statistics
**Module**: `mssql_view_entry`

## Purpose

Represents SQL Server views in the DuckDB catalog, providing read-only access to view data and blocking write operations.

## Interface

```cpp
namespace duckdb {

//! Represents a SQL Server view in DuckDB's catalog
class MSSQLViewEntry : public TableCatalogEntry {
public:
    //! Constructor
    //! @param catalog Parent catalog
    //! @param schema Parent schema
    //! @param info Table info (from SQL Server metadata)
    MSSQLViewEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info);

    // ─────────────────────────────────────────────────────────────────
    // Catalog Entry Overrides
    // ─────────────────────────────────────────────────────────────────

    //! Get catalog entry type (returns VIEW_ENTRY)
    CatalogType GetCatalogType() const override;

    //! Check if this entry supports write operations
    //! @return Always returns false for views
    bool SupportsWrite() const override;

    // ─────────────────────────────────────────────────────────────────
    // Table Scan (Read Operations)
    // ─────────────────────────────────────────────────────────────────

    //! Get table function for scanning this view
    //! @return mssql_scan function configured for this view
    TableFunction GetScanFunction(ClientContext &context,
                                  unique_ptr<FunctionData> &bind_data) override;

    //! Get statistics for this view
    //! @param context Client context
    //! @param column_ids Columns to get statistics for
    //! @return Statistics or nullptr
    unique_ptr<BaseStatistics> GetStatistics(ClientContext &context,
                                             column_t column_id) override;

    // ─────────────────────────────────────────────────────────────────
    // Write Operations (All Blocked)
    // ─────────────────────────────────────────────────────────────────

    //! Get insert function - throws error
    TableFunction GetInsertFunction() override;

    //! Get update function - throws error
    TableFunction GetUpdateFunction() override;

    //! Get delete function - throws error
    TableFunction GetDeleteFunction() override;

private:
    //! Throw read-only error for write operations
    [[noreturn]] void ThrowViewReadOnlyError() const;
};

} // namespace duckdb
```

## Behavior

### Read Operations

- **SELECT**: Works normally via `mssql_scan` table function
- **GetStatistics**: Returns row count from `sys.dm_db_partition_stats` (same as tables)

### Write Operations (All Blocked)

| Operation | Error Message |
| --------- | ------------- |
| INSERT | `MSSQL view '[schema].[view]' is read-only` |
| UPDATE | `MSSQL view '[schema].[view]' is read-only` |
| DELETE | `MSSQL view '[schema].[view]' is read-only` |

### DDL Operations

- DROP VIEW: Not supported (out of scope for this feature)
- ALTER VIEW: Not supported (out of scope)
- Views are not modifiable via DuckDB DDL

## Discovery

Views are discovered from `sys.objects` during metadata cache load:

```sql
SELECT
  s.name AS schema_name,
  o.name AS object_name,
  o.type AS object_type
FROM sys.objects o
JOIN sys.schemas s ON s.schema_id = o.schema_id
WHERE o.type IN ('U', 'V')  -- U=Table, V=View
  AND s.name = @schema;
```

- `type = 'U'` → Create `MSSQLTableEntry`
- `type = 'V'` → Create `MSSQLViewEntry`

## Comparison: Table vs View Entry

| Aspect | MSSQLTableEntry | MSSQLViewEntry |
| ------ | --------------- | -------------- |
| CatalogType | TABLE_ENTRY | VIEW_ENTRY |
| SupportsWrite() | true | false |
| SELECT | mssql_scan | mssql_scan |
| INSERT | Allowed (future) | Error |
| UPDATE | Allowed (future) | Error |
| DELETE | Allowed (future) | Error |
| Statistics | Row count | Row count |

## Usage in SHOW Commands

```sql
-- Shows only tables (not views)
SHOW TABLES FROM mssql.dbo;

-- Shows only views
SHOW VIEWS FROM mssql.dbo;

-- Shows all objects
SHOW ALL TABLES FROM mssql.dbo;
```

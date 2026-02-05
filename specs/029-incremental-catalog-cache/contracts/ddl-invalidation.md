# API Contract: DDL Point Invalidation

**Feature**: 029-incremental-catalog-cache
**Version**: 1.0.0
**Date**: 2026-02-05

## Overview

When DDL operations (CREATE/DROP/ALTER TABLE, CREATE/DROP SCHEMA) are executed via DuckDB Catalog commands, only the affected cache entries are invalidated rather than the entire cache.

## Invalidation Matrix

| DDL Operation | DuckDB Command | Entry Point | Cache Invalidation |
|---------------|----------------|-------------|-------------------|
| CREATE TABLE | `CREATE TABLE sqlserver.schema.table (...)` | `MSSQLSchemaEntry::CreateTable()` | `InvalidateSchema(schema)` |
| DROP TABLE | `DROP TABLE sqlserver.schema.table` | `MSSQLSchemaEntry::DropEntry()` | Remove table + `InvalidateSchema(schema)` |
| ALTER TABLE | `ALTER TABLE sqlserver.schema.table ...` | `MSSQLSchemaEntry::Alter()` | `InvalidateTable(schema, table)` |
| CREATE SCHEMA | `CREATE SCHEMA sqlserver.schema` | `MSSQLCatalog::CreateSchema()` | `InvalidateAll()` (schema list) |
| DROP SCHEMA | `DROP SCHEMA sqlserver.schema` | `MSSQLCatalog::DropSchema()` | Remove schema from cache |

## Detailed Behavior

### CREATE TABLE

**Flow**:
```
User: CREATE TABLE sqlserver.dbo.newtable (id INT, name VARCHAR(100))
    ↓
MSSQLSchemaEntry::CreateTable(BoundCreateTableInfo)
    ↓
1. CheckWriteAccess("CREATE TABLE")
2. TranslateCreateTable() → T-SQL
3. ExecuteDDL(context, tsql)
4. InvalidateSchema("dbo")          ← Point invalidation
5. Return new table entry
```

**Cache State After**:
- `dbo.tables_load_state = NOT_LOADED`
- Other schemas: unchanged
- Column metadata for existing tables: unchanged

**Next Access to `dbo`**:
- Table list reloaded (includes new table)
- Columns for `newtable` loaded on first query

---

### DROP TABLE

**Flow**:
```
User: DROP TABLE sqlserver.dbo.oldtable
    ↓
MSSQLSchemaEntry::DropEntry(CatalogTransaction, DropInfo)
    ↓
1. CheckWriteAccess("DROP TABLE")
2. TranslateDropTable() → T-SQL
3. ExecuteDDL(context, tsql)
4. Remove "oldtable" from schema's table map
5. InvalidateSchema("dbo")          ← Point invalidation
```

**Cache State After**:
- `oldtable` removed from `dbo.tables`
- `dbo.tables_load_state = NOT_LOADED`
- Other schemas: unchanged

**Why InvalidateSchema?**:
- Ensures table list is refreshed on next access
- Handles edge cases where SQL Server may create related objects

---

### ALTER TABLE

**Flow**:
```
User: ALTER TABLE sqlserver.dbo.orders ADD customer_id INT
    ↓
MSSQLSchemaEntry::Alter(CatalogTransaction, AlterInfo)
    ↓
1. CheckWriteAccess("ALTER TABLE")
2. TranslateAlterTable() → T-SQL
3. ExecuteDDL(context, tsql)
4. InvalidateTable("dbo", "orders")  ← Point invalidation
```

**Cache State After**:
- `dbo.orders.columns_load_state = NOT_LOADED`
- `dbo.tables_load_state`: unchanged (table list valid)
- Other tables: unchanged

**Next Access to `dbo.orders`**:
- Columns reloaded (includes new column)
- No table list refresh needed

---

### CREATE SCHEMA

**Flow**:
```
User: CREATE SCHEMA sqlserver.reporting
    ↓
MSSQLCatalog::CreateSchema(CatalogTransaction, CreateSchemaInfo)
    ↓
1. CheckWriteAccess("CREATE SCHEMA")
2. TranslateCreateSchema() → T-SQL
3. ExecuteDDL(context, tsql)
4. InvalidateAll()                   ← Full invalidation
```

**Cache State After**:
- `schemas_load_state_ = NOT_LOADED`
- All schemas' `tables_load_state = NOT_LOADED`
- All tables' `columns_load_state = NOT_LOADED`

**Why Full Invalidation?**:
- Schema list must be refreshed to include new schema
- Simpler than selective schema list update

---

### DROP SCHEMA

**Flow**:
```
User: DROP SCHEMA sqlserver.staging
    ↓
MSSQLCatalog::DropSchema(ClientContext, DropInfo)
    ↓
1. CheckWriteAccess("DROP SCHEMA")
2. TranslateDropSchema() → T-SQL
3. ExecuteDDL(context, tsql)
4. Remove "staging" from schemas_ map
5. InvalidateAll()                   ← Full invalidation
```

**Cache State After**:
- `staging` removed from `schemas_`
- `schemas_load_state_ = NOT_LOADED`
- All remaining schemas refreshed on next access

---

## Non-DDL Operations

### `mssql_exec()`

```sql
-- Raw SQL execution does NOT trigger point invalidation
SELECT mssql_exec('sqlserver', 'ALTER TABLE dbo.orders ADD notes VARCHAR(MAX)');
```

**Cache State After**: **Unchanged**

**Rationale**:
- Parsing arbitrary SQL is out of scope
- User must call `mssql_refresh_cache()` manually if needed

---

### `mssql_refresh_cache()`

```sql
-- Full cache refresh
SELECT mssql_refresh_cache('sqlserver');
```

**Flow**:
```
mssql_refresh_cache(context_name)
    ↓
MSSQLCatalog::GetMetadataCache().Refresh(conn, collation)
    ↓
Full eager load of all schemas, tables, columns
```

**Use Cases**:
- After `mssql_exec()` DDL
- After external schema changes (SSMS, other tools)
- Force refresh regardless of TTL

---

## Entry Point Modifications

### MSSQLSchemaEntry::CreateTable()

```cpp
optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateTable(CatalogTransaction transaction,
                                                          BoundCreateTableInfo &info) {
    auto &mssql_catalog = GetMSSQLCatalog();
    mssql_catalog.CheckWriteAccess("CREATE TABLE");

    // Generate and execute T-SQL
    string tsql = MSSQLDDLTranslator::TranslateCreateTable(name, info);
    mssql_catalog.ExecuteDDL(context, tsql);

    // Point invalidation: only this schema's table list
    mssql_catalog.GetMetadataCache().InvalidateSchema(name);

    // Ensure tables reloaded for this schema
    // (connection acquired internally)
    return tables_.GetEntry(context, info.Base().table);
}
```

### MSSQLSchemaEntry::DropEntry()

```cpp
void MSSQLSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
    auto &mssql_catalog = GetMSSQLCatalog();
    mssql_catalog.CheckWriteAccess("DROP TABLE");

    string tsql = MSSQLDDLTranslator::TranslateDropTable(name, info.name);
    mssql_catalog.ExecuteDDL(context, tsql);

    // Point invalidation
    mssql_catalog.GetMetadataCache().InvalidateSchema(name);

    // Invalidate table set to force reload
    tables_.Invalidate();
}
```

### MSSQLSchemaEntry::Alter()

```cpp
void MSSQLSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
    auto &mssql_catalog = GetMSSQLCatalog();
    mssql_catalog.CheckWriteAccess("ALTER TABLE");

    string tsql = MSSQLDDLTranslator::TranslateAlterTable(name, info);
    mssql_catalog.ExecuteDDL(context, tsql);

    // Point invalidation: only this table's columns
    auto &alter_table = info.Cast<AlterTableInfo>();
    mssql_catalog.GetMetadataCache().InvalidateTable(name, alter_table.table);
}
```

---

## Testing Verification

### Test: CREATE TABLE Point Invalidation

```sql
-- Setup: cache dbo schema
SELECT * FROM sqlserver.dbo.existing_table LIMIT 1;

-- Verify dbo.tables_load_state = LOADED
-- Verify sales.tables_load_state = NOT_LOADED (never accessed)

-- Create new table
CREATE TABLE sqlserver.dbo.newtable (id INT);

-- Verify:
-- dbo.tables_load_state = NOT_LOADED (invalidated)
-- sales.tables_load_state = NOT_LOADED (unchanged)
-- existing_table columns still cached

-- Access new table
SELECT * FROM sqlserver.dbo.newtable;
-- → Triggers dbo table list reload + newtable column load
```

### Test: ALTER TABLE Point Invalidation

```sql
-- Setup: cache orders table
SELECT * FROM sqlserver.dbo.orders LIMIT 1;

-- Verify dbo.orders.columns_load_state = LOADED

-- Alter table
ALTER TABLE sqlserver.dbo.orders ADD notes VARCHAR(100);

-- Verify:
-- dbo.orders.columns_load_state = NOT_LOADED (invalidated)
-- dbo.tables_load_state = LOADED (unchanged)
-- dbo.customers.columns_load_state = LOADED (unchanged if cached)

-- Query altered table
SELECT * FROM sqlserver.dbo.orders LIMIT 1;
-- → Triggers only orders column reload
```

# Research: Catalog-Driven DDL and Statistics

**Feature**: 008-catalog-ddl-statistics
**Date**: 2026-01-18

## 1. DuckDB Catalog API for DDL Operations

### Decision: Use DuckDB's Catalog Hook Methods

**Rationale**: DuckDB's Catalog class provides virtual methods that extensions can override to intercept DDL operations. The MSSQL extension already has `MSSQLCatalog` and `MSSQLSchemaEntry` with placeholder implementations that throw "not supported" errors.

**Alternatives Considered**:
- SQL string parsing: Rejected per FR-004 requirement
- Custom DDL functions: Rejected - breaks DuckDB-native UX principle

### DDL Hook Methods to Implement

**MSSQLCatalog** (in `mssql_catalog.hpp`):

```cpp
// Schema operations
optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction,
                                         CreateSchemaInfo &info);
void DropSchema(ClientContext &context, DropInfo &info);
```

**MSSQLSchemaEntry** (in `mssql_schema_entry.hpp`):

```cpp
// Table operations
optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction,
                                        BoundCreateTableInfo &info);
void DropEntry(ClientContext &context, DropInfo &info);

// Alter operations
void Alter(CatalogTransaction transaction, AlterInfo &info);
```

### AlterInfo Subclasses

DuckDB uses polymorphism for ALTER operations:

| AlterInfo Subclass | DDL Operation |
|-------------------|---------------|
| RenameTableInfo | RENAME TABLE |
| RenameColumnInfo | RENAME COLUMN |
| AddColumnInfo | ADD COLUMN |
| RemoveColumnInfo | DROP COLUMN |
| ChangeColumnTypeInfo | ALTER COLUMN type |
| SetNotNullInfo | ALTER COLUMN NOT NULL |
| DropNotNullInfo | ALTER COLUMN NULL |

## 2. T-SQL Translation Patterns

### Decision: Create MSSQLDDLTranslator Module

**Rationale**: Centralizes all DDL-to-T-SQL translation logic in one place, making it testable and maintainable.

### Identifier Quoting

SQL Server uses square brackets with `]` escaped as `]]`:

```cpp
string QuoteIdentifier(const string &identifier) {
    string result = "[";
    for (char c : identifier) {
        if (c == ']') result += "]]";
        else result += c;
    }
    result += "]";
    return result;
}
```

### T-SQL Templates

| Operation | T-SQL Template |
|-----------|----------------|
| CREATE SCHEMA | `CREATE SCHEMA [schema];` |
| DROP SCHEMA | `DROP SCHEMA [schema];` |
| CREATE TABLE | `CREATE TABLE [schema].[table] ([col] type [NULL\|NOT NULL], ...);` |
| DROP TABLE | `DROP TABLE [schema].[table];` |
| RENAME TABLE | `EXEC sp_rename N'[schema].[old]', N'new';` |
| ADD COLUMN | `ALTER TABLE [schema].[table] ADD [col] type [NULL\|NOT NULL];` |
| DROP COLUMN | `ALTER TABLE [schema].[table] DROP COLUMN [col];` |
| RENAME COLUMN | `EXEC sp_rename N'[schema].[table].[old]', N'new', N'COLUMN';` |
| ALTER TYPE | `ALTER TABLE [schema].[table] ALTER COLUMN [col] type [NULL\|NOT NULL];` |

## 3. Type Mapping (DuckDB to SQL Server)

### Decision: Extend MSSQLColumnInfo with Reverse Mapping

**Rationale**: Existing `MSSQLColumnInfo::MapToLogicalType()` handles SQL Server → DuckDB. Need reverse for DDL.

### Type Mapping Table

| DuckDB LogicalType | SQL Server Type |
|-------------------|-----------------|
| BOOLEAN | BIT |
| TINYINT | TINYINT |
| SMALLINT | SMALLINT |
| INTEGER | INT |
| BIGINT | BIGINT |
| UTINYINT | TINYINT |
| USMALLINT | INT |
| UINTEGER | BIGINT |
| UBIGINT | DECIMAL(20,0) |
| FLOAT | REAL |
| DOUBLE | FLOAT |
| DECIMAL(p,s) | DECIMAL(p,s) (clamped to 38,38) |
| VARCHAR | NVARCHAR(n) or NVARCHAR(MAX) |
| BLOB | VARBINARY(MAX) |
| DATE | DATE |
| TIME | TIME(7) |
| TIMESTAMP | DATETIME2(6) |
| TIMESTAMP_TZ | DATETIMEOFFSET(7) |
| UUID | UNIQUEIDENTIFIER |

**Note**: String types default to NVARCHAR for Unicode safety per FR-020.

## 4. Statistics from SQL Server DMVs

### Decision: Use sys.dm_db_partition_stats for Row Count

**Rationale**: This DMV provides row counts without requiring elevated permissions or full table scans.

### Row Count Query

```sql
SELECT SUM(row_count) AS row_count
FROM sys.dm_db_partition_stats
WHERE object_id = OBJECT_ID(N'[schema].[table]')
  AND (index_id = 0 OR index_id = 1);
```

- `index_id = 0`: Heap (no clustered index)
- `index_id = 1`: Clustered index

### Optional Column Statistics (Level 1+)

```sql
-- Find statistics name for column
SELECT s.name AS stats_name
FROM sys.stats s
JOIN sys.stats_columns sc ON s.stats_id = sc.stats_id AND s.object_id = sc.object_id
JOIN sys.columns c ON sc.column_id = c.column_id AND sc.object_id = c.object_id
WHERE s.object_id = OBJECT_ID(N'[schema].[table]')
  AND c.name = N'column_name';

-- Get histogram (requires DBCC permission)
DBCC SHOW_STATISTICS (N'[schema].[table]', N'stats_name') WITH HISTOGRAM;
```

### Statistics Cache Design

Extend `MSSQLMetadataCache` or create separate `MSSQLStatisticsCache`:

- Key: `(schema, table)` or `(schema, table, column)`
- Value: `{row_count, min, max, ndv, timestamp}`
- TTL: Configurable via `mssql_statistics_cache_ttl_seconds` (default: 300)

## 5. Table vs View Distinction

### Decision: Query sys.objects for Object Type

**Rationale**: SQL Server's `sys.objects.type` column provides definitive object classification.

### Discovery Query

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

### Implementation

Modify `MSSQLMetadataCache::LoadTables()` to:

1. Store object type in metadata
2. Create `MSSQLTableEntry` for type='U'
3. Create `MSSQLViewEntry` for type='V'

`MSSQLViewEntry` extends `TableCatalogEntry` but:

- Returns `CatalogType::VIEW_ENTRY`
- Throws on write operations (INSERT/UPDATE/DELETE)

## 6. READ_ONLY Mode Implementation

### Decision: Store Access Mode in MSSQLCatalog

**Rationale**: DuckDB's ATTACH syntax supports `READ_ONLY` parameter. Store flag in catalog and check before DDL.

### Implementation Pattern

```cpp
// In MSSQLCatalog
AccessMode access_mode_;  // Set during ATTACH

void CheckWriteAccess() {
    if (access_mode_ == AccessMode::READ_ONLY) {
        throw CatalogException("Cannot modify MSSQL catalog: attached in read-only mode");
    }
}

// In all DDL hooks
optional_ptr<CatalogEntry> CreateTable(...) {
    CheckWriteAccess();
    // ... proceed with DDL
}
```

### mssql_exec Blocking

```cpp
// mssql_exec function
if (catalog.IsReadOnly()) {
    throw InvalidInputException("Cannot execute mssql_exec: catalog is read-only");
}
```

## 7. Cache Invalidation Strategy

### Decision: Invalidate Affected Cache Entries After DDL

**Rationale**: Ensures catalog reflects actual SQL Server state.

### Invalidation Rules

| DDL Operation | Cache Invalidation |
|--------------|-------------------|
| CREATE SCHEMA | Schema list cache |
| DROP SCHEMA | Schema list cache |
| CREATE TABLE | Schema's table set |
| DROP TABLE | Schema's table set |
| RENAME TABLE | Both old and new names in table set |
| ALTER TABLE | Table entry + statistics cache |

### Implementation

```cpp
// After successful DDL
metadata_cache_->InvalidateSchema(schema_name);
// or more granular:
metadata_cache_->InvalidateTable(schema_name, table_name);
```

## 8. Error Handling

### Decision: Surface SQL Server Errors with Context

**Rationale**: FR-039 to FR-041 require operation context in error messages.

### Error Message Format

```
MSSQL DDL error (CREATE_TABLE): [schema].[table]
SQL Server error 2714, state 6, class 16: There is already an object named 'table' in the database.
```

### Implementation

```cpp
try {
    ExecuteTSQL(connection, sql);
} catch (const TdsException &e) {
    throw CatalogException(
        "MSSQL DDL error (%s): [%s].[%s]\nSQL Server error %d, state %d, class %d: %s",
        operation_name, schema, table,
        e.error_number(), e.state(), e.severity(), e.message()
    );
}
```

## Summary of Research Findings

| Topic | Decision | Key Insight |
|-------|----------|-------------|
| DDL Hooks | Override Catalog/SchemaEntry methods | DuckDB provides structured DDL data, not raw SQL |
| Translation | MSSQLDDLTranslator module | Centralized, testable T-SQL generation |
| Type Mapping | Extend MSSQLColumnInfo | Reverse mapping for DDL, NVARCHAR default |
| Row Count | sys.dm_db_partition_stats | No special permissions required |
| Column Stats | DBCC SHOW_STATISTICS | Opt-in, requires permissions |
| Table/View | sys.objects.type | 'U' for table, 'V' for view |
| READ_ONLY | Access mode flag in catalog | Block all DDL + mssql_exec |
| Cache | Invalidate on DDL success | Per-schema or per-table granularity |
| Errors | Include operation + SQL Server details | Structured error messages |

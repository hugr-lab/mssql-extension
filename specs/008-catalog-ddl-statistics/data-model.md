# Data Model: Catalog-Driven DDL and Statistics

**Feature**: 008-catalog-ddl-statistics
**Date**: 2026-01-18

## Entity Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        MSSQLCatalog                              │
│  - access_mode: AccessMode (READ_ONLY | READ_WRITE)             │
│  - metadata_cache: MSSQLMetadataCache                           │
│  - connection_pool: ConnectionPool                              │
├─────────────────────────────────────────────────────────────────┤
│  + CreateSchema(), DropSchema()                                 │
│  + IsReadOnly(), CheckWriteAccess()                             │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      MSSQLSchemaEntry                            │
│  - schema_name: string                                          │
│  - table_set: MSSQLTableSet                                     │
├─────────────────────────────────────────────────────────────────┤
│  + CreateTable(), DropEntry(), Alter()                          │
└─────────────────────────────────────────────────────────────────┘
                              │
                    ┌─────────┴─────────┐
                    ▼                   ▼
┌───────────────────────────┐ ┌───────────────────────────┐
│     MSSQLTableEntry       │ │     MSSQLViewEntry        │
│  - is_view: false         │ │  - is_view: true          │
│  - columns: ColumnInfo[]  │ │  - columns: ColumnInfo[]  │
│  - statistics: TableStats │ │  - statistics: TableStats │
├───────────────────────────┤ ├───────────────────────────┤
│  + GetStatistics()        │ │  + GetStatistics()        │
│  + SupportsWrite() = true │ │  + SupportsWrite() = false│
└───────────────────────────┘ └───────────────────────────┘
```

## Entities

### 1. AccessMode (Enum)

Determines whether the attached catalog allows modifications.

| Value | Description |
|-------|-------------|
| READ_WRITE | Default. DDL and mssql_exec allowed |
| READ_ONLY | SELECT only. DDL and mssql_exec blocked |

**Set at**: ATTACH time via `READ_ONLY` parameter
**Stored in**: MSSQLCatalog

### 2. DDLOperation (Enum)

Identifies the type of DDL operation for error messages and logging.

| Value | Description |
|-------|-------------|
| CREATE_SCHEMA | Creating a new schema |
| DROP_SCHEMA | Dropping an existing schema |
| CREATE_TABLE | Creating a new table |
| DROP_TABLE | Dropping an existing table |
| RENAME_TABLE | Renaming a table |
| ADD_COLUMN | Adding a column to table |
| DROP_COLUMN | Removing a column from table |
| RENAME_COLUMN | Renaming a column |
| ALTER_COLUMN_TYPE | Changing column data type |
| ALTER_COLUMN_NULL | Changing column nullability |

### 3. TableStatistics

Cached statistics for a table, used by DuckDB optimizer.

| Field | Type | Description |
|-------|------|-------------|
| row_count | int64_t | Approximate row count from DMV |
| last_updated | timestamp | When stats were fetched |
| column_stats | map<string, ColumnStats> | Optional per-column stats |

**Lifecycle**:

- Created: On first GetStatistics() call
- Updated: On cache miss (TTL expired)
- Invalidated: After DDL affecting the table

### 4. ColumnStatistics

Optional column-level statistics (when statistics_level >= 1).

| Field | Type | Description |
|-------|------|-------------|
| min_value | Value | Minimum value (numeric/temporal) |
| max_value | Value | Maximum value (numeric/temporal) |
| distinct_count | int64_t | Approximate NDV (level 2) |
| null_count | int64_t | Number of NULL values |
| has_stats | bool | Whether SQL Server has stats for column |

### 5. CatalogObjectKind (Enum)

Distinguishes tables from views in the catalog.

| Value | sys.objects.type | DuckDB Entry Type |
|-------|-----------------|-------------------|
| TABLE | 'U' | MSSQLTableEntry |
| VIEW | 'V' | MSSQLViewEntry |

### 6. TypeMapping

Maps DuckDB LogicalType to SQL Server type string for DDL generation.

| DuckDB Type | SQL Server Type | Notes |
|-------------|-----------------|-------|
| BOOLEAN | BIT | |
| TINYINT | TINYINT | |
| SMALLINT | SMALLINT | |
| INTEGER | INT | |
| BIGINT | BIGINT | |
| UTINYINT | TINYINT | Unsigned, maps to signed |
| USMALLINT | INT | Wider to fit range |
| UINTEGER | BIGINT | Wider to fit range |
| UBIGINT | DECIMAL(20,0) | No native unsigned 64-bit |
| FLOAT | REAL | 32-bit float |
| DOUBLE | FLOAT | 64-bit float |
| DECIMAL(p,s) | DECIMAL(p,s) | Clamp p≤38, s≤p |
| VARCHAR | NVARCHAR(n) | Unicode default |
| VARCHAR(MAX) | NVARCHAR(MAX) | Large text |
| BLOB | VARBINARY(MAX) | Binary data |
| DATE | DATE | |
| TIME | TIME(7) | Max precision |
| TIMESTAMP | DATETIME2(6) | Microsecond precision |
| TIMESTAMP_TZ | DATETIMEOFFSET(7) | With timezone |
| UUID | UNIQUEIDENTIFIER | |

## State Transitions

### MetadataCache States

```
    ┌────────┐
    │ EMPTY  │◄────────────────────────┐
    └───┬────┘                         │
        │ LoadMetadata()               │ Invalidate()
        ▼                              │
    ┌────────┐                    ┌────┴───┐
    │LOADING │                    │INVALID │
    └───┬────┘                    └────────┘
        │ Success                      ▲
        ▼                              │ TTL expired
    ┌────────┐      Refresh()     ┌────┴───┐
    │ LOADED │───────────────────►│ STALE  │
    └────────┘                    └────────┘
```

### Statistics Cache Flow

```
GetStatistics(table)
        │
        ▼
   ┌─────────────────┐
   │ Cache has entry │──No──►┌─────────────────┐
   │   for table?    │       │ Fetch from MSSQL│
   └────────┬────────┘       │ via DMV query   │
           Yes               └────────┬────────┘
            │                         │
            ▼                         │
   ┌─────────────────┐                │
   │  TTL expired?   │──No──►Return   │
   └────────┬────────┘  cached        │
           Yes                        │
            │                         │
            ▼                         │
   ┌─────────────────┐                │
   │ Fetch fresh     │◄───────────────┘
   │ stats from MSSQL│
   └────────┬────────┘
            │
            ▼
   ┌─────────────────┐
   │ Update cache    │
   │ Return stats    │
   └─────────────────┘
```

## Validation Rules

### Identifier Validation

- Schema names: Must be valid SQL Server identifier (≤128 chars)
- Table names: Must be valid SQL Server identifier (≤128 chars)
- Column names: Must be valid SQL Server identifier (≤128 chars)
- Special characters: Escaped via `[` `]` with `]` → `]]`

### Type Validation

- DECIMAL precision: 1 ≤ p ≤ 38
- DECIMAL scale: 0 ≤ s ≤ p
- VARCHAR length: 1 ≤ n ≤ 4000 or MAX
- VARBINARY length: 1 ≤ n ≤ 8000 or MAX

### DDL Validation

- CREATE TABLE: At least one column required
- DROP SCHEMA: Schema must be empty (SQL Server enforces)
- ALTER COLUMN TYPE: Type must be compatible (SQL Server enforces)
- ADD COLUMN NOT NULL: Must provide default for non-empty tables

## Relationships

```
MSSQLCatalog 1 ──────────── * MSSQLSchemaEntry
     │                              │
     │                              │
     └─── access_mode               └─── table_set
          metadata_cache                     │
          connection_pool                    │
                                            │
                           ┌────────────────┴────────────────┐
                           │                                 │
                           ▼                                 ▼
                    MSSQLTableEntry                  MSSQLViewEntry
                           │                                 │
                           │                                 │
                           └────── columns[]                 └────── columns[]
                                   statistics                        statistics
```

## Configuration Settings

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| mssql_enable_statistics | bool | true | Enable statistics collection |
| mssql_statistics_level | int | 0 | 0=rowcount, 1=+histogram, 2=+NDV |
| mssql_statistics_use_dbcc | bool | false | Allow DBCC commands |
| mssql_statistics_cache_ttl_seconds | int | 300 | Statistics cache TTL |

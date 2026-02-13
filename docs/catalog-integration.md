# Catalog Integration

The catalog layer integrates with DuckDB's catalog system to expose SQL Server schemas, tables, and columns as native DuckDB objects. Source files are in `src/catalog/` and `src/include/catalog/`.

## Class Hierarchy

```
duckdb::Catalog
  └── MSSQLCatalog
        ├── MSSQLMetadataCache (metadata with TTL)
        ├── MSSQLCatalogFilter (regex-based visibility)
        ├── MSSQLStatisticsProvider (row count cache)
        └── schema_entries_: Map<name, MSSQLSchemaEntry>

duckdb::SchemaCatalogEntry
  └── MSSQLSchemaEntry
        └── MSSQLTableSet (lazy-loaded table collection)

duckdb::TableCatalogEntry
  └── MSSQLTableEntry
        ├── mssql_columns_: vector<MSSQLColumnInfo>
        ├── pk_info_: PrimaryKeyInfo (lazy-loaded)
        └── object_type_: TABLE or VIEW
```

## MSSQLCatalog

**Files**: `src/catalog/mssql_catalog.cpp`, `src/include/catalog/mssql_catalog.hpp`

Top-level catalog for an attached MSSQL database. One instance per `ATTACH ... TYPE mssql` call.

### DuckDB Overrides

| Method | Behavior |
|---|---|
| `GetCatalogType()` | Returns `"mssql"` |
| `Initialize(bool)` | Retrieves connection pool, queries database collation |
| `LookupSchema(txn, info)` | Schema lookup via cache + lazy creation |
| `ScanSchemas(context, callback)` | Iterate all cached schemas |
| `CreateSchema(txn, info)` | Execute `CREATE SCHEMA` on SQL Server |
| `DropSchema(context, info)` | Execute `DROP SCHEMA` on SQL Server |
| `PlanInsert(context, info, plan)` | Create `MSSQLPhysicalInsert` operator |
| `PlanUpdate(context, info, plan)` | Create `MSSQLPhysicalUpdate` operator |
| `PlanDelete(context, info, plan)` | Create `MSSQLPhysicalDelete` operator |
| `PlanCreateTableAs(...)` | Throws (not supported) |
| `BindCreateIndex(...)` | Throws (not supported) |
| `InMemory()` | Returns `false` |
| `GetDBPath()` | Returns connection info string |
| `OnDetach(context)` | Shuts down pool, removes from manager |

### MSSQL-Specific Methods

| Method | Purpose |
|---|---|
| `GetConnectionPool()` | Access the TDS connection pool |
| `EnsureCacheLoaded(context)` | Load or refresh metadata cache if needed |
| `InvalidateMetadataCache()` | Force cache refresh on next access |
| `GetMetadataCache()` | Direct cache access |
| `GetCatalogFilter()` | Access catalog visibility filter |
| `GetStatisticsProvider()` | Access statistics provider |

### Schema Lookup Flow

```
LookupSchema(txn, info)
  ├─ EnsureCacheLoaded(context)
  │   ├─ Is cache EMPTY, STALE, or INVALID? → Refresh()
  │   └─ Otherwise: use cached data
  ├─ Check cache for schema name
  └─ GetOrCreateSchemaEntry(name)
       ├─ Lock schema_mutex_
       ├─ Check schema_entries_ map
       └─ Create MSSQLSchemaEntry if not found
```

## MSSQLSchemaEntry

**Files**: `src/catalog/mssql_schema_entry.cpp`, `src/include/catalog/mssql_schema_entry.hpp`

Represents a SQL Server schema (e.g., `dbo`, `sales`). Manages table lookup and DDL operations.

### DuckDB Overrides

| Method | Behavior |
|---|---|
| `LookupEntry(txn, info)` | Delegates to MSSQLTableSet::GetEntry() |
| `Scan(context, type, callback)` | Iterate tables via MSSQLTableSet::Scan() |
| `CreateTable(txn, info)` | Execute `CREATE TABLE` on SQL Server |
| `Alter(txn, info)` | Execute `ALTER TABLE` on SQL Server |
| `DropEntry(context, info)` | Execute `DROP TABLE/VIEW` on SQL Server |
| `CreateFunction/Index/View/...` | Throw `NotImplementedException` |

## MSSQLTableEntry

**Files**: `src/catalog/mssql_table_entry.cpp`, `src/include/catalog/mssql_table_entry.hpp`

Represents a SQL Server table or view with column metadata, primary key info, and scan function binding.

### DuckDB Overrides

| Method | Behavior |
|---|---|
| `GetScanFunction(context, data)` | Returns table function for scanning this table |
| `GetStatistics(context, col)` | Returns nullptr (statistics via provider) |
| `GetStorageInfo(context)` | Returns approximate row count |
| `BindUpdateConstraints(...)` | Validates PK exists for UPDATE |
| `GetVirtualColumns()` | Exposes `rowid` virtual column if PK exists |

### Column Metadata

```cpp
struct MSSQLColumnInfo {
    string name;
    int32_t column_id;         // 1-based ordinal
    string sql_type_name;      // e.g., "varchar", "int"
    LogicalType duckdb_type;   // Mapped DuckDB type

    int16_t max_length;        // -1 for MAX types
    uint8_t precision;
    uint8_t scale;
    bool is_nullable;

    string collation_name;
    bool is_case_sensitive;    // Derived from collation (_CS_ or _BIN)
    bool is_unicode;           // NVARCHAR/NCHAR/NTEXT
    bool is_utf8;              // _UTF8 collation
};
```

### Primary Key and Rowid Support

Primary key metadata is lazy-loaded on first access via `EnsurePKLoaded()`.

**Discovery query** joins `sys.key_constraints`, `sys.indexes`, `sys.index_columns`, `sys.columns`, and `sys.types` to find the PK constraint and its columns ordered by `key_ordinal`.

```cpp
struct PrimaryKeyInfo {
    bool loaded = false;        // Discovery attempted?
    bool exists = false;        // Table has a PK?
    vector<PKColumnInfo> columns;  // Ordered by key_ordinal
    LogicalType rowid_type;     // Scalar or STRUCT
};
```

**Rowid type computation**:
- Single-column PK: rowid is the PK column's DuckDB type directly
- Composite PK: rowid is a `STRUCT` with one field per PK column

**Virtual column**: When a PK exists, `GetVirtualColumns()` returns a `rowid` column, enabling DuckDB's UPDATE/DELETE operators to identify rows.

## MSSQLTableSet

**Files**: `src/catalog/mssql_table_set.cpp`, `src/include/catalog/mssql_table_set.hpp`

Lazy-loaded collection of table entries per schema. Implements multi-level lazy loading to avoid loading all tables at once.

### Entry Access Strategy

`GetEntry(name)` uses a cascading lookup to minimize SQL Server round trips:

1. **Cache hit** — check `entries_` map (fast path)
2. **Already attempted** — check `attempted_tables_` set (avoid retries)
3. **Fully loaded** — if `is_fully_loaded_`, table doesn't exist
4. **Table filter** — check `MSSQLCatalogFilter` (skip filtered-out tables)
5. **Known names** — if `names_loaded_` and name not in `known_table_names_`, table doesn't exist
6. **Load single entry** — `LoadSingleEntry()` queries only this table's metadata

### Table Name Preloading

`EnsureNamesLoaded()` loads only table names (no column metadata) for a schema using a lightweight query. This populates `known_table_names_` which acts as a negative lookup cache — if a table name isn't in this set, it doesn't exist and we skip the expensive column query.

### Bulk Scan

`Scan()` triggers `LoadAllTableMetadata()` which loads all tables and columns for the schema from cache (or SQL Server if not cached). This is used for `duckdb_tables()` and similar catalog browsing queries.

## MSSQLMetadataCache

**Files**: `src/catalog/mssql_metadata_cache.cpp`, `src/include/catalog/mssql_metadata_cache.hpp`

In-memory cache of schema, table, and column metadata with optional TTL-based expiration.

### Incremental Cache with Lazy Loading

The metadata cache uses a three-level lazy loading strategy to minimize SQL Server queries:

```
Level 1: Schema List
  │ EnsureSchemasLoaded() - loads schema names from sys.schemas
  ▼
Level 2: Table List (per schema)
  │ EnsureTablesLoaded(schema) - loads table names from sys.objects
  ▼
Level 3: Column Metadata (per table)
    EnsureColumnsLoaded(schema, table) - loads columns from sys.columns
```

Each level has its own `CacheLoadState`:
- `NOT_LOADED` (0) — never loaded or invalidated
- `LOADING` (1) — currently being loaded (by another thread)
- `LOADED` (2) — successfully loaded and valid
- `STALE` (3) — TTL expired, needs refresh on next access

### Lazy Loading Behavior

1. **On ATTACH**: No metadata is loaded immediately
2. **On first schema access**: Only schema list is loaded
3. **On first table access**: Only that schema's table list is loaded
4. **On first query**: Only that table's column metadata is loaded

This reduces ATTACH time from seconds to milliseconds for databases with many schemas/tables.

### Point Invalidation

DDL operations trigger targeted cache invalidation instead of full refresh:

| Operation | Invalidation |
|-----------|--------------|
| `CREATE TABLE` | `InvalidateSchema(schema)` — reload table list |
| `DROP TABLE` | `InvalidateSchema(schema)` — reload table list |
| `ALTER TABLE ADD/DROP COLUMN` | `InvalidateTable(schema, table)` — reload columns |
| `CREATE SCHEMA` | `InvalidateAll()` — reload schema list |
| `DROP SCHEMA` | `InvalidateAll()` — reload schema list |

Point invalidation allows immediate visibility of changes without full cache refresh.

### Global Cache State Machine (Backward Compatibility)

```
EMPTY (initial)
  │ Refresh()
  ▼
LOADING (fetching from SQL Server)
  ├─ Success → LOADED
  └─ Error → INVALID
       │ Refresh()
       ▼
     LOADING ...

LOADED
  ├─ TTL expired → STALE → Refresh() → LOADING → ...
  └─ Invalidate() → INVALID → Refresh() → LOADING → ...
```

**States** (`MSSQLCacheState`):
- `EMPTY` (0) — no metadata loaded
- `LOADING` (1) — currently fetching from SQL Server
- `LOADED` (2) — metadata valid
- `STALE` (3) — TTL expired, needs refresh
- `INVALID` (4) — manually invalidated

### Full Refresh Process

`Refresh(TdsConnection, database_name)` performs a full cache reload:
1. Set state to `LOADING`
2. Query `sys.schemas` for user schemas
3. For each schema: query `sys.objects` for tables/views with row counts from `sys.partitions`
4. For each table: query `sys.columns` for column metadata
5. Set state to `LOADED`, update `last_refresh_` timestamp
6. On error: set state to `INVALID`, rethrow

### TTL Behavior

- `ttl_seconds_ = 0` (default): manual refresh only via `mssql_refresh_cache()`
- `ttl_seconds_ > 0`: automatic refresh when cache level's TTL expires

### Data Structures

```cpp
struct MSSQLTableMetadata {
    string name;
    MSSQLObjectType object_type;  // TABLE or VIEW
    vector<MSSQLColumnInfo> columns;
    idx_t approx_row_count;
};

struct MSSQLSchemaMetadata {
    string name;
    unordered_map<string, MSSQLTableMetadata> tables;
};
```

### Thread Safety

All cache access is protected by `std::mutex`. The mutex is held during the entire `Refresh()` operation to prevent concurrent refresh attempts.

## MSSQLStatisticsProvider

**Files**: `src/catalog/mssql_statistics.cpp`, `src/include/catalog/mssql_statistics.hpp`

Caches row count statistics from `sys.dm_db_partition_stats` with configurable TTL.

### Methods

| Method | Purpose |
|---|---|
| `GetRowCount(conn, schema, table)` | Cached row count lookup |
| `InvalidateTable(schema, table)` | Remove single entry from cache |
| `InvalidateSchema(schema)` | Remove all entries for a schema |
| `InvalidateAll()` | Clear entire statistics cache |

Statistics are keyed by `"schema.table"` and expire after `cache_ttl_seconds_` (default 300s).

## MSSQLCatalogFilter

**Files**: `src/catalog/mssql_catalog_filter.cpp`, `src/include/catalog/mssql_catalog_filter.hpp`

Regex-based visibility filter for schemas and tables. Configured via `schema_filter` and `table_filter` parameters in secrets, connection strings, or ATTACH options.

### Configuration Sources

Filters can be specified in multiple places (ATTACH options override secret/connection string values):

| Source | Schema Key | Table Key |
|--------|-----------|-----------|
| MSSQL Secret | `schema_filter` | `table_filter` |
| ADO.NET Connection String | `SchemaFilter` | `TableFilter` |
| URI Query Parameters | `schema_filter` | `table_filter` |
| ATTACH Options | `schema_filter` | `table_filter` |

### Filter Matching

- Uses C++ `std::regex` with `std::regex_constants::icase` (case-insensitive)
- Matching is via `std::regex_search` (partial match — pattern doesn't need to match the entire name)
- Use `^` and `$` anchors for exact matching: `^dbo$` matches only "dbo"

### SQL Server-Side Optimization

When possible, regex patterns are converted to SQL Server `LIKE` clauses via `TryRegexToSQLLike()` to filter at the SQL Server level rather than client-side. Simple patterns like `^dbo$` become `s.name = N'dbo'`, and `^(dbo|sales)$` becomes `s.name IN (N'dbo', N'sales')`.

### Integration Points

- `MSSQLCatalog::LookupSchema()` — filters schema lookups
- `MSSQLCatalog::ScanSchemas()` — filters schema enumeration
- `MSSQLTableSet::GetEntry()` — filters table lookups
- `MSSQLMetadataCache::Refresh()` — adds SQL WHERE clauses to metadata queries

## Bulk Preload (mssql_preload_catalog)

**Files**: `src/catalog/mssql_preload_catalog.cpp`, `src/include/catalog/mssql_preload_catalog.hpp`

The `mssql_preload_catalog(catalog_name [, schema_name])` scalar function triggers `MSSQLMetadataCache::BulkLoadAll()` to load all schemas, tables, and columns in bulk.

### Per-Schema Iteration Strategy

`BulkLoadAll()` uses a per-schema iteration strategy instead of a single cross-schema query:

1. **Discover schemas** — lightweight query against `sys.schemas`
2. **Per-schema bulk query** — for each schema, load all tables and columns using `BULK_METADATA_SCHEMA_SQL_TEMPLATE`
3. **Streaming parse** — results are parsed row-by-row, building `MSSQLTableMetadata` entries

This avoids SQL Server tempdb sort spills that occur when `ORDER BY s.name, o.name, c.column_id` operates on millions of rows in a single cross-schema query. Each per-schema query sorts only within one schema's data, which fits in SQL Server's memory grant.

### Statistics Pre-Population

After bulk loading, `mssql_preload_catalog()` also pre-populates the statistics cache with approximate row counts from the bulk query. This avoids per-table DMV queries when DuckDB calls `GetStorageInfo()`.

## Data Flow

### Metadata Discovery

```
ATTACH ... AS context_name (TYPE mssql, SECRET ...)
  │
  ▼
MSSQLCatalog::Initialize()
  ├─ Get connection pool from MssqlPoolManager
  └─ Query database collation

First query: SELECT * FROM context.schema.table
  │
  ▼
LookupSchema() → EnsureCacheLoaded()
  ├─ cache.NeedsRefresh()? → cache.Refresh()
  │   ├─ Query sys.schemas → schema names
  │   ├─ Query sys.objects → table names, types, row counts
  │   └─ Query sys.columns → column metadata
  └─ Create MSSQLSchemaEntry → MSSQLTableSet

MSSQLTableSet::GetEntry(table_name)
  ├─ Check entries_ cache (fast path)
  ├─ Check catalog filter (skip filtered-out tables)
  ├─ Check known_table_names_ (negative lookup cache)
  └─ LoadSingleEntry() → load only this table's metadata

MSSQLTableEntry::GetScanFunction()
  ├─ EnsurePKLoaded() → Query sys.key_constraints
  └─ Return table function with rowid support
```

## Thread Safety Summary

| Component | Mechanism | Protects |
|---|---|---|
| `MSSQLCatalog` | `schema_mutex_` | `schema_entries_` map |
| `MSSQLMetadataCache` | `mutex_` | All cache state and data |
| `MSSQLTableSet` | `load_mutex_` + `entry_mutex_` | Loading state + entries map |
| `MSSQLStatisticsProvider` | `mutex_` | Statistics cache |
| `MSSQLTransaction` | `connection_mutex_` | Pinned connection operations |

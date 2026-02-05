# Catalog Integration

The catalog layer integrates with DuckDB's catalog system to expose SQL Server schemas, tables, and columns as native DuckDB objects. Source files are in `src/catalog/` and `src/include/catalog/`.

## Class Hierarchy

```
duckdb::Catalog
  └── MSSQLCatalog
        ├── MSSQLMetadataCache (metadata with TTL)
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

Lazy-loaded collection of table entries per schema. Uses double-checked locking for thread-safe initialization.

```cpp
void EnsureLoaded(ClientContext &context) {
    if (is_loaded_.load()) return;        // Fast path (atomic)
    std::lock_guard<std::mutex> lock(load_mutex_);
    if (is_loaded_.load()) return;        // Slow path
    LoadEntries(context);
    is_loaded_.store(true);
}
```

`LoadEntries()` reads table metadata from `MSSQLMetadataCache` and creates `MSSQLTableEntry` for each table/view.

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
  ├─ EnsureLoaded() → LoadEntries() from cache
  └─ Create MSSQLTableEntry per table

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

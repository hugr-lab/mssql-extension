# Research: Incremental Catalog Cache

**Feature**: 029-incremental-catalog-cache
**Date**: 2026-02-05
**Status**: Complete

## Executive Summary

The current catalog cache loads ALL metadata (schemas, tables, columns) eagerly during `EnsureCacheLoaded()`. For databases with 500+ tables, this causes:
- Slow initial `ATTACH` time
- Unnecessary memory usage for unaccessed tables
- Full cache refresh on any DDL operation

This research analyzes the existing architecture and defines the approach for incremental lazy loading with granular TTL and point invalidation.

## Current Architecture Analysis

### 1. Metadata Cache Flow

```
ATTACH 'mssql://..' AS sqlserver
    ↓
MSSQLCatalog::Initialize()
    ↓
[catalog_enabled_ = true]
    ↓
First catalog access (e.g., SELECT * FROM sqlserver.dbo.orders)
    ↓
MSSQLCatalog::EnsureCacheLoaded(context)
    ↓
metadata_cache_->NeedsRefresh() || metadata_cache_->IsExpired()
    ↓ [YES]
MSSQLMetadataCache::Refresh(connection, collation)
    ↓
LoadSchemas()     → sys.schemas (1 query)
    ↓
For EACH schema:
  LoadTables()    → sys.objects + sys.partitions (N queries)
      ↓
  For EACH table:
    LoadColumns() → sys.columns + sys.types (N×M queries)
```

**Problem**: For a database with 10 schemas × 50 tables/schema = 500 tables, this executes 1 + 10 + 500 = **511 metadata queries** before any user query runs.

### 2. Cache State Machine (Current)

```
MSSQLCacheState:
    EMPTY → LOADING → LOADED → STALE/INVALID → LOADING → ...
```

States apply to entire cache, not individual schemas/tables.

### 3. Key Classes and Files

| Class | File | Role |
|-------|------|------|
| `MSSQLMetadataCache` | `src/catalog/mssql_metadata_cache.cpp` | Cache storage and refresh logic |
| `MSSQLCatalog` | `src/catalog/mssql_catalog.cpp` | DuckDB catalog integration |
| `MSSQLSchemaEntry` | `src/catalog/mssql_schema_entry.cpp` | Schema DDL entry points |
| `MSSQLTableSet` | `src/catalog/mssql_table_set.cpp` | Table lazy loading (already implemented!) |
| `MSSQLTableEntry` | `src/catalog/mssql_table_entry.cpp` | Table/view metadata |

### 4. Existing Lazy Loading Pattern

`MSSQLTableSet` already implements lazy loading with double-checked locking:

```cpp
void MSSQLTableSet::EnsureLoaded(ClientContext &context) {
    if (is_loaded_.load()) return;              // First check (no lock)

    std::lock_guard<std::mutex> lock(load_mutex_);
    if (is_loaded_.load()) return;              // Second check (locked)

    LoadEntries(context);
    is_loaded_.store(true);
}
```

This pattern should be extended to schema and column loading.

### 5. DDL Entry Points

DDL operations flow through these methods:

| Operation | Entry Point | Current Behavior |
|-----------|-------------|------------------|
| CREATE TABLE | `MSSQLSchemaEntry::CreateTable()` | Invalidates entire cache |
| DROP TABLE | `MSSQLSchemaEntry::DropEntry()` | Invalidates entire cache |
| ALTER TABLE | `MSSQLSchemaEntry::Alter()` | Invalidates entire cache |
| CREATE SCHEMA | `MSSQLCatalog::CreateSchema()` | Invalidates entire cache |
| DROP SCHEMA | `MSSQLCatalog::DropSchema()` | Invalidates entire cache |

All call `InvalidateMetadataCache()` which sets entire cache to INVALID state.

## Proposed Architecture

### 1. Three-Level Lazy Loading

```
Level 0: Schema List (catalog-wide)
    - Loaded on first schema access
    - Contains: schema names only
    - TTL: mssql_catalog_cache_ttl (per-level expiration)

Level 1: Table List (per-schema)
    - Loaded on first table access in schema
    - Contains: table names, object types, approx row counts
    - TTL: mssql_catalog_cache_ttl (per-level expiration)

Level 2: Column Metadata (per-table)
    - Loaded on first query/bind to table
    - Contains: full column metadata
    - TTL: mssql_catalog_cache_ttl (per-level expiration)
```

### 2. Extended State Machine

```cpp
enum class CacheLoadState : uint8_t {
    NOT_LOADED = 0,  // Never loaded
    LOADING = 1,     // Currently loading
    LOADED = 2,      // Loaded and valid
    STALE = 3        // TTL expired, needs refresh
};
```

Applied independently to:
- Catalog-level schema list
- Each schema's table list
- Each table's column metadata

### 3. New Data Structures

```cpp
struct MSSQLSchemaMetadata {
    string name;
    CacheLoadState tables_load_state = CacheLoadState::NOT_LOADED;
    std::chrono::steady_clock::time_point tables_last_refresh;
    unordered_map<string, MSSQLTableMetadata> tables;
    std::mutex load_mutex;  // Per-schema loading synchronization
};

struct MSSQLTableMetadata {
    string name;
    MSSQLObjectType object_type;
    idx_t approx_row_count;

    CacheLoadState columns_load_state = CacheLoadState::NOT_LOADED;
    std::chrono::steady_clock::time_point columns_last_refresh;
    vector<MSSQLColumnInfo> columns;  // Empty until loaded
    std::mutex load_mutex;  // Per-table loading synchronization
};

class MSSQLMetadataCache {
    // Catalog-level
    CacheLoadState schemas_load_state_ = CacheLoadState::NOT_LOADED;
    std::chrono::steady_clock::time_point schemas_last_refresh_;

    // Per-schema data with independent load states
    unordered_map<string, MSSQLSchemaMetadata> schemas_;

    // TTL configuration (from mssql_catalog_cache_ttl)
    int64_t ttl_seconds_ = 0;  // Applies to all levels
};
```

### 4. Loading Sequence

```
Query: SELECT * FROM sqlserver.dbo.orders LIMIT 1

1. MSSQLCatalog::LookupSchema("dbo")
   └─ EnsureSchemasLoaded()
      └─ schemas_load_state_ == NOT_LOADED?
         └─ LoadSchemaList() → sys.schemas
            └─ Creates MSSQLSchemaMetadata for each schema
               └─ tables_load_state = NOT_LOADED (no tables loaded yet)

2. MSSQLSchemaEntry::LookupEntry("orders")
   └─ MSSQLTableSet::GetEntry("orders")
      └─ EnsureTablesLoaded(schema)
         └─ schema.tables_load_state == NOT_LOADED?
            └─ LoadTables(schema) → sys.objects for "dbo" only
               └─ Creates MSSQLTableMetadata for each table
                  └─ columns_load_state = NOT_LOADED

3. MSSQLTableEntry::GetScanFunction() / Bind
   └─ EnsureColumnsLoaded(table)
      └─ table.columns_load_state == NOT_LOADED?
         └─ LoadColumns(schema, table) → sys.columns for "orders" only
```

### 5. Point Invalidation Strategy

| DDL Operation | Invalidation Scope |
|---------------|-------------------|
| CREATE TABLE | Schema's `tables_load_state` → NOT_LOADED |
| DROP TABLE | Remove table from schema, no cascade |
| ALTER TABLE | Table's `columns_load_state` → NOT_LOADED |
| CREATE SCHEMA | Catalog's `schemas_load_state` → NOT_LOADED |
| DROP SCHEMA | Remove schema from cache, no cascade |

**Key insight**: Point invalidation only marks the affected level as needing reload. Other cached data remains valid.

### 6. TTL Configuration

Uses existing `mssql_catalog_cache_ttl` setting for all cache levels:

| Setting | Default | Description |
|---------|---------|-------------|
| `mssql_catalog_cache_ttl` | 0 | TTL for all metadata levels (seconds) |

- `0` = no automatic expiration (manual refresh via `mssql_refresh_cache()` or DDL point invalidation)
- `> 0` = metadata expires after this many seconds; next access triggers refresh of that level only

### 7. Thread Safety

Each level has its own mutex for loading synchronization:
- `MSSQLMetadataCache::schemas_mutex_` - schema list loading
- `MSSQLSchemaMetadata::load_mutex` - per-schema table list loading
- `MSSQLTableMetadata::load_mutex` - per-table column loading

Double-checked locking pattern (from existing `MSSQLTableSet`):
```cpp
void EnsureColumnsLoaded(MSSQLTableMetadata &table) {
    if (table.columns_load_state == CacheLoadState::LOADED &&
        !IsTableExpired(table)) {
        return;  // Fast path, no lock
    }

    std::lock_guard<std::mutex> lock(table.load_mutex);
    if (table.columns_load_state == CacheLoadState::LOADED &&
        !IsTableExpired(table)) {
        return;  // Check again under lock
    }

    table.columns_load_state = CacheLoadState::LOADING;
    LoadColumns(table);
    table.columns_load_state = CacheLoadState::LOADED;
    table.columns_last_refresh = std::chrono::steady_clock::now();
}
```

## Azure SQL Compatibility

Azure SQL Database uses identical `sys.*` catalog views:
- `sys.schemas` - schema discovery
- `sys.objects` - table/view discovery
- `sys.columns` + `sys.types` - column discovery

No Azure-specific changes needed for lazy loading. Test coverage:
- Use `AZURE_SQL_TEST_DSN` environment variable
- Skip tests gracefully when not configured
- Validate lazy loading with Azure's higher latency (lazy loading even more beneficial)

## Backward Compatibility

| Feature | Behavior |
|---------|----------|
| `mssql_catalog_cache_ttl` | Still works as fallback TTL |
| `mssql_refresh_cache()` | Forces full cache reload (all levels) |
| `InvalidateMetadataCache()` | Marks all levels as NOT_LOADED |
| Existing tests | Pass without modification |

## Implementation Approach

### Phase 1: Lazy Loading Foundation (P1 stories)
1. Extend `MSSQLSchemaMetadata` and `MSSQLTableMetadata` with load states
2. Implement `EnsureSchemasLoaded()`, `EnsureTablesLoaded()`, `EnsureColumnsLoaded()`
3. Modify `Refresh()` to use lazy loading path
4. Add tests for lazy loading behavior

### Phase 2: TTL Integration (P2 stories)
1. Use existing `mssql_catalog_cache_ttl` for per-level TTL checking
2. Each level expires independently based on its own `last_refresh` timestamp
3. Add TTL tests

### Phase 3: Point Invalidation (P2-P3 stories)
1. Modify DDL hooks to use targeted invalidation
2. Add point invalidation tests
3. Verify no regression in existing DDL tests

### Phase 4: Azure SQL Testing (P2)
1. Add Azure-specific test file with skip logic
2. Validate lazy loading and TTL with Azure SQL
3. Document Azure-specific considerations

## Resolved Unknowns

| Question | Resolution |
|----------|------------|
| How to handle concurrent schema access? | Per-schema mutex with double-checked locking |
| What happens on TTL=0? | No automatic expiration; manual/DDL invalidation only |
| How does `mssql_exec` interact? | No automatic invalidation; user calls `mssql_refresh_cache()` |
| Azure SQL compatibility? | Uses same `sys.*` views; no changes needed |
| Memory overhead of per-entry timestamps? | ~24 bytes per schema + table; negligible |

## References

- Existing implementation: `src/catalog/mssql_metadata_cache.cpp`
- Lazy loading pattern: `src/catalog/mssql_table_set.cpp`
- DDL entry points: `src/catalog/mssql_schema_entry.cpp`
- Constitution: `.specify/memory/constitution.md`

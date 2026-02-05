# Data Model: Incremental Catalog Cache

**Feature**: 029-incremental-catalog-cache
**Date**: 2026-02-05
**Status**: Complete

## Entity Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    MSSQLMetadataCache                           │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ schemas_load_state: CacheLoadState                       │   │
│  │ schemas_last_refresh: time_point                         │   │
│  │ ttl_seconds: int64_t                                     │   │
│  └─────────────────────────────────────────────────────────┘   │
│                              │                                  │
│                              ▼                                  │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ schemas_: map<string, MSSQLSchemaMetadata>               │   │
│  └─────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                   MSSQLSchemaMetadata                           │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ name: string                                             │   │
│  │ tables_load_state: CacheLoadState                        │   │
│  │ tables_last_refresh: time_point                          │   │
│  │ load_mutex: mutex                                        │   │
│  └─────────────────────────────────────────────────────────┘   │
│                              │                                  │
│                              ▼                                  │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ tables: map<string, MSSQLTableMetadata>                  │   │
│  └─────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                   MSSQLTableMetadata                            │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ name: string                                             │   │
│  │ object_type: MSSQLObjectType                             │   │
│  │ approx_row_count: idx_t                                  │   │
│  │ columns_load_state: CacheLoadState                       │   │
│  │ columns_last_refresh: time_point                         │   │
│  │ load_mutex: mutex                                        │   │
│  └─────────────────────────────────────────────────────────┘   │
│                              │                                  │
│                              ▼                                  │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ columns: vector<MSSQLColumnInfo>                         │   │
│  └─────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

## Enumerations

### CacheLoadState

Represents the loading state of a cache level (schemas, tables, or columns).

```cpp
enum class CacheLoadState : uint8_t {
    NOT_LOADED = 0,  // Never loaded or invalidated
    LOADING = 1,     // Currently being loaded (by another thread)
    LOADED = 2,      // Successfully loaded and valid
    STALE = 3        // TTL expired, needs refresh on next access
};
```

**State Transitions**:

```
                    ┌──────────────┐
                    │  NOT_LOADED  │
                    └──────┬───────┘
                           │ EnsureLoaded()
                           ▼
                    ┌──────────────┐
              ┌─────│   LOADING    │
              │     └──────┬───────┘
              │            │ Success
              │            ▼
              │     ┌──────────────┐
              │     │   LOADED     │◄──────────────┐
              │     └──────┬───────┘               │
              │            │ TTL expires           │ Refresh
              │            ▼                       │
              │     ┌──────────────┐               │
              │     │    STALE     │───────────────┘
              │     └──────────────┘
              │            │ Invalidate()
              │            ▼
              └────►┌──────────────┐
     Error          │  NOT_LOADED  │◄─── Invalidate()
                    └──────────────┘
```

### MSSQLObjectType (Existing)

```cpp
enum class MSSQLObjectType : uint8_t {
    TABLE = 0,  // 'U' in sys.objects
    VIEW = 1    // 'V' in sys.objects
};
```

## Entity Definitions

### MSSQLMetadataCache (Modified)

Root cache container with catalog-level state.

```cpp
class MSSQLMetadataCache {
public:
    // Configuration (uses existing mssql_catalog_cache_ttl)
    void SetTTL(int64_t seconds);
    int64_t GetTTL() const;

    // Schema-level operations (lazy)
    vector<string> GetSchemaNames(tds::TdsConnection &conn);
    bool HasSchema(const string &name) const;

    // Table-level operations (lazy)
    vector<string> GetTableNames(tds::TdsConnection &conn, const string &schema);
    const MSSQLTableMetadata *GetTableMetadata(tds::TdsConnection &conn,
                                                const string &schema,
                                                const string &table);

    // Column-level operations (lazy)
    void EnsureColumnsLoaded(tds::TdsConnection &conn,
                             const string &schema,
                             const string &table);

    // Cache management
    void Refresh(tds::TdsConnection &conn, const string &collation);  // Full refresh
    void InvalidateSchema(const string &schema);      // Point invalidation
    void InvalidateTable(const string &schema, const string &table);
    void InvalidateAll();                             // Full invalidation

    // State queries
    CacheLoadState GetSchemasState() const;
    CacheLoadState GetTablesState(const string &schema) const;
    CacheLoadState GetColumnsState(const string &schema, const string &table) const;

private:
    // Catalog-level state
    mutable std::mutex schemas_mutex_;
    CacheLoadState schemas_load_state_ = CacheLoadState::NOT_LOADED;
    std::chrono::steady_clock::time_point schemas_last_refresh_;

    // TTL configuration (from mssql_catalog_cache_ttl setting)
    int64_t ttl_seconds_ = 0;  // 0 = manual only, applies to all levels

    // Schema data
    unordered_map<string, MSSQLSchemaMetadata> schemas_;
    string database_collation_;

    // Internal loading (with connection parameter)
    void EnsureSchemasLoaded(tds::TdsConnection &conn);
    void EnsureTablesLoaded(tds::TdsConnection &conn, const string &schema);
    void LoadSchemaList(tds::TdsConnection &conn);
    void LoadTableList(tds::TdsConnection &conn, MSSQLSchemaMetadata &schema);
    void LoadColumnList(tds::TdsConnection &conn, const string &schema,
                        MSSQLTableMetadata &table);

    // TTL helpers (all use ttl_seconds_)
    bool IsExpired(const std::chrono::steady_clock::time_point &last_refresh) const;
};
```

**Key Changes from Current**:
- `schemas_load_state_` replaces global `state_`
- Loading methods accept `TdsConnection` parameter (lazy loading needs connection)
- New `InvalidateSchema()` and `InvalidateTable()` for point invalidation
- Separate TTL for schemas vs tables

### MSSQLSchemaMetadata (Modified)

Per-schema metadata with independent loading state.

```cpp
struct MSSQLSchemaMetadata {
    // Identity
    string name;

    // Loading state for table list
    CacheLoadState tables_load_state = CacheLoadState::NOT_LOADED;
    std::chrono::steady_clock::time_point tables_last_refresh;
    mutable std::mutex load_mutex;

    // Table data (empty until loaded)
    unordered_map<string, MSSQLTableMetadata> tables;

    // Constructors
    MSSQLSchemaMetadata() = default;
    explicit MSSQLSchemaMetadata(const string &schema_name);

    // Move-only (mutex not copyable)
    MSSQLSchemaMetadata(MSSQLSchemaMetadata &&other) noexcept;
    MSSQLSchemaMetadata &operator=(MSSQLSchemaMetadata &&other) noexcept;
    MSSQLSchemaMetadata(const MSSQLSchemaMetadata &) = delete;
    MSSQLSchemaMetadata &operator=(const MSSQLSchemaMetadata &) = delete;
};
```

**Key Changes from Current**:
- Added `tables_load_state` and `tables_last_refresh`
- Added `load_mutex` for thread-safe table loading
- Made move-only due to mutex member

### MSSQLTableMetadata (Modified)

Per-table metadata with independent column loading state.

```cpp
struct MSSQLTableMetadata {
    // Identity (loaded during table list discovery)
    string name;
    MSSQLObjectType object_type = MSSQLObjectType::TABLE;
    idx_t approx_row_count = 0;

    // Loading state for columns
    CacheLoadState columns_load_state = CacheLoadState::NOT_LOADED;
    std::chrono::steady_clock::time_point columns_last_refresh;
    mutable std::mutex load_mutex;

    // Column data (empty until loaded)
    vector<MSSQLColumnInfo> columns;

    // Constructors
    MSSQLTableMetadata() = default;
    MSSQLTableMetadata(const string &table_name, MSSQLObjectType type, idx_t row_count);

    // Move-only (mutex not copyable)
    MSSQLTableMetadata(MSSQLTableMetadata &&other) noexcept;
    MSSQLTableMetadata &operator=(MSSQLTableMetadata &&other) noexcept;
    MSSQLTableMetadata(const MSSQLTableMetadata &) = delete;
    MSSQLTableMetadata &operator=(const MSSQLTableMetadata &) = delete;
};
```

**Key Changes from Current**:
- Added `columns_load_state` and `columns_last_refresh`
- Added `load_mutex` for thread-safe column loading
- Made move-only due to mutex member

### MSSQLColumnInfo (Unchanged)

Existing column metadata structure remains unchanged.

```cpp
struct MSSQLColumnInfo {
    string name;
    int32_t column_id;
    string sql_type_name;
    LogicalType duckdb_type;
    int16_t max_length;
    uint8_t precision;
    uint8_t scale;
    bool is_nullable;
    string collation_name;
    bool is_case_sensitive;
    bool is_unicode;
    bool is_utf8;

    // Constructor and type mapping (unchanged)
};
```

## Settings

### Existing Setting (Unchanged)

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `mssql_catalog_cache_ttl` | BIGINT | 0 | TTL for all cache levels (seconds). 0 = manual refresh only. |

**Behavior with Incremental Loading**:
- `0`: No automatic expiration. Metadata refreshed only via DDL point invalidation or `mssql_refresh_cache()`.
- `> 0`: Each cache level (schemas, tables, columns) expires independently based on its own `last_refresh` timestamp. Only the accessed level is refreshed on expiration.

## Invalidation Events

### InvalidationScope Enum

```cpp
enum class InvalidationScope : uint8_t {
    CATALOG = 0,    // Full cache (schemas + all tables + all columns)
    SCHEMA = 1,     // Schema's table list only
    TABLE = 2       // Table's column list only
};
```

### Point Invalidation Mapping

| DDL Operation | Invalidation Method | Scope |
|---------------|---------------------|-------|
| CREATE TABLE | `InvalidateSchema(schema)` | SCHEMA |
| DROP TABLE | Direct removal + `InvalidateSchema(schema)` | SCHEMA |
| ALTER TABLE | `InvalidateTable(schema, table)` | TABLE |
| CREATE SCHEMA | `InvalidateAll()` (schemas list) | CATALOG |
| DROP SCHEMA | Direct removal from cache | SCHEMA (removed) |
| `mssql_exec()` | None (user calls `mssql_refresh_cache()`) | N/A |
| `mssql_refresh_cache()` | `Refresh()` | CATALOG |

## Memory Layout

### Estimated Memory per Entity

| Entity | Fixed Overhead | Variable |
|--------|----------------|----------|
| MSSQLMetadataCache | ~100 bytes | + schemas |
| MSSQLSchemaMetadata | ~80 bytes + mutex (~40) | + tables |
| MSSQLTableMetadata | ~80 bytes + mutex (~40) | + columns |
| MSSQLColumnInfo | ~200 bytes | (per column) |

### Memory Scaling

For a database with 500 tables, 10 columns average:
- **Eager loading**: ~500 × (80 + 40 + 10 × 200) = ~1.06 MB
- **Lazy loading (1 table accessed)**: ~(100 + 80 + 80 + 10 × 200) = ~2.3 KB

Memory scales with **accessed tables**, not total tables.

## Thread Safety Model

### Lock Hierarchy

```
schemas_mutex_ (MSSQLMetadataCache)
    └─ load_mutex (MSSQLSchemaMetadata)
        └─ load_mutex (MSSQLTableMetadata)
```

**Rule**: Never acquire a higher-level lock while holding a lower-level lock.

### Double-Checked Locking Pattern

All `Ensure*Loaded()` methods follow this pattern:

```cpp
void EnsureColumnsLoaded(TdsConnection &conn, MSSQLTableMetadata &table) {
    // Fast path: no lock if already loaded and not expired
    if (table.columns_load_state == CacheLoadState::LOADED &&
        !IsColumnsExpired(table)) {
        return;
    }

    // Slow path: acquire lock and check again
    std::lock_guard<std::mutex> lock(table.load_mutex);
    if (table.columns_load_state == CacheLoadState::LOADED &&
        !IsColumnsExpired(table)) {
        return;
    }

    // Load under lock
    table.columns_load_state = CacheLoadState::LOADING;
    LoadColumnList(conn, schema_name, table);
    table.columns_load_state = CacheLoadState::LOADED;
    table.columns_last_refresh = std::chrono::steady_clock::now();
}
```

## SQL Queries (Unchanged)

The metadata queries remain the same, but are now called lazily:

```sql
-- Schema list (called once per catalog)
SELECT s.name FROM sys.schemas s WHERE ...

-- Table list (called once per schema)
SELECT o.name, o.type, p.rows
FROM sys.objects o LEFT JOIN sys.partitions p ...
WHERE SCHEMA_NAME(o.schema_id) = '{schema}'

-- Column list (called once per table)
SELECT c.name, c.column_id, t.name, c.max_length, ...
FROM sys.columns c JOIN sys.types t ...
WHERE c.object_id = OBJECT_ID('[{schema}].[{table}]')
```

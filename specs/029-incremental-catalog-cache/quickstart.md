# Quickstart: Incremental Catalog Cache Implementation

**Feature**: 029-incremental-catalog-cache
**Date**: 2026-02-05

## Overview

This guide provides the implementation approach for transforming the catalog cache from eager full-refresh to incremental lazy loading with granular TTL.

## Implementation Order

### Phase 1: Lazy Loading Foundation (P1 stories)

**Goal**: Load metadata only when accessed, not during ATTACH.

#### Step 1.1: Extend Data Structures

**File**: `src/include/catalog/mssql_metadata_cache.hpp`

1. Add `CacheLoadState` enum (if not reusing `MSSQLCacheState`):
```cpp
enum class CacheLoadState : uint8_t {
    NOT_LOADED = 0,
    LOADING = 1,
    LOADED = 2,
    STALE = 3
};
```

2. Extend `MSSQLSchemaMetadata`:
```cpp
struct MSSQLSchemaMetadata {
    string name;
    CacheLoadState tables_load_state = CacheLoadState::NOT_LOADED;
    std::chrono::steady_clock::time_point tables_last_refresh;
    mutable std::mutex load_mutex;
    unordered_map<string, MSSQLTableMetadata> tables;

    // Move-only constructors (mutex not copyable)
    MSSQLSchemaMetadata() = default;
    explicit MSSQLSchemaMetadata(const string &n) : name(n) {}
    MSSQLSchemaMetadata(MSSQLSchemaMetadata &&) noexcept;
    MSSQLSchemaMetadata &operator=(MSSQLSchemaMetadata &&) noexcept;
};
```

3. Extend `MSSQLTableMetadata`:
```cpp
struct MSSQLTableMetadata {
    string name;
    MSSQLObjectType object_type = MSSQLObjectType::TABLE;
    idx_t approx_row_count = 0;

    CacheLoadState columns_load_state = CacheLoadState::NOT_LOADED;
    std::chrono::steady_clock::time_point columns_last_refresh;
    mutable std::mutex load_mutex;
    vector<MSSQLColumnInfo> columns;

    // Move-only constructors
};
```

4. Add catalog-level state to `MSSQLMetadataCache`:
```cpp
class MSSQLMetadataCache {
private:
    CacheLoadState schemas_load_state_ = CacheLoadState::NOT_LOADED;
    std::chrono::steady_clock::time_point schemas_last_refresh_;
    // ... existing members
};
```

#### Step 1.2: Implement Lazy Loading Methods

**File**: `src/catalog/mssql_metadata_cache.cpp`

1. Add `EnsureSchemasLoaded()`:
```cpp
void MSSQLMetadataCache::EnsureSchemasLoaded(tds::TdsConnection &conn) {
    // Fast path
    if (schemas_load_state_ == CacheLoadState::LOADED && !IsSchemasExpired()) {
        return;
    }

    std::lock_guard<std::mutex> lock(schemas_mutex_);
    // Double-check
    if (schemas_load_state_ == CacheLoadState::LOADED && !IsSchemasExpired()) {
        return;
    }

    schemas_load_state_ = CacheLoadState::LOADING;
    LoadSchemaList(conn);
    schemas_load_state_ = CacheLoadState::LOADED;
    schemas_last_refresh_ = std::chrono::steady_clock::now();
}
```

2. Add `EnsureTablesLoaded()`:
```cpp
void MSSQLMetadataCache::EnsureTablesLoaded(tds::TdsConnection &conn,
                                             MSSQLSchemaMetadata &schema) {
    // Fast path
    if (schema.tables_load_state == CacheLoadState::LOADED &&
        !IsTablesExpired(schema)) {
        return;
    }

    std::lock_guard<std::mutex> lock(schema.load_mutex);
    // Double-check
    if (schema.tables_load_state == CacheLoadState::LOADED &&
        !IsTablesExpired(schema)) {
        return;
    }

    schema.tables_load_state = CacheLoadState::LOADING;
    LoadTableList(conn, schema);
    schema.tables_load_state = CacheLoadState::LOADED;
    schema.tables_last_refresh = std::chrono::steady_clock::now();
}
```

3. Add `EnsureColumnsLoaded()`:
```cpp
void MSSQLMetadataCache::EnsureColumnsLoaded(tds::TdsConnection &conn,
                                              const string &schema_name,
                                              MSSQLTableMetadata &table) {
    // Fast path
    if (table.columns_load_state == CacheLoadState::LOADED &&
        !IsColumnsExpired(table)) {
        return;
    }

    std::lock_guard<std::mutex> lock(table.load_mutex);
    // Double-check
    if (table.columns_load_state == CacheLoadState::LOADED &&
        !IsColumnsExpired(table)) {
        return;
    }

    table.columns_load_state = CacheLoadState::LOADING;
    LoadColumnList(conn, schema_name, table);
    table.columns_load_state = CacheLoadState::LOADED;
    table.columns_last_refresh = std::chrono::steady_clock::now();
}
```

4. Modify `LoadSchemaList()` to only load schema names:
```cpp
void MSSQLMetadataCache::LoadSchemaList(tds::TdsConnection &conn) {
    schemas_.clear();
    ExecuteMetadataQuery(conn, SCHEMA_DISCOVERY_SQL, [this](const vector<string> &values) {
        if (!values.empty()) {
            string schema_name = values[0];
            schemas_.emplace(schema_name, MSSQLSchemaMetadata(schema_name));
            // Note: tables NOT loaded here (tables_load_state = NOT_LOADED)
        }
    });
}
```

5. Modify `LoadTableList()` to only load table metadata (no columns):
```cpp
void MSSQLMetadataCache::LoadTableList(tds::TdsConnection &conn,
                                        MSSQLSchemaMetadata &schema) {
    schema.tables.clear();
    string query = StringUtil::Format(TABLE_DISCOVERY_SQL_TEMPLATE, schema.name);

    ExecuteMetadataQuery(conn, query, [&schema](const vector<string> &values) {
        if (values.size() >= 3) {
            MSSQLTableMetadata table;
            table.name = values[0];
            table.object_type = (values[1][0] == 'V') ? MSSQLObjectType::VIEW
                                                       : MSSQLObjectType::TABLE;
            table.approx_row_count = std::stoll(values[2]);
            // Note: columns NOT loaded (columns_load_state = NOT_LOADED)
            schema.tables.emplace(table.name, std::move(table));
        }
    });
}
```

#### Step 1.3: Update Public API Methods

Modify existing methods to use lazy loading:

```cpp
vector<string> MSSQLMetadataCache::GetSchemaNames(tds::TdsConnection &conn) {
    EnsureSchemasLoaded(conn);

    std::lock_guard<std::mutex> lock(schemas_mutex_);
    vector<string> names;
    for (const auto &pair : schemas_) {
        names.push_back(pair.first);
    }
    return names;
}

vector<string> MSSQLMetadataCache::GetTableNames(tds::TdsConnection &conn,
                                                  const string &schema_name) {
    EnsureSchemasLoaded(conn);

    auto it = schemas_.find(schema_name);
    if (it == schemas_.end()) {
        return {};
    }

    EnsureTablesLoaded(conn, it->second);

    vector<string> names;
    for (const auto &pair : it->second.tables) {
        names.push_back(pair.first);
    }
    return names;
}

const MSSQLTableMetadata *MSSQLMetadataCache::GetTableMetadata(
    tds::TdsConnection &conn,
    const string &schema_name,
    const string &table_name) {

    EnsureSchemasLoaded(conn);

    auto schema_it = schemas_.find(schema_name);
    if (schema_it == schemas_.end()) {
        return nullptr;
    }

    EnsureTablesLoaded(conn, schema_it->second);

    auto table_it = schema_it->second.tables.find(table_name);
    if (table_it == schema_it->second.tables.end()) {
        return nullptr;
    }

    EnsureColumnsLoaded(conn, schema_name, table_it->second);
    return &table_it->second;
}
```

#### Step 1.4: Update MSSQLCatalog Integration

**File**: `src/catalog/mssql_catalog.cpp`

Modify `EnsureCacheLoaded()` to NOT trigger full refresh:

```cpp
void MSSQLCatalog::EnsureCacheLoaded(ClientContext &context) {
    if (!catalog_enabled_) {
        return;
    }

    // Load TTL from existing setting
    int64_t cache_ttl = LoadCatalogCacheTTL(context);
    metadata_cache_->SetTTL(cache_ttl);

    // Don't eagerly load - lazy loading handles this
    // Old code that called Refresh() is removed
}
```

Update schema/table lookups to pass connection:

```cpp
optional_ptr<SchemaCatalogEntry> MSSQLCatalog::LookupSchema(...) {
    // Acquire connection for lazy loading
    auto conn = connection_pool_->AcquireConnection();
    auto schema_names = metadata_cache_->GetSchemaNames(*conn);
    // ... rest of lookup
}
```

---

### Phase 2: TTL Integration (P2 stories)

Uses existing `mssql_catalog_cache_ttl` setting for all cache levels.

#### Step 2.1: Implement TTL Checking

**File**: `src/catalog/mssql_metadata_cache.cpp`

```cpp
bool MSSQLMetadataCache::IsExpired(
    const std::chrono::steady_clock::time_point &last_refresh) const {
    if (ttl_seconds_ <= 0) return false;  // TTL disabled
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - last_refresh).count();
    return elapsed >= ttl_seconds_;
}
```

#### Step 2.2: Update EnsureCacheLoaded

**File**: `src/catalog/mssql_catalog.cpp`

```cpp
void MSSQLCatalog::EnsureCacheLoaded(ClientContext &context) {
    if (!catalog_enabled_) {
        return;
    }

    // Load TTL from existing setting
    int64_t cache_ttl = LoadCatalogCacheTTL(context);
    metadata_cache_->SetTTL(cache_ttl);

    // Don't eagerly load - lazy loading handles refresh on access
}
```

---

### Phase 3: Point Invalidation (P2-P3 stories)

#### Step 3.1: Add Invalidation Methods

```cpp
void MSSQLMetadataCache::InvalidateSchema(const string &schema_name) {
    std::lock_guard<std::mutex> lock(schemas_mutex_);
    auto it = schemas_.find(schema_name);
    if (it != schemas_.end()) {
        it->second.tables_load_state = CacheLoadState::NOT_LOADED;
    }
}

void MSSQLMetadataCache::InvalidateTable(const string &schema_name,
                                          const string &table_name) {
    std::lock_guard<std::mutex> lock(schemas_mutex_);
    auto schema_it = schemas_.find(schema_name);
    if (schema_it == schemas_.end()) return;

    auto table_it = schema_it->second.tables.find(table_name);
    if (table_it != schema_it->second.tables.end()) {
        table_it->second.columns_load_state = CacheLoadState::NOT_LOADED;
    }
}

void MSSQLMetadataCache::InvalidateAll() {
    std::lock_guard<std::mutex> lock(schemas_mutex_);
    schemas_load_state_ = CacheLoadState::NOT_LOADED;
    for (auto &[name, schema] : schemas_) {
        schema.tables_load_state = CacheLoadState::NOT_LOADED;
        for (auto &[tname, table] : schema.tables) {
            table.columns_load_state = CacheLoadState::NOT_LOADED;
        }
    }
}
```

#### Step 3.2: Update DDL Entry Points

**File**: `src/catalog/mssql_schema_entry.cpp`

```cpp
optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateTable(...) {
    auto &mssql_catalog = GetMSSQLCatalog();
    mssql_catalog.CheckWriteAccess("CREATE TABLE");

    string tsql = MSSQLDDLTranslator::TranslateCreateTable(name, info);
    mssql_catalog.ExecuteDDL(context, tsql);

    // Point invalidation (replaces full InvalidateMetadataCache)
    mssql_catalog.GetMetadataCache().InvalidateSchema(name);

    return tables_.GetEntry(context, info.Base().table);
}
```

---

### Phase 4: Azure SQL Testing

**File**: `test/sql/azure/azure_lazy_loading.test`

```sql
# name: test/sql/azure/azure_lazy_loading.test
# description: Test lazy loading with Azure SQL Database
# group: [azure]

require mssql

# Skip if AZURE_SQL_TEST_DSN not set
require-env AZURE_SQL_TEST_DSN

statement ok
ATTACH '${AZURE_SQL_TEST_DSN}' AS azure_db (TYPE mssql);

# Verify lazy loading - no sys.columns queries yet
statement ok
SELECT schema_name FROM duckdb_schemas() WHERE database_name = 'azure_db';

# Query specific table - triggers lazy column load
statement ok
SELECT * FROM azure_db.dbo.test_table LIMIT 1;

# Verify other tables' columns not loaded
# (would need internal state inspection or query counting)

statement ok
DETACH azure_db;
```

---

## Testing Strategy

### Unit Tests (C++)

**File**: `test/cpp/test_incremental_cache.cpp`

```cpp
TEST_CASE("Lazy loading state transitions", "[catalog][incremental]") {
    MSSQLMetadataCache cache;

    SECTION("Initial state is NOT_LOADED") {
        REQUIRE(cache.GetSchemasState() == CacheLoadState::NOT_LOADED);
    }

    SECTION("EnsureSchemasLoaded transitions to LOADED") {
        // Mock connection
        auto conn = CreateMockConnection();
        cache.EnsureSchemasLoaded(*conn);
        REQUIRE(cache.GetSchemasState() == CacheLoadState::LOADED);
    }

    SECTION("InvalidateSchema marks schema as NOT_LOADED") {
        // Setup loaded cache
        // ...
        cache.InvalidateSchema("dbo");
        REQUIRE(cache.GetTablesState("dbo") == CacheLoadState::NOT_LOADED);
    }
}
```

### Integration Tests (SQL)

**File**: `test/sql/catalog/lazy_loading.test`

```sql
# name: test/sql/catalog/lazy_loading.test
# description: Test incremental lazy loading behavior
# group: [catalog]

require mssql

# Setup test database
statement ok
ATTACH 'mssql://sa:TestPassword1@localhost:1433/TestDB' AS sqlserver (TYPE mssql);

# First access - should load schemas only
statement ok
SELECT * FROM duckdb_schemas() WHERE database_name = 'sqlserver';

# Query single table - should load only that table's columns
statement ok
SELECT * FROM sqlserver.dbo.test_table LIMIT 1;

# Verify no unnecessary queries (check via pool stats or timing)
# ...

statement ok
DETACH sqlserver;
```

---

## Verification Checklist

- [ ] ATTACH with 500+ tables completes without loading all columns
- [ ] First table query loads only that table's columns
- [ ] Repeated queries don't trigger metadata queries (cache hit)
- [ ] Schema TTL expiration refreshes only that schema
- [ ] Table TTL expiration refreshes only that table's columns
- [ ] CREATE TABLE makes new table visible immediately
- [ ] DROP TABLE removes table from cache
- [ ] ALTER TABLE refreshes only that table's columns
- [ ] `mssql_refresh_cache()` still works (full refresh)
- [ ] Existing tests pass without modification
- [ ] Azure SQL tests pass when `AZURE_SQL_TEST_DSN` configured

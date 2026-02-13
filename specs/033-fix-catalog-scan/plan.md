# Implementation Plan: Fix Catalog Scan & Add Object Visibility Filters

**Branch**: `033-fix-catalog-scan` | **Date**: 2026-02-13 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/033-fix-catalog-scan/spec.md`

## Summary

The extension eagerly loads column metadata for ALL tables during catalog scan operations, making it unusable for databases with many tables (65K+ in ERP scenarios). The fix decouples table enumeration from column loading, adds regex-based object visibility filters, and provides a bulk preload function for users who need full catalog access efficiently.

## Technical Context

**Language/Version**: C++17 (C++11-compatible for ODR with DuckDB)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (vcpkg), TDS protocol layer
**Storage**: In-memory metadata cache (`MSSQLMetadataCache`)
**Testing**: SQLLogicTest (integration tests requiring SQL Server), C++ unit tests (no SQL Server)
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW)
**Project Type**: DuckDB extension (single project)
**Performance Goals**: Single-table query on 65K-table DB completes in seconds; bulk preload via single SQL round trip
**Constraints**: DuckDB's `TableCatalogEntry::columns` ColumnList is immutable after construction; `GetColumns()` is non-virtual
**Scale/Scope**: Databases with 1K-100K+ tables

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | All changes use native TDS protocol, no external libraries added |
| II. Streaming First | PASS | No buffering changes; bulk preload streams results from TDS into cache |
| III. Correctness over Convenience | PASS | Deferred loading is transparent; no silent failures or data corruption |
| IV. Explicit State Machines | PASS | Cache states (NOT_LOADED/LOADING/LOADED) remain explicit and testable |
| V. DuckDB-Native UX | PASS | Catalog integration improved; schemas/tables remain browsable via standard DuckDB |
| VI. Incremental Delivery | PASS | Three independent phases: bug fix → filters → preload |

No violations. No complexity tracking needed.

## Project Structure

### Documentation (this feature)

```text
specs/033-fix-catalog-scan/
├── plan.md              # This file
├── spec.md              # Feature specification
├── research.md          # Phase 0 research findings
├── data-model.md        # Phase 1 data model
├── quickstart.md        # Phase 1 quickstart
├── contracts/           # Phase 1 contracts
│   └── api.md           # Function signatures and settings
└── checklists/
    └── requirements.md  # Quality checklist
```

### Source Code (repository root)

```text
src/
├── include/
│   ├── mssql_storage.hpp               # MSSQLConnectionInfo (+ filter fields)
│   ├── catalog/
│   │   ├── mssql_metadata_cache.hpp    # Cache API (+ bulk load, filter support)
│   │   ├── mssql_table_set.hpp         # MSSQLTableSet (+ deferred loading)
│   │   ├── mssql_table_entry.hpp       # MSSQLTableEntry (unchanged interface)
│   │   └── mssql_catalog_filter.hpp    # NEW: MSSQLCatalogFilter class
│   └── connection/
│       └── mssql_settings.hpp          # Extension settings
├── catalog/
│   ├── mssql_metadata_cache.cpp        # Cache impl (+ bulk load, filters)
│   ├── mssql_table_set.cpp             # MSSQLTableSet impl (deferred loading)
│   ├── mssql_table_entry.cpp           # MSSQLTableEntry impl (unchanged)
│   └── mssql_catalog_filter.cpp        # NEW: MSSQLCatalogFilter impl
├── connection/
│   └── mssql_settings.cpp              # Settings registration
├── mssql_storage.cpp                   # ATTACH flow (+ filter parsing)
└── mssql_extension.cpp                 # Function registration (+ preload)

test/
├── sql/
│   └── catalog/
│       ├── deferred_columns.test       # NEW: Deferred column loading tests
│       ├── catalog_filter.test         # NEW: Regex filter tests
│       └── preload_catalog.test        # NEW: Bulk preload tests
└── cpp/
    └── test_catalog_filter.cpp         # NEW: Unit tests for MSSQLCatalogFilter
```

**Structure Decision**: Existing DuckDB extension layout preserved. New files follow `mssql_<component>` naming convention. All changes fit within existing catalog/ and connection/ directories.

## Naming Conventions (per CLAUDE.md)

| Item | Convention | Examples |
|------|-----------|----------|
| New files | `mssql_<component>.{hpp,cpp}` | `mssql_catalog_filter.hpp`, `mssql_catalog_filter.cpp` |
| Classes (duckdb ns) | PascalCase with MSSQL prefix | `MSSQLCatalogFilter`, `MSSQLPreloadCatalogFunction` |
| Methods | PascalCase | `SetFilters()`, `MatchesSchema()`, `BulkLoadAll()` |
| Member variables | snake_case with trailing _ | `schema_filter_`, `table_filter_`, `known_table_names_` |
| Constants | UPPER_SNAKE_CASE | `BULK_METADATA_SQL_TEMPLATE` |
| Test files (SQL) | `<feature>_<scenario>.test` | `deferred_columns.test`, `catalog_filter.test` |
| Test files (C++) | `test_<component>.cpp` | `test_catalog_filter.cpp` |
| SQL function names | snake_case | `mssql_preload_catalog` |

---

## Phase 1: Deferred Column Loading (Bug Fix)

### Root Cause

`MSSQLTableSet::LoadEntries()` (line 98-139 in `mssql_table_set.cpp`) calls `cache.GetTableMetadata()` for every table name. `GetTableMetadata()` triggers `EnsureColumnsLoaded()` which fires a per-table column-discovery SQL query. With 65K tables = 65K round trips.

`Scan()` calls `EnsureLoaded()` → `LoadEntries()`, so any DuckDB operation that triggers a schema scan (SHOW TABLES, error similarity matching, system functions) causes the full column load.

### Design: Two-Level Loading in MSSQLTableSet

**Key constraint**: DuckDB's `TableCatalogEntry::columns` ColumnList is immutable after construction (set in constructor initializer list, non-virtual `GetColumns()`). We cannot create entries with empty columns and fill them later.

**Approach**: Separate name tracking from entry creation.

#### Modified MSSQLTableSet State

```cpp
class MSSQLTableSet {
private:
    MSSQLSchemaEntry &schema_;

    // Level 1: Table names only (fast, no column queries)
    unordered_set<string> known_table_names_;
    std::atomic<bool> names_loaded_{false};

    // Level 2: Full entries with columns (created on demand)
    unordered_map<string, unique_ptr<MSSQLTableEntry>> entries_;
    std::atomic<bool> is_fully_loaded_{false};

    unordered_set<string> attempted_tables_;
    std::mutex entry_mutex_;
    std::mutex load_mutex_;
    std::mutex names_mutex_;
};
```

#### Modified GetEntry() — already lazy, minor adjustment

```cpp
optional_ptr<CatalogEntry> MSSQLTableSet::GetEntry(ClientContext &context, const string &name) {
    // 1. Check cached entries (fast path)
    {
        std::lock_guard<std::mutex> lock(entry_mutex_);
        auto it = entries_.find(name);
        if (it != entries_.end()) {
            return it->second.get();
        }
        if (attempted_tables_.count(name)) {
            return nullptr;
        }
    }

    // 2. If fully loaded, table doesn't exist
    if (is_fully_loaded_.load()) {
        return nullptr;
    }

    // 3. Check names list — if names loaded and name not in list, table doesn't exist
    if (names_loaded_.load()) {
        std::lock_guard<std::mutex> lock(names_mutex_);
        if (known_table_names_.find(name) == known_table_names_.end()) {
            // Name not in known list — mark as attempted, return null
            std::lock_guard<std::mutex> elock(entry_mutex_);
            attempted_tables_.insert(name);
            return nullptr;
        }
    }

    // 4. Load single entry with full metadata (columns included)
    if (LoadSingleEntry(context, name)) {
        std::lock_guard<std::mutex> lock(entry_mutex_);
        auto it = entries_.find(name);
        if (it != entries_.end()) {
            return it->second.get();
        }
    }
    return nullptr;
}
```

**Improvement**: Step 3 short-circuits the expensive `LoadSingleEntry()` call when we already know all table names and the requested name isn't in the list. This avoids a round trip to SQL Server for nonexistent tables.

#### Modified Scan() — deferred column loading

```cpp
void MSSQLTableSet::Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback) {
    // Step 1: Ensure table names are loaded (no column queries)
    EnsureNamesLoaded(context);

    // Step 2: For each known name, ensure entry exists (loads columns on demand)
    auto &catalog = schema_.GetMSSQLCatalog();
    auto &cache = catalog.GetMetadataCache();
    auto &pool = catalog.GetConnectionPool();

    catalog.EnsureCacheLoaded(context);
    auto connection = pool.Acquire();

    std::lock_guard<std::mutex> lock(entry_mutex_);
    for (const auto &table_name : known_table_names_) {
        // Skip if already loaded
        if (entries_.find(table_name) != entries_.end()) {
            callback(*entries_[table_name]);
            continue;
        }

        // Load columns for this table and create entry
        auto table_meta = cache.GetTableMetadata(*connection, schema_.name, table_name);
        if (table_meta) {
            auto entry = CreateTableEntry(*table_meta);
            if (entry) {
                auto &ref = *entry;
                entries_[entry->name] = std::move(entry);
                callback(ref);
            }
        }
    }

    pool.Release(std::move(connection));
    is_fully_loaded_.store(true);
}
```

**Important behavioral change**: `Scan()` still loads all columns, but ONLY when `Scan()` is actually called. The key fix is that `GetEntry()` (the path used by single-table queries) no longer triggers `Scan()` or `LoadEntries()`.

#### New EnsureNamesLoaded() — fast name-only loading

```cpp
void MSSQLTableSet::EnsureNamesLoaded(ClientContext &context) {
    if (names_loaded_.load()) {
        return;
    }

    std::lock_guard<std::mutex> lock(names_mutex_);
    if (names_loaded_.load()) {
        return;
    }

    auto &catalog = schema_.GetMSSQLCatalog();
    auto &cache = catalog.GetMetadataCache();
    auto &pool = catalog.GetConnectionPool();

    catalog.EnsureCacheLoaded(context);
    auto connection = pool.Acquire();

    // Only loads table names (fast, no column queries)
    auto table_names = cache.GetTableNames(*connection, schema_.name);

    pool.Release(std::move(connection));

    for (const auto &name : table_names) {
        known_table_names_.insert(name);
    }
    names_loaded_.store(true);
}
```

#### Modified EnsureLoaded() — removed, replaced by EnsureNamesLoaded()

The old `EnsureLoaded()` called `LoadEntries()` which loaded all columns. Replace all callers with `EnsureNamesLoaded()` for the names-only path, or use `Scan()` directly when full entries are needed.

#### Modified Invalidate()

```cpp
void MSSQLTableSet::Invalidate() {
    std::lock_guard<std::mutex> lock(load_mutex_);
    is_fully_loaded_.store(false);
    names_loaded_.store(false);
    {
        std::lock_guard<std::mutex> elock(entry_mutex_);
        entries_.clear();
        attempted_tables_.clear();
    }
    {
        std::lock_guard<std::mutex> nlock(names_mutex_);
        known_table_names_.clear();
    }
}
```

### Files Changed (Phase 1)

| File | Change |
|------|--------|
| `src/include/catalog/mssql_table_set.hpp` | Add `known_table_names_`, `names_loaded_`, `names_mutex_`; add `EnsureNamesLoaded()` |
| `src/catalog/mssql_table_set.cpp` | Rewrite `Scan()`, `GetEntry()`, add `EnsureNamesLoaded()`, update `Invalidate()` |
| `test/sql/catalog/deferred_columns.test` | NEW: Integration tests for deferred column loading |

---

## Phase 2: Object Visibility Filters

### Design: MSSQLCatalogFilter

A utility class that holds compiled regex patterns and provides matching methods. Lives in the `duckdb` namespace with MSSQL prefix (per convention for shared namespace).

#### Header: `src/include/catalog/mssql_catalog_filter.hpp`

```cpp
#pragma once
#include <regex>
#include <string>

namespace duckdb {

class MSSQLCatalogFilter {
public:
    MSSQLCatalogFilter() = default;

    // Set filters from string patterns. Throws on invalid regex.
    void SetSchemaFilter(const string &pattern);
    void SetTableFilter(const string &pattern);

    // Validate a regex pattern string (returns empty string if valid, error message if invalid)
    static string ValidatePattern(const string &pattern);

    // Check if a name matches the filter (returns true if no filter set)
    bool MatchesSchema(const string &name) const;
    bool MatchesTable(const string &name) const;

    // Check if any filter is active
    bool HasSchemaFilter() const;
    bool HasTableFilter() const;
    bool HasFilters() const;

    // Get pattern strings (for display/debugging)
    const string &GetSchemaPattern() const;
    const string &GetTablePattern() const;

private:
    string schema_pattern_;
    string table_pattern_;
    std::regex schema_regex_;     // Compiled with std::regex::icase
    std::regex table_regex_;      // Compiled with std::regex::icase
    bool has_schema_filter_ = false;
    bool has_table_filter_ = false;
};

} // namespace duckdb
```

#### Integration Points

1. **MSSQLConnectionInfo** — add `schema_filter` and `table_filter` string fields
2. **ATTACH parsing** (`mssql_storage.cpp`) — extract `schema_filter` and `table_filter` from:
   - ATTACH options (`TYPE mssql, schema_filter '...', table_filter '...'`)
   - Connection string (`SchemaFilter=...;TableFilter=...`)
   - Secret fields (`schema_filter`, `table_filter`)
   - Validate patterns at ATTACH time; fail-fast on invalid regex
3. **MSSQLCatalog** — store `MSSQLCatalogFilter` instance, pass to metadata cache
4. **MSSQLMetadataCache** — apply filter in `GetSchemaNames()` and `GetTableNames()`
5. **MSSQLTableSet::GetEntry()** — check table filter before loading

#### Filter Application in Metadata Cache

```cpp
vector<string> MSSQLMetadataCache::GetSchemaNames(tds::TdsConnection &connection) {
    EnsureSchemasLoaded(connection);

    vector<string> names;
    std::lock_guard<std::mutex> lock(schemas_mutex_);
    for (const auto &pair : schemas_) {
        if (filter_ && !filter_->MatchesSchema(pair.first)) {
            continue;  // Filtered out
        }
        names.push_back(pair.first);
    }
    return names;
}

vector<string> MSSQLMetadataCache::GetTableNames(tds::TdsConnection &connection, const string &schema_name) {
    EnsureTablesLoaded(connection, schema_name);

    // ... existing code ...
    for (const auto &pair : schema_it->second.tables) {
        if (filter_ && !filter_->MatchesTable(pair.first)) {
            continue;  // Filtered out
        }
        names.push_back(pair.first);
    }
    return names;
}
```

### Files Changed (Phase 2)

| File | Change |
|------|--------|
| `src/include/catalog/mssql_catalog_filter.hpp` | NEW: MSSQLCatalogFilter class header |
| `src/catalog/mssql_catalog_filter.cpp` | NEW: MSSQLCatalogFilter implementation |
| `src/include/mssql_storage.hpp` | Add `schema_filter`, `table_filter` to MSSQLConnectionInfo |
| `src/mssql_storage.cpp` | Parse filters from ATTACH options, connection string, secret; validate at ATTACH time |
| `src/include/catalog/mssql_metadata_cache.hpp` | Add `SetFilter()`, store `MSSQLCatalogFilter*` |
| `src/catalog/mssql_metadata_cache.cpp` | Apply filter in `GetSchemaNames()`, `GetTableNames()` |
| `src/catalog/mssql_table_set.cpp` | Check table filter in `GetEntry()`, apply to `known_table_names_` |
| `src/catalog/mssql_catalog.cpp` | Pass filter from connection info to metadata cache |
| `test/sql/catalog/catalog_filter.test` | NEW: Integration tests for regex filters |
| `test/cpp/test_catalog_filter.cpp` | NEW: Unit tests for MSSQLCatalogFilter |

---

## Phase 3: Bulk Catalog Preload

### Design: MSSQLPreloadCatalogFunction

A scalar function `mssql_preload_catalog(context_name [, schema_name])` that executes a single bulk SQL query to load all metadata.

#### Bulk SQL Query

```sql
SELECT
    s.name AS schema_name,
    o.name AS object_name,
    o.type AS object_type,
    ISNULL(p.rows, 0) AS approx_rows,
    c.name AS column_name,
    c.column_id,
    t.name AS type_name,
    c.max_length,
    c.precision,
    c.scale,
    c.is_nullable,
    ISNULL(c.collation_name, '') AS collation_name
FROM sys.schemas s
INNER JOIN sys.objects o ON o.schema_id = s.schema_id
INNER JOIN sys.columns c ON c.object_id = o.object_id
JOIN sys.types t ON c.user_type_id = t.user_type_id
LEFT JOIN sys.partitions p ON o.object_id = p.object_id AND p.index_id IN (0, 1)
WHERE s.schema_id NOT IN (3, 4)
  AND s.principal_id != 0
  AND s.name NOT IN ('guest', 'INFORMATION_SCHEMA', 'sys', 'db_owner', 'db_accessadmin',
                     'db_securityadmin', 'db_ddladmin', 'db_backupoperator', 'db_datareader',
                     'db_datawriter', 'db_denydatareader', 'db_denydatawriter')
  AND o.type IN ('U', 'V')
  AND o.is_ms_shipped = 0
ORDER BY s.name, o.name, c.column_id
```

When `schema_name` is provided, add: `AND s.name = '<schema_name>'`

#### New MSSQLMetadataCache::BulkLoadAll()

```cpp
void MSSQLMetadataCache::BulkLoadAll(tds::TdsConnection &connection, const string &schema_filter = "");
```

**Logic**:
1. Execute bulk SQL query via `ExecuteMetadataQuery()`
2. Stream rows, grouping by `schema_name + object_name` (order guarantees contiguous groups)
3. For each new schema: create `MSSQLSchemaMetadata`, mark `tables_load_state = LOADED`
4. For each new table: create `MSSQLTableMetadata`, populate columns, mark `columns_load_state = LOADED`
5. Apply `MSSQLCatalogFilter` to skip filtered-out schemas/tables
6. Mark `schemas_load_state_ = LOADED`
7. Return counts for status message

#### Function Registration

```cpp
// In mssql_extension.cpp
ScalarFunction preload_func("mssql_preload_catalog",
                            {LogicalType::VARCHAR},  // context_name
                            LogicalType::VARCHAR,     // result message
                            MSSQLPreloadCatalogFunction::Execute);
preload_func.varargs = LogicalType::VARCHAR;  // optional schema_name
```

### Files Changed (Phase 3)

| File | Change |
|------|--------|
| `src/include/catalog/mssql_metadata_cache.hpp` | Add `BulkLoadAll()` declaration, add `BULK_METADATA_SQL_TEMPLATE` |
| `src/catalog/mssql_metadata_cache.cpp` | Implement `BulkLoadAll()` with streaming group-by parse |
| `src/mssql_extension.cpp` | Register `mssql_preload_catalog` scalar function |
| `src/catalog/mssql_preload_catalog.cpp` | NEW: MSSQLPreloadCatalogFunction implementation |
| `src/include/catalog/mssql_preload_catalog.hpp` | NEW: MSSQLPreloadCatalogFunction header |
| `test/sql/catalog/preload_catalog.test` | NEW: Integration tests for bulk preload |

---

## Constitution Re-Check (Post-Design)

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | No external libraries added; regex uses C++ stdlib |
| II. Streaming First | PASS | Bulk preload streams from TDS into cache; no full-result buffering |
| III. Correctness over Convenience | PASS | Filtered tables return "not found" (correct); no silent omission of query results |
| IV. Explicit State Machines | PASS | New `names_loaded_` state is explicit; cache states unchanged |
| V. DuckDB-Native UX | PASS | Filters and preload enhance catalog experience |
| VI. Incremental Delivery | PASS | Each phase is independently usable: P1 fixes the bug, P2 adds filters, P3 adds preload |

All gates pass. Design is ready for task generation.

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| DuckDB expects columns from Scan entries | Medium | High | Scan still loads columns per entry; only GetEntry path is deferred |
| `std::regex` performance on Windows | Low | Low | Filter applied to name list (not per-row); pattern compiled once |
| Thread safety with dual-level loading | Medium | Medium | Separate mutexes for names and entries; double-checked locking pattern |
| Bulk query timeout for very large DBs | Low | Medium | Uses existing TDS streaming; query timeout setting applies |
| Connection string parsing conflicts | Low | Low | New parameters use distinct names; no overlap with existing params |

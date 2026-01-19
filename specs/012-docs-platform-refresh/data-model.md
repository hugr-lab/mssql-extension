# Data Model: Documentation, Platform Support & Cache Refresh

**Branch**: `012-docs-platform-refresh` | **Date**: 2026-01-19

## Overview

This feature introduces no new persistent data entities. It extends existing in-memory structures and creates documentation files.

## Existing Entities (Reference Only)

### MSSQLMetadataCache

**Location**: `src/include/catalog/mssql_metadata_cache.hpp`

**State Machine**:
```
EMPTY → LOADING → READY
                    ↓
                  STALE → LOADING → READY
```

**Key Methods Used**:
- `Invalidate()` - Marks cache as STALE
- `Refresh(connection)` - Reloads from SQL Server
- `IsExpired()` - Checks TTL expiration

### MSSQLCatalog

**Location**: `src/include/catalog/mssql_catalog.hpp`

**Key Methods Used**:
- `InvalidateMetadataCache()` - Triggers cache invalidation
- `EnsureCacheLoaded(context)` - Ensures cache is current

## New Structures

### MSSQLRefreshCacheBindData

**Purpose**: Hold validated catalog name between bind and execute phases

**Structure**:
```cpp
struct MSSQLRefreshCacheBindData : public FunctionData {
    string catalog_name;

    // Required overrides
    unique_ptr<FunctionData> Copy() const;
    bool Equals(const FunctionData &other) const;
};
```

**Lifecycle**:
1. Created during bind phase with validated catalog name
2. Passed to execute phase via ExpressionState
3. Destroyed after query completion

## File Artifacts

### description.yml

**Purpose**: DuckDB community extension metadata
**Location**: Repository root
**Format**: YAML

**Schema**:
```yaml
extension:
  name: string          # "mssql"
  description: string   # Short description
  version: string       # Semver "0.1.0"
  language: string      # "C++"
  build: string         # "cmake"
  licence: string       # "MIT"
  maintainers: list     # Name + GitHub handle
  excluded_platforms: list  # ["osx_amd64", "windows_arm64"]

repo:
  github: string        # "hugr-lab/mssql-extension"
  ref: string           # Tag or branch

docs:
  hello_world: string   # SQL example (multiline)
  extended_description: string  # Feature summary
```

## Relationships

```
┌─────────────────────┐
│ mssql_refresh_cache │ (scalar function)
└──────────┬──────────┘
           │ calls
           ▼
┌─────────────────────┐     ┌─────────────────────┐
│    MSSQLCatalog     │────▶│ MSSQLMetadataCache  │
│  (catalog entry)    │     │   (in-memory)       │
└─────────────────────┘     └─────────────────────┘
           │                          │
           │ acquires                 │ queries
           ▼                          ▼
┌─────────────────────┐     ┌─────────────────────┐
│   ConnectionPool    │────▶│    SQL Server       │
│    (TDS layer)      │     │   (metadata)        │
└─────────────────────┘     └─────────────────────┘
```

## Validation Rules

| Field | Rule | Error |
|-------|------|-------|
| catalog_name (bind) | Non-empty string | InvalidInputException |
| catalog_name (bind) | Exists in attached catalogs | BinderException |
| catalog_name (bind) | Is MSSQL type catalog | BinderException |

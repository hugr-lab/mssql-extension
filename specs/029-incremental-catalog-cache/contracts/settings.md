# API Contract: Settings

**Feature**: 029-incremental-catalog-cache
**Version**: 1.0.0
**Date**: 2026-02-05

## Existing Setting (Unchanged)

### `mssql_catalog_cache_ttl`

TTL (time-to-live) for all catalog metadata levels.

| Property | Value |
|----------|-------|
| Type | BIGINT |
| Default | 0 |
| Scope | GLOBAL |
| Unit | Seconds |

**Behavior with Incremental Loading**:

- `0`: No automatic expiration. Metadata refreshed only via:
  - DDL point invalidation (CREATE/DROP/ALTER TABLE, CREATE/DROP SCHEMA)
  - Manual `mssql_refresh_cache()` call

- `> 0`: Each cache level expires independently based on its own `last_refresh` timestamp:
  - Schema list: refreshed when accessed after TTL expires
  - Table list (per-schema): refreshed when that schema is accessed after TTL expires
  - Column metadata (per-table): refreshed when that table is accessed after TTL expires

**Key Difference from Pre-Incremental Behavior**:

| Scenario | Before (Eager) | After (Incremental) |
|----------|----------------|---------------------|
| TTL expires, query `dbo.orders` | Full cache refresh (all schemas, all tables, all columns) | Only `dbo.orders` columns refreshed (if columns expired) |
| TTL expires, list schemas | Full cache refresh | Only schema list refreshed |

---

## Setting Loader Function

```cpp
int64_t LoadCatalogCacheTTL(ClientContext &context) {
    Value val;
    if (context.TryGetCurrentSetting("mssql_catalog_cache_ttl", val)) {
        return val.GetValue<int64_t>();
    }
    return 0;  // Manual refresh only
}
```

---

## Usage Examples

### Scenario 1: Large Database with Stable Schema

Schema rarely changes during session.

```sql
-- No automatic refresh; use DDL invalidation or manual refresh
SET mssql_catalog_cache_ttl = 0;
```

### Scenario 2: Active Development

Schema changes frequently, want automatic refresh.

```sql
-- All levels refresh after 5 minutes of staleness
SET mssql_catalog_cache_ttl = 300;
```

### Scenario 3: Long-Running Analytics Session

Want periodic refresh but not too aggressive.

```sql
-- 1 hour TTL
SET mssql_catalog_cache_ttl = 3600;
```

---

## Backward Compatibility

The setting behavior is unchanged:
- Default value: 0 (manual refresh only)
- Type and scope: unchanged
- `mssql_refresh_cache()`: still performs full eager refresh

The only difference is that TTL expiration now triggers **incremental** refresh (only the accessed level) rather than full cache reload.

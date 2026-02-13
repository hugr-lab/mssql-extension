# Research: DDL Schema Support

**Feature**: 035-ddl-schema-support
**Date**: 2026-02-13

## R1: How DuckDB passes IF NOT EXISTS to CreateSchema

**Decision**: Use `CreateSchemaInfo::on_conflict` field from the `CreateInfo` base class.

**Rationale**: `CreateSchemaInfo` inherits from `CreateInfo`, which has `on_conflict` of type `OnCreateConflict`. When DuckDB parses `CREATE SCHEMA IF NOT EXISTS`, it sets `on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT`. This is the same pattern used for `CREATE TABLE IF NOT EXISTS` in `MSSQLSchemaEntry::CreateTable()`.

**Alternatives considered**:
- Parsing the SQL string for "IF NOT EXISTS" — rejected, fragile and unnecessary since DuckDB already parses it.

## R2: How to check schema existence before CREATE

**Decision**: Use `MSSQLMetadataCache::HasSchema()` when cache is loaded; fall back to a direct SQL Server query otherwise.

**Rationale**: `HasSchema()` already exists and checks `schemas_.find(schema_name)`. However, the cache may not be loaded yet (state `NOT_LOADED`). For correctness, when the cache isn't loaded, we should query SQL Server directly with `SELECT SCHEMA_ID(N'schema_name')` — if the result is non-NULL, the schema exists.

**Alternatives considered**:
- Always force-loading the full schema list — rejected, too heavy for a simple existence check.
- Always querying SQL Server — rejected, wasteful when cache is already loaded.
- Using the existing `GetSchema()` method which already queries SQL Server — preferred, as it returns nullptr when schema doesn't exist. This also populates the cache entry.

## R3: How DropSchema handles IF EXISTS

**Decision**: Check `DropInfo::if_not_found` field. When set to `OnEntryNotFound::RETURN_NULL`, skip the DROP if schema doesn't exist.

**Rationale**: `DropInfo` has `if_not_found` of type `OnEntryNotFound`. DuckDB sets this to `RETURN_NULL` for `DROP SCHEMA IF EXISTS`. The current `DropSchema` implementation doesn't check this field and always attempts the DROP, which would fail on SQL Server if the schema doesn't exist.

**Alternatives considered**: None — this is the standard DuckDB pattern.

## R4: Cache invalidation verification

**Decision**: The current `InvalidateAll()` after CREATE/DROP SCHEMA is correct but can be optimized.

**Rationale**: `InvalidateAll()` resets `schemas_load_state_` to `NOT_LOADED` and clears all per-schema table lists. This is correct but heavy-handed. For CREATE SCHEMA, we could just add the schema to the cache. For DROP SCHEMA, we already erase the schema entry from `schema_entries_`. The `InvalidateAll()` ensures the schema list is re-fetched on next access.

**Alternatives considered**:
- Point invalidation (add/remove only the affected schema) — could optimize later but `InvalidateAll()` is correct and simple for now.

## R5: Existing CREATE TABLE IF NOT EXISTS pattern (reference)

**Decision**: Follow the same pattern from `MSSQLSchemaEntry::CreateTable()` (lines 86-144 of `mssql_schema_entry.cpp`).

**Pattern**:
```cpp
switch (base_info.on_conflict) {
case OnCreateConflict::IGNORE_ON_CONFLICT:
    // IF NOT EXISTS: check existence, return early if exists
    if (existing_entry) {
        return existing_entry;
    }
    break;
case OnCreateConflict::ERROR_ON_CONFLICT:
default:
    // Default: let SQL Server handle the error
    break;
}
```

For schemas, the equivalent is:
1. Check `info.on_conflict`
2. If `IGNORE_ON_CONFLICT`, check if schema exists (via `GetSchema` or cache)
3. If exists, return the existing schema entry without executing DDL
4. If not exists, proceed with CREATE SCHEMA as normal

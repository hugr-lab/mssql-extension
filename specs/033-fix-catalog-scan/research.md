# Research: Fix Catalog Scan & Object Visibility Filters

## R1: Root Cause of Eager Column Loading

**Decision**: The bug is in `MSSQLTableSet::LoadEntries()` which calls `cache.GetTableMetadata()` for every table. `GetTableMetadata()` triggers `EnsureColumnsLoaded()` → one SQL query per table.

**Rationale**: Traced through:
1. `MSSQLTableSet::Scan()` (line 47) → `EnsureLoaded()` (line 163) → `LoadEntries()` (line 98)
2. `LoadEntries()` iterates all table names and calls `cache.GetTableMetadata()` (line 126) for each
3. `GetTableMetadata()` calls `EnsureColumnsLoaded()` (line 168) which fires a per-table column discovery SQL query
4. With 65K tables → 65K round trips to SQL Server

**Alternatives considered**:
- Pushing column discovery SQL to the server side (WHERE clause filter) — not applicable, the issue is the number of queries, not per-query cost
- Batch column loading in `LoadEntries()` — considered as optimization but doesn't solve the fundamental issue of loading columns during scan

## R2: DuckDB TableCatalogEntry Column Immutability

**Decision**: `TableCatalogEntry::columns` (ColumnList) is set once in the constructor and is immutable. `GetColumns()` is non-virtual. We cannot create entries with empty columns and fill them later through the standard API.

**Rationale**: Examined DuckDB source:
- Constructor: `columns(std::move(info.columns))` — moved in initializer list
- `GetColumns()` returns `const ColumnList&` — non-virtual, returns protected member directly
- No public methods to modify ColumnList after construction

**Approach**: Two-level system in `MSSQLTableSet`:
- Track table NAMES separately from table ENTRIES
- Create entries with full columns only when actually accessed (GetEntry)
- For Scan, create entries on-the-fly with columns loaded per table (or batch load)

## R3: What Triggers Scan in DuckDB

**Decision**: Multiple DuckDB code paths trigger `SchemaCatalogEntry::Scan()`:

**Rationale**: Identified trigger paths:
1. **Error handling**: `CreateMissingEntryException()` → `SimilarEntriesInSchemas()` → iterates ALL schemas calling `Scan()` to find similar table names for error messages
2. **System functions**: `duckdb_tables()`, `duckdb_columns()` → `schema.Scan(CatalogType::TABLE_ENTRY, callback)`
3. **SHOW ALL TABLES**: Maps to `duckdb_tables()` → triggers Scan
4. **IDE/tool catalog queries**: Same as system functions

Key finding: When a schema lookup returns "not found" for any reason, DuckDB calls `GetSimilarEntry()` → `Scan()` on ALL schemas. This can be triggered during normal binding, not just explicit SHOW commands.

**Alternatives considered**:
- Overriding DuckDB's error handling — not feasible without modifying DuckDB core
- Preventing Scan calls entirely — not possible, Scan is part of DuckDB's catalog API

## R4: Deferred Column Loading Strategy

**Decision**: Use a name-tracking + lazy entry creation pattern:

1. `LoadEntries()` only loads table names (via `cache.GetTableNames()`, no column queries)
2. `GetEntry(name)` creates full entry with columns on demand (existing behavior, works correctly)
3. `Scan()` creates entries on-the-fly for each known name, loading columns per table

**Rationale**: This is the minimal change that:
- Fixes the single-table query case (GetEntry path, no unnecessary column loading)
- Keeps Scan working correctly (entries have full columns when returned)
- Avoids modifying DuckDB's non-virtual `GetColumns()` or ColumnList internals

**Trade-off**: Scan still loads all columns (one per table), but:
- Scan is only triggered by explicit operations (SHOW TABLES, etc.)
- The new `mssql_preload_catalog()` function provides a bulk alternative (1 query vs N)
- Regex filters reduce the number of tables that need column loading

## R5: Regex Library Choice

**Decision**: Use C++ `<regex>` with `std::regex_search` and `std::regex::icase` flag.

**Rationale**:
- Available in C++11, no external dependencies
- `regex_search` (not `regex_match`) allows partial matches — users can write `dbo` instead of `^dbo$`
- Case-insensitive by default matches SQL Server's typical collation behavior
- Performance is acceptable for filtering string lists (not per-row filtering)

**Alternatives considered**:
- DuckDB's `RE2` — would add a dependency on DuckDB internals
- POSIX regex — platform-dependent behavior
- Simple wildcard/LIKE matching — less powerful, users expect regex

## R6: Bulk Preload Query Design

**Decision**: Use a single SQL query joining sys.schemas, sys.objects, sys.columns, sys.types, and sys.partitions:

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

**Rationale**:
- Single round trip vs N+1 queries (1 for table names + N for columns)
- Ordered by schema/table/column_id → efficient streaming parse with group-by-break
- Reuses existing filter criteria from current schema/table/column discovery queries
- For 65K tables with ~10 columns each → ~650K rows in one result set (manageable)

**Alternatives considered**:
- Multiple batched queries — more complex, more round trips
- Server-side cursor with FETCH — unnecessary complexity for metadata

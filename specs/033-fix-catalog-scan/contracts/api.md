# API Contracts: Fix Catalog Scan & Object Visibility Filters

## New DuckDB Functions

### `mssql_preload_catalog(context_name [, schema_name])` — Scalar Function

**Purpose**: Bulk-load all metadata (schemas, tables, columns) for an attached MSSQL database in a single SQL Server round trip.

**Signature**:
```sql
mssql_preload_catalog(context_name VARCHAR) → VARCHAR
mssql_preload_catalog(context_name VARCHAR, schema_name VARCHAR) → VARCHAR
```

**Parameters**:
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| context_name | VARCHAR | Yes | Name of the attached MSSQL database context |
| schema_name | VARCHAR | No | Limit preload to a single schema |

**Returns**: Status message (e.g., `"Loaded 5 schemas, 1234 tables, 15678 columns"`)

**Behavior**:
- Executes a single bulk SQL query joining sys.schemas/objects/columns/types/partitions
- Streams results and populates the metadata cache
- Applies configured regex filters before caching
- Sets all cache load states to LOADED
- Subsequent catalog operations (Scan, GetEntry) use the preloaded cache

**Error conditions**:
- Unknown context_name → error
- Connection failure → error
- If schema_name provided but doesn't exist → warning, empty result

**Examples**:
```sql
-- Preload all metadata
SELECT mssql_preload_catalog('erp');
-- → "Loaded 3 schemas, 65234 tables, 487123 columns"

-- Preload single schema
SELECT mssql_preload_catalog('erp', 'dbo');
-- → "Loaded 1 schema, 42100 tables, 301234 columns"
```

---

## New ATTACH Options

### `schema_filter` — Regex Pattern

**Purpose**: Limit which SQL Server schemas are visible in the DuckDB catalog.

**Type**: VARCHAR (regex pattern string)
**Default**: Empty (all schemas visible)
**Scope**: Per-attached database

**Usage in ATTACH**:
```sql
ATTACH 'Server=host;Database=db;User Id=u;Password=p' AS mydb (
    TYPE mssql,
    schema_filter 'dbo|sales'
);
```

**Usage in connection string**:
```
Server=host;Database=db;User Id=u;Password=p;SchemaFilter=dbo|sales
```

**Usage in secret**:
```sql
CREATE SECRET my_secret (
    TYPE mssql,
    host 'host', database 'db', user 'u', password 'p',
    schema_filter 'dbo|sales'
);
```

**Behavior**:
- Pattern is compiled as case-insensitive regex
- `regex_search` is used (partial match), so `dbo` matches `dbo` without anchors
- For exact match, use anchors: `^dbo$`
- Invalid regex → error at ATTACH time
- Applied during `EnsureSchemasLoaded()` — filtered schemas never enter the cache

### `table_filter` — Regex Pattern

**Purpose**: Limit which tables/views are visible in the DuckDB catalog.

**Type**: VARCHAR (regex pattern string)
**Default**: Empty (all tables/views visible)
**Scope**: Per-attached database

**Usage in ATTACH**:
```sql
ATTACH 'Server=host;Database=db;User Id=u;Password=p' AS mydb (
    TYPE mssql,
    table_filter '^(Orders|Products|Customers)$'
);
```

**Usage in connection string**:
```
Server=host;Database=db;User Id=u;Password=p;TableFilter=^(Orders|Products|Customers)$
```

**Usage in secret**:
```sql
CREATE SECRET my_secret (
    TYPE mssql,
    host 'host', database 'db', user 'u', password 'p',
    table_filter '^(Orders|Products|Customers)$'
);
```

**Behavior**:
- Same regex behavior as `schema_filter`
- Applied during `EnsureTablesLoaded()` — filtered tables never enter the cache
- Also checked in `GetEntry()` — direct lookup of filtered table returns not found
- Does NOT affect `mssql_scan()` or `mssql_exec()` (raw T-SQL bypass)

---

## Precedence Rules

| Source | Priority |
|--------|----------|
| ATTACH option | Highest (overrides everything) |
| Connection string parameter | Second |
| Secret field | Lowest (used as default) |

When ATTACH specifies `schema_filter` or `table_filter`, that value is used regardless of what the connection string or secret contains.

---

## Modified Behavior

### `MSSQLTableSet::Scan()` — Changed

**Before**: Calls `EnsureLoaded()` which loads ALL table metadata including columns for every table.

**After**: Calls `EnsureNamesLoaded()` which loads only table names. For each known name, creates entries on-the-fly with full column loading. Entries are cached for subsequent calls.

### `MSSQLTableSet::GetEntry()` — Changed

**Before**: Checks cached entries, then calls `LoadSingleEntry()` for uncached tables. Correct behavior.

**After**: Same logic, but also checks `known_table_names_` before attempting to load. Also respects `table_filter` — returns nullptr for filtered-out tables.

### `MSSQLMetadataCache::GetSchemaNames()` — Changed

**After**: Applies `schema_filter` regex to results. Only matching schemas are returned.

### `MSSQLMetadataCache::GetTableNames()` — Changed

**After**: Applies `table_filter` regex to results. Only matching tables are returned.

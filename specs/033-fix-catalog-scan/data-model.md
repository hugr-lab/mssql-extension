# Data Model: Fix Catalog Scan & Object Visibility Filters

## Modified Entities

### MSSQLConnectionInfo (modified)

```
MSSQLConnectionInfo
├── host: string
├── port: uint16_t (default: 1433)
├── database: string
├── user: string
├── password: string
├── use_encrypt: bool (default: true)
├── catalog_enabled: bool (default: true)
├── use_azure_auth: bool
├── azure_secret_name: string
├── access_token: string
├── is_fabric_endpoint: bool
├── schema_filter: string          # NEW - regex pattern for schema visibility
└── table_filter: string           # NEW - regex pattern for table/view visibility
```

**Validation**: `schema_filter` and `table_filter` are validated as valid regex at ATTACH time. Empty string means "no filter" (all objects visible).

### MSSQLMetadataCache (modified)

```
MSSQLMetadataCache
├── schemas_: map<string, MSSQLSchemaMetadata>
├── state_: MSSQLCacheState
├── schemas_load_state_: CacheLoadState
├── ttl_seconds_: int64_t
├── database_collation_: string
├── schema_filter_: optional<regex>    # NEW - compiled schema filter
├── table_filter_: optional<regex>     # NEW - compiled table filter
│
├── GetSchemaNames(conn) → vector<string>           # Modified: applies schema_filter_
├── GetTableNames(conn, schema) → vector<string>    # Modified: applies table_filter_
├── GetTableMetadata(conn, schema, table) → *meta   # Unchanged
├── GetTableBasicMetadata(schema, table) → *meta    # NEW: no column loading
├── BulkLoadAll(conn [, schema]) → void             # NEW: single-query full load
├── SetFilters(schema_re, table_re) → void          # NEW: set compiled filters
└── MatchesFilter(name, filter) → bool              # NEW: test name against filter
```

### MSSQLTableSet (modified)

```
MSSQLTableSet
├── schema_: MSSQLSchemaEntry&
├── entries_: map<string, unique_ptr<MSSQLTableEntry>>  # Fully loaded entries
├── known_table_names_: set<string>                     # NEW: names-only tracking
├── names_loaded_: atomic<bool>                         # NEW: names loaded flag
├── is_fully_loaded_: atomic<bool>                      # All entries created with columns
├── entry_mutex_: mutex
├── load_mutex_: mutex
├── attempted_tables_: set<string>
│
├── GetEntry(ctx, name) → CatalogEntry*     # Modified: creates entry on demand
├── Scan(ctx, callback) → void              # Modified: uses names, defers columns
├── LoadNames(ctx) → void                   # NEW: loads table names only
├── LoadEntries(ctx) → void                 # Modified: creates entries with columns
└── EnsureLoaded(ctx) → void                # Modified: names-only loading
```

### MSSQLObjectFilter (new)

```
MSSQLObjectFilter
├── schema_pattern_: optional<regex>
├── table_pattern_: optional<regex>
│
├── static Compile(pattern_str) → regex          # Compile with icase flag
├── static Validate(pattern_str) → bool          # Check if valid regex
├── MatchesSchema(name) → bool                   # Test schema name
├── MatchesTable(name) → bool                    # Test table name
└── HasFilters() → bool                          # Any filter active?
```

## Entity Relationships

```
MSSQLConnectionInfo ──has──> schema_filter, table_filter (string patterns)
        │
        │ parsed at ATTACH time
        ▼
MSSQLObjectFilter ──compiled──> regex patterns
        │
        │ passed to
        ▼
MSSQLMetadataCache ──filters──> schema/table discovery results
        │
        │ provides names and metadata to
        ▼
MSSQLTableSet ──creates──> MSSQLTableEntry (on demand, with full columns)
        │
        │ returned by Scan() and GetEntry()
        ▼
DuckDB Catalog API ──uses──> CatalogEntry for binding, scanning
```

## State Transitions

### MSSQLTableSet Loading States

```
                 ┌─────────────┐
                 │  EMPTY       │  (names_loaded_=false, is_fully_loaded_=false)
                 └──────┬──────┘
                        │ LoadNames() or first GetEntry()
                        ▼
                 ┌─────────────┐
                 │ NAMES_LOADED │  (names_loaded_=true, is_fully_loaded_=false)
                 └──────┬──────┘
                        │ individual GetEntry() creates entries on demand
                        │ Scan() creates all entries
                        ▼
                 ┌─────────────┐
                 │ FULLY_LOADED │  (names_loaded_=true, is_fully_loaded_=true)
                 └─────────────┘
```

### Bulk Preload Flow

```
mssql_preload_catalog('ctx')
    │
    ▼
BulkLoadAll(connection)
    │
    ├── Execute single JOIN query
    ├── Stream results, group by schema/table
    ├── Apply regex filters to each schema/table name
    ├── Populate schemas_, tables, columns in cache
    ├── Set all load states to LOADED
    │
    ▼
Cache fully populated → all subsequent operations use cache
```

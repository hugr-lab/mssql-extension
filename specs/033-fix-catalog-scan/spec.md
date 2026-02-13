# Feature Specification: Fix Catalog Scan & Add Object Visibility Filters

**Feature Branch**: `033-fix-catalog-scan`
**Created**: 2026-02-13
**Status**: Draft
**Input**: User description: "Connecting to an ERP database with ~65,000 tables. Even a simple SELECT on a single table with LIMIT 1 never finishes because the extension fires thousands of metadata column queries for every table in the schema. Also: add parameter to connection string and secret with regex patterns to select visible objects (schemas, tables, views) in the DuckDB catalog."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Query a Single Table in a Large Database (Priority: P1)

A user connects to an ERP database with ~65,000 tables using `ATTACH`. They run a simple query like `SELECT * FROM mssql_db.dbo.Orders LIMIT 1`. The query should complete quickly by loading metadata only for the `Orders` table, not for all 65,000 tables in the schema.

**Why this priority**: This is the core bug. Without this fix, the extension is unusable for any database with a large number of tables, which is the primary use case for ERP systems.

**Independent Test**: Can be fully tested by attaching to a database with many tables and running a single-table SELECT with LIMIT. Verify that only one column-discovery query is issued (for the target table), not thousands.

**Acceptance Scenarios**:

1. **Given** a SQL Server database with 1,000+ tables is attached, **When** the user runs `SELECT * FROM attached.dbo.SomeTable LIMIT 1`, **Then** the query completes without loading column metadata for any table other than `SomeTable`.
2. **Given** a SQL Server database with 1,000+ tables is attached, **When** the user runs the same single-table query, **Then** the number of column-discovery queries issued to SQL Server is exactly 1 (for the target table only).
3. **Given** a SQL Server database is attached, **When** the user runs `SELECT * FROM attached.dbo.TableA LIMIT 1` followed by `SELECT * FROM attached.dbo.TableB LIMIT 1`, **Then** column metadata is loaded incrementally (1 query per table accessed, 2 total).

---

### User Story 2 - Schema Scan Defers Column Loading (Priority: P2)

When a full schema scan is triggered (e.g., by `SHOW TABLES`, `information_schema` queries, or DuckDB internal catalog operations), the extension should load only table names and basic metadata (object type, approximate row count) without eagerly loading column metadata for every table. Column metadata should be loaded on-demand when a specific table is first accessed.

**Why this priority**: Schema scans are triggered by many DuckDB operations and IDE tools. If each scan loads columns for all tables, the extension remains unusable for large databases even when the user hasn't explicitly queried all tables.

**Independent Test**: Can be tested by triggering a schema scan (e.g., `SHOW ALL TABLES`) on a large database and verifying that no column-discovery queries are issued during the scan itself.

**Acceptance Scenarios**:

1. **Given** a SQL Server database with 1,000+ tables is attached, **When** a schema scan is triggered (e.g., `SHOW ALL TABLES`), **Then** no column-discovery queries are issued to SQL Server during the scan.
2. **Given** a schema scan has completed, **When** the user subsequently queries a specific table, **Then** column metadata is loaded on-demand for just that table.
3. **Given** a schema scan has completed, **When** the user queries 5 different tables, **Then** exactly 5 column-discovery queries are issued (one per table).

---

### User Story 3 - Filter Visible Objects via Regex in Connection String (Priority: P2)

A user attaching to a large ERP database wants to limit which schemas, tables, and views are visible in the DuckDB catalog. They provide regex filter patterns via the connection string or ATTACH options to include or exclude objects by name.

For example:
```sql
-- Only see tables starting with "Sales" or "Inv" in the dbo schema
ATTACH 'Server=host;Database=erp;...' AS erp (
    TYPE mssql,
    schema_filter 'dbo|sales',
    table_filter '^(Sales|Inv).*'
);
```

**Why this priority**: For databases with 65,000 tables, even a deferred schema scan returns 65,000 table names. Regex filtering at the catalog level lets users scope the visible catalog to just the subset they need, dramatically reducing noise and improving usability with IDEs and tools that enumerate all tables.

**Independent Test**: Can be tested by attaching with a table filter pattern and verifying that only matching tables appear in `SHOW ALL TABLES` and that non-matching tables are invisible to queries.

**Acceptance Scenarios**:

1. **Given** a SQL Server database with many tables is attached with `table_filter '^Orders$'`, **When** the user runs `SHOW ALL TABLES`, **Then** only the `Orders` table is listed.
2. **Given** a SQL Server database is attached with `schema_filter '^dbo$'`, **When** the user runs `SHOW ALL TABLES`, **Then** only tables from the `dbo` schema are listed, not tables from other schemas.
3. **Given** a SQL Server database is attached with `table_filter '^Sales.*'`, **When** the user tries to query a table not matching the filter (e.g., `SELECT * FROM attached.dbo.Inventory`), **Then** the query fails with a "table not found" error.
4. **Given** a SQL Server database is attached without any filter parameters, **When** the user runs queries, **Then** all schemas and tables are visible (backward compatible).

---

### User Story 4 - Filter Visible Objects via Regex in Secret (Priority: P3)

A user defines an MSSQL secret with regex filter patterns for reusable connection profiles. The filters work identically to connection string filters.

For example:
```sql
CREATE SECRET erp_secret (
    TYPE mssql,
    host 'erp-server',
    database 'erp_db',
    user 'reader',
    password 'pass',
    schema_filter 'dbo',
    table_filter '^(Orders|Products|Customers).*'
);
ATTACH '' AS erp (TYPE mssql, SECRET erp_secret);
```

**Why this priority**: Secrets provide reusable connection configurations. Filter patterns in secrets ensure consistent object visibility across sessions without repeating filter options on each ATTACH.

**Independent Test**: Can be tested by creating a secret with filter patterns, attaching with that secret, and verifying only matching objects are visible.

**Acceptance Scenarios**:

1. **Given** a secret with `table_filter '^Orders$'` is created, **When** the user attaches using that secret, **Then** only the `Orders` table is visible.
2. **Given** a secret with filters is created and ATTACH also specifies filters, **When** the user attaches, **Then** the ATTACH-level filters override the secret-level filters.

---

### User Story 5 - Bulk Catalog Preload Function (Priority: P3)

A user who knows they will need metadata for many or all tables can trigger a single bulk preload operation that fetches all schema, table, and column metadata in one efficient SQL query rather than N individual queries.

For example:
```sql
-- Preload all metadata for the attached database in a single round trip
SELECT mssql_preload_catalog('erp');

-- Or preload a specific schema only
SELECT mssql_preload_catalog('erp', 'dbo');
```

This uses a single bulk SQL query joining `sys.schemas`, `sys.objects`, `sys.columns`, `sys.types`, and `sys.partitions` to fetch all metadata in one round trip. The results are parsed and used to populate the metadata cache. Regex filters (if configured) are applied to the results before caching.

**Why this priority**: For users who need full catalog access (IDE integration, data exploration), a single bulk query is orders of magnitude faster than N individual column queries. This provides an opt-in way to preload the entire catalog efficiently.

**Independent Test**: Can be tested by calling `mssql_preload_catalog('ctx')` and verifying that the metadata cache is fully populated with a single SQL Server round trip, and that subsequent queries use the cached data.

**Acceptance Scenarios**:

1. **Given** a SQL Server database with 1,000+ tables is attached, **When** the user calls `mssql_preload_catalog('ctx')`, **Then** all schema, table, and column metadata is loaded via a single bulk SQL query.
2. **Given** `mssql_preload_catalog('ctx')` has been called, **When** the user queries any table, **Then** no additional metadata queries are issued (all data is cached).
3. **Given** regex filters are configured and `mssql_preload_catalog('ctx')` is called, **When** the cache is populated, **Then** only objects matching the filters are included.
4. **Given** a SQL Server database with 65,000 tables is attached, **When** `mssql_preload_catalog('ctx')` is called, **Then** it completes in seconds (single query) rather than minutes (65,000 queries).

---

### User Story 6 - Existing Single-Table Lookup Remains Efficient (Priority: P3)

The existing lazy single-table lookup path (`GetEntry`) must continue to work correctly and efficiently, loading only the requested table's metadata including columns.

**Why this priority**: This path already works correctly for direct table lookups. This story ensures the fix for the scan path does not regress the single-table path.

**Independent Test**: Can be tested by directly querying a table by its fully-qualified name and verifying only that table's metadata is loaded.

**Acceptance Scenarios**:

1. **Given** a SQL Server database is attached and no tables have been loaded yet, **When** the user queries `SELECT * FROM attached.dbo.SpecificTable LIMIT 1`, **Then** only `SpecificTable`'s column metadata is loaded.
2. **Given** `SpecificTable` was previously loaded, **When** the user queries it again, **Then** no additional metadata queries are issued (cache hit).

---

### Edge Cases

- What happens when a schema scan is in progress and a single-table lookup is requested concurrently? The single-table lookup should proceed independently without waiting for the full scan.
- What happens when a table's column metadata is needed during a scan callback (e.g., `duckdb_columns()` function)? Column loading should be deferred until the table entry is actually inspected, not during the scan enumeration.
- What happens when the metadata cache TTL expires during a scan? The table list may be refreshed, but column metadata should not be eagerly reloaded for all tables.
- What happens when `mssql_refresh_cache()` is called? It should invalidate table lists and column caches, but not trigger eager reloading of all columns.
- What happens when a regex filter pattern is invalid? The system should reject it at ATTACH time with a clear error message.
- What happens when a table matches the filter but the user queries it with a fully-qualified name that doesn't match? The filter applies to object names only, not to query syntax.
- What happens when filters are changed by re-attaching? The new filters take effect; previously cached metadata is invalidated.
- What happens when `table_filter` is set but the user directly queries a non-matching table via `mssql_scan()` (raw T-SQL)? The `mssql_scan()` function bypasses the catalog and should still work, as filters only affect DuckDB catalog visibility.

## Requirements *(mandatory)*

### Functional Requirements

**Deferred Column Loading (Bug Fix)**:

- **FR-001**: The `MSSQLTableSet::Scan()` method MUST NOT trigger column-discovery queries for tables during enumeration. It should create table entries with deferred column loading.
- **FR-002**: The `MSSQLTableSet::LoadEntries()` method MUST load only table names and basic metadata (name, object type, row count) from the metadata cache, NOT column metadata for each table.
- **FR-003**: Column metadata MUST be loaded on-demand when a table entry's columns are first accessed (e.g., during query planning or execution), not when the table entry is created.
- **FR-004**: The existing `MSSQLTableSet::GetEntry()` single-table lookup path MUST continue to load full metadata (including columns) for the requested table only.
- **FR-005**: The `MSSQLTableEntry` MUST support a deferred column loading pattern where columns are populated lazily on first access.
- **FR-006**: Thread safety MUST be maintained for concurrent single-table lookups and schema scans.
- **FR-007**: The metadata cache's three-level loading (schemas -> tables -> columns) MUST be respected, with `LoadEntries` only triggering up to the tables level, not the columns level.

**Object Visibility Filters (New Feature)**:

- **FR-008**: The system MUST accept an optional `schema_filter` parameter (regex pattern) via the connection string, ATTACH options, and MSSQL secrets. When set, only schemas whose names match the pattern are visible in the DuckDB catalog.
- **FR-009**: The system MUST accept an optional `table_filter` parameter (regex pattern) via the connection string, ATTACH options, and MSSQL secrets. When set, only tables and views whose names match the pattern are visible in the DuckDB catalog.
- **FR-010**: Filter patterns MUST use standard regex syntax (case-insensitive matching by default).
- **FR-011**: Filter patterns MUST be validated at ATTACH time. An invalid regex MUST produce a clear error message and prevent the attach from succeeding.
- **FR-012**: When no filter parameters are specified, all schemas and tables MUST be visible (backward compatible behavior).
- **FR-013**: Filters MUST apply to catalog-level operations (schema discovery, table discovery, `SHOW TABLES`, `information_schema` queries, DuckDB internal catalog scans). They MUST NOT affect raw T-SQL execution via `mssql_scan()` or `mssql_exec()`.
- **FR-014**: When both a secret and ATTACH options specify filter values, the ATTACH option values MUST take precedence.
- **FR-015**: Filters MUST apply at the metadata cache level so that filtered-out objects never enter the DuckDB catalog and never trigger metadata queries.
- **FR-016**: The `GetEntry()` single-table lookup MUST also respect filters — looking up a table that doesn't match the filter MUST return "not found".

**Bulk Catalog Preload (New Feature)**:

- **FR-017**: The system MUST provide a scalar function `mssql_preload_catalog(context_name [, schema_name])` that bulk-loads all metadata (schemas, tables, columns) in a single SQL query.
- **FR-018**: The bulk preload query MUST join `sys.schemas`, `sys.objects`, `sys.columns`, `sys.types`, and `sys.partitions` to fetch all metadata in one round trip.
- **FR-019**: The bulk preload results MUST respect configured regex filters (`schema_filter`, `table_filter`), discarding non-matching objects before caching.
- **FR-020**: After bulk preload, all subsequent catalog operations (Scan, GetEntry) MUST use the preloaded cache without issuing additional metadata queries.
- **FR-021**: If an optional `schema_name` argument is provided, the bulk preload MUST be scoped to that single schema only.

### Key Entities

- **MSSQLTableSet**: Manages the set of table entries for a schema. Currently calls `GetTableMetadata()` (which loads columns) for every table during `LoadEntries()`. Must be changed to create lightweight table entries without columns.
- **MSSQLTableEntry**: Represents a table in the DuckDB catalog. Must support deferred column loading so that columns are populated on first access rather than at creation time.
- **MSSQLMetadataCache**: Three-level cache (schemas -> tables -> columns). The `GetTableMetadata()` method triggers `EnsureColumnsLoaded()`. A new method or flag is needed to get table-level metadata without triggering column loading. Must also apply schema/table filters during discovery.
- **MSSQLTableMetadata**: Metadata struct for a table. Already has `columns_load_state` field to track whether columns are loaded.
- **MSSQLConnectionInfo**: Connection configuration struct. Must be extended to carry `schema_filter` and `table_filter` regex patterns from connection string, ATTACH options, or secrets.
- **Object Visibility Filter**: A filter configuration that holds compiled regex patterns for schemas and tables. Applied during metadata cache discovery queries and catalog entry enumeration.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A single-table query on a database with 1,000+ tables completes with only 1 column-discovery query, not N queries (where N is the total table count).
- **SC-002**: Attaching to a database with 65,000 tables and running a simple `SELECT ... LIMIT 1` completes in seconds, not minutes or hours.
- **SC-003**: A full schema scan (`SHOW ALL TABLES`) on a database with 1,000+ tables completes without issuing any column-discovery queries.
- **SC-004**: Memory usage during attach and first query remains proportional to the number of tables actually accessed, not the total table count.
- **SC-005**: When `table_filter` is set to match 10 tables out of 65,000, `SHOW ALL TABLES` returns only those 10 tables.
- **SC-006**: When `schema_filter` is set to match 1 schema out of many, only that schema's objects are discoverable.
- **SC-007**: An invalid regex pattern in `schema_filter` or `table_filter` produces a clear error at ATTACH time.
- **SC-008**: All existing integration tests continue to pass with no regressions (backward compatibility).

## Assumptions

- The DuckDB catalog API's `Scan` method is used for enumeration purposes and does not require fully-populated column metadata for each entry during the scan itself.
- Table entries created without column metadata can still satisfy DuckDB's catalog requirements for enumeration (name, type information) without columns.
- The `MSSQLTableEntry` class can be extended to support deferred column loading without breaking the DuckDB catalog entry contract.
- The `MSSQLMetadataCache` already supports the table-level metadata without columns (via `EnsureTablesLoaded`), so a method to retrieve table metadata without triggering column loading is feasible.
- Standard C++ regex (`<regex>` header) or a simple pattern matching approach is sufficient for filter patterns. No external regex library is needed.
- Regex patterns are applied client-side (in the extension) after fetching schema/table names from SQL Server, not pushed down as SQL Server `LIKE` or `PATINDEX` conditions. This keeps the SQL queries simple and the filtering logic consistent.

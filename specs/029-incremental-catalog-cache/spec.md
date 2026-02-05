# Feature Specification: Incremental Catalog Cache with TTL and Point Invalidation

**Feature Branch**: `029-incremental-catalog-cache`
**Created**: 2026-02-05
**Status**: Draft
**Input**: User description: "Incremental MSSQL Catalog Cache with Schema/Table TTL and Point Invalidation via DuckDB Catalog DDL"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Lazy Column Loading on First Query (Priority: P1)

A data analyst attaches a large SQL Server database with hundreds of tables and immediately queries a specific table. The system should load only the metadata needed for that specific table, not all tables' columns.

**Why this priority**: This is the core performance improvement. Large databases with many tables currently load all column metadata upfront, causing slow initial access. Lazy loading provides immediate value by dramatically reducing initial query time.

**Independent Test**: Can be fully tested by attaching a database and running `SELECT * FROM schema.table LIMIT 1` - only that table's columns should be loaded, measurable via metadata query count.

**Acceptance Scenarios**:

1. **Given** a database with 500+ tables attached, **When** user executes `SELECT * FROM dbo.orders LIMIT 1`, **Then** only `dbo.orders` column metadata is loaded (not columns for other tables)
2. **Given** an attached database, **When** user queries `dbo.customers` then `dbo.products`, **Then** columns are loaded incrementally for each table on first access
3. **Given** a table's columns are already cached, **When** user queries the same table again, **Then** no additional metadata queries are executed

---

### User Story 2 - Lazy Schema and Table List Loading (Priority: P1)

A user attaches a SQL Server database and browses schemas incrementally. The system should not load all schemas' table lists upfront - only when a specific schema is accessed.

**Why this priority**: Combined with P1 story above, this completes the lazy loading foundation. Loading table lists per-schema (instead of all at once) further reduces initial overhead for multi-schema databases.

**Independent Test**: Can be tested by attaching a database and accessing `dbo` schema - only `dbo` tables should be enumerated, not tables from other schemas.

**Acceptance Scenarios**:

1. **Given** `ATTACH '' AS sqlserver (TYPE mssql, ...)`, **When** attach completes, **Then** no `sys.columns` queries are executed (columns not loaded eagerly)
2. **Given** an attached database, **When** user accesses `sqlserver.dbo.tablename`, **Then** table list for `dbo` schema is loaded but not for other schemas
3. **Given** `dbo` schema tables are cached, **When** user accesses `sqlserver.sales.orders`, **Then** only `sales` schema table list is loaded

---

### User Story 3 - Schema-Level TTL Expiration (Priority: P2)

A database administrator configures a TTL for schema metadata. When the TTL expires, the next access to that schema should refresh only that schema's table list, not the entire cache.

**Why this priority**: TTL-based refresh is important for long-running sessions where schema structure may change. Per-schema TTL is more efficient than global refresh but depends on lazy loading infrastructure.

**Independent Test**: Can be tested by setting a short TTL, waiting for expiration, and verifying only the accessed schema is refreshed.

**Acceptance Scenarios**:

1. **Given** schema TTL set to 60 seconds and `dbo` tables cached 90 seconds ago, **When** user queries `dbo.orders`, **Then** only `dbo` table list is refreshed
2. **Given** schema TTL expired for `dbo` but not for `sales`, **When** user queries `dbo.orders`, **Then** `sales` schema cache remains unchanged
3. **Given** TTL = 0 (disabled), **When** time passes, **Then** no automatic refresh occurs (only manual or DDL-driven invalidation)

---

### User Story 4 - Table-Level TTL Expiration (Priority: P2)

A user works with a table whose columns may change over time. When the table's column metadata TTL expires, only that table's columns should be refreshed on next access.

**Why this priority**: Granular TTL at table level allows fine-grained control over freshness. Depends on lazy loading and schema TTL infrastructure.

**Independent Test**: Can be tested by setting a short column TTL, modifying a table externally, waiting, and verifying column refresh occurs only for the accessed table.

**Acceptance Scenarios**:

1. **Given** table TTL set to 120 seconds and `dbo.orders` columns cached 150 seconds ago, **When** user queries `dbo.orders`, **Then** only `dbo.orders` columns are refreshed
2. **Given** table TTL expired for `dbo.orders`, **When** user queries `dbo.customers`, **Then** `dbo.orders` columns remain stale until accessed
3. **Given** both schema TTL and table TTL configured, **When** schema TTL expires, **Then** table list refreshes but column metadata uses its own TTL

---

### User Story 5 - Point Invalidation on CREATE TABLE (Priority: P2)

A user creates a new table via DuckDB's catalog DDL. The new table should appear in the schema immediately without requiring a global cache refresh.

**Why this priority**: DDL-driven invalidation provides instant visibility of changes made through DuckDB, improving developer experience. Depends on cache structure changes from P1 stories.

**Independent Test**: Can be tested by creating a table via `CREATE TABLE sqlserver.dbo.newtable (...)` and immediately querying `duckdb_tables()` to verify it appears.

**Acceptance Scenarios**:

1. **Given** cached schema `dbo` with tables [orders, customers], **When** user executes `CREATE TABLE sqlserver.dbo.products (...)`, **Then** `products` appears in schema without full cache refresh
2. **Given** `CREATE TABLE` executed in `dbo`, **When** user queries `sales` schema, **Then** `sales` cache is not refreshed
3. **Given** `CREATE TABLE` completes successfully, **When** user immediately runs `SELECT * FROM sqlserver.dbo.products`, **Then** query succeeds with correct column metadata

---

### User Story 6 - Point Invalidation on DROP TABLE (Priority: P2)

A user drops a table via DuckDB's catalog DDL. The table should disappear from the schema cache immediately.

**Why this priority**: Complements CREATE TABLE invalidation for consistent DDL behavior.

**Independent Test**: Can be tested by dropping a table via `DROP TABLE sqlserver.dbo.tablename` and verifying it no longer appears in `duckdb_tables()`.

**Acceptance Scenarios**:

1. **Given** cached table `dbo.products`, **When** user executes `DROP TABLE sqlserver.dbo.products`, **Then** `products` is removed from cache immediately
2. **Given** `DROP TABLE` executed, **When** user queries `duckdb_tables()` for `dbo`, **Then** dropped table does not appear
3. **Given** `dbo.products` dropped, **When** user queries `dbo.orders`, **Then** no unnecessary cache refresh occurs

---

### User Story 7 - Point Invalidation on ALTER TABLE (Priority: P3)

A user alters a table's columns via DuckDB's catalog DDL. Only the altered table's column metadata should be invalidated.

**Why this priority**: ALTER TABLE is less frequent than CREATE/DROP but still important for DDL consistency. Lower priority as it affects fewer workflows.

**Independent Test**: Can be tested by adding a column via `ALTER TABLE` and verifying only that table's columns are refreshed.

**Acceptance Scenarios**:

1. **Given** cached table `dbo.orders` with columns [id, date, total], **When** user executes `ALTER TABLE sqlserver.dbo.orders ADD customer_id INT`, **Then** only `dbo.orders` columns are invalidated
2. **Given** `ALTER TABLE` executed on `dbo.orders`, **When** user queries `dbo.orders`, **Then** new column `customer_id` is visible
3. **Given** `ALTER TABLE` executed on `dbo.orders`, **When** user queries `dbo.customers`, **Then** `dbo.customers` cache is unchanged

---

### User Story 8 - Point Invalidation on Schema DDL (Priority: P3)

A user creates or drops a schema via DuckDB's catalog DDL. Only the affected schema should be invalidated.

**Why this priority**: Schema-level DDL is least frequent but completes the DDL invalidation coverage.

**Independent Test**: Can be tested by creating a schema and verifying it appears without full cache refresh.

**Acceptance Scenarios**:

1. **Given** cached schemas [dbo, sales], **When** user executes `CREATE SCHEMA sqlserver.reporting`, **Then** `reporting` appears in schema list without full refresh
2. **Given** `DROP SCHEMA sqlserver.staging`, **When** executed, **Then** `staging` is removed from cache immediately
3. **Given** schema DDL executed, **When** user queries unrelated schema `dbo`, **Then** `dbo` cache is unchanged

---

### Edge Cases

- What happens when a schema is accessed that doesn't exist? → Clear error message, no cache pollution
- What happens when TTL expires during an active transaction? → Current transaction continues with cached data, refresh occurs on next transaction
- What happens when multiple concurrent queries access an uncached table? → Only one metadata query executes, others wait (existing thread-safety)
- What happens when `mssql_exec` modifies schema structure? → No automatic invalidation; user must call `mssql_refresh_cache()` manually
- What happens when external tools modify SQL Server schema? → No automatic invalidation; TTL-based refresh or manual refresh required

## Requirements *(mandatory)*

### Functional Requirements

**Lazy Loading**:

- **FR-001**: System MUST NOT load column metadata for all tables during `ATTACH`
- **FR-002**: System MUST load schema list only when first schema is accessed
- **FR-003**: System MUST load table list for a schema only when that schema is first accessed
- **FR-004**: System MUST load column metadata for a table only when that table is first accessed (binding/scan)
- **FR-005**: System MUST track loading state (not_loaded, loading, loaded) at schema and table levels

**TTL Management**:

- **FR-006**: System MUST support separate TTL configuration for schema metadata (table lists)
- **FR-007**: System MUST support separate TTL configuration for table metadata (columns)
- **FR-008**: System MUST track last refresh timestamp at schema and table levels independently
- **FR-009**: System MUST refresh only the stale metadata level on TTL expiration (not entire cache)
- **FR-010**: When TTL = 0, system MUST disable automatic expiration (manual/DDL invalidation only)

**Point Invalidation**:

- **FR-011**: `CREATE TABLE` via DuckDB Catalog MUST invalidate only the parent schema's table list
- **FR-012**: `DROP TABLE` via DuckDB Catalog MUST remove table from cache and invalidate schema's table list
- **FR-013**: `ALTER TABLE` via DuckDB Catalog MUST invalidate only the affected table's column metadata
- **FR-014**: `CREATE SCHEMA` via DuckDB Catalog MUST add schema to cache without global refresh
- **FR-015**: `DROP SCHEMA` via DuckDB Catalog MUST remove schema and its contents from cache
- **FR-016**: Point invalidation MUST NOT be triggered by `mssql_exec()` (no SQL parsing)

**Compatibility**:

- **FR-017**: System MUST maintain existing thread-safety guarantees in catalog cache
- **FR-018**: System MUST maintain backward compatibility with existing `mssql_refresh_cache()` function
- **FR-019**: Existing `mssql_catalog_cache_ttl` setting behavior MUST be preserved as default/fallback

### Key Entities

- **SchemaMetadata**: Represents cached schema with table list, loading state, and last refresh timestamp
- **TableMetadata**: Represents cached table with column list, loading state, and last refresh timestamp
- **CacheLoadState**: Enumeration of metadata loading states (not_loaded, loading, loaded, stale)
- **InvalidationEvent**: Represents a cache invalidation triggered by DDL (schema, table, operation type)

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Attaching a database with 500+ tables completes without loading any column metadata (0 `sys.columns` queries during ATTACH)
- **SC-002**: First query to a specific table loads only that table's columns (1 `sys.columns` query for the accessed table)
- **SC-003**: Subsequent queries to cached tables execute without additional metadata queries (cache hit rate > 99% for repeated access)
- **SC-004**: Schema TTL expiration triggers refresh of only that schema's table list (no cross-schema refresh)
- **SC-005**: Table TTL expiration triggers refresh of only that table's columns (no cross-table refresh)
- **SC-006**: `CREATE TABLE` via DuckDB makes new table visible in under 1 second without global refresh
- **SC-007**: `DROP TABLE` via DuckDB removes table from cache in under 1 second without global refresh
- **SC-008**: All existing catalog tests pass without modification (backward compatibility)
- **SC-009**: Memory usage for catalog cache scales linearly with actually accessed tables, not total tables

## Assumptions

- DuckDB Catalog DDL operations (CREATE/DROP/ALTER TABLE, CREATE/DROP SCHEMA) call well-defined entry points in MSSQLSchemaEntry and MSSQLCatalog classes
- The existing thread-safety mechanisms (mutexes for cache, schema entries, table sets) are sufficient for the incremental loading pattern
- Users accept that `mssql_exec()` changes require manual cache refresh, as parsing arbitrary SQL is out of scope
- Schema and table TTLs will use the same time unit (seconds) as the existing `mssql_catalog_cache_ttl` setting
- External changes to SQL Server schema (via SSMS, other tools) are handled by TTL expiration or manual refresh, not real-time notifications

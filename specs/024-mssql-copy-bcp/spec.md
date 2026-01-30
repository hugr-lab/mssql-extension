# Feature Specification: COPY TO MSSQL via TDS BulkLoadBCP

**Feature Branch**: `024-mssql-copy-bcp`
**Created**: 2026-01-29
**Status**: Draft
**Input**: User description: "High-throughput COPY support from DuckDB into SQL Server using BCP-style bulk load over TDS (packet type 0x07), supporting temp tables, regular tables, catalog-resolved targets, with create-if-missing and overwrite semantics."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Basic COPY to Regular Table (Priority: P1)

A data engineer needs to bulk load a large dataset from DuckDB into a SQL Server table. They execute a COPY statement targeting an existing or new table, and the data streams efficiently without hitting SQL string size limits.

**Why this priority**: This is the core use case - moving data from DuckDB to SQL Server at scale. Without this, the feature has no value.

**Independent Test**: Can be fully tested by executing `COPY (SELECT * FROM local_data) TO 'mssql://db/dbo/target_table' (FORMAT 'bcp')` and verifying all rows appear in SQL Server.

**Acceptance Scenarios**:

1. **Given** a DuckDB session with an attached MSSQL database and a local table with 1M rows, **When** user executes `COPY (SELECT * FROM local_table) TO 'mssql://db/dbo/new_table' (FORMAT 'bcp')`, **Then** all 1M rows are inserted into SQL Server without memory exhaustion.

2. **Given** an existing SQL Server table with matching schema, **When** user executes COPY to that table, **Then** rows are appended successfully.

3. **Given** an existing SQL Server table with `OVERWRITE=true` option, **When** user executes COPY, **Then** the existing table is dropped, recreated with DuckDB schema, and populated.

---

### User Story 2 - COPY to Session-Scoped Temp Table (Priority: P1)

A data engineer needs to stage data in a session-scoped temp table (`#temp`) for subsequent SQL Server processing. The temp table must persist on the same connection for downstream queries.

**Why this priority**: Temp tables are essential for ETL workflows and must work with connection pinning. Equal priority to regular tables as it's a core use case.

**Independent Test**: Can be tested by executing `COPY (SELECT ...) TO 'mssql://db/#staging' (FORMAT 'bcp')` followed by a query against `#staging` on the same session.

**Acceptance Scenarios**:

1. **Given** an attached MSSQL database, **When** user executes `COPY (SELECT * FROM local_data) TO 'mssql://db/#temp_staging' (FORMAT 'bcp')`, **Then** the temp table `#temp_staging` is created and populated on the pinned connection.

2. **Given** a populated `#temp` table, **When** user runs a subsequent query on the same session, **Then** the temp table data is accessible.

3. **Given** `CREATE_TABLE=false` and no existing temp table, **When** user executes COPY, **Then** a clear error is returned.

---

### User Story 3 - Catalog-Based Target Resolution (Priority: P2)

A data engineer wants to use DuckDB's catalog syntax (`catalog.schema.table`) instead of URL syntax, making queries more natural and consistent with DuckDB conventions.

**Why this priority**: Improves usability but URL syntax provides equivalent functionality. A convenience feature.

**Independent Test**: Can be tested by executing `COPY (SELECT ...) TO sqlsrv.dbo.table2 (FORMAT 'bcp')` where `sqlsrv` is an attached catalog.

**Acceptance Scenarios**:

1. **Given** an attached MSSQL catalog named `sqlsrv`, **When** user executes `COPY (SELECT * FROM data) TO sqlsrv.dbo.target (FORMAT 'bcp')`, **Then** data is copied to `[dbo].[target]` on that SQL Server.

2. **Given** a catalog-resolved target that is a VIEW, **When** user attempts COPY, **Then** a clear error indicates views are not supported.

---

### User Story 4 - Create Table with Auto-Schema (Priority: P2)

A data engineer wants the extension to automatically create the destination table based on DuckDB's source schema when the table doesn't exist.

**Why this priority**: Reduces manual DDL work, but users can pre-create tables. Convenience feature.

**Independent Test**: Can be tested by executing COPY to a non-existent table with `CREATE_TABLE=true` (default) and verifying table structure matches source.

**Acceptance Scenarios**:

1. **Given** a DuckDB source with columns `(id INTEGER, name VARCHAR, amount DECIMAL(10,2))`, **When** COPY targets a non-existent table with `CREATE_TABLE=true`, **Then** SQL Server table is created with appropriate type mappings (`int`, `nvarchar(max)`, `decimal(10,2)`).

2. **Given** `CREATE_TABLE=false` and target table does not exist, **When** user executes COPY, **Then** a clear error is returned.

---

### User Story 5 - Transaction-Safe COPY (Priority: P2)

A data engineer performs COPY within a DuckDB transaction context, expecting it to respect existing transaction semantics and connection pinning.

**Why this priority**: Important for data consistency but has restrictions in MVP. Users can work around by running COPY outside transactions.

**Independent Test**: Can be tested by verifying COPY from local DuckDB data (not mssql_scan) works within a transaction, and COPY from mssql_scan throws the expected error.

**Acceptance Scenarios**:

1. **Given** a DuckDB transaction is active with a pinned MSSQL connection, **When** user executes COPY with a purely local DuckDB source, **Then** COPY executes on the pinned connection within the transaction.

2. **Given** a DuckDB transaction is active, **When** user attempts `COPY (SELECT * FROM mssql.dbo.source) TO 'mssql://db/dbo/dest' (FORMAT 'bcp')`, **Then** a clear error explains that COPY TO is not supported inside DuckDB transactions when source requires mssql_scan.

3. **Given** COPY fails mid-stream inside a transaction, **When** the error occurs, **Then** the transaction is rolled back and no partial data remains.

---

### User Story 6 - Large-Scale Data Ingestion (Priority: P3)

A data engineer bulk loads 10M+ rows in a single COPY operation, expecting bounded memory usage and streaming throughput.

**Why this priority**: Performance at scale matters but basic functionality must work first. Can be validated after core implementation.

**Independent Test**: Can be tested by COPY of 10M rows while monitoring memory usage and verifying completion without OOM.

**Acceptance Scenarios**:

1. **Given** a local DuckDB table with 10M rows, **When** user executes COPY to MSSQL, **Then** operation completes with bounded memory (not buffering entire dataset).

2. **Given** batch settings `mssql_copy_batch_rows=10000`, **When** COPY executes, **Then** data streams in chunks respecting batch size.

---

### Edge Cases

- What happens when the target table has more columns than source? Error with schema mismatch.
- What happens when source has NULL values for NOT NULL columns? SQL Server returns error, COPY aborts.
- How does system handle network interruption mid-COPY? Error with approximate rows streamed, transaction rolled back if applicable.
- What happens when attempting COPY to a temp table outside the pinned session? Temp table not found error.
- How does system handle COPY when the same connection is streaming results from a prior query? Clear error: "cannot start COPY/BCP on a connection that is currently streaming results."
- What happens with zero-row source? Valid operation, creates empty table if CREATE_TABLE=true.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST support COPY TO syntax with URL-based targets (`mssql://<attach_alias>/<schema>/<table>`, `mssql://<attach_alias>/#temp_name`)
- **FR-002**: System MUST support COPY TO syntax with catalog-based targets (`catalog.schema.table`)
- **FR-003**: System MUST use TDS BulkLoadBCP (packet type 0x07) for data transfer, avoiding SQL string size limits
- **FR-004**: System MUST execute COPY on a single pinned SQL Server connection for the entire operation
- **FR-005**: System MUST check table existence on the pinned connection (not catalog cache) using `OBJECT_ID()`
- **FR-006**: System MUST create destination table automatically when `CREATE_TABLE=true` (default) and table does not exist
- **FR-007**: System MUST drop and recreate table when `OVERWRITE=true` and table exists
- **FR-008**: System MUST stream data chunk-by-chunk without buffering the entire dataset
- **FR-009**: System MUST validate column count and type compatibility when copying to existing tables
- **FR-010**: System MUST prevent COPY when source requires mssql_scan inside a DuckDB transaction
- **FR-011**: System MUST detect and prevent COPY on connections currently streaming results
- **FR-012**: System MUST quote all SQL identifiers using `[]` brackets, never interpolating user values
- **FR-013**: System MUST send all string data as Unicode (SQL Server handles collation/conversion)
- **FR-014**: System MUST return an error if target resolves to a VIEW
- **FR-015**: System MUST report row count progress via DuckDB's standard progress mechanism during COPY execution
- **FR-016**: System MUST emit debug output via existing `MSSQL_DEBUG` environment variable (Level 1: COPY start/end with totals; Level 2+: batch-level details)

### COPY Options

| Option         | Type   | Default    | Description                    |
|----------------|--------|------------|--------------------------------|
| `FORMAT`       | STRING | (required) | Must be `'bcp'` for BulkLoadBCP |
| `CREATE_TABLE` | BOOL   | `true`     | Create table if missing        |
| `OVERWRITE`    | BOOL   | `false`    | Drop and recreate if exists    |

### Configuration Settings

| Setting                     | Default | Description                      |
|-----------------------------|---------|----------------------------------|
| `mssql_copy_batch_rows`     | 10000   | Rows per BCP batch               |
| `mssql_copy_max_batch_bytes`| 32MB    | Maximum batch size in bytes      |

### Type Mapping (DuckDB to SQL Server)

| DuckDB Type   | SQL Server Type    |
|---------------|--------------------|
| BOOLEAN       | bit                |
| TINYINT       | tinyint            |
| SMALLINT      | smallint           |
| INTEGER       | int                |
| BIGINT        | bigint             |
| FLOAT         | real               |
| DOUBLE        | float              |
| DECIMAL(p,s)  | decimal(p,s)       |
| VARCHAR       | nvarchar(max)      |
| UUID          | uniqueidentifier   |
| BLOB          | varbinary(max)     |
| DATE          | date               |
| TIME          | time               |
| TIMESTAMP     | datetime2(7)       |
| TIMESTAMP_TZ  | datetimeoffset(7)  |

### Key Entities

- **COPY Target**: Represents the destination - can be URL-based (`mssql://...`) or catalog-based (`catalog.schema.table`). Must resolve to a table, not a view.
- **Pinned Connection**: A single SQL Server connection held for the duration of COPY to ensure session-scoped temp table visibility and BCP stream integrity.
- **BulkLoadBCP Payload**: TDS packet stream (type 0x07) containing COLMETADATA, ROW tokens, and DONE token.
- **Batch**: A unit of rows streamed together, controlled by `mssql_copy_batch_rows` and `mssql_copy_max_batch_bytes`.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can COPY 10M rows from DuckDB to SQL Server in a single operation without memory exhaustion
- **SC-002**: COPY operations to temp tables (`#temp`) remain visible for subsequent queries on the same session
- **SC-003**: COPY to non-existent tables auto-creates the table with correct schema mapping in 100% of cases
- **SC-004**: Users experience no SQL string size limit errors regardless of dataset size (vs. current INSERT limitations)
- **SC-005**: Error messages include target identifier, approximate rows streamed, and server error details when COPY fails
- **SC-006**: COPY maintains bounded memory usage (configurable batch size) regardless of total row count
- **SC-007**: Both URL (`mssql://...`) and catalog (`catalog.schema.table`) target syntaxes work identically
- **SC-008**: COPY achieves minimum 50K rows/second throughput for simple row types over LAN connection

## Scope & Boundaries

### In Scope (MVP)

- COPY TO with URL and catalog targets
- Session-scoped (`#`) and global (`##`) temp tables
- Regular (persistent) tables
- CREATE_TABLE and OVERWRITE options
- BulkLoadBCP streaming
- Connection pinning for temp table visibility
- Type mapping for baseline types
- Error handling with rollback

### Out of Scope

- COPY FROM MSSQL (reading from SQL Server)
- RETURNING / OUTPUT clause
- UPDATE / DELETE via COPY
- MERGE / UPSERT operations
- Inserting into views
- Authentication flows (AAD, Fabric) - assumed solved at connection time
- Explicit temp table lifecycle management (handled by connection pool)

## Clarifications

### Session 2026-01-29

- Q: Should COPY operations report progress to users during execution? → A: Yes, via DuckDB's standard progress mechanism (row count)
- Q: What level of debug/diagnostic output should COPY operations emit? → A: Integrate with existing MSSQL_DEBUG env var (batch progress at debug levels)
- Q: What minimum throughput should COPY achieve for integration test validation? → A: 50K rows/second minimum (for simple row types over LAN)

## Assumptions

- TDS v7.4 protocol support (current extension baseline)
- SQL Server 2016+ for all type mappings
- OpenSSL TLS transport already functional
- Connection pool handles session reset and temp table cleanup
- DuckDB COPY TO extensibility allows custom FORMAT implementations

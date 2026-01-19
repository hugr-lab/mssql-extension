# Feature Specification: High-Performance DML INSERT

**Feature Branch**: `009-dml-insert`
**Created**: 2026-01-19
**Status**: Draft
**Input**: User description: "High-Performance DML INSERT (SQL Batch MVP + Returning)"

## Overview

Provide a working and fast INSERT path from DuckDB into SQL Server using SQL Batch execution. This feature enables high-volume batched inserts (targeting 10M+ rows), INSERT with RETURNING semantics (returning inserted rows to DuckDB), correct type mapping and encoding with SQL Server-side collation conversion, and clear failure semantics.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Bulk Data Migration (Priority: P1)

A data engineer needs to migrate millions of rows from DuckDB analytical results into SQL Server operational tables for downstream applications.

**Why this priority**: This is the core use case - enabling high-volume data transfer from DuckDB to SQL Server. Without bulk insert capability, the extension cannot serve ETL/ELT workloads.

**Independent Test**: Can be fully tested by inserting a large dataset (10M rows) from DuckDB into a SQL Server table and verifying all rows arrive intact.

**Acceptance Scenarios**:

1. **Given** a DuckDB table with 10 million rows, **When** executing `INSERT INTO mssql_table SELECT * FROM duckdb_table`, **Then** all 10 million rows are inserted into SQL Server without memory exhaustion
2. **Given** a batch of rows with mixed data types (integers, decimals, strings, dates, UUIDs, binary), **When** inserting into corresponding SQL Server columns, **Then** all values are correctly converted and stored
3. **Given** an insert operation in progress, **When** a constraint violation occurs, **Then** the current batch fails atomically with a clear error message indicating the row range affected

---

### User Story 2 - Insert with Returned Values (Priority: P2)

A developer needs to insert records and immediately receive back the inserted data, including server-generated values like identity columns or defaults, for use in subsequent application logic.

**Why this priority**: RETURNING semantics are essential for applications that need immediate feedback on inserted data, especially for identity/auto-increment columns. This enables common application patterns.

**Independent Test**: Can be fully tested by inserting rows with RETURNING clause and verifying the returned data matches what was stored, including generated identity values.

**Acceptance Scenarios**:

1. **Given** a table with an identity column, **When** executing `INSERT INTO mssql_table (name) VALUES ('test') RETURNING *`, **Then** the returned row includes the generated identity value
2. **Given** a multi-row insert with RETURNING, **When** inserting 100 rows, **Then** all 100 rows are returned with their complete column values
3. **Given** a table with default values, **When** inserting without specifying those columns and using RETURNING *, **Then** the returned rows include the server-computed default values

---

### User Story 3 - Unicode and International Data (Priority: P3)

A global application needs to insert text data containing characters from multiple languages and scripts into SQL Server columns with various collations.

**Why this priority**: Unicode support is critical for international applications but builds on the core insert mechanism. The server-side collation conversion approach simplifies implementation.

**Independent Test**: Can be fully tested by inserting Unicode strings into VARCHAR/NVARCHAR columns with different collations and verifying correct storage and retrieval.

**Acceptance Scenarios**:

1. **Given** a string containing Chinese, Arabic, and emoji characters, **When** inserting into an NVARCHAR column, **Then** the characters are stored correctly and can be queried back
2. **Given** Unicode text, **When** inserting into a VARCHAR column with a non-UTF collation, **Then** SQL Server performs the conversion and either stores the data or returns a clear conversion error
3. **Given** strings with special SQL characters (quotes, brackets), **When** inserting, **Then** the values are properly escaped and stored without SQL injection risk

---

### User Story 4 - Batch Size Tuning (Priority: P4)

A database administrator needs to tune insert batch sizes to optimize throughput for their specific SQL Server environment and network conditions.

**Why this priority**: Performance tuning is important but represents optimization of the core feature rather than new functionality.

**Independent Test**: Can be fully tested by running inserts with different batch size settings and measuring throughput differences.

**Acceptance Scenarios**:

1. **Given** a setting to change batch size, **When** setting `mssql_insert_batch_size` to 500, **Then** inserts are batched in groups of 500 rows
2. **Given** a large row size that exceeds byte limits, **When** inserting, **Then** the batch is automatically split to stay within configured limits
3. **Given** default settings, **When** inserting rows, **Then** batches of 2000 rows (default) are used

---

### Edge Cases

- What happens when a single row exceeds the maximum SQL statement size? The operation fails with a clear error before attempting execution.
- How does the system handle NULL values across all supported types? NULLs are emitted as SQL NULL literals and handled correctly by SQL Server.
- What happens when inserting into a table with an identity column and providing explicit values? The operation fails with a clear error explaining that explicit identity values are not supported in MVP.
- How does the system handle NaN or Infinity values for floating-point columns? These values are rejected with a clear error (or mapped to NULL based on configuration).
- What happens if SQL Server returns a conversion error for a specific collation? The error is surfaced to the user with the SQL Server error message.
- How does the system handle very long strings or binary data within batch limits? Large values are supported within the configured max SQL bytes; if a single value causes the row to exceed limits, the operation fails with a clear error.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST support two insert modes: bulk insert (maximum throughput, no returned rows) and insert with RETURNING (returns inserted rows)
- **FR-002**: System MUST batch inserts using SQL Batch execution with configurable batch sizes
- **FR-003**: System MUST provide a default batch size of 2000 rows, configurable via `mssql_insert_batch_size` setting
- **FR-004**: System MUST cap SQL statement size at a configurable limit (default 8MB via `mssql_insert_max_sql_bytes`)
- **FR-005**: System MUST cap rows per statement at a configurable limit (default 2000 via `mssql_insert_max_rows_per_statement`)
- **FR-006**: System MUST escape all identifiers (schema, table, column names) using T-SQL bracket quoting with proper escaping of `]` as `]]`
- **FR-007**: System MUST convert DuckDB values to T-SQL literals safely for these types: BOOLEAN, TINYINT/SMALLINT/INTEGER/BIGINT, UTINYINT/USMALLINT/UINTEGER/UBIGINT, FLOAT/DOUBLE, DECIMAL, VARCHAR, UUID, BLOB, DATE, TIME, TIMESTAMP, TIMESTAMP_TZ
- **FR-008**: System MUST emit text values as Unicode literals (N'...') to enable SQL Server-side collation conversion
- **FR-009**: System MUST support INSERT ... RETURNING by using SQL Server's OUTPUT INSERTED clause
- **FR-010**: System MUST map DuckDB RETURNING * to output all inserted columns in table order
- **FR-011**: System MUST map DuckDB RETURNING col1, col2 to OUTPUT INSERTED.[col1], INSERTED.[col2]
- **FR-012**: System MUST omit identity columns from the INSERT column list by default unless DuckDB explicitly provides values (which triggers an error in MVP)
- **FR-013**: System MUST ensure each INSERT statement is atomic - if any row fails, the entire statement fails
- **FR-014**: System MUST include in error messages: statement index, row offset range attempted, and remote error message/code
- **FR-015**: System MUST reject NaN and Infinity floating-point values with a clear error
- **FR-016**: System MUST fail with a clear error if a single row exceeds the configured SQL size limits
- **FR-017**: System MUST provide a `mssql_insert_use_returning_output` setting (default true) to control RETURNING behavior
- **FR-018**: System MUST emit NULL values as SQL NULL literals
- **FR-019**: System MUST NOT accept raw SQL fragments as inputs - all SQL must be generated from DuckDB's bound insert pipeline
- **FR-020**: System MUST integrate with DuckDB catalog table write hooks for MSSQL tables

### Key Entities

- **Insert Batch**: A collection of rows to be inserted in a single SQL statement, constrained by row count and byte size limits
- **Insert Statement**: A generated SQL INSERT statement with VALUES clause, optionally including OUTPUT INSERTED for returning mode
- **Type Converter**: Logic that converts DuckDB values to T-SQL literal representations with proper escaping
- **Insert Executor**: Component that accepts DuckDB input chunks, generates SQL Batch statements, executes them on pooled connections, and optionally decodes OUTPUT results back to DuckDB

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 10 million rows can be inserted from DuckDB to SQL Server without memory exhaustion or process crash
- **SC-002**: All supported data types (integers, decimals, strings, UUIDs, binary, dates, times, timestamps) are correctly stored and retrievable
- **SC-003**: INSERT with RETURNING correctly returns all inserted rows including server-generated identity values
- **SC-004**: Unicode characters from any language are correctly stored when inserting into NVARCHAR columns
- **SC-005**: Constraint violations result in atomic statement failure with error messages containing statement index and row range
- **SC-006**: Batch size configuration changes are reflected in actual statement execution
- **SC-007**: SQL injection attempts via data values are prevented (special characters in strings do not execute as SQL)

## Assumptions

- SQL Server accepts Unicode literals (N'...') and performs server-side collation conversion for the target column type
- DuckDB catalog table write hooks provide the necessary integration points for implementing insert functionality
- Connection pooling (from spec 003) is available for executing insert batches
- The TDS layer supports SQL Batch execution with large payloads
- SQL Server's OUTPUT INSERTED clause is universally supported for tables (no computed columns or triggers that would prevent it in MVP scope)
- SET IDENTITY_INSERT ON functionality is explicitly out of scope for MVP

## Scope Boundaries

### In Scope

- INSERT INTO table (cols) VALUES (...)
- INSERT INTO table (cols) SELECT ... (from DuckDB pipeline)
- INSERT ... RETURNING ... (column references only)
- SQL Batch execution (plain SQL text)
- Configurable batch sizes and SQL size limits
- All baseline DuckDB-to-SQL Server type mappings
- Statement-level atomic failure semantics

### Out of Scope (MVP)

- RPC/TDS RPC execution (sp_executesql, sp_prepexec)
- BCP / BulkLoadBCP / COPY protocol (future spec for insert-only, no returning)
- CDC ingestion formats (Debezium, etc.)
- MERGE-based upserts
- Multi-table inserts
- Insert into views
- Trigger-aware semantics beyond SQL Server default behavior
- RETURNING with expressions (only column references supported)
- SET IDENTITY_INSERT ON for explicit identity values
- Partial insert with per-row error reporting

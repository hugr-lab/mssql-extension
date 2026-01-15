# Feature Specification: Streaming SELECT and Query Cancellation

**Feature Branch**: `004-streaming-select-cancel`
**Created**: 2026-01-15
**Status**: Draft
**Input**: Execute SELECT queries against SQL Server and stream results into DuckDB. Implement SQL batch execution, TDS token parsing (COLMETADATA, ROW, DONE*, ERROR/INFO), chunked result streaming into DuckDB DataChunks, and query cancellation that works during both execution and row streaming without breaking the connection. SQL text must be transmitted as UTF-16LE. Include integration tests with connection pooling using a test table with at least 10M generated rows.

## Clarifications

### Session 2026-01-15

- Q: How should INFO messages and query execution be observable for debugging? → A: Log INFO/warnings to DuckDB's standard warning mechanism; use DuckDB's debug logging (`CALL enable_logging(level = 'debug')`) for detailed tracing.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Execute SELECT Query and Stream Results (Priority: P1)

As a DuckDB user, I want to execute a SELECT query against SQL Server and receive results streamed into DuckDB so that I can analyze remote data without loading everything into memory at once.

**Why this priority**: This is the core functionality of the feature. Without streaming SELECT execution, users cannot retrieve data from SQL Server through DuckDB.

**Independent Test**: Can be fully tested by executing a SELECT query via `mssql_scan` on an attached database and verifying results stream into DuckDB DataChunks with correct schema and data values.

**Acceptance Scenarios**:

1. **Given** an attached mssql database context with valid credentials, **When** I execute `SELECT * FROM mssql_scan('mydb', 'SELECT id, name, created_at FROM users')`, **Then** results are streamed into DuckDB as DataChunks with columns matching the SQL Server column metadata.
2. **Given** a SELECT query returning 10 million rows, **When** `mssql_scan` executes, **Then** results are streamed in chunks rather than loaded entirely into memory before returning.
3. **Given** a SELECT query with various SQL Server data types (INT, VARCHAR, DATETIME, DECIMAL), **When** `mssql_scan` executes, **Then** each SQL Server type is correctly mapped to the corresponding DuckDB type.

---

### User Story 2 - Handle Query Errors from SQL Server (Priority: P1)

As a DuckDB user, I want to receive clear error messages when my SQL Server query fails so that I can diagnose and fix issues.

**Why this priority**: Error handling is essential for a usable query interface. Users need feedback when queries fail due to syntax errors, permission issues, or missing objects.

**Independent Test**: Can be fully tested by executing an invalid query and verifying the SQL Server error is propagated to DuckDB with message text, error number, and severity.

**Acceptance Scenarios**:

1. **Given** a SELECT query with invalid syntax, **When** `mssql_scan` executes, **Then** SQL Server's error message is propagated to DuckDB including error number, severity level, and descriptive text.
2. **Given** a SELECT query referencing a non-existent table, **When** `mssql_scan` executes, **Then** the error message clearly identifies the missing object.
3. **Given** a SELECT query that partially succeeds before encountering an error (e.g., division by zero in computed column), **When** `mssql_scan` executes, **Then** rows before the error are returned and the error is reported after.

---

### User Story 3 - Cancel Query During Execution (Priority: P1)

As a DuckDB user, I want to cancel a long-running query while it is executing on SQL Server so that I don't have to wait for completion.

**Why this priority**: Query cancellation is critical for interactive use. Users need the ability to abort slow or runaway queries without terminating their entire session.

**Independent Test**: Can be fully tested by starting a long-running query, sending a cancellation signal, and verifying the query stops promptly and the connection remains usable.

**Acceptance Scenarios**:

1. **Given** a query that takes 60 seconds to execute on SQL Server, **When** I send an interrupt signal (Ctrl+C or DuckDB cancellation) after 5 seconds, **Then** the query execution is cancelled within 2 seconds.
2. **Given** a cancelled query, **When** cancellation completes, **Then** the underlying connection returns to idle state and remains in the connection pool.
3. **Given** a cancelled query, **When** I execute a subsequent query on the same context, **Then** the new query executes successfully without connection errors.

---

### User Story 4 - Cancel Query During Row Streaming (Priority: P1)

As a DuckDB user, I want to cancel a query while rows are being streamed from SQL Server so that I can stop large result sets I no longer need.

**Why this priority**: Large result sets can take significant time to stream. Users need cancellation capability during row retrieval, not just during execution.

**Independent Test**: Can be fully tested by executing a query returning 10M rows, cancelling after receiving some rows, and verifying the stream stops and connection is reusable.

**Acceptance Scenarios**:

1. **Given** a query returning 10 million rows being streamed, **When** I cancel after 100,000 rows have been received, **Then** streaming stops within 2 seconds.
2. **Given** a cancelled streaming query, **When** cancellation completes, **Then** all remaining TDS packets from SQL Server are properly consumed or discarded.
3. **Given** a cancelled streaming query, **When** the connection returns to idle, **Then** no partial or corrupt data remains on the connection.

---

### User Story 5 - Receive Informational Messages (Priority: P2)

As a DuckDB user, I want to receive informational messages from SQL Server (e.g., row counts, warnings) so that I have full visibility into query execution.

**Why this priority**: Informational messages (INFO tokens) provide useful feedback but are not required for core query functionality.

**Independent Test**: Can be fully tested by executing a query that generates PRINT statements or row count messages and verifying they are captured and accessible.

**Acceptance Scenarios**:

1. **Given** a query preceded by `SET NOCOUNT OFF`, **When** `mssql_scan` executes, **Then** row count messages are captured and available.
2. **Given** a stored procedure that executes PRINT statements, **When** I call it via `mssql_execute`, **Then** PRINT output is captured in the result.
3. **Given** a query that generates warnings (e.g., truncation warnings), **When** `mssql_scan` executes, **Then** warnings are captured alongside results.

---

### User Story 6 - Integration with Connection Pooling (Priority: P2)

As a DuckDB user, I want query execution to work seamlessly with the connection pool so that multiple queries reuse connections efficiently.

**Why this priority**: Connection pooling integration ensures the streaming query feature works within the established connection management architecture.

**Independent Test**: Can be fully tested by executing multiple sequential queries and verifying pool statistics show connection reuse rather than new connections per query.

**Acceptance Scenarios**:

1. **Given** a connection pool with pooling enabled, **When** I execute multiple SELECT queries sequentially, **Then** connections are acquired from and returned to the pool correctly.
2. **Given** a query that completes successfully, **When** results have been fully consumed, **Then** the connection is returned to idle state in the pool.
3. **Given** a pool with multiple idle connections, **When** concurrent queries are executed, **Then** each query acquires a separate connection without blocking.

---

### Edge Cases

- What happens when SQL Server closes the connection mid-stream? → Query fails with a network error, partial results are discarded, and the connection is removed from the pool.
- How does the system handle very wide result sets (hundreds of columns)? → All columns are parsed from COLMETADATA; system supports up to the SQL Server limit of 4,096 columns.
- What happens when a single row exceeds the TDS packet size? → Multi-packet rows are reassembled correctly (TDS packet continuation).
- How does the system handle NULL values in result rows? → NULLs are correctly identified from ROW token and mapped to DuckDB NULL values.
- What happens when cancellation is sent but query completes before cancellation is processed? → Results are returned normally; cancellation is a no-op in this case.
- How does the system handle queries that return multiple result sets (e.g., stored procedures)? → Only the first result set is processed; subsequent result sets are consumed and discarded.
- What happens when COLMETADATA contains unsupported data types? → Query fails with clear error identifying the unsupported type; connection remains usable.

## Requirements *(mandatory)*

### Functional Requirements

#### SQL Batch Execution

- **FR-001**: System MUST construct and send SQL_BATCH TDS packets to execute SELECT queries.
- **FR-002**: System MUST encode SQL text as UTF-16LE before transmission.
- **FR-003**: System MUST support SQL text up to the TDS batch size limit (64KB per packet, with packet continuation for larger queries).
- **FR-004**: System MUST transition connection state from `idle` to `executing` when batch execution begins.

#### TDS Token Parsing

- **FR-005**: System MUST parse COLMETADATA tokens to extract result set column definitions including name, type, precision, scale, and nullability.
- **FR-006**: System MUST parse ROW tokens to extract individual row data according to column metadata.
- **FR-007**: System MUST parse DONE, DONEPROC, and DONEINPROC tokens to detect query completion and row counts.
- **FR-008**: System MUST parse ERROR tokens to extract SQL Server error messages including error number, state, severity, and message text.
- **FR-009**: System MUST parse INFO tokens to capture informational messages, warnings, and PRINT output.
- **FR-009a**: System MUST surface INFO messages and warnings through DuckDB's standard warning mechanism.
- **FR-009b**: System SHOULD emit detailed query execution tracing via DuckDB's debug logging (enabled via `CALL enable_logging(level = 'debug')`).
- **FR-010**: System MUST handle multi-packet TDS responses where tokens span packet boundaries.

#### Result Streaming

- **FR-011**: System MUST stream results into DuckDB DataChunks rather than buffering entire result sets.
- **FR-012**: System MUST map SQL Server column types to appropriate DuckDB types (INT→INTEGER, VARCHAR→VARCHAR, DATETIME→TIMESTAMP, etc.).
- **FR-013**: System MUST respect DuckDB's standard DataChunk size (2048 rows by default) for output.
- **FR-014**: System MUST handle NULL values in result rows by mapping to DuckDB NULL.
- **FR-015**: System MUST handle variable-length data types (VARCHAR, NVARCHAR, VARBINARY) with correct length decoding.

#### Query Cancellation

- **FR-016**: System MUST support query cancellation via TDS Attention packet.
- **FR-017**: System MUST allow cancellation during query execution phase (before results arrive).
- **FR-018**: System MUST allow cancellation during row streaming phase (while receiving results).
- **FR-019**: System MUST consume or discard remaining TDS packets after cancellation until DONE token with attention acknowledgment.
- **FR-020**: System MUST return connection to `idle` state after successful cancellation.
- **FR-021**: System MUST NOT close the connection when cancellation succeeds; connection remains poolable.
- **FR-022**: System MUST timeout cancellation after a configurable period and close connection if cancellation hangs.

#### Integration

- **FR-023**: System MUST integrate with connection pool for connection acquisition and release.
- **FR-024**: System MUST acquire connections from pool at query start and return on completion or cancellation.
- **FR-025**: System MUST propagate SQL Server errors to DuckDB as exceptions with original error details.

### Key Entities

- **SqlBatch**: Represents a SQL_BATCH TDS message containing UTF-16LE encoded SQL text. Handles packet splitting for large queries.
- **ColumnMetadata**: Describes a single result column parsed from COLMETADATA token. Contains name, SQL Server type ID, length, precision, scale, collation, and nullability.
- **ResultRow**: Represents a single row of data parsed from ROW token. Contains values matching the column metadata schema.
- **QueryResult**: Streaming result iterator that yields DataChunks. Manages column metadata, row parsing, and completion detection.
- **TdsError**: Error information from ERROR token. Contains error number, state, severity class, line number, procedure name, and message text.
- **TdsInfo**: Informational message from INFO token. Contains message number, state, class, and message text.
- **AttentionPacket**: TDS Attention signal for query cancellation. Empty packet with attention status flag.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: SELECT queries returning 10 million rows complete streaming within 60 seconds on a local network.
- **SC-002**: Query cancellation stops execution or streaming within 2 seconds of cancellation request.
- **SC-003**: Memory usage during streaming remains bounded regardless of result set size (no full buffering).
- **SC-004**: Cancelled queries return connections to pool successfully 100% of the time (no connection leaks).
- **SC-005**: 100 sequential queries execute with connection reuse (verified via pool statistics showing connection reuse).
- **SC-006**: Error messages from SQL Server are propagated with complete details (error number, message, severity).
- **SC-007**: Integration tests pass with 10M+ row test data demonstrating streaming and cancellation work at scale.

## Scope Boundaries

### In Scope

- SQL_BATCH packet construction and transmission
- UTF-16LE encoding of SQL text
- COLMETADATA token parsing
- ROW token parsing
- DONE/DONEPROC/DONEINPROC token parsing
- ERROR token parsing
- INFO token parsing
- Result streaming into DuckDB DataChunks
- Type mapping from SQL Server to DuckDB
- Query cancellation via Attention packet
- Connection pool integration
- Integration tests with large (10M+ row) datasets

### Out of Scope (Non-Goals)

- Catalog integration (schema discovery, table listing)
- Write operations (INSERT, UPDATE, DELETE)
- Parameterized queries (RPC_REQUEST)
- Stored procedure calls (beyond raw SQL)
- Multiple result set handling (beyond consuming/discarding)
- Transaction management (BEGIN/COMMIT/ROLLBACK)
- Cursor support
- Query plan retrieval (SET SHOWPLAN)
- All SQL Server data types (only common types required initially)

## Assumptions

- Connection pooling from spec 003 is implemented and functional.
- TDS 7.4 protocol (SQL Server 2019+) is the target; no older protocol versions.
- The existing `mssql_scan` function signature from spec 002 is used.
- DuckDB's DataChunk API is available for streaming result output.
- Single result set per query is sufficient for initial implementation.
- Supported data types for initial implementation: INT, BIGINT, SMALLINT, TINYINT, BIT, FLOAT, REAL, DECIMAL, NUMERIC, MONEY, SMALLMONEY, CHAR, VARCHAR, NCHAR, NVARCHAR, DATETIME, DATETIME2, DATE, TIME, UNIQUEIDENTIFIER, VARBINARY, BINARY.
- Unsupported types will cause query failure with clear error (no silent data corruption).
- Integration tests use a pre-configured SQL Server instance with test database and appropriate permissions.
- DuckDB's logging infrastructure (`enable_logging`) is available for debug-level tracing.

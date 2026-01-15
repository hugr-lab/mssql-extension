# Tasks: Streaming SELECT and Query Cancellation

**Input**: Design documents from `/specs/004-streaming-select-cancel/`
**Prerequisites**: plan.md, spec.md, data-model.md, contracts/token-parsing.md, contracts/type-mapping.md, quickstart.md

**Tests**: Integration tests with 10M+ rows are requested in the feature specification.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project initialization and header file creation

- [x] T001 Create query/ directory structure: src/include/query/ and src/query/
- [x] T002 [P] Add TDS token type constants to src/include/tds/tds_types.hpp (COLMETADATA=0x81, ROW=0xD1, DONE=0xFD, ERROR=0xAA, INFO=0xAB, etc.)
- [x] T003 [P] Create empty header src/include/tds/tds_token_parser.hpp with TokenParser class declaration
- [x] T004 [P] Create empty header src/include/tds/tds_column_metadata.hpp with ColumnMetadata struct declaration
- [x] T005 [P] Create empty header src/include/tds/tds_row_reader.hpp with RowReader class declaration
- [x] T006 [P] Create empty header src/include/encoding/type_converter.hpp with TypeConverter class declaration
- [x] T007 [P] Create empty header src/include/encoding/decimal_encoding.hpp with DecimalEncoding functions
- [x] T008 [P] Create empty header src/include/encoding/datetime_encoding.hpp with DateTimeEncoding functions
- [x] T009 [P] Create empty header src/include/encoding/guid_encoding.hpp with GuidEncoding functions
- [x] T010 [P] Create empty header src/include/query/mssql_query_executor.hpp with MSSQLQueryExecutor class declaration
- [x] T011 [P] Create empty header src/include/query/mssql_result_stream.hpp with MSSQLResultStream class declaration
- [x] T012 Update CMakeLists.txt to include new source files in build

**Checkpoint**: Project structure ready for implementation

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**CRITICAL**: No user story work can begin until this phase is complete

### TDS Token Parsing Infrastructure

- [x] T013 Implement ColumnMetadata struct in src/tds/tds_column_metadata.cpp with all attributes from data-model.md (name, type_id, max_length, precision, scale, collation, is_nullable, is_identity)
- [x] T014 Implement TokenParser state machine in src/tds/tds_token_parser.cpp with states: WaitingForToken, ParsingColMetadata, ParsingRow, ParsingDone, ParsingError, ParsingInfo, Complete
- [x] T015 Implement TokenParser::Feed() to accumulate data in buffer in src/tds/tds_token_parser.cpp
- [x] T016 Implement TokenParser::TryParseNext() dispatch by token type byte in src/tds/tds_token_parser.cpp
- [x] T017 Implement COLMETADATA parsing in TokenParser (column count, column definitions, type info) in src/tds/tds_token_parser.cpp
- [x] T018 Implement DONE/DONEPROC/DONEINPROC parsing (status flags, row count) in src/tds/tds_token_parser.cpp

### Type Encoding Layer

- [x] T019 [P] Implement DecimalEncoding::ConvertDecimal() for DECIMAL/NUMERIC sign+magnitude format in src/encoding/decimal_encoding.cpp
- [x] T020 [P] Implement DecimalEncoding::ConvertMoney() and ConvertSmallMoney() in src/encoding/decimal_encoding.cpp
- [x] T021 [P] Implement DateTimeEncoding::ConvertDate() (3-byte days since 0001-01-01) in src/encoding/datetime_encoding.cpp
- [x] T022 [P] Implement DateTimeEncoding::ConvertTime() (scale-dependent ticks) in src/encoding/datetime_encoding.cpp
- [x] T023 [P] Implement DateTimeEncoding::ConvertDatetime() (4+4 byte days+ticks) in src/encoding/datetime_encoding.cpp
- [x] T024 [P] Implement DateTimeEncoding::ConvertDatetime2() (time+date composite) in src/encoding/datetime_encoding.cpp
- [x] T025 [P] Implement GuidEncoding::ConvertGuid() with mixed-endian byte reordering in src/encoding/guid_encoding.cpp
- [x] T026 Implement TypeConverter::GetDuckDBType() mapping SQL Server type IDs to DuckDB LogicalTypes in src/encoding/type_converter.cpp
- [x] T027 Implement TypeConverter::ConvertValue() dispatch to appropriate encoder in src/encoding/type_converter.cpp
- [x] T028 Implement TypeConverter::IsSupported() returning false for XML, GEOGRAPHY, GEOMETRY, SQL_VARIANT, etc. in src/encoding/type_converter.cpp

### Row Reading Infrastructure

- [x] T029 Implement RowReader class with column metadata reference in src/tds/tds_row_reader.cpp
- [x] T030 Implement RowReader::ReadValue() for fixed-length types (TINYINT, SMALLINT, INT, BIGINT, REAL, FLOAT) in src/tds/tds_row_reader.cpp
- [x] T031 Implement RowReader::ReadValue() for nullable fixed-length types (INTN, FLOATN, BITN) in src/tds/tds_row_reader.cpp
- [x] T032 Implement RowReader::ReadValue() for variable-length types (VARCHAR, NVARCHAR, VARBINARY) in src/tds/tds_row_reader.cpp
- [x] T033 Implement RowReader::ReadValue() for DECIMAL/NUMERIC using DecimalEncoding in src/tds/tds_row_reader.cpp
- [x] T034 Implement RowReader::ReadValue() for date/time types using DateTimeEncoding in src/tds/tds_row_reader.cpp
- [x] T035 Implement RowReader::ReadValue() for UNIQUEIDENTIFIER using GuidEncoding in src/tds/tds_row_reader.cpp
- [x] T036 Implement RowReader::ReadRow() that extracts all column values and builds RowData in src/tds/tds_row_reader.cpp

### SQL Batch Construction

- [x] T037 Add SqlBatch class to src/tds/tds_packet.cpp with UTF-16LE encoding using existing TdsPacket::AppendUTF16LE()
- [x] T038 Implement SqlBatch packet splitting for large queries (continuation status flags) in src/tds/tds_packet.cpp

### Connection State Extensions

- [x] T039 Add ExecuteBatch() method to TdsConnection in src/tds/tds_connection.cpp that sends SQL_BATCH and transitions state to Executing
- [x] T040 Add SendAttention() method to TdsConnection in src/tds/tds_connection.cpp for query cancellation

**Checkpoint**: Foundation ready - user story implementation can now begin

---

## Phase 3: User Story 1 - Execute SELECT Query and Stream Results (Priority: P1) MVP

**Goal**: Execute SELECT queries against SQL Server and stream results into DuckDB DataChunks

**Independent Test**: Execute `mssql_scan('ctx', 'SELECT id, name FROM users')` and verify results arrive in DataChunks with correct schema and values

### Implementation for User Story 1

- [x] T041 [US1] Integrate ROW token parsing into TokenParser with RowReader in src/tds/tds_token_parser.cpp
- [x] T042 [US1] Implement MSSQLResultStream class that yields DataChunks in src/query/mssql_result_stream.cpp
- [x] T043 [US1] Implement MSSQLResultStream::GetColumnTypes() returning DuckDB types from ColumnMetadata in src/query/mssql_result_stream.cpp
- [x] T044 [US1] Implement MSSQLResultStream::GetColumnNames() returning column names in src/query/mssql_result_stream.cpp
- [x] T045 [US1] Implement MSSQLResultStream::FillChunk() that reads rows into DataChunk vectors (2048 rows) in src/query/mssql_result_stream.cpp
- [x] T046 [US1] Implement MSSQLQueryExecutor::Execute() that acquires connection, sends SqlBatch, returns MSSQLResultStream in src/query/mssql_query_executor.cpp
- [x] T047 [US1] Replace stub mssql_scan implementation with real MSSQLQueryExecutor integration in src/mssql_functions.cpp
- [x] T048 [US1] Add debug logging for TDS packet send/receive via DuckDB logging in src/query/mssql_query_executor.cpp

### Tests for User Story 1

- [x] T049 [P] [US1] Create basic SELECT test in test/sql/query/basic_select.test (SELECT with INT, VARCHAR, DATETIME columns)
- [x] T050 [P] [US1] Create type mapping test in test/sql/query/type_mapping.test (all supported SQL Server types)

**Checkpoint**: User Story 1 complete - basic SELECT queries work with streaming results

---

## Phase 4: User Story 2 - Handle Query Errors from SQL Server (Priority: P1)

**Goal**: Receive clear error messages when SQL Server queries fail

**Independent Test**: Execute `mssql_scan('ctx', 'SELECT * FROM nonexistent_table')` and verify SQL Server error number, severity, and message are propagated

### Implementation for User Story 2

- [x] T051 [US2] Implement ERROR token parsing in TokenParser (error number, state, severity, message) in src/tds/tds_token_parser.cpp
- [x] T052 [US2] Create TdsError struct with all attributes from data-model.md in src/tds/tds_token_parser.cpp
- [x] T053 [US2] Implement error accumulation in MSSQLResultStream (collect errors during streaming) in src/query/mssql_result_stream.cpp
- [x] T054 [US2] Implement error propagation to DuckDB as InvalidInputException in src/query/mssql_result_stream.cpp
- [x] T055 [US2] Add severity-based error handling (fatal errors >=20 throw immediately) in src/query/mssql_result_stream.cpp
- [x] T056 [US2] Add unsupported type error with clear message including type name and column in src/encoding/type_converter.cpp

### Tests for User Story 2

- [x] T057 [P] [US2] Create error handling test in test/sql/query/error_handling.test (syntax error, missing table, permission denied)

**Checkpoint**: User Story 2 complete - SQL Server errors propagate to DuckDB with full details

---

## Phase 5: User Story 3 - Cancel Query During Execution (Priority: P1)

**Goal**: Cancel long-running queries before results arrive

**Independent Test**: Start a slow query (`WAITFOR DELAY '00:00:30'`), send Ctrl+C, verify cancellation completes within 2 seconds and connection is reusable

### Implementation for User Story 3

- [x] T058 [US3] Add is_cancelled atomic flag to MSSQLResultStream in src/query/mssql_result_stream.cpp
- [x] T059 [US3] Implement MSSQLResultStream::Cancel() that sets flag and calls TdsConnection::SendAttention() in src/query/mssql_result_stream.cpp
- [x] T060 [US3] Implement cancellation detection in TokenParser (check flag before blocking read) in src/tds/tds_token_parser.cpp
- [x] T061 [US3] Implement DONE token with ATTN flag detection for cancellation acknowledgment in src/tds/tds_token_parser.cpp
- [x] T062 [US3] Connect DuckDB interrupt mechanism to MSSQLResultStream::Cancel() in src/mssql_functions.cpp
- [x] T063 [US3] Add connection state transition from Executing to Cancelling to Idle in src/tds/tds_connection.cpp

### Tests for User Story 3

- [x] T064 [P] [US3] Create cancellation test in test/sql/query/cancellation.test (mock slow query, verify cancellation)

**Checkpoint**: User Story 3 complete - queries can be cancelled during execution phase

---

## Phase 6: User Story 4 - Cancel Query During Row Streaming (Priority: P1)

**Goal**: Cancel queries while rows are being streamed from SQL Server

**Independent Test**: Start streaming 10M rows, cancel after 100K rows, verify streaming stops within 2 seconds and connection remains usable

### Implementation for User Story 4

- [x] T065 [US4] Implement Draining state in MSSQLResultStream (consume remaining packets after cancel) in src/query/mssql_result_stream.cpp
- [x] T066 [US4] Implement packet draining loop until DONE+ATTN received in src/query/mssql_result_stream.cpp
- [x] T067 [US4] Add cancellation timeout with connection close fallback in src/query/mssql_result_stream.cpp
- [x] T068 [US4] Ensure connection returns to pool after successful cancellation in src/query/mssql_query_executor.cpp

### Tests for User Story 4

- [ ] T069 [P] [US4] Create integration test for cancel during streaming in test/integration/sqlserver/cancel_streaming.test

**Checkpoint**: User Story 4 complete - queries can be cancelled during row streaming

---

## Phase 7: User Story 5 - Receive Informational Messages (Priority: P2)

**Goal**: Capture INFO messages, PRINT output, and warnings from SQL Server

**Independent Test**: Execute a stored procedure with PRINT statements and verify output is captured via DuckDB warnings

### Implementation for User Story 5

- [x] T070 [US5] Implement INFO token parsing in TokenParser (same structure as ERROR) in src/tds/tds_token_parser.cpp
- [x] T071 [US5] Create TdsInfo struct with all attributes from data-model.md in src/tds/tds_token_parser.cpp
- [x] T072 [US5] Implement INFO message accumulation in MSSQLResultStream in src/query/mssql_result_stream.cpp
- [x] T073 [US5] Surface INFO messages via DuckDB ClientContext::AddWarning() in src/query/mssql_result_stream.cpp
- [x] T074 [US5] Add debug logging for INFO messages via DuckDB logging in src/query/mssql_result_stream.cpp

### Tests for User Story 5

- [x] T075 [P] [US5] Create info messages test in test/sql/query/info_messages.test (PRINT output, row count messages)

**Checkpoint**: User Story 5 complete - INFO messages and warnings are captured

---

## Phase 8: User Story 6 - Integration with Connection Pooling (Priority: P2)

**Goal**: Query execution works seamlessly with connection pool for connection reuse

**Independent Test**: Execute 100 sequential queries and verify pool statistics show connection reuse

### Implementation for User Story 6

- [x] T076 [US6] Implement MSSQLQueryExecutor::ValidateContext() to verify context exists in MSSQLContextManager in src/query/mssql_query_executor.cpp
- [x] T077 [US6] Add pool acquire timeout handling in MSSQLQueryExecutor::Execute() in src/query/mssql_query_executor.cpp
- [x] T078 [US6] Ensure connection release on query completion (normal and error paths) in src/query/mssql_query_executor.cpp
- [x] T079 [US6] Ensure connection release on query cancellation in src/query/mssql_query_executor.cpp
- [x] T080 [US6] Add connection validation after cancellation before pool return in src/query/mssql_query_executor.cpp

### Tests for User Story 6

- [ ] T081 [P] [US6] Create pool integration test in test/integration/sqlserver/pool_integration.test (connection reuse verification)

**Checkpoint**: User Story 6 complete - queries reuse pooled connections correctly

---

## Phase 9: Polish & Cross-Cutting Concerns

**Purpose**: Integration tests with large datasets and final validation

### Large Dataset Integration Tests

- [ ] T082 [P] Create 10M row streaming benchmark test in test/integration/sqlserver/streaming_10m.test
- [ ] T083 [P] Create cancel during execution integration test in test/integration/sqlserver/cancel_execution.test
- [ ] T084 Validate streaming completes within 60 seconds for 10M rows (SC-001)
- [ ] T085 Validate cancellation completes within 2 seconds (SC-002)
- [ ] T086 Validate memory usage remains bounded during streaming (SC-003)
- [ ] T087 Validate 100% connection return after cancellation (SC-004)

### Final Validation

- [ ] T088 Run all test/sql/query/*.test files and verify pass
- [ ] T089 Run quickstart.md scenarios manually against test SQL Server
- [ ] T090 Code cleanup: remove any debug code, ensure consistent error messages

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3-8)**: All depend on Foundational phase completion
  - US1 and US2 can proceed in parallel (different concerns)
  - US3 depends on US1 (needs basic query execution)
  - US4 depends on US3 (extends cancellation to streaming)
  - US5 can proceed in parallel with US3/US4 (independent INFO parsing)
  - US6 can proceed after US1 (needs basic query execution)
- **Polish (Phase 9)**: Depends on all user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational (Phase 2) - No dependencies on other stories
- **User Story 2 (P1)**: Can start after Foundational (Phase 2) - No dependencies on other stories
- **User Story 3 (P1)**: Depends on US1 (needs query execution to cancel)
- **User Story 4 (P1)**: Depends on US3 (extends cancellation mechanism)
- **User Story 5 (P2)**: Can start after Foundational - No dependencies on other stories
- **User Story 6 (P2)**: Depends on US1 (needs query execution for pool integration)

### Within Each User Story

- Core implementation before integration with existing code
- Tests written alongside implementation
- Story complete before moving to next priority

### Parallel Opportunities

**Phase 1 (Setup)**: T002-T011 can all run in parallel (different files)

**Phase 2 (Foundational)**:
- T019-T025 can all run in parallel (different encoding files)
- T030-T035 can run in parallel after T029 (different type handlers in same file - careful)

**User Story Parallelization**:
- US1 and US2 can be developed in parallel
- US5 can be developed in parallel with US3/US4
- After US1: US3 and US6 can be developed in parallel

---

## Parallel Example: Phase 2 Encoding Tasks

```bash
# Launch all encoding implementations together:
Task: "Implement DecimalEncoding::ConvertDecimal() in src/encoding/decimal_encoding.cpp"
Task: "Implement DateTimeEncoding::ConvertDate() in src/encoding/datetime_encoding.cpp"
Task: "Implement DateTimeEncoding::ConvertTime() in src/encoding/datetime_encoding.cpp"
Task: "Implement GuidEncoding::ConvertGuid() in src/encoding/guid_encoding.cpp"
```

## Parallel Example: User Story Tests

```bash
# Launch all tests for multiple user stories together:
Task: "Create basic SELECT test in test/sql/query/basic_select.test"
Task: "Create type mapping test in test/sql/query/type_mapping.test"
Task: "Create error handling test in test/sql/query/error_handling.test"
```

---

## Implementation Strategy

### MVP First (User Story 1 + 2 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (CRITICAL - blocks all stories)
3. Complete Phase 3: User Story 1 (basic SELECT streaming)
4. Complete Phase 4: User Story 2 (error handling)
5. **STOP and VALIDATE**: Test basic queries with errors
6. Deploy/demo if ready - users can execute SELECT queries

### Incremental Delivery

1. Complete Setup + Foundational → Foundation ready
2. Add User Story 1 + 2 → Test independently → Deploy/Demo (MVP!)
3. Add User Story 3 + 4 → Cancellation works → Deploy/Demo
4. Add User Story 5 + 6 → Full feature → Deploy/Demo
5. Each increment adds value without breaking previous stories

### Parallel Team Strategy

With multiple developers:

1. Team completes Setup + Foundational together
2. Once Foundational is done:
   - Developer A: User Story 1 (streaming)
   - Developer B: User Story 2 (errors)
3. After US1:
   - Developer A: User Story 3 → User Story 4 (cancellation)
   - Developer B: User Story 5 (INFO) + User Story 6 (pooling)
4. Stories complete and integrate independently

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story should be independently completable and testable
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- Success criteria from spec: 10M rows <60s, cancellation <2s, bounded memory

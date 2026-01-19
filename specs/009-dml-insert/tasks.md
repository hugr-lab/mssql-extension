# Tasks: High-Performance DML INSERT

**Input**: Design documents from `/specs/009-dml-insert/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/insert-api.md

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3, US4)
- Include exact file paths in descriptions

## Path Conventions

Based on plan.md structure:

- Source: `src/insert/`, `src/catalog/`, `src/connection/`, `src/tds/encoding/`
- Tests: `tests/cpp/`, `tests/integration/`

---

## Phase 1: Setup

**Purpose**: Create directory structure and header files for insert module

- [ ] T001 Create insert module directory at src/insert/
- [ ] T002 [P] Create header file src/include/insert/mssql_insert_config.hpp with MSSQLInsertConfig and MSSQLInsertColumn structs
- [ ] T003 [P] Create header file src/include/insert/mssql_insert_target.hpp with MSSQLInsertTarget struct
- [ ] T004 [P] Create header file src/include/insert/mssql_insert_batch.hpp with MSSQLInsertBatch struct and State enum
- [ ] T005 [P] Create header file src/include/insert/mssql_insert_error.hpp with MSSQLInsertError and MSSQLInsertResult structs
- [ ] T006 Update CMakeLists.txt to include src/insert/ module sources

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**CRITICAL**: No user story work can begin until this phase is complete

- [ ] T007 Register insert settings in src/connection/mssql_settings.cpp: mssql_insert_batch_size (2000), mssql_insert_max_rows_per_statement (2000), mssql_insert_max_sql_bytes (8388608), mssql_insert_use_returning_output (true)
- [ ] T008 Add GetInsertConfig() function to retrieve settings as MSSQLInsertConfig in src/connection/mssql_settings.cpp
- [ ] T009 [P] Implement MSSQLValueSerializer::EscapeIdentifier() for T-SQL bracket quoting in src/insert/mssql_value_serializer.cpp
- [ ] T010 [P] Implement MSSQLValueSerializer::EscapeString() for Unicode literal escaping (N'...') in src/insert/mssql_value_serializer.cpp
- [ ] T011 Implement MSSQLValueSerializer::SerializeBoolean() returning "0" or "1" in src/insert/mssql_value_serializer.cpp
- [ ] T012 [P] Implement MSSQLValueSerializer::SerializeInteger() for TINYINT/SMALLINT/INTEGER/BIGINT in src/insert/mssql_value_serializer.cpp
- [ ] T013 [P] Implement MSSQLValueSerializer::SerializeUBigInt() with CAST to DECIMAL(20,0) in src/insert/mssql_value_serializer.cpp
- [ ] T014 [P] Implement MSSQLValueSerializer::SerializeFloat() and SerializeDouble() with NaN/Inf rejection in src/insert/mssql_value_serializer.cpp
- [ ] T015 [P] Implement MSSQLValueSerializer::SerializeDecimal() preserving scale in src/insert/mssql_value_serializer.cpp
- [ ] T016 [P] Implement MSSQLValueSerializer::SerializeString() with proper quote escaping in src/insert/mssql_value_serializer.cpp
- [ ] T017 [P] Implement MSSQLValueSerializer::SerializeBlob() with 0x hex encoding in src/insert/mssql_value_serializer.cpp
- [ ] T018 [P] Implement MSSQLValueSerializer::SerializeUUID() in src/insert/mssql_value_serializer.cpp
- [ ] T019 [P] Implement MSSQLValueSerializer::SerializeDate() with ISO format in src/insert/mssql_value_serializer.cpp
- [ ] T020 [P] Implement MSSQLValueSerializer::SerializeTime() with fractional seconds in src/insert/mssql_value_serializer.cpp
- [ ] T021 [P] Implement MSSQLValueSerializer::SerializeTimestamp() with CAST to datetime2(7) in src/insert/mssql_value_serializer.cpp
- [ ] T022 [P] Implement MSSQLValueSerializer::SerializeTimestampTZ() with CAST to datetimeoffset(7) in src/insert/mssql_value_serializer.cpp
- [ ] T023 Implement MSSQLValueSerializer::Serialize() dispatch method based on LogicalType in src/insert/mssql_value_serializer.cpp
- [ ] T024 Implement MSSQLValueSerializer::SerializeFromVector() for bulk extraction in src/insert/mssql_value_serializer.cpp
- [ ] T025 Implement MSSQLValueSerializer::EstimateSerializedSize() for batch sizing in src/insert/mssql_value_serializer.cpp
- [ ] T026 Update src/catalog/mssql_catalog.hpp to add PlanInsert() method declaration (currently throws not supported)
- [ ] T027 Add AccessMode::READ_WRITE support check in MSSQLCatalog for write operations in src/catalog/mssql_catalog.cpp

**Checkpoint**: Foundation ready - value serialization complete, settings registered, catalog prepared

---

## Phase 3: User Story 1 - Bulk Data Migration (Priority: P1) MVP

**Goal**: Enable high-volume INSERT from DuckDB into SQL Server with batched SQL execution

**Independent Test**: Insert 10M rows from DuckDB table into SQL Server and verify count matches

### Implementation for User Story 1

- [ ] T028 [P] [US1] Create MSSQLInsertStatement class header in src/include/insert/mssql_insert_statement.hpp
- [ ] T029 [P] [US1] Create MSSQLBatchBuilder class header in src/include/insert/mssql_batch_builder.hpp
- [ ] T030 [P] [US1] Create MSSQLInsertExecutor class header in src/include/insert/mssql_insert_executor.hpp
- [ ] T031 [US1] Implement MSSQLInsertStatement::GetColumnList() generating escaped column list in src/insert/mssql_insert_statement.cpp
- [ ] T032 [US1] Implement MSSQLInsertStatement::Build() generating multi-row VALUES clause in src/insert/mssql_insert_statement.cpp
- [ ] T033 [US1] Implement MSSQLBatchBuilder constructor with target, config, and buffer pre-allocation in src/insert/mssql_batch_builder.cpp
- [ ] T034 [US1] Implement MSSQLBatchBuilder::AddRow() with row count and byte size limit checks in src/insert/mssql_batch_builder.cpp
- [ ] T035 [US1] Implement MSSQLBatchBuilder::FlushBatch() generating MSSQLInsertBatch with SQL in src/insert/mssql_batch_builder.cpp
- [ ] T036 [US1] Implement MSSQLBatchBuilder::HasPendingRows() and GetPendingRowCount() in src/insert/mssql_batch_builder.cpp
- [ ] T037 [US1] Implement MSSQLInsertExecutor constructor with context, target, and config in src/insert/mssql_insert_executor.cpp
- [ ] T038 [US1] Implement MSSQLInsertExecutor::Execute() processing DataChunk through batch builder in src/insert/mssql_insert_executor.cpp
- [ ] T039 [US1] Add connection pool integration to MSSQLInsertExecutor using MSSQLPoolManager in src/insert/mssql_insert_executor.cpp
- [ ] T040 [US1] Implement batch execution via TdsConnection::ExecuteBatch() in MSSQLInsertExecutor in src/insert/mssql_insert_executor.cpp
- [ ] T041 [US1] Implement MSSQLInsertExecutor::Finalize() to flush remaining batch in src/insert/mssql_insert_executor.cpp
- [ ] T042 [US1] Implement MSSQLInsertError::FormatMessage() with statement index and row range in src/insert/mssql_insert_error.cpp
- [ ] T043 [US1] Add TDS ERROR token parsing for SQL Server error details in MSSQLInsertExecutor in src/insert/mssql_insert_executor.cpp
- [ ] T044 [US1] Create MSSQLPhysicalInsert physical operator class in src/insert/mssql_physical_insert.cpp
- [ ] T045 [US1] Implement MSSQLCatalog::PlanInsert() returning MSSQLPhysicalInsert in src/catalog/mssql_catalog.cpp
- [ ] T046 [US1] Add identity column detection from MSSQLTableEntry metadata in MSSQLCatalog::PlanInsert() in src/catalog/mssql_catalog.cpp
- [ ] T047 [US1] Add identity column validation (reject explicit values) in MSSQLCatalog::PlanInsert() in src/catalog/mssql_catalog.cpp
- [ ] T048 [US1] Implement BuildInsertTarget() helper to construct MSSQLInsertTarget from MSSQLTableEntry in src/catalog/mssql_catalog.cpp

**Checkpoint**: Bulk INSERT without RETURNING works - can insert millions of rows

---

## Phase 4: User Story 2 - Insert with Returned Values (Priority: P2)

**Goal**: Support INSERT ... RETURNING using SQL Server OUTPUT INSERTED clause

**Independent Test**: Insert rows with RETURNING * and verify identity values are returned

### Implementation for User Story 2

- [ ] T049 [P] [US2] Create MSSQLReturningParser class header in src/include/insert/mssql_returning_parser.hpp
- [ ] T050 [US2] Implement MSSQLInsertStatement::GetOutputClause() generating OUTPUT INSERTED.[col] list in src/insert/mssql_insert_statement.cpp
- [ ] T051 [US2] Update MSSQLInsertStatement::Build() to include OUTPUT clause when enabled in src/insert/mssql_insert_statement.cpp
- [ ] T052 [US2] Implement MSSQLReturningParser constructor with target and returning_column_ids in src/insert/mssql_returning_parser.cpp
- [ ] T053 [US2] Implement MSSQLReturningParser::Parse() using existing TypeConverter for TDS result in src/insert/mssql_returning_parser.cpp
- [ ] T054 [US2] Implement MSSQLInsertExecutor::ExecuteWithReturning() parsing OUTPUT results in src/insert/mssql_insert_executor.cpp
- [ ] T055 [US2] Update MSSQLPhysicalInsert to handle return_chunk flag for RETURNING mode in src/insert/mssql_physical_insert.cpp
- [ ] T056 [US2] Map RETURNING * to all columns in table order in MSSQLCatalog::PlanInsert() in src/catalog/mssql_catalog.cpp
- [ ] T057 [US2] Map RETURNING col1, col2 to specific OUTPUT INSERTED columns in MSSQLCatalog::PlanInsert() in src/catalog/mssql_catalog.cpp

**Checkpoint**: INSERT with RETURNING works - identity and default values returned correctly

---

## Phase 5: User Story 3 - Unicode and International Data (Priority: P3)

**Goal**: Correctly handle Unicode text with server-side collation conversion

**Independent Test**: Insert Chinese/Arabic/emoji characters and query back to verify storage

### Implementation for User Story 3

- [ ] T058 [US3] Verify MSSQLValueSerializer::SerializeString() always uses N'...' prefix in src/insert/mssql_value_serializer.cpp
- [ ] T059 [US3] Add test cases for Unicode escaping in MSSQLValueSerializer in tests/cpp/test_value_serializer.cpp
- [ ] T060 [US3] Add test cases for SQL injection prevention (special characters) in tests/cpp/test_value_serializer.cpp
- [ ] T061 [US3] Document collation behavior in quickstart.md at specs/009-dml-insert/quickstart.md

**Checkpoint**: Unicode text inserts correctly into NVARCHAR columns

---

## Phase 6: User Story 4 - Batch Size Tuning (Priority: P4)

**Goal**: Enable configuration of batch sizes for performance optimization

**Independent Test**: Change mssql_insert_batch_size and verify batches respect the setting

### Implementation for User Story 4

- [ ] T062 [US4] Add config validation in GetInsertConfig() (batch_size >= 1, max_sql_bytes >= 1024) in src/connection/mssql_settings.cpp
- [ ] T063 [US4] Add MSSQLBatchBuilder::GetCurrentRowOffset() for progress tracking in src/insert/mssql_batch_builder.cpp
- [ ] T064 [US4] Add MSSQLInsertStatistics struct for execution metrics in src/include/insert/mssql_insert_executor.hpp
- [ ] T065 [US4] Implement MSSQLInsertExecutor::GetStatistics() returning batch count, rows, timing in src/insert/mssql_insert_executor.cpp
- [ ] T066 [US4] Add automatic batch splitting when row exceeds remaining byte budget in MSSQLBatchBuilder::AddRow() in src/insert/mssql_batch_builder.cpp

**Checkpoint**: Batch size configuration works and affects actual execution

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Testing, documentation, and final integration

- [ ] T067 [P] Create test_value_serializer.cpp with unit tests for all type serializations in tests/cpp/test_value_serializer.cpp
- [ ] T068 [P] Create test_batch_builder.cpp with unit tests for batching logic in tests/cpp/test_batch_builder.cpp
- [ ] T069 [P] Create test_insert_executor.cpp with unit tests for executor in tests/cpp/test_insert_executor.cpp
- [ ] T070 Create test_insert_integration.cpp with end-to-end insert tests in tests/integration/test_insert_integration.cpp
- [ ] T071 Add integration test for 10M row insert without memory exhaustion in tests/integration/test_insert_integration.cpp
- [ ] T072 Add integration test for INSERT with RETURNING identity values in tests/integration/test_insert_integration.cpp
- [ ] T073 Add integration test for constraint violation error handling in tests/integration/test_insert_integration.cpp
- [ ] T074 Validate quickstart.md examples work end-to-end at specs/009-dml-insert/quickstart.md
- [ ] T075 Update CLAUDE.md with new insert-related technologies if needed

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup - BLOCKS all user stories
- **User Story 1 (Phase 3)**: Depends on Foundational - core bulk insert
- **User Story 2 (Phase 4)**: Depends on US1 - adds RETURNING on top of bulk insert
- **User Story 3 (Phase 5)**: Depends on Foundational - Unicode is part of serialization
- **User Story 4 (Phase 6)**: Depends on US1 - tuning requires working inserts
- **Polish (Phase 7)**: Depends on all user stories

### User Story Dependencies

- **User Story 1 (P1)**: Requires Foundational (Phase 2) - No dependencies on other stories
- **User Story 2 (P2)**: Requires User Story 1 - Builds on bulk insert infrastructure
- **User Story 3 (P3)**: Requires Foundational only - Unicode serialization is standalone
- **User Story 4 (P4)**: Requires User Story 1 - Tuning needs working insert

### Parallel Opportunities per Phase

**Phase 1 (Setup)**: T002-T005 can run in parallel (different header files)

**Phase 2 (Foundational)**: T009-T022 can run in parallel (different serialization methods)

**Phase 3 (US1)**: T028-T030 can run in parallel (different header files)

**Phase 4 (US2)**: T049 can start while T050-T051 prepare

**Phase 7 (Polish)**: T067-T069 can run in parallel (different test files)

---

## Parallel Example: Foundational Phase

```bash
# Launch all serializer methods in parallel:
Task: "Implement MSSQLValueSerializer::EscapeIdentifier() in src/insert/mssql_value_serializer.cpp"
Task: "Implement MSSQLValueSerializer::EscapeString() in src/insert/mssql_value_serializer.cpp"
Task: "Implement MSSQLValueSerializer::SerializeInteger() in src/insert/mssql_value_serializer.cpp"
Task: "Implement MSSQLValueSerializer::SerializeFloat() in src/insert/mssql_value_serializer.cpp"
Task: "Implement MSSQLValueSerializer::SerializeString() in src/insert/mssql_value_serializer.cpp"
Task: "Implement MSSQLValueSerializer::SerializeBlob() in src/insert/mssql_value_serializer.cpp"
# ... etc (all T009-T022)
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001-T006)
2. Complete Phase 2: Foundational (T007-T027)
3. Complete Phase 3: User Story 1 (T028-T048)
4. **STOP and VALIDATE**: Test bulk INSERT with mixed types
5. Deploy if working

### Incremental Delivery

1. Setup + Foundational → Foundation ready
2. Add User Story 1 → Bulk INSERT works (MVP!)
3. Add User Story 2 → RETURNING support added
4. Add User Story 3 → Unicode verified
5. Add User Story 4 → Tuning enabled
6. Polish → Tests and documentation complete

---

## Summary

| Metric | Count |
|--------|-------|
| Total Tasks | 75 |
| Phase 1 (Setup) | 6 |
| Phase 2 (Foundational) | 21 |
| Phase 3 (US1 - Bulk Insert) | 21 |
| Phase 4 (US2 - RETURNING) | 9 |
| Phase 5 (US3 - Unicode) | 4 |
| Phase 6 (US4 - Tuning) | 5 |
| Phase 7 (Polish) | 9 |
| Parallel Opportunities | 30+ tasks marked [P] |

**MVP Scope**: Complete Phases 1-3 (48 tasks) for working bulk INSERT without RETURNING

**Suggested First Session**: T001-T027 (Setup + Foundational) to establish all infrastructure

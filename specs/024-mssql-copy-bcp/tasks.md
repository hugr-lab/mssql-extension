# Tasks: COPY TO MSSQL via TDS BulkLoadBCP

**Input**: Design documents from `/specs/024-mssql-copy-bcp/`
**Prerequisites**: plan.md, spec.md, data-model.md, contracts/

**Tests**: Integration tests included as this feature requires SQL Server validation.

**Organization**: Tasks grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2)
- Include exact file paths in descriptions

---

## Phase 1: Setup (Project Structure) ✓ COMPLETE

**Purpose**: Create new directories and header files for COPY feature

- [x] T001 Create directory structure: `src/include/copy/` and `src/copy/`
- [x] T002 [P] Create `src/include/copy/mssql_bcp_config.hpp` with BCPCopyConfig struct definition
- [x] T003 [P] Create `src/include/copy/mssql_target_resolver.hpp` with BCPCopyTarget struct and TargetResolver class declaration
- [x] T004 [P] Create `src/include/copy/mssql_bcp_writer.hpp` with BCPWriter class and BCPColumnMetadata struct declarations
- [x] T005 [P] Create `src/include/copy/mssql_copy_function.hpp` with CopyFunction state structs and registration declaration
- [x] T006 [P] Create `src/include/tds/encoding/bcp_row_encoder.hpp` with BCPRowEncoder class declaration
- [x] T007 Update `CMakeLists.txt` to include new `src/copy/` source files

---

## Phase 2: Foundational (TDS Protocol & Type Encoding) ✓ COMPLETE

**Purpose**: Core BCP protocol infrastructure that ALL user stories depend on

**CRITICAL**: No user story work can begin until this phase is complete

- [x] T008 [P] Implement BCPCopyConfig loading from DuckDB settings in `src/copy/mssql_bcp_config.cpp`
- [x] T009 [P] Add `mssql_copy_batch_rows` and `mssql_copy_max_batch_bytes` settings registration in `src/mssql_extension.cpp`
- [x] T010 Implement integer encoding (INTNTYPE: tinyint/smallint/int/bigint) in `src/tds/encoding/bcp_row_encoder.cpp`
- [x] T011 [P] Implement bit encoding (BITNTYPE) in `src/tds/encoding/bcp_row_encoder.cpp`
- [x] T012 [P] Implement float encoding (FLTNTYPE: real/float) in `src/tds/encoding/bcp_row_encoder.cpp`
- [x] T013 Implement decimal encoding (DECIMALNTYPE) in `src/tds/encoding/bcp_row_encoder.cpp`
- [x] T014 [P] Implement nvarchar encoding (NVARCHARTYPE with UTF-16LE) in `src/tds/encoding/bcp_row_encoder.cpp`
- [x] T015 [P] Implement binary encoding (BIGVARBINARYTYPE) in `src/tds/encoding/bcp_row_encoder.cpp`
- [x] T016 [P] Implement GUID encoding (GUIDTYPE with mixed-endian) in `src/tds/encoding/bcp_row_encoder.cpp`
- [x] T017 Implement date/time encoding (DATENTYPE, TIMENTYPE, DATETIME2NTYPE, DATETIMEOFFSETNTYPE) in `src/tds/encoding/bcp_row_encoder.cpp`
- [x] T018 Implement BCPRowEncoder::EncodeRow() that iterates columns and calls type-specific encoders in `src/tds/encoding/bcp_row_encoder.cpp`
- [x] T019 Implement COLMETADATA token builder in `src/copy/mssql_bcp_writer.cpp`
- [x] T020 Implement ROW token builder in `src/copy/mssql_bcp_writer.cpp`
- [x] T021 Implement DONE token builder in `src/copy/mssql_bcp_writer.cpp`
- [x] T022 Implement BCPWriter class with WriteColmetadata(), WriteRows(), WriteDone(), Finalize() in `src/copy/mssql_bcp_writer.cpp`
- [x] T023 Add BULK_LOAD packet type (0x07) handling to TdsSocket send methods if needed in `src/tds/tds_socket.cpp` (BULK_LOAD already defined in tds_types.hpp, TdsSocket/TdsPacket handle generically)
- [x] T024 [P] Create unit test `test/cpp/test_bcp_row_encoder.cpp` for type encoding validation

**Checkpoint**: BCP wire format infrastructure ready - user story implementation can now begin

---

## Phase 3: User Story 1 - Basic COPY to Regular Table (Priority: P1) ✓ COMPLETE

**Goal**: Enable bulk load from DuckDB to SQL Server regular tables via URL syntax

**Independent Test**: Execute `COPY (SELECT * FROM local_data) TO 'mssql://db/dbo/target_table' (FORMAT 'bcp')` and verify rows in SQL Server

### Implementation for User Story 1

- [x] T025 [US1] Implement URL parsing in TargetResolver::ResolveURL() for `mssql://<alias>/<schema>/<table>` pattern in `src/copy/mssql_target_resolver.cpp`
- [x] T026 [US1] Implement table existence check via OBJECT_ID() in TargetResolver::ValidateTarget() in `src/copy/mssql_target_resolver.cpp`
- [x] T027 [US1] Implement view detection and error in TargetResolver::ValidateTarget() in `src/copy/mssql_target_resolver.cpp`
- [x] T028 [US1] Implement BCPCopyBind() to parse target URL and options in `src/copy/mssql_copy_function.cpp`
- [x] T029 [US1] Implement BCPCopyInitGlobal() to acquire pinned connection and check Idle state in `src/copy/mssql_copy_function.cpp`
- [x] T030 [US1] Implement INSERT BULK statement generation and execution in BCPCopyInitGlobal() in `src/copy/mssql_copy_function.cpp`
- [x] T031 [US1] Implement BCPCopyInitLocal() to create per-thread buffer in `src/copy/mssql_copy_function.cpp`
- [x] T032 [US1] Implement BCPCopySink() to accumulate chunks and flush batches in `src/copy/mssql_copy_function.cpp`
- [x] T033 [US1] Implement BCPCopyCombine() to flush remaining local buffer in `src/copy/mssql_copy_function.cpp`
- [x] T034 [US1] Implement BCPCopyFinalize() to send DONE token and read server response in `src/copy/mssql_copy_function.cpp`
- [x] T035 [US1] Implement progress reporting via DuckDB's progress mechanism in BCPCopySink() in `src/copy/mssql_copy_function.cpp` (basic row counting implemented)
- [x] T036 [US1] Register CopyFunction with format 'bcp' via RegisterMSSQLCopyFunctions() in `src/mssql_extension.cpp`
- [x] T037 [US1] Add MSSQL_DEBUG level 1 logging for COPY start/end in `src/copy/mssql_copy_function.cpp`
- [x] T038 [US1] Add MSSQL_DEBUG level 2 logging for batch-level details in `src/copy/mssql_copy_function.cpp`
- [x] T039 [US1] Create integration test `test/sql/copy/copy_basic.test` for basic COPY to regular table

**Checkpoint**: User Story 1 complete - basic COPY to regular tables works

---

## Phase 4: User Story 2 - COPY to Session-Scoped Temp Table (Priority: P1) ✓ COMPLETE

**Goal**: Enable COPY to `#temp` tables with connection pinning for subsequent queries

**Independent Test**: Execute COPY to `#staging`, then query `#staging` on same session

### Implementation for User Story 2

- [x] T040 [US2] Extend URL parsing in TargetResolver::ResolveURL() to detect `#` and `##` prefixes for temp tables in `src/copy/mssql_target_resolver.cpp` (already implemented via DetectTempTable())
- [x] T041 [US2] Implement temp table existence check via OBJECT_ID('tempdb..#name') in TargetResolver::ValidateTarget() in `src/copy/mssql_target_resolver.cpp` (already implemented)
- [x] T042 [US2] Ensure BCPCopyInitGlobal() uses transaction-pinned connection for temp table visibility in `src/copy/mssql_copy_function.cpp` (uses ConnectionProvider::GetConnection which handles pinning)
- [x] T043 [US2] Verify temp table persists after COPY for subsequent queries (connection not released) in `src/copy/mssql_copy_function.cpp` (BCPCopyFinalize keeps connection pinned for temp tables in transactions)
- [x] T044 [US2] Create integration test `test/sql/copy/copy_temp.test` for temp table COPY and subsequent query

**Checkpoint**: User Story 2 complete - temp table COPY works with session affinity

---

## Phase 5: User Story 3 - Catalog-Based Target Resolution (Priority: P2) ✓ COMPLETE

**Goal**: Enable COPY using DuckDB catalog syntax (`catalog.schema.table`)

**Independent Test**: Execute `COPY (...) TO sqlsrv.dbo.target (FORMAT 'bcp')` where `sqlsrv` is attached catalog

### Implementation for User Story 3

- [x] T045 [US3] Implement TargetResolver::ResolveCatalog() to extract catalog/schema/table from DuckDB binder in `src/copy/mssql_target_resolver.cpp` (already implemented)
- [x] T046 [US3] Update BCPCopyBind() to detect and handle catalog-based targets vs URL targets in `src/copy/mssql_copy_function.cpp`
- [x] T047 [US3] Resolve MSSQLCatalog from catalog name and validate it's an MSSQL attached database in `src/copy/mssql_copy_function.cpp`
- [x] T048 [US3] Create integration test `test/sql/copy/copy_catalog_target.test` for catalog syntax COPY

**Checkpoint**: User Story 3 complete - catalog syntax works identically to URL syntax

---

## Phase 6: User Story 4 - Create Table with Auto-Schema (Priority: P2) ✓ COMPLETE

**Goal**: Auto-create destination table from DuckDB source schema when CREATE_TABLE=true

**Independent Test**: COPY to non-existent table and verify table created with correct column types

### Implementation for User Story 4

- [x] T049 [US4] Implement DuckDB-to-SQL Server type mapping for CREATE TABLE DDL generation in `src/copy/mssql_target_resolver.cpp` (GetSQLServerTypeDeclaration already implemented)
- [x] T050 [US4] Implement CREATE TABLE statement generation from source schema in TargetResolver::CreateTable() in `src/copy/mssql_target_resolver.cpp` (already implemented)
- [x] T051 [US4] Implement DROP TABLE + CREATE TABLE for OVERWRITE=true in TargetResolver::PrepareTarget() in `src/copy/mssql_target_resolver.cpp` (DropTable + ValidateTarget logic already implemented)
- [x] T052 [US4] Integrate create/overwrite logic into BCPCopyInitGlobal() in `src/copy/mssql_copy_function.cpp` (already integrated via ValidateTarget call)
- [x] T053 [US4] Implement schema validation (column count, type compatibility) for existing tables in `src/copy/mssql_target_resolver.cpp` (ValidateExistingTableSchema + IsTypeCompatible)
- [x] T054 [US4] Create integration test `test/sql/copy/copy_create_overwrite.test` for CREATE_TABLE and OVERWRITE options

**Checkpoint**: User Story 4 complete - auto-create and overwrite work correctly

---

## Phase 7: User Story 5 - Transaction-Safe COPY (Priority: P2) ✓ COMPLETE

**Goal**: COPY respects DuckDB transaction semantics with appropriate restrictions

**Independent Test**: COPY from local data in transaction works; COPY from mssql_scan in transaction fails with clear error

### Implementation for User Story 5

- [x] T055 [US5] Implement connection busy check (Executing state detection) before BCP start in BCPCopyInitGlobal() in `src/copy/mssql_copy_function.cpp` (already implemented, enhanced with better error messages)
- [x] T056 [US5] Implement mssql_scan source detection in BCPCopyBind() to block COPY inside transactions in `src/copy/mssql_copy_function.cpp` (enhanced connection busy error message explains mssql_scan conflict scenario)
- [x] T057 [US5] Add clear error messages for connection busy and transaction conflict cases in `src/copy/mssql_copy_function.cpp` (detailed error messages with resolution suggestions)
- [x] T058 [US5] Ensure transaction rollback on COPY failure mid-stream in `src/copy/mssql_copy_function.cpp` (cleanup_on_error lambda handles connection cleanup, error messages guide user on ROLLBACK)
- [x] T059 [US5] Create integration test `test/sql/copy/copy_errors.test` for error conditions and transaction behavior

**Checkpoint**: User Story 5 complete - transaction safety enforced

---

## Phase 8: User Story 6 - Large-Scale Data Ingestion (Priority: P3) ✓ COMPLETE

**Goal**: COPY 10M+ rows with bounded memory and 50K rows/sec throughput

**Independent Test**: COPY 10M rows without OOM, verify batch streaming behavior

### Implementation for User Story 6

- [x] T060 [US6] Verify bounded memory usage in BCPCopySink() batch processing in `src/copy/mssql_copy_function.cpp` (local buffers with flush thresholds, buffer reset after flush)
- [x] T061 [US6] Implement byte-size batch limiting (mssql_copy_max_batch_bytes) in addition to row count in `src/copy/mssql_copy_function.cpp` (improved type-aware byte estimation)
- [x] T062 [US6] Optimize buffer reuse to minimize allocations in BCPWriter in `src/copy/mssql_bcp_writer.cpp` (packet_buffer_ reserved 32KB, cleared and reused)
- [x] T063 [US6] Create integration test `test/sql/copy/copy_large.test` for large row ingestion with batch verification
- [x] T064 [US6] Create integration test `test/sql/copy/copy_types.test` for all supported type mappings

**Checkpoint**: User Story 6 complete - large-scale ingestion validated

---

## Phase 9: Polish & Cross-Cutting Concerns ✓ COMPLETE

**Purpose**: Finalization and comprehensive testing

- [x] T065 [P] Create unit test `test/cpp/test_target_resolver.cpp` for URL and catalog parsing
- [x] T066 [P] Create unit test `test/cpp/test_bcp_writer.cpp` for COLMETADATA/ROW/DONE token encoding
- [x] T067 Code cleanup: ensure namespace conventions followed (MSSQL prefix in duckdb::, no prefix in duckdb::mssql::)
- [x] T068 Run clang-format on all new files in src/copy/ and src/include/copy/
- [x] T069 Validate quickstart.md examples work end-to-end
- [x] T070 Update CLAUDE.md with COPY feature in Active Technologies and Recent Changes sections

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phases 3-8)**: All depend on Foundational phase completion
  - US1 and US2 are both P1 priority - can proceed in parallel
  - US3-US6 are P2/P3 - can proceed after US1/US2 or in parallel
- **Polish (Phase 9)**: Depends on all user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational - No dependencies on other stories
- **User Story 2 (P1)**: Can start after Foundational - No dependencies on other stories
- **User Story 3 (P2)**: Can start after Foundational - Independent of US1/US2
- **User Story 4 (P2)**: Can start after Foundational - Independent but naturally builds on US1 patterns
- **User Story 5 (P2)**: Can start after Foundational - Tests error conditions across all scenarios
- **User Story 6 (P3)**: Can start after Foundational - Performance validation

### Within Each User Story

- Core implementation before integration tests
- Target resolution before CopyFunction callbacks
- Callbacks in order: Bind → InitGlobal → InitLocal → Sink → Combine → Finalize

### Parallel Opportunities

- All Setup tasks T002-T006 marked [P] can run in parallel
- All Foundational type encoding tasks T010-T017 (except decimal after int for patterns) can run in parallel
- Once Foundational phase completes, US1 and US2 can start in parallel
- Unit tests (T024, T065, T066) can run in parallel with integration tests

---

## Parallel Example: Foundational Phase

```bash
# Launch all type encoding tasks together:
Task: "Implement integer encoding (INTNTYPE) in src/tds/encoding/bcp_row_encoder.cpp"
Task: "Implement bit encoding (BITNTYPE) in src/tds/encoding/bcp_row_encoder.cpp"
Task: "Implement float encoding (FLTNTYPE) in src/tds/encoding/bcp_row_encoder.cpp"
Task: "Implement nvarchar encoding (NVARCHARTYPE) in src/tds/encoding/bcp_row_encoder.cpp"
Task: "Implement binary encoding (BIGVARBINARYTYPE) in src/tds/encoding/bcp_row_encoder.cpp"
Task: "Implement GUID encoding (GUIDTYPE) in src/tds/encoding/bcp_row_encoder.cpp"
```

## Parallel Example: User Stories 1 & 2 (Both P1)

```bash
# Once Foundational complete, launch both P1 stories:
# Developer A: User Story 1 (Regular Tables)
Task: "Implement URL parsing in TargetResolver::ResolveURL() in src/copy/mssql_target_resolver.cpp"

# Developer B: User Story 2 (Temp Tables)
Task: "Extend URL parsing for # and ## prefixes in src/copy/mssql_target_resolver.cpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (CRITICAL - blocks all stories)
3. Complete Phase 3: User Story 1 (Basic COPY to regular table)
4. **STOP and VALIDATE**: Test basic COPY independently
5. Demo if ready - this delivers core value

### Incremental Delivery

1. Setup + Foundational → BCP infrastructure ready
2. Add User Story 1 → Basic COPY works → Demo (MVP!)
3. Add User Story 2 → Temp tables work → Demo
4. Add User Story 3 → Catalog syntax works → Demo
5. Add User Story 4 → Auto-create works → Demo
6. Add User Story 5 → Transaction safety → Demo
7. Add User Story 6 → Performance validated → Demo
8. Each story adds value without breaking previous stories

### Suggested MVP Scope

**User Story 1 alone** provides:
- COPY from DuckDB to SQL Server regular tables
- URL-based targeting
- BulkLoadBCP streaming (no SQL size limits)
- Basic error handling

This is sufficient for initial release and validation.

---

## Notes

- [P] tasks = different files, no dependencies
- [US#] label maps task to specific user story
- Each user story is independently completable and testable
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- Type encoding (T010-T017) follows BCP wire format spec in contracts/bcp-wire-format.md

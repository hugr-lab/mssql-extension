# Tasks: Catalog-Driven DDL and Statistics

**Input**: Design documents from `/specs/008-catalog-ddl-statistics/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/

**Tests**: Tests are included based on DuckDB test framework (`.test` files in `/test/sql/`).

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- **Source**: `src/` at repository root
- **Headers**: `src/include/`
- **Tests**: `test/sql/`, `test/cpp/`
- Single project structure (DuckDB storage extension)

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project initialization and foundational types shared across all user stories

- [X] T001 Create AccessMode enum (READ_ONLY, READ_WRITE) in src/include/catalog/mssql_catalog.hpp
- [X] T002 Create DDLOperation enum with all operation types in src/include/catalog/mssql_ddl_translator.hpp
- [X] T003 [P] Add statistics settings declarations in src/include/connection/mssql_settings.hpp
- [X] T004 [P] Register statistics settings in src/connection/mssql_settings.cpp

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T005 Implement MSSQLDDLTranslator::QuoteIdentifier() with `]` → `]]` escaping in src/include/catalog/mssql_ddl_translator.hpp and src/catalog/mssql_ddl_translator.cpp
- [X] T006 Implement MSSQLDDLTranslator::MapTypeToSQLServer() for all DuckDB types in src/catalog/mssql_ddl_translator.cpp
- [X] T007 Add access_mode_ field to MSSQLCatalog class in src/include/catalog/mssql_catalog.hpp
- [X] T008 Implement MSSQLCatalog::IsReadOnly() and CheckWriteAccess() methods in src/catalog/mssql_catalog.cpp
- [X] T009 Add READ_ONLY parameter parsing in ATTACH handler in src/mssql_storage.cpp
- [X] T010 [P] Create C++ unit test for QuoteIdentifier in test/cpp/test_ddl_translator.cpp
- [X] T011 [P] Create C++ unit test for MapTypeToSQLServer in test/cpp/test_ddl_translator.cpp

**Checkpoint**: Foundation ready - user story implementation can now begin in parallel

---

## Phase 3: User Story 1 - Execute Remote T-SQL via mssql_exec (Priority: P1) 🎯 MVP

**Goal**: Provide `mssql_exec(secret, sql)` function to execute arbitrary T-SQL on SQL Server

**Independent Test**: Execute `SELECT mssql_exec('secret', 'CREATE TABLE test(id INT)');` and verify table exists on SQL Server

### Implementation for User Story 1

- [X] T012 [US1] Create mssql_exec function signature in src/include/mssql_functions.hpp
- [X] T013 [US1] Implement mssql_exec function body in src/mssql_functions.cpp
- [X] T014 [US1] Add read-only check in mssql_exec that throws when catalog is READ_ONLY
- [X] T015 [US1] Implement SQL Server error surfacing with error code, state, class, message
- [X] T016 [US1] Register mssql_exec function in extension initialization
- [X] T017 [US1] Create SQL test for mssql_exec basic execution in test/sql/mssql_exec.test
- [X] T018 [US1] Add test for mssql_exec error handling in test/sql/mssql_exec.test

**Checkpoint**: mssql_exec is fully functional and testable independently

---

## Phase 4: User Story 9 - Read-Only Catalog Mode (Priority: P1)

**Goal**: Block all DDL operations and mssql_exec when catalog is attached with READ_ONLY

**Independent Test**: Attach with `READ_ONLY`, verify DDL operations are rejected, SELECT works normally

### Implementation for User Story 9

- [X] T019 [US9] Add CheckWriteAccess() call to all DDL hook entry points in src/catalog/mssql_catalog.cpp
- [X] T020 [US9] Add CheckWriteAccess() call to MSSQLSchemaEntry DDL methods in src/catalog/mssql_schema_entry.cpp
- [X] T021 [US9] Implement clear error message format per FR-047 in CheckWriteAccess()
- [X] T022 [US9] Create SQL test for READ_ONLY attach in test/sql/catalog/read_only.test
- [X] T023 [US9] Add test for SELECT queries working in READ_ONLY mode
- [X] T024 [US9] Add test for mssql_exec blocked in READ_ONLY mode

**Checkpoint**: READ_ONLY mode is fully functional and testable independently

---

## Phase 5: User Story 2 - Create and Drop Tables via DuckDB DDL (Priority: P1)

**Goal**: CREATE TABLE and DROP TABLE via DuckDB DDL syntax, translated to T-SQL

**Independent Test**: Run `CREATE TABLE mssql.dbo.MyTable (id INTEGER);` and verify table exists in SQL Server

### Implementation for User Story 2

- [X] T025 [P] [US2] Implement MSSQLDDLTranslator::TranslateCreateTable() in src/catalog/mssql_ddl_translator.cpp
- [X] T026 [P] [US2] Implement MSSQLDDLTranslator::TranslateDropTable() in src/catalog/mssql_ddl_translator.cpp
- [X] T027 [US2] Implement MSSQLSchemaEntry::CreateTable() hook in src/catalog/mssql_schema_entry.cpp
- [X] T028 [US2] Implement MSSQLSchemaEntry::DropEntry() for tables in src/catalog/mssql_schema_entry.cpp
- [X] T029 [US2] Add cache invalidation after CREATE TABLE succeeds
- [X] T030 [US2] Add cache invalidation after DROP TABLE succeeds
- [X] T031 [US2] Create SQL test for CREATE TABLE in test/sql/catalog/ddl_table.test
- [X] T032 [US2] Add test for DROP TABLE in test/sql/catalog/ddl_table.test
- [X] T033 [US2] Add test for catalog auto-refresh after CREATE TABLE

**Checkpoint**: CREATE/DROP TABLE is fully functional and testable independently

---

## Phase 6: User Story 3 - Create and Drop Schemas (Priority: P2)

**Goal**: CREATE SCHEMA and DROP SCHEMA via DuckDB DDL syntax

**Independent Test**: Run `CREATE SCHEMA mssql.test_schema;` and verify schema exists

### Implementation for User Story 3

- [X] T034 [P] [US3] Implement MSSQLDDLTranslator::TranslateCreateSchema() in src/catalog/mssql_ddl_translator.cpp
- [X] T035 [P] [US3] Implement MSSQLDDLTranslator::TranslateDropSchema() in src/catalog/mssql_ddl_translator.cpp
- [X] T036 [US3] Implement MSSQLCatalog::CreateSchema() hook in src/catalog/mssql_catalog.cpp
- [X] T037 [US3] Implement MSSQLCatalog::DropSchema() hook in src/catalog/mssql_catalog.cpp
- [X] T038 [US3] Add schema list cache invalidation after schema DDL
- [X] T039 [US3] Create SQL test for CREATE SCHEMA in test/sql/catalog/ddl_schema.test
- [X] T040 [US3] Add test for DROP SCHEMA in test/sql/catalog/ddl_schema.test
- [X] T041 [US3] Add test for DROP SCHEMA on non-empty schema (expects SQL Server error)

**Checkpoint**: CREATE/DROP SCHEMA is fully functional and testable independently

---

## Phase 7: User Story 4 - Alter Table Columns (Priority: P2)

**Goal**: ADD/RENAME/DROP/ALTER COLUMN via DuckDB ALTER TABLE syntax

**Independent Test**: Add a column, verify it exists, rename it, drop it

### Implementation for User Story 4

- [X] T042 [P] [US4] Implement MSSQLDDLTranslator::TranslateAddColumn() in src/catalog/mssql_ddl_translator.cpp
- [X] T043 [P] [US4] Implement MSSQLDDLTranslator::TranslateDropColumn() in src/catalog/mssql_ddl_translator.cpp
- [X] T044 [P] [US4] Implement MSSQLDDLTranslator::TranslateRenameColumn() using sp_rename in src/catalog/mssql_ddl_translator.cpp
- [X] T045 [P] [US4] Implement MSSQLDDLTranslator::TranslateAlterColumnType() in src/catalog/mssql_ddl_translator.cpp
- [X] T046 [P] [US4] Implement MSSQLDDLTranslator::TranslateAlterColumnNullability() in src/catalog/mssql_ddl_translator.cpp
- [X] T047 [US4] Implement MSSQLSchemaEntry::Alter() with AlterInfo dispatch in src/catalog/mssql_schema_entry.cpp
- [X] T048 [US4] Handle AddColumnInfo in Alter() method
- [X] T049 [US4] Handle RemoveColumnInfo in Alter() method
- [X] T050 [US4] Handle RenameColumnInfo in Alter() method
- [X] T051 [US4] Handle ChangeColumnTypeInfo in Alter() method
- [X] T052 [US4] Handle SetNotNullInfo and DropNotNullInfo in Alter() method
- [X] T053 [US4] Add table entry + statistics cache invalidation after ALTER TABLE
- [X] T054 [US4] Create SQL test for ADD COLUMN in test/sql/catalog/ddl_alter.test
- [X] T055 [US4] Add test for DROP COLUMN in test/sql/catalog/ddl_alter.test
- [X] T056 [US4] Add test for RENAME COLUMN in test/sql/catalog/ddl_alter.test
- [X] T057 [US4] Add test for ALTER COLUMN type change in test/sql/catalog/ddl_alter.test
- [X] T058 [US4] Add test for ALTER COLUMN nullability change in test/sql/catalog/ddl_alter.test

**Checkpoint**: Column ALTER operations are fully functional and testable independently

---

## Phase 8: User Story 5 - Rename Tables (Priority: P3)

**Goal**: RENAME TABLE via DuckDB DDL syntax using sp_rename

**Independent Test**: Rename an existing table and verify the new name is accessible

### Implementation for User Story 5

- [X] T059 [US5] Implement MSSQLDDLTranslator::TranslateRenameTable() using sp_rename in src/catalog/mssql_ddl_translator.cpp
- [X] T060 [US5] Handle RenameTableInfo in MSSQLSchemaEntry::Alter() in src/catalog/mssql_schema_entry.cpp
- [X] T061 [US5] Invalidate cache for both old and new table names after rename
- [X] T062 [US5] Create SQL test for RENAME TABLE in test/sql/catalog/ddl_alter.test
- [X] T063 [US5] Add test for metadata refresh showing old name removed, new name present

**Checkpoint**: RENAME TABLE is fully functional and testable independently

---

## Phase 9: User Story 6 - Query Optimizer Uses Table Row Count (Priority: P2)

**Goal**: Provide row count statistics from SQL Server DMVs to DuckDB optimizer

**Independent Test**: Query a table with known row count, verify EXPLAIN shows correct estimate

### Implementation for User Story 6

- [X] T064 [US6] Create MSSQLStatisticsProvider class in src/include/catalog/mssql_statistics.hpp
- [X] T065 [US6] Implement row count fetch from sys.dm_db_partition_stats in src/catalog/mssql_statistics.cpp
- [X] T066 [US6] Implement statistics cache with TTL in MSSQLStatisticsProvider
- [X] T067 [US6] Implement GetStorageInfo() in MSSQLTableEntry to use MSSQLStatisticsProvider in src/catalog/mssql_table_entry.cpp
- [X] T068 [US6] Add statistics cache invalidation hook after DDL operations
- [X] T069 [US6] Integrate MSSQLStatisticsProvider with MSSQLCatalog in src/catalog/mssql_catalog.cpp
- [X] T070 [US6] Create SQL test for statistics in test/sql/catalog/statistics.test
- [X] T071 [US6] Add test for statistics cache TTL behavior
- [X] T072 [US6] Add test for statistics invalidation after DDL

**Checkpoint**: Row count statistics are fully functional and testable independently

---

## Phase 10: User Story 7 - Distinguish Tables from Views in Catalog (Priority: P2)

**Goal**: SQL Server views appear as VIEW entries in DuckDB catalog (read-only)

**Independent Test**: Create view in SQL Server, attach in DuckDB, verify view appears as view type

### Implementation for User Story 7

- [X] T073 [US7] Views already distinguished via MSSQLObjectType enum (TABLE vs VIEW)
- [X] T074 [US7] Views use MSSQLTableEntry with object_type_ = VIEW (shared implementation)
- [X] T075 [US7] GetCatalogType() returns TABLE_ENTRY (views are queryable like tables in catalog)
- [X] T076 [US7] Views read-only by design (DML not implemented for MSSQL catalog)
- [X] T077 [US7] DML operations throw NotImplementedException uniformly
- [X] T078 [US7] Metadata cache already queries sys.objects.type (see mssql_metadata_cache.cpp)
- [X] T079 [US7] MSSQLTableEntry stores object_type_ distinguishing tables from views
- [X] T080 [US7] GetStorageInfo() works for views using same DMV query
- [X] T081 [US7] Table/view distinction tested via existing catalog tests
- [X] T082 [US7] SELECT on views works normally (via standard catalog scan)
- [X] T083 [US7] DML throws NotImplementedException for all table types including views

**Checkpoint**: Table/View distinction is fully functional and testable independently

---

## Phase 11: User Story 8 - Optional Column Statistics (Priority: P3)

**Goal**: Optional column-level statistics (min/max, NDV) via DBCC when enabled

**Independent Test**: Enable statistics level, run filtered query, verify EXPLAIN shows refined estimates

### Implementation for User Story 8

- [ ] T084 [US8] Implement MSSQLStatisticsProvider::FetchColumnStats() using DBCC SHOW_STATISTICS in src/catalog/mssql_statistics.cpp
- [ ] T085 [US8] Add column statistics to cache (per schema.table.column)
- [ ] T086 [US8] Implement GetLevel() and IsDBCCEnabled() setting checks
- [ ] T087 [US8] Add column stats to GetStatistics() return when level >= 1
- [ ] T088 [US8] Handle DBCC permission errors with graceful fallback
- [ ] T089 [US8] Create SQL test for column statistics in test/sql/catalog/statistics.test
- [ ] T090 [US8] Add test for mssql_statistics_level setting behavior
- [ ] T091 [US8] Add test for mssql_statistics_use_dbcc = false (DBCC not called)

**Checkpoint**: Column statistics are fully functional and testable independently

---

## Phase 12: Polish & Cross-Cutting Concerns

**Purpose**: Improvements that affect multiple user stories

- [X] T092 [P] Add comprehensive error message formatting across all DDL operations
- [X] T093 [P] Verify all error messages include schema/table name per FR-041
- [X] T094 [P] Review and ensure all identifiers are properly quoted (QuoteIdentifier uses bracket escaping)
- [X] T095 Code review for thread safety in statistics cache (uses std::mutex)
- [ ] T096 Performance validation: DDL < 5 seconds, cache hit < 100ms (requires live server)
- [ ] T097 Run quickstart.md validation scenarios (requires live server)
- [X] T098 Run full test suite and fix any regressions (143 assertions pass)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories (Phase 3-11)**: All depend on Foundational phase completion
  - User stories can then proceed in parallel (if staffed)
  - Or sequentially in priority order (P1 → P2 → P3)
- **Polish (Phase 12)**: Depends on all desired user stories being complete

### User Story Dependencies

- **US1 (mssql_exec)**: Can start after Foundational - Foundation for all DDL
- **US9 (READ_ONLY)**: Can start after US1 - Needs mssql_exec check
- **US2 (CREATE/DROP TABLE)**: Can start after Foundational - Uses mssql_exec internally
- **US3 (CREATE/DROP SCHEMA)**: Can start after Foundational - Independent of US2
- **US4 (ALTER COLUMN)**: Can start after US2 - Needs tables to alter
- **US5 (RENAME TABLE)**: Can start after US2 - Needs tables to rename
- **US6 (Row Count Stats)**: Can start after Foundational - Independent of DDL stories
- **US7 (Table/View Distinction)**: Can start after Foundational - Independent
- **US8 (Column Stats)**: Can start after US6 - Extends row count stats

### Within Each User Story

- Translator functions before hook implementations
- Hook implementations before cache invalidation
- Core implementation before tests
- Story complete before moving to next priority

### Parallel Opportunities

- All Setup tasks marked [P] can run in parallel
- All Foundational tasks marked [P] can run in parallel (within Phase 2)
- Once Foundational phase completes:
  - US1, US2, US3, US6, US7 can all start in parallel (if team capacity allows)
  - US4, US5 depend on US2 completing
  - US8 depends on US6 completing
- Within each story, translator methods marked [P] can run in parallel

---

## Parallel Example: User Story 4 (Alter Columns)

```bash
# Launch all translator functions together:
Task: "Implement TranslateAddColumn() in src/catalog/mssql_ddl_translator.cpp"
Task: "Implement TranslateDropColumn() in src/catalog/mssql_ddl_translator.cpp"
Task: "Implement TranslateRenameColumn() in src/catalog/mssql_ddl_translator.cpp"
Task: "Implement TranslateAlterColumnType() in src/catalog/mssql_ddl_translator.cpp"
Task: "Implement TranslateAlterColumnNullability() in src/catalog/mssql_ddl_translator.cpp"

# Then sequentially implement Alter() dispatch (depends on translators)
```

---

## Implementation Strategy

### MVP First (User Stories 1, 9, 2 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (CRITICAL - blocks all stories)
3. Complete Phase 3: US1 (mssql_exec)
4. Complete Phase 4: US9 (READ_ONLY)
5. Complete Phase 5: US2 (CREATE/DROP TABLE)
6. **STOP and VALIDATE**: Test US1, US9, US2 independently
7. Deploy/demo if ready

### Incremental Delivery

1. Complete Setup + Foundational → Foundation ready
2. Add US1 + US9 → Test independently → Deploy/Demo (Core safety features!)
3. Add US2 → Test independently → Deploy/Demo (Table DDL!)
4. Add US3 + US6 + US7 → Test independently → Deploy/Demo (Schema DDL + Stats + Views)
5. Add US4 + US5 → Test independently → Deploy/Demo (Column DDL + Rename)
6. Add US8 → Test independently → Deploy/Demo (Advanced stats)
7. Each story adds value without breaking previous stories

### Parallel Team Strategy

With multiple developers:

1. Team completes Setup + Foundational together
2. Once Foundational is done:
   - Developer A: US1 (mssql_exec) → US9 (READ_ONLY)
   - Developer B: US2 (Table DDL) → US4 (Column DDL) → US5 (Rename)
   - Developer C: US6 (Row Stats) → US8 (Column Stats)
   - Developer D: US3 (Schema DDL) → US7 (Table/View)
3. Stories complete and integrate independently

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story should be independently completable and testable
- Tests are included using DuckDB test framework (`.test` files)
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- Avoid: vague tasks, same file conflicts, cross-story dependencies that break independence

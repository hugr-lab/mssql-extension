# Tasks: Incremental Catalog Cache with TTL and Point Invalidation

**Input**: Design documents from `/specs/029-incremental-catalog-cache/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md

**Tests**: Integration tests included as they are essential for validating lazy loading behavior.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1-US9)
- Include exact file paths in descriptions

## Path Conventions

- **Source**: `src/` at repository root
- **Headers**: `src/include/` mirrors source layout
- **SQL Tests**: `test/sql/`
- **C++ Unit Tests**: `test/cpp/`

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Prepare header files with extended data structures

- [x] T001 [P] Add `CacheLoadState` enum to `src/include/catalog/mssql_metadata_cache.hpp`
- [x] T002 [P] Extend `MSSQLSchemaMetadata` struct with `tables_load_state`, `tables_last_refresh`, `load_mutex` in `src/include/catalog/mssql_metadata_cache.hpp`
- [x] T003 [P] Extend `MSSQLTableMetadata` struct with `columns_load_state`, `columns_last_refresh`, `load_mutex` in `src/include/catalog/mssql_metadata_cache.hpp`
- [x] T004 Add catalog-level `schemas_load_state_` and `schemas_last_refresh_` members to `MSSQLMetadataCache` class in `src/include/catalog/mssql_metadata_cache.hpp`
- [x] T005 Add new public methods declarations (`EnsureSchemasLoaded`, `EnsureTablesLoaded`, `EnsureColumnsLoaded`, `InvalidateSchema`, `InvalidateTable`, `InvalidateAll`, `GetSchemasState`, `GetTablesState`, `GetColumnsState`) to `src/include/catalog/mssql_metadata_cache.hpp`
- [x] T006 Update `GetSchemaNames`, `GetTableNames`, `GetTableMetadata` method signatures to accept `TdsConnection &conn` parameter in `src/include/catalog/mssql_metadata_cache.hpp`

**Checkpoint**: Header declarations complete - implementation can begin ✅

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core lazy loading infrastructure that ALL user stories depend on

**CRITICAL**: No user story implementation can begin until this phase is complete

- [x] T007 Implement `MSSQLSchemaMetadata` move constructor and move assignment operator in `src/catalog/mssql_metadata_cache.cpp`
- [x] T008 Implement `MSSQLTableMetadata` move constructor and move assignment operator in `src/catalog/mssql_metadata_cache.cpp`
- [x] T009 Implement `IsTTLExpired(time_point)` TTL helper method in `src/catalog/mssql_metadata_cache.cpp`
- [x] T010 Implement `EnsureSchemasLoaded(conn)` with double-checked locking pattern in `src/catalog/mssql_metadata_cache.cpp`
- [x] T011 Implement `EnsureTablesLoaded(conn, schema)` with double-checked locking pattern in `src/catalog/mssql_metadata_cache.cpp`
- [x] T012 Implement `EnsureColumnsLoaded(conn, schema_name, table)` with double-checked locking pattern in `src/catalog/mssql_metadata_cache.cpp`
- [x] T013 Modify `LoadSchemaList(conn)` to only load schema names (no tables) in `src/catalog/mssql_metadata_cache.cpp`
- [x] T014 Modify `LoadTableList(conn, schema)` to only load table metadata (no columns) in `src/catalog/mssql_metadata_cache.cpp`
- [x] T015 Implement `GetSchemasState()`, `GetTablesState(schema)`, `GetColumnsState(schema, table)` state query methods in `src/catalog/mssql_metadata_cache.cpp`

**Checkpoint**: Foundation ready - lazy loading methods available, user story implementation can begin ✅

---

## Phase 3: User Story 1 & 2 - Lazy Column and Schema/Table Loading (Priority: P1)

**Goal**: ATTACH completes without loading all columns; metadata loads incrementally on first access

**Independent Test**: Attach database, query single table, verify only that table's columns are loaded

### Implementation for User Stories 1 & 2

- [x] T016 [US1] Update `GetSchemaNames(conn)` to call `EnsureSchemasLoaded(conn)` in `src/catalog/mssql_metadata_cache.cpp`
- [x] T017 [US2] Update `GetTableNames(conn, schema)` to call `EnsureSchemasLoaded` then `EnsureTablesLoaded` in `src/catalog/mssql_metadata_cache.cpp`
- [x] T018 [US1] Update `GetTableMetadata(conn, schema, table)` to call all three `Ensure*Loaded` methods in `src/catalog/mssql_metadata_cache.cpp`
- [x] T019 [US1] Update `HasSchema(name)` to NOT trigger lazy loading (read cached state only) in `src/catalog/mssql_metadata_cache.cpp`
- [x] T020 [US2] Update `HasTable(schema, table)` to NOT trigger lazy loading (read cached state only) in `src/catalog/mssql_metadata_cache.cpp`
- [x] T021 [US1] Modify `MSSQLCatalog::EnsureCacheLoaded()` to only set TTL, remove eager `Refresh()` call in `src/catalog/mssql_catalog.cpp`
- [x] T022 [US1] Update `MSSQLCatalog::LookupSchema()` to acquire connection and pass to cache methods in `src/catalog/mssql_catalog.cpp`
- [x] T023 [US2] Update `MSSQLCatalog::ScanSchemas()` to acquire connection and pass to cache methods in `src/catalog/mssql_catalog.cpp`
- [x] T024 [US1] Update `MSSQLTableSet::LoadEntries()` to acquire connection and pass to `GetTableMetadata()` in `src/catalog/mssql_table_set.cpp`
- [x] T025 [US1] Update `MSSQLTableEntry` construction to handle lazy column loading in `src/catalog/mssql_table_set.cpp`

### Tests for User Stories 1 & 2

- [x] T026 [P] [US1] Create lazy loading integration test in `test/sql/catalog/lazy_loading.test`
- [x] T027 [P] [US2] Add schema-level lazy loading test cases to `test/sql/catalog/lazy_loading.test`

**Checkpoint**: Lazy loading functional - ATTACH no longer loads all columns, metadata loads on demand

---

## Phase 4: User Story 3 & 4 - TTL Expiration (Priority: P2)

**Goal**: Schema and table metadata expire independently based on per-level timestamps

**Independent Test**: Set short TTL, wait, verify only accessed level refreshes

### Implementation for User Stories 3 & 4

- [x] T028 [US3] Integrate `IsTTLExpired()` check into `EnsureSchemasLoaded()` for schema-level TTL in `src/catalog/mssql_metadata_cache.cpp`
- [x] T029 [US3] Integrate `IsTTLExpired()` check into `EnsureTablesLoaded()` for table-list TTL in `src/catalog/mssql_metadata_cache.cpp`
- [x] T030 [US4] Integrate `IsTTLExpired()` check into `EnsureColumnsLoaded()` for column-level TTL in `src/catalog/mssql_metadata_cache.cpp`
- [x] T031 [US3] Update existing `Refresh()` method to reset all `last_refresh` timestamps in `src/catalog/mssql_metadata_cache.cpp`

### Tests for User Stories 3 & 4

- [x] T032 [P] [US3] Create TTL expiration integration test in `test/sql/catalog/incremental_ttl.test`
- [x] T033 [P] [US4] Add table-level TTL test cases to `test/sql/catalog/incremental_ttl.test`

**Checkpoint**: TTL functional - stale metadata refreshes incrementally on access

---

## Phase 5: User Story 5 & 6 - Point Invalidation CREATE/DROP TABLE (Priority: P2)

**Goal**: CREATE/DROP TABLE via DuckDB invalidates only parent schema's table list

**Independent Test**: CREATE TABLE, verify new table visible without full cache refresh

### Implementation for User Stories 5 & 6

- [x] T034 [US5] Implement `InvalidateSchema(schema_name)` method in `src/catalog/mssql_metadata_cache.cpp`
- [x] T035 [US5] Update `MSSQLSchemaEntry::CreateTable()` to call `InvalidateSchemaTableSet()` for point invalidation in `src/catalog/mssql_schema_entry.cpp`
- [x] T036 [US6] Update `MSSQLSchemaEntry::DropEntry()` to call `InvalidateSchemaTableSet()` for point invalidation in `src/catalog/mssql_schema_entry.cpp`
- [x] T037 [US5] Update `MSSQLTableSet::Invalidate()` to work with new incremental cache structure in `src/catalog/mssql_table_set.cpp`

### Tests for User Stories 5 & 6

- [x] T038 [P] [US5] Add CREATE TABLE point invalidation test to `test/sql/catalog/lazy_loading.test`
- [x] T039 [P] [US6] Add DROP TABLE point invalidation test to `test/sql/catalog/lazy_loading.test`

**Checkpoint**: CREATE/DROP TABLE invalidation functional - new/dropped tables visible immediately

---

## Phase 6: User Story 7 - Point Invalidation ALTER TABLE (Priority: P3)

**Goal**: ALTER TABLE invalidates only the affected table's column metadata

**Independent Test**: ALTER TABLE ADD COLUMN, verify only that table's columns refresh

### Implementation for User Story 7

- [x] T040 [US7] Implement `InvalidateTable(schema_name, table_name)` method in `src/catalog/mssql_metadata_cache.cpp`
- [x] T041 [US7] Update `MSSQLSchemaEntry::Alter()` to call `InvalidateTable()` for AlterTableInfo in `src/catalog/mssql_schema_entry.cpp`

### Tests for User Story 7

- [x] T042 [P] [US7] Add ALTER TABLE point invalidation test to `test/sql/catalog/lazy_loading.test`

**Checkpoint**: ALTER TABLE invalidation functional - column changes visible immediately

---

## Phase 7: User Story 8 - Point Invalidation Schema DDL (Priority: P3)

**Goal**: CREATE/DROP SCHEMA invalidates only schema list, not all tables

**Independent Test**: CREATE SCHEMA, verify new schema visible without refreshing all table metadata

### Implementation for User Story 8

- [x] T043 [US8] Implement `InvalidateAll()` method for schema list invalidation in `src/catalog/mssql_metadata_cache.cpp`
- [x] T044 [US8] Update `MSSQLCatalog::CreateSchema()` to call `InvalidateAll()` in `src/catalog/mssql_catalog.cpp`
- [x] T045 [US8] Update `MSSQLCatalog::DropSchema()` to remove schema from cache and call `InvalidateAll()` in `src/catalog/mssql_catalog.cpp`

### Tests for User Story 8

- [x] T046 [P] [US8] Add CREATE/DROP SCHEMA invalidation test to `test/sql/catalog/lazy_loading.test`

**Checkpoint**: Schema DDL invalidation functional - schema changes visible immediately

---

## Phase 8: User Story 9 - Azure SQL Database Compatibility (Priority: P2)

**Goal**: All lazy loading and TTL features work correctly with Azure SQL Database

**Independent Test**: Run tests with `AZURE_SQL_TEST_DSN` environment variable

### Implementation for User Story 9

- [x] T047 [US9] Create Azure SQL test directory structure at `test/sql/azure/`
- [x] T048 [US9] Create Azure SQL lazy loading test with `require-env AZURE_SQL_TEST_DSN` in `test/sql/azure/azure_lazy_loading.test`
- [x] T049 [US9] Add Azure SQL TTL test cases to `test/sql/azure/azure_lazy_loading.test`
- [x] T050 [US9] Add Azure SQL point invalidation test cases to `test/sql/azure/azure_lazy_loading.test`

**Checkpoint**: Azure SQL compatibility validated - all features work with cloud database

---

## Phase 9: Polish & Cross-Cutting Concerns

**Purpose**: Backward compatibility, cleanup, and verification

- [x] T051 Verify `mssql_refresh_cache()` function still performs full cache refresh in `src/catalog/mssql_refresh_function.cpp` (added `RefreshCache()` method)
- [x] T052 [P] Add C++ unit tests for `CacheLoadState` transitions in `test/cpp/test_incremental_cache.cpp`
- [x] T053 [P] Add C++ unit tests for invalidation methods in `test/cpp/test_incremental_cache.cpp`
- [x] T054 Run existing catalog tests to verify backward compatibility
- [x] T055 Run full test suite with `make test-all`
- [x] T056 Verify quickstart.md scenarios work as documented (verified via lazy_loading.test and incremental_ttl.test)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories 1 & 2 (Phase 3)**: Depends on Foundational - core lazy loading
- **User Stories 3 & 4 (Phase 4)**: Depends on Phase 3 - TTL uses lazy loading infrastructure
- **User Stories 5 & 6 (Phase 5)**: Can start after Phase 3 - independent of TTL
- **User Story 7 (Phase 6)**: Depends on Phase 5 - extends invalidation pattern
- **User Story 8 (Phase 7)**: Depends on Phase 5 - extends invalidation pattern
- **User Story 9 (Phase 8)**: Can start after Phase 4 - independent testing
- **Polish (Phase 9)**: Depends on all desired user stories being complete

### User Story Dependencies

```text
Phase 2 (Foundational)
    │
    ├──► US1 & US2 (Lazy Loading) ──────────────────────────────┐
    │           │                                                │
    │           ├──► US3 & US4 (TTL Expiration) ──► US9 (Azure) │
    │           │                                                │
    │           └──► US5 & US6 (CREATE/DROP TABLE) ─────────────┤
    │                       │                                    │
    │                       ├──► US7 (ALTER TABLE) ─────────────┤
    │                       │                                    │
    │                       └──► US8 (Schema DDL) ──────────────┤
    │                                                            │
    └────────────────────────────────────────────────────────────┴──► Phase 9 (Polish)
```

### Within Each Phase

- Header changes before implementation
- Core methods before dependent methods
- Implementation before tests
- All [P] tasks within a phase can run in parallel

### Parallel Opportunities

- All Setup tasks (T001-T003) can run in parallel
- T026, T027 (tests) can run in parallel
- T032, T033 (TTL tests) can run in parallel
- T038, T039, T042, T046 (invalidation tests) can run in parallel
- T052, T053 (C++ unit tests) can run in parallel
- Azure SQL testing (Phase 8) can run in parallel with Phase 6-7

---

## Parallel Example: Phase 5 (CREATE/DROP TABLE Invalidation)

```bash
# After T034 completes, these can run in parallel:
Task T035: "Update CreateTable() to call InvalidateSchema()"
Task T036: "Update DropEntry() to call InvalidateSchema()"

# After T035, T036 complete, tests can run in parallel:
Task T038: "CREATE TABLE point invalidation test"
Task T039: "DROP TABLE point invalidation test"
```

---

## Implementation Strategy

### MVP First (User Stories 1 & 2 Only)

1. Complete Phase 1: Setup (header declarations)
2. Complete Phase 2: Foundational (lazy loading infrastructure)
3. Complete Phase 3: User Stories 1 & 2 (lazy loading)
4. **STOP and VALIDATE**: Test lazy loading independently
5. Deploy/demo if ready - ATTACH is now fast with large databases

### Incremental Delivery

1. Setup + Foundational → Infrastructure ready
2. Add US1 & US2 → Test → Deploy (MVP - lazy loading)
3. Add US3 & US4 → Test → Deploy (TTL expiration)
4. Add US5 & US6 → Test → Deploy (CREATE/DROP invalidation)
5. Add US7 & US8 → Test → Deploy (ALTER TABLE, schema DDL)
6. Add US9 → Test → Deploy (Azure SQL validation)
7. Each story adds value without breaking previous stories

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- User Stories 1 & 2 are combined (same P1 priority, tightly coupled)
- User Stories 3 & 4 are combined (same P2 priority, both TTL-related)
- User Stories 5 & 6 are combined (same P2 priority, both table DDL)
- Verify tests fail before implementing (TDD approach for test tasks)
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently

# Tasks: Catalog Integration & Read-Only SELECT with Pushdown

**Input**: Design documents from `/specs/007-catalog-integration/`
**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: This spec mentions integration tests but does not explicitly request TDD. Tests are included in the Polish phase.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

Based on plan.md structure (C++ DuckDB extension):
- Headers: `src/include/catalog/`, `src/include/pushdown/`
- Implementation: `src/catalog/`, `src/pushdown/`
- Tests: `test/cpp/`, `test/sql/`

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create directory structure and base files for new modules

- [x] T001 Create catalog directory structure: `src/include/catalog/` and `src/catalog/`
- [x] T002 Create pushdown directory structure: `src/include/pushdown/` and `src/pushdown/`
- [x] T003 [P] Create test directory structure: `test/sql/` for integration tests
- [x] T004 [P] Update CMakeLists.txt to include new source directories (catalog/, pushdown/)
- [x] T005 [P] Add `mssql_catalog_cache_ttl` setting in `src/connection/mssql_settings.cpp`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

### Core Data Structures

- [ ] T006 [P] Create MSSQLColumnInfo struct in `src/include/catalog/mssql_column_info.hpp`
- [ ] T007 [P] Implement MSSQLColumnInfo with collation parsing in `src/catalog/mssql_column_info.cpp`
- [ ] T008 [P] Create MSSQLMetadataCache class in `src/include/catalog/mssql_metadata_cache.hpp`
- [ ] T009 Implement MSSQLMetadataCache with TTL support in `src/catalog/mssql_metadata_cache.cpp`

### Catalog Base Classes

- [ ] T010 Create MSSQLCatalog class (inherits duckdb::Catalog) in `src/include/catalog/mssql_catalog.hpp`
- [ ] T011 [P] Create MSSQLSchemaEntry class in `src/include/catalog/mssql_schema_entry.hpp`
- [ ] T012 [P] Create MSSQLTableEntry class in `src/include/catalog/mssql_table_entry.hpp`
- [ ] T013 [P] Create MSSQLTableSet class for lazy loading in `src/include/catalog/mssql_table_set.hpp`

### Storage Extension Integration

- [ ] T014 Update MSSQLStorageExtension to return MSSQLCatalog in `src/mssql_storage.cpp`
- [ ] T015 Implement MSSQLCatalog::Initialize() to query database collation in `src/catalog/mssql_catalog.cpp`

**Checkpoint**: Foundation ready - catalog classes exist, storage extension wired up

---

## Phase 3: User Story 1 - Attach and Browse Schemas (Priority: P1) 🎯 MVP

**Goal**: Attach SQL Server database and list schemas via `SHOW SCHEMAS FROM catalog`

**Independent Test**: Execute `ATTACH '' AS sales (TYPE mssql, SECRET my_mssql); SHOW SCHEMAS FROM sales;`

### Implementation for User Story 1

- [ ] T016 [US1] Implement MSSQLCatalog constructor with connection pool and cache setup in `src/catalog/mssql_catalog.cpp`
- [ ] T017 [US1] Implement MSSQLCatalog::LookupSchema() with lazy loading in `src/catalog/mssql_catalog.cpp`
- [ ] T018 [US1] Implement MSSQLCatalog::ScanSchemas() for SHOW SCHEMAS in `src/catalog/mssql_catalog.cpp`
- [ ] T019 [US1] Implement schema discovery query (sys.schemas) in `src/catalog/mssql_metadata_cache.cpp`
- [ ] T020 [US1] Implement MSSQLSchemaEntry constructor and basic methods in `src/catalog/mssql_schema_entry.cpp`
- [ ] T021 [US1] Add write operation rejection (CreateSchema, DropSchema throw NotImplementedException) in `src/catalog/mssql_catalog.cpp`

**Checkpoint**: ATTACH + SHOW SCHEMAS works

---

## Phase 4: User Story 2 - List Tables and Views (Priority: P1)

**Goal**: List tables and views via `SHOW TABLES FROM catalog.schema`

**Independent Test**: Execute `SHOW TABLES FROM sales.dbo;`

### Implementation for User Story 2

- [ ] T022 [US2] Implement MSSQLSchemaEntry::Scan() for SHOW TABLES in `src/catalog/mssql_schema_entry.cpp`
- [ ] T023 [US2] Implement MSSQLSchemaEntry::LookupEntry() for table lookup in `src/catalog/mssql_schema_entry.cpp`
- [ ] T024 [US2] Implement MSSQLTableSet::LoadEntries() with sys.tables/sys.views query in `src/catalog/mssql_table_set.cpp`
- [ ] T025 [US2] Implement MSSQLTableEntry constructor with column metadata in `src/catalog/mssql_table_entry.cpp`
- [ ] T026 [US2] Add write rejection (CreateTable, DropEntry throw) in `src/catalog/mssql_schema_entry.cpp`

**Checkpoint**: SHOW TABLES works, tables and views listed

---

## Phase 5: User Story 3 - Describe Table Columns (Priority: P1)

**Goal**: Describe table columns with types, nullability, and collation via `DESCRIBE catalog.schema.table`

**Independent Test**: Execute `DESCRIBE sales.dbo.customers;`

### Implementation for User Story 3

- [ ] T027 [US3] Implement column metadata query (sys.columns, sys.types) in `src/catalog/mssql_metadata_cache.cpp`
- [ ] T028 [US3] Implement SQL Server to DuckDB type mapping in `src/catalog/mssql_column_info.cpp`
- [ ] T029 [US3] Parse collation name to extract case_sensitive, accent_sensitive, utf8 flags in `src/catalog/mssql_column_info.cpp`
- [ ] T030 [US3] Implement MSSQLTableEntry column list population in `src/catalog/mssql_table_entry.cpp`
- [ ] T031 [US3] Ensure DESCRIBE returns correct DuckDB types and nullability in `src/catalog/mssql_table_entry.cpp`

**Checkpoint**: DESCRIBE works with correct type mappings

---

## Phase 6: User Story 4 - Query via Catalog Namespace (Priority: P1)

**Goal**: Execute `SELECT * FROM catalog.schema.table` and stream results

**Independent Test**: Execute `SELECT * FROM sales.dbo.orders LIMIT 10;`

### Table Function Implementation

- [ ] T032 [P] [US4] Create MSSQLBindData struct in `src/include/catalog/mssql_bind_data.hpp`
- [ ] T033 [P] [US4] Create table function header in `src/include/catalog/mssql_table_function.hpp`
- [ ] T034 [US4] Implement MSSQLTableEntry::GetScanFunction() returning table function in `src/catalog/mssql_table_entry.cpp`
- [ ] T035 [US4] Implement MSSQLTableScanBind() populating bind data in `src/catalog/mssql_table_function.cpp`
- [ ] T036 [US4] Implement MSSQLTableScanInitGlobal() acquiring connection in `src/catalog/mssql_table_function.cpp`
- [ ] T037 [US4] Implement MSSQLTableScanInitLocal() preparing query in `src/catalog/mssql_table_function.cpp`
- [ ] T038 [US4] Implement MSSQLTableScan() streaming results via MSSQLResultStream in `src/catalog/mssql_table_function.cpp`

### Projection Pushdown

- [ ] T039 [P] [US4] Create MSSQLProjectionBuilder in `src/include/pushdown/mssql_projection_builder.hpp`
- [ ] T040 [US4] Implement projection pushdown using column_ids in `src/pushdown/mssql_projection_builder.cpp`
- [ ] T041 [US4] Implement SQL Server identifier quoting ([identifier], escape ] as ]]) in `src/pushdown/mssql_projection_builder.cpp`
- [ ] T042 [US4] Integrate projection builder into table function InitLocal in `src/catalog/mssql_table_function.cpp`

**Checkpoint**: SELECT works with projection pushdown

---

## Phase 7: User Story 5 - Filter Pushdown with Parameters (Priority: P1)

**Goal**: Push WHERE clauses to SQL Server using `sp_executesql` with bound parameters

**Independent Test**: Execute filtered query and verify via SQL Server profiler that sp_executesql is used

### Filter Translation

- [ ] T043 [P] [US5] Create MSSQLFilterPushdown class in `src/include/pushdown/mssql_filter_translator.hpp`
- [ ] T044 [P] [US5] Create MSSQLPreparedStatement class in `src/include/pushdown/mssql_prepared_statement.hpp`
- [ ] T045 [US5] Implement comparison filter translation (=, <>, <, <=, >, >=) in `src/pushdown/mssql_filter_translator.cpp`
- [ ] T046 [US5] Implement NULL check translation (IS NULL, IS NOT NULL) in `src/pushdown/mssql_filter_translator.cpp`
- [ ] T047 [US5] Implement boolean logic translation (AND, OR, NOT) in `src/pushdown/mssql_filter_translator.cpp`
- [ ] T048 [US5] Implement BETWEEN translation in `src/pushdown/mssql_filter_translator.cpp`
- [ ] T049 [US5] Implement IN list translation (up to limit) in `src/pushdown/mssql_filter_translator.cpp`
- [ ] T050 [US5] Implement LIKE translation with backslash escape in `src/pushdown/mssql_filter_translator.cpp`

### Parameterized Execution

- [ ] T051 [US5] Implement parameter generation (@p1, @p2...) in `src/pushdown/mssql_filter_translator.cpp`
- [ ] T052 [US5] Implement parameter signature generation (NVARCHAR(MAX), INT...) in `src/pushdown/mssql_prepared_statement.cpp`
- [ ] T053 [US5] Implement sp_executesql SQL generation in `src/pushdown/mssql_prepared_statement.cpp`
- [ ] T054 [US5] Integrate filter pushdown into table function InitLocal in `src/catalog/mssql_table_function.cpp`
- [ ] T055 [US5] Track unsupported filters for local evaluation in `src/pushdown/mssql_filter_translator.cpp`

**Checkpoint**: Filter pushdown works with sp_executesql

---

## Phase 8: User Story 6 - VARCHAR Collation Handling (Priority: P1)

**Goal**: Generate sargable queries with collation-aware parameter binding

**Independent Test**: Query varchar column with explicit collation, verify CONVERT...COLLATE pattern in generated SQL

### Collation-Aware Binding

- [ ] T056 [US6] Implement collation detection from column metadata in `src/pushdown/mssql_filter_translator.cpp`
- [ ] T057 [US6] Generate CONVERT(varchar(max), @pN) COLLATE <collation> for VARCHAR filters in `src/pushdown/mssql_filter_translator.cpp`
- [ ] T058 [US6] Use database default collation when column collation is NULL in `src/pushdown/mssql_filter_translator.cpp`
- [ ] T059 [US6] Detect case-insensitive collations (_CI_) for ILIKE optimization in `src/catalog/mssql_column_info.cpp`
- [ ] T060 [US6] Convert ILIKE to LIKE for CI columns, keep local for CS columns in `src/pushdown/mssql_filter_translator.cpp`
- [ ] T061 [US6] Keep collation-uncertain predicates local to avoid incorrect semantics in `src/pushdown/mssql_filter_translator.cpp`

**Checkpoint**: VARCHAR queries use sargable CONVERT...COLLATE pattern

---

## Phase 9: User Story 7 - Metadata Caching and Refresh (Priority: P2)

**Goal**: Cache metadata with TTL support and explicit refresh function

**Independent Test**: Call `CALL mssql_refresh_catalog('sales')` after schema change, verify update

### Cache Management

- [ ] T062 [US7] Implement TTL check (IsExpired) in `src/catalog/mssql_metadata_cache.cpp`
- [ ] T063 [US7] Implement automatic refresh on TTL expiration (when TTL > 0) in `src/catalog/mssql_metadata_cache.cpp`
- [ ] T064 [US7] Implement cache Invalidate() method in `src/catalog/mssql_metadata_cache.cpp`

### Refresh Function

- [ ] T065 [US7] Create mssql_refresh_catalog table function in `src/include/catalog/mssql_refresh_function.hpp`
- [ ] T066 [US7] Implement mssql_refresh_catalog() function in `src/catalog/mssql_refresh_function.cpp`
- [ ] T067 [US7] Register mssql_refresh_catalog in extension load in `src/mssql_extension.cpp`

**Checkpoint**: Cache TTL and manual refresh work

---

## Phase 10: User Story 8 - Unsupported Filter Fallback (Priority: P2)

**Goal**: Unsupported filters work correctly via local filtering

**Independent Test**: Query with complex function filter, verify correct results

### Fallback Implementation

- [ ] T068 [US8] Return unsupported filter indices from MSSQLFilterPushdown in `src/pushdown/mssql_filter_translator.cpp`
- [ ] T069 [US8] Pass remaining filters to DuckDB for local evaluation in `src/catalog/mssql_table_function.cpp`
- [ ] T070 [US8] Implement scalar function pushdown (LOWER→LOWER, UPPER→UPPER, LENGTH→LEN) in `src/pushdown/mssql_filter_translator.cpp`
- [ ] T071 [US8] Keep complex functions local (not pushed) in `src/pushdown/mssql_filter_translator.cpp`

**Checkpoint**: All queries return correct results, unsupported filters evaluated locally

---

## Phase 11: User Story 9 - Write Operations Blocked (Priority: P1)

**Goal**: Clear errors for INSERT/UPDATE/DELETE attempts

**Independent Test**: Attempt INSERT, verify "Write operations not supported" error

### Write Rejection

- [ ] T072 [US9] Implement MSSQLCatalog::PlanInsert() throwing NotImplementedException in `src/catalog/mssql_catalog.cpp`
- [ ] T073 [US9] Implement MSSQLCatalog::PlanUpdate() throwing NotImplementedException in `src/catalog/mssql_catalog.cpp`
- [ ] T074 [US9] Implement MSSQLCatalog::PlanDelete() throwing NotImplementedException in `src/catalog/mssql_catalog.cpp`
- [ ] T075 [US9] Implement MSSQLTableEntry::BindUpdateConstraints() throwing in `src/catalog/mssql_table_entry.cpp`

**Checkpoint**: Write operations fail with clear error message

---

## Phase 12: Polish & Cross-Cutting Concerns

**Purpose**: Improvements that affect multiple user stories

### Testing

- [ ] T076 [P] Create integration test for catalog discovery in `test/sql/catalog_discovery.test`
- [ ] T077 [P] Create integration test for projection pushdown in `test/sql/projection_pushdown.test`
- [ ] T078 [P] Create integration test for filter pushdown in `test/sql/filter_pushdown.test`
- [ ] T079 [P] Create integration test for collation handling in `test/sql/collation_handling.test`
- [ ] T080 [P] Create unit test for metadata cache in `test/cpp/test_metadata_cache.cpp`
- [ ] T081 [P] Create unit test for filter translation in `test/cpp/test_filter_pushdown.cpp`

### Documentation and Cleanup

- [ ] T082 [P] Add error handling for SQL Server connection failures in `src/catalog/mssql_catalog.cpp`
- [ ] T083 [P] Add logging for filter pushdown decisions in `src/pushdown/mssql_filter_translator.cpp`
- [ ] T084 Run quickstart.md validation with live SQL Server
- [ ] T085 Code review: verify all tasks align with constitution principles

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - BLOCKS all user stories
- **User Stories 1-3 (Phase 3-5)**: Catalog discovery - sequential (US1→US2→US3)
- **User Story 4 (Phase 6)**: Query capability - depends on US3 (columns needed)
- **User Stories 5-6 (Phase 7-8)**: Filter pushdown - depends on US4 (query execution)
- **User Story 7 (Phase 9)**: Caching - can proceed after US3
- **User Story 8 (Phase 10)**: Fallback - depends on US5 (filter pushdown)
- **User Story 9 (Phase 11)**: Write rejection - can proceed after Foundational
- **Polish (Phase 12)**: Depends on all user stories complete

### User Story Dependencies

```
US1 (Attach/Schemas) ──┬──▶ US2 (Tables) ──▶ US3 (Columns) ──┬──▶ US4 (Query) ──▶ US5 (Filters) ──▶ US6 (Collation) ──▶ US8 (Fallback)
                       │                                      │
                       │                                      └──▶ US7 (Caching)
                       │
                       └──▶ US9 (Write Block) [can start early]
```

### Parallel Opportunities

- **Phase 2**: T006-T007, T008, T010-T013 can run in parallel (different files)
- **Phase 6**: T032-T033, T039 can run in parallel (different files)
- **Phase 7**: T043-T044 can run in parallel (different files)
- **Phase 12**: All test tasks (T076-T081) can run in parallel

---

## Parallel Example: Phase 2 Foundational

```bash
# Launch these in parallel (different files):
Task: "Create MSSQLColumnInfo struct in src/include/catalog/mssql_column_info.hpp"
Task: "Create MSSQLMetadataCache class in src/include/catalog/mssql_metadata_cache.hpp"
Task: "Create MSSQLCatalog class in src/include/catalog/mssql_catalog.hpp"
Task: "Create MSSQLSchemaEntry class in src/include/catalog/mssql_schema_entry.hpp"
Task: "Create MSSQLTableEntry class in src/include/catalog/mssql_table_entry.hpp"
```

---

## Implementation Strategy

### MVP First (User Stories 1-4)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational (CRITICAL - blocks all stories)
3. Complete Phases 3-6: US1 → US2 → US3 → US4
4. **STOP and VALIDATE**: ATTACH + SHOW SCHEMAS + SHOW TABLES + DESCRIBE + SELECT work
5. Deploy/demo basic catalog integration

### Full Delivery

1. Complete MVP (US1-4)
2. Add US5-6: Filter pushdown with collation handling
3. Add US7: Caching with TTL
4. Add US8: Fallback for unsupported filters
5. Verify US9: Write operations blocked
6. Complete Phase 12: Tests and polish

### Incremental Testing

After each phase checkpoint:
- US1: `ATTACH '' AS sales (TYPE mssql, SECRET my_mssql); SHOW SCHEMAS FROM sales;`
- US2: `SHOW TABLES FROM sales.dbo;`
- US3: `DESCRIBE sales.dbo.customers;`
- US4: `SELECT * FROM sales.dbo.orders LIMIT 10;`
- US5: `SELECT * FROM sales.dbo.orders WHERE status = 'shipped';`
- US6: `SELECT * FROM sales.dbo.customers WHERE name = 'Smith';`
- US7: `CALL mssql_refresh_catalog('sales');`
- US8: `SELECT * FROM sales.dbo.orders WHERE UPPER(name) = 'TEST';`
- US9: `INSERT INTO sales.dbo.orders VALUES (...);` -- should fail

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story should be independently completable and testable
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- All filter pushdown must use sp_executesql with bound parameters (FR-027)
- VARCHAR columns require CONVERT...COLLATE pattern for sargability (FR-031, FR-032)

# Tasks: Fix Catalog Scan & Add Object Visibility Filters

**Input**: Design documents from `/specs/033-fix-catalog-scan/`
**Prerequisites**: plan.md (required), spec.md (required), research.md, data-model.md, contracts/

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup

**Purpose**: No project initialization needed — this is a modification to an existing codebase. Skip to foundational work.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Add the two-level name tracking infrastructure to MSSQLTableSet that all subsequent user stories depend on.

**CRITICAL**: No user story work can begin until this phase is complete.

- [x] T001 Add `known_table_names_`, `names_loaded_`, `names_mutex_` members and `EnsureNamesLoaded()` declaration to `src/include/catalog/mssql_table_set.hpp`
- [x] T002 Implement `EnsureNamesLoaded()` in `src/catalog/mssql_table_set.cpp` — double-checked locking, calls `cache.GetTableNames()` only (no column queries), populates `known_table_names_`
- [x] T003 Update `Invalidate()` in `src/catalog/mssql_table_set.cpp` — reset `names_loaded_` to false and clear `known_table_names_` alongside existing `entries_` and `attempted_tables_` cleanup

**Checkpoint**: Foundation ready — MSSQLTableSet has the name-tracking infrastructure. User story implementation can begin.

---

## Phase 3: User Story 1 — Query a Single Table in a Large Database (Priority: P1) MVP

**Goal**: A single-table query on a 65K-table database loads columns for only the queried table, not all tables.

**Independent Test**: Attach to SQL Server with `MSSQL_DEBUG=2`, run `SELECT * FROM attached.dbo.SomeTable LIMIT 1`, verify only 1 column-discovery query is issued.

### Implementation for User Story 1

- [x] T004 [US1] Rewrite `GetEntry()` in `src/catalog/mssql_table_set.cpp` — add step 3: if `names_loaded_` is true and name not in `known_table_names_`, short-circuit to nullptr without calling `LoadSingleEntry()` (avoids SQL Server round trip for nonexistent tables)
- [x] T005 [US1] Remove `EnsureLoaded()` method from `src/catalog/mssql_table_set.cpp` and `src/include/catalog/mssql_table_set.hpp` — this method triggered `LoadEntries()` which eagerly loaded all columns; it is no longer called
- [x] T006 [US1] Remove `LoadEntries()` method from `src/catalog/mssql_table_set.cpp` and `src/include/catalog/mssql_table_set.hpp` — replaced by `EnsureNamesLoaded()` for name tracking and on-demand entry creation in `Scan()`/`GetEntry()`
- [x] T007 [US1] Rewrite `Scan()` in `src/catalog/mssql_table_set.cpp` — call `EnsureNamesLoaded()` (not `EnsureLoaded()`), then iterate `known_table_names_`, create entries on-the-fly via `cache.GetTableMetadata()` per table, cache in `entries_`, mark `is_fully_loaded_` after loop
- [x] T008 [US1] Verify `IsLoaded()` in `src/catalog/mssql_table_set.cpp` still returns `is_fully_loaded_` (no change needed, but confirm no callers depend on old behavior)
- [ ] T009 [US1] Integration test: create `test/sql/catalog/deferred_columns.test` — test that a single-table SELECT only triggers 1 column query by verifying query succeeds and timing is fast; test that querying a second table triggers exactly 1 more column query

**Checkpoint**: Single-table queries on large databases now complete without loading all columns. This is the MVP.

---

## Phase 4: User Story 2 — Schema Scan Defers Column Loading (Priority: P2)

**Goal**: `SHOW ALL TABLES` and schema scan operations load only table names, not columns. Columns are loaded on-demand.

**Independent Test**: Attach to SQL Server with `MSSQL_DEBUG=2`, run `SHOW ALL TABLES`, verify no column-discovery queries are issued during the scan.

### Implementation for User Story 2

- [ ] T010 [US2] Verify `Scan()` rewrite from T007 correctly defers column loading — when `Scan()` is called, it should load names first via `EnsureNamesLoaded()`, then create entries per table. Confirm this already satisfies US2 since names are loaded without column queries. If `SHOW ALL TABLES` triggers column loading (via `duckdb_tables()` accessing columns), document the behavior.
- [ ] T011 [US2] Add integration test to `test/sql/catalog/deferred_columns.test` — test `SHOW ALL TABLES` on an attached database lists tables correctly, test that column metadata is only loaded when a specific table is subsequently queried

**Checkpoint**: Schema scans are fast. Column loading is truly on-demand.

---

## Phase 5: User Story 3 — Filter Visible Objects via Regex in Connection String (Priority: P2)

**Goal**: Users can specify `schema_filter` and `table_filter` regex patterns in ATTACH options to limit which objects are visible.

**Independent Test**: Attach with `table_filter '^Orders$'`, verify `SHOW ALL TABLES` lists only `Orders`, verify querying a non-matching table returns "table not found".

### Implementation for User Story 3

- [x] T012 [P] [US3] Create `src/include/catalog/mssql_catalog_filter.hpp` — `MSSQLCatalogFilter` class with `SetSchemaFilter()`, `SetTableFilter()`, `ValidatePattern()`, `MatchesSchema()`, `MatchesTable()`, `HasFilters()`, `GetSchemaPattern()`, `GetTablePattern()` per plan.md design
- [x] T013 [P] [US3] Implement `MSSQLCatalogFilter` in `src/catalog/mssql_catalog_filter.cpp` — compile regex with `std::regex::icase`, use `std::regex_search()` for matching, throw on invalid pattern in `SetSchemaFilter()`/`SetTableFilter()`, return `true` (match-all) when no filter is set
- [x] T014 [P] [US3] Add C++ unit tests in `test/cpp/test_catalog_filter.cpp` — test basic matching, case insensitivity, partial match with `regex_search`, anchored patterns (`^dbo$`), invalid regex throws, empty pattern matches all, multiple alternatives (`dbo|sales`)
- [x] T015 [US3] Add `schema_filter` and `table_filter` string fields to `MSSQLConnectionInfo` in `src/include/mssql_storage.hpp`
- [x] T016 [US3] Parse `schema_filter` and `table_filter` from ATTACH options in `MSSQLAttach()` in `src/mssql_storage.cpp` — extract from options map, validate with `MSSQLCatalogFilter::ValidatePattern()`, fail-fast with clear error on invalid regex
- [x] T017 [US3] Parse `SchemaFilter` and `TableFilter` from ADO.NET connection string in `ParseConnectionString()` in `src/mssql_storage.cpp` — add case-insensitive key matching for `SchemaFilter` and `TableFilter`
- [x] T018 [US3] Parse `schema_filter` and `table_filter` from URI query parameters in `ParseUri()` in `src/mssql_storage.cpp` — handle `?schema_filter=...&table_filter=...`
- [x] T019 [US3] Add `SetFilter(const MSSQLCatalogFilter *)` method to `MSSQLMetadataCache` in `src/include/catalog/mssql_metadata_cache.hpp` — store pointer to filter, add `filter_` member variable
- [x] T020 [US3] Apply schema filter in `GetSchemaNames()` in `src/catalog/mssql_metadata_cache.cpp` — skip schemas where `filter_->MatchesSchema()` returns false
- [x] T021 [US3] Apply table filter in `GetTableNames()` in `src/catalog/mssql_metadata_cache.cpp` — skip tables where `filter_->MatchesTable()` returns false
- [x] T022 [US3] Create and configure `MSSQLCatalogFilter` in `MSSQLCatalog` constructor in `src/catalog/mssql_catalog.cpp` — read filter strings from connection info, call `SetSchemaFilter()`/`SetTableFilter()`, pass filter to metadata cache via `SetFilter()`
- [x] T023 [US3] Apply table filter in `MSSQLTableSet::GetEntry()` in `src/catalog/mssql_table_set.cpp` — before `LoadSingleEntry()`, check if the requested table name matches the table filter; return nullptr if filtered out
- [ ] T024 [US3] Integration test: create `test/sql/catalog/catalog_filter.test` — test `schema_filter '^dbo$'` only shows dbo schema, test `table_filter '^Orders$'` only shows Orders table, test querying filtered-out table fails with not found, test no filters = all visible, test invalid regex fails at ATTACH

**Checkpoint**: Users can filter visible objects via ATTACH options and connection string.

---

## Phase 6: User Story 4 — Filter Visible Objects via Regex in Secret (Priority: P3)

**Goal**: Users can specify `schema_filter` and `table_filter` in MSSQL secrets for reusable filter configuration.

**Independent Test**: Create secret with `table_filter`, attach using secret, verify only matching tables visible.

### Implementation for User Story 4

- [x] T025 [US4] Parse `schema_filter` and `table_filter` from MSSQL secret in `MSSQLConnectionInfo::FromSecret()` in `src/mssql_storage.cpp` — read from `KeyValueSecret`, populate connection info fields
- [x] T026 [US4] Implement ATTACH-level filter override in `MSSQLAttach()` in `src/mssql_storage.cpp` — if ATTACH options specify filters, override the values from the secret
- [ ] T027 [US4] Integration test: add secret filter tests to `test/sql/catalog/catalog_filter.test` — test secret with `table_filter` works, test ATTACH option overrides secret filter

**Checkpoint**: Secrets support filter patterns with proper ATTACH override precedence.

---

## Phase 7: User Story 5 — Bulk Catalog Preload Function (Priority: P3)

**Goal**: `mssql_preload_catalog()` loads all metadata in a single SQL Server round trip.

**Independent Test**: Call `mssql_preload_catalog('ctx')`, verify cache is fully populated, verify subsequent queries issue no metadata queries.

### Implementation for User Story 5

- [x] T028 [P] [US5] Add `BULK_METADATA_SQL_TEMPLATE` constant and `BulkLoadAll()` declaration to `src/include/catalog/mssql_metadata_cache.hpp` — method signature: `void BulkLoadAll(tds::TdsConnection &connection, const string &schema_name = "")`
- [x] T029 [US5] Implement `BulkLoadAll()` in `src/catalog/mssql_metadata_cache.cpp` — execute bulk JOIN query from plan.md, stream results grouped by schema_name+object_name (contiguous groups due to ORDER BY), create `MSSQLSchemaMetadata` and `MSSQLTableMetadata` entries with columns populated, apply `MSSQLCatalogFilter` to skip filtered objects, mark all load states as LOADED, optionally add `AND s.name = '%s'` when schema_name is provided
- [x] T030 [P] [US5] Create `src/include/catalog/mssql_preload_catalog.hpp` — declare `MSSQLPreloadCatalogFunction` struct with static `Execute()` method
- [x] T031 [US5] Implement `MSSQLPreloadCatalogFunction::Execute()` in `src/catalog/mssql_preload_catalog.cpp` — resolve context from first VARCHAR arg, optionally extract schema_name from second arg, acquire connection from pool, call `cache.BulkLoadAll()`, return status string with schema/table/column counts
- [x] T032 [US5] Register `mssql_preload_catalog` scalar function in `src/mssql_extension.cpp` — `ScalarFunction("mssql_preload_catalog", {VARCHAR}, VARCHAR, Execute)` with `varargs = VARCHAR` for optional schema_name, add to `CreateScalarFunctions()`
- [x] T033 [US5] Ensure `MSSQLTableSet` recognizes bulk-loaded cache state — after `BulkLoadAll()`, the metadata cache is fully populated; `EnsureNamesLoaded()` and `GetEntry()` should use cached data without additional queries. Verify `names_loaded_` gets set when `cache.GetTableNames()` returns from the bulk-loaded cache.
- [ ] T034 [US5] Integration test: create `test/sql/catalog/preload_catalog.test` — test `mssql_preload_catalog('ctx')` returns status message, test subsequent `SHOW ALL TABLES` uses cache, test single-table query after preload uses cache, test preload with schema argument scopes to one schema, test preload respects regex filters

**Checkpoint**: Users can bulk-preload the entire catalog in one round trip.

---

## Phase 8: User Story 6 — Existing Single-Table Lookup Remains Efficient (Priority: P3)

**Goal**: Verify no regressions in the existing lazy single-table lookup path.

**Independent Test**: Query a fully-qualified table, verify only that table's metadata is loaded. Query again, verify cache hit.

### Implementation for User Story 6

- [ ] T035 [US6] Regression test: add to `test/sql/catalog/deferred_columns.test` — test that `SELECT * FROM attached.dbo.ExistingTable LIMIT 1` works as before, test cache hit on second query, test nonexistent table returns error without excessive metadata queries
- [ ] T036 [US6] Run all existing integration tests (`make integration-test`) and verify no failures from the deferred loading changes

**Checkpoint**: All existing behavior preserved. No regressions.

---

## Phase 9: Polish & Cross-Cutting Concerns

**Purpose**: Final validation and cleanup across all user stories.

- [x] T037 [P] Add `mssql_preload_catalog` to the Extension Functions table in `CLAUDE.md`
- [x] T038 [P] Add `schema_filter` and `table_filter` documentation to the Extension Settings table in `CLAUDE.md`
- [x] T039 Verify thread safety: review all new mutex usage in `MSSQLTableSet` for potential deadlocks (no nested lock acquisition on `names_mutex_` and `entry_mutex_` in same scope)
- [ ] T040 Build and test on all platforms: `make` (release), `make debug`, `make test` (unit tests), `make integration-test` (with SQL Server)
- [ ] T041 Run quickstart.md validation — manually test the three usage patterns: basic (no changes), with filters, with bulk preload

---

## Dependencies & Execution Order

### Phase Dependencies

- **Foundational (Phase 2)**: No dependencies — can start immediately
- **US1 (Phase 3)**: Depends on Phase 2 (T001-T003) — BLOCKS MVP
- **US2 (Phase 4)**: Depends on Phase 3 (T004-T009) — extends the Scan() rewrite
- **US3 (Phase 5)**: Depends on Phase 2 (T001-T003) — can run in parallel with US1/US2
- **US4 (Phase 6)**: Depends on Phase 5 (US3 filter infrastructure)
- **US5 (Phase 7)**: Depends on Phase 2 (T001-T003) — can run in parallel with US1-US4; benefits from US3 filters
- **US6 (Phase 8)**: Depends on Phase 3 (US1 changes are in place)
- **Polish (Phase 9)**: Depends on all desired user stories being complete

### User Story Dependencies

```text
Phase 2 (Foundation)
    │
    ├──> US1 (P1: Deferred columns) ──> US2 (P2: Scan defers) ──> US6 (P3: Regression)
    │
    ├──> US3 (P2: Regex filters) ──> US4 (P3: Secret filters)
    │
    └──> US5 (P3: Bulk preload) [enhanced by US3 filters]
```

### Within Each User Story

- Infrastructure changes (headers) before implementation (cpp)
- Core logic before integration points
- Implementation before integration tests

### Parallel Opportunities

- **T012, T013, T014** (US3) can all run in parallel — separate files, no dependencies
- **T028, T030** (US5) can run in parallel — header files, no dependencies
- **T037, T038** (Polish) can run in parallel — different sections of CLAUDE.md
- **US3 and US5** can run in parallel after Phase 2 — independent feature tracks
- **US1 and US3** can run in parallel after Phase 2 — US1 modifies mssql_table_set.cpp, US3 creates new files

---

## Parallel Example: User Story 3

```text
# Launch all independent tasks together:
T012: Create mssql_catalog_filter.hpp
T013: Implement mssql_catalog_filter.cpp
T014: C++ unit tests for MSSQLCatalogFilter

# Then sequential integration:
T015 → T016 → T017 → T018 → T019 → T020 → T021 → T022 → T023 → T024
```

## Parallel Example: User Story 5

```text
# Launch header tasks together:
T028: Add BulkLoadAll() to mssql_metadata_cache.hpp
T030: Create mssql_preload_catalog.hpp

# Then sequential:
T029 → T031 → T032 → T033 → T034
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 2: Foundational (T001-T003)
2. Complete Phase 3: User Story 1 (T004-T009)
3. **STOP and VALIDATE**: Test single-table query on large database
4. This alone fixes the reported bug

### Incremental Delivery

1. Foundation (T001-T003) → ready
2. US1 (T004-T009) → MVP: single-table queries fixed
3. US2 (T010-T011) → Schema scans verified
4. US3 (T012-T024) → Regex filters available
5. US4 (T025-T027) → Secret filters
6. US5 (T028-T034) → Bulk preload
7. US6 (T035-T036) → Regression verification
8. Polish (T037-T041) → Documentation and cross-platform validation

### Parallel Team Strategy

With multiple developers:

1. Team completes Foundation (Phase 2) together
2. Once Foundation is done:
   - Developer A: US1 → US2 → US6 (deferred loading track)
   - Developer B: US3 → US4 (filter track)
   - Developer C: US5 (bulk preload track)
3. Stories complete and integrate independently

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story is independently completable and testable
- Commit after each task or logical group
- Stop at any checkpoint to validate story independently
- `MSSQL_DEBUG=2` environment variable useful for verifying which metadata queries fire

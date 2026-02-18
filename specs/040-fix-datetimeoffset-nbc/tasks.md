# Tasks: Fix DATETIMEOFFSET in NBC Row Reader

**Input**: Design documents from `/specs/040-fix-datetimeoffset-nbc/`
**Prerequisites**: plan.md (required), spec.md (required), research.md, data-model.md, contracts/

**Tests**: Integration tests are included as they are part of the core deliverable (validating the bug fix and scale coverage).

**Organization**: Tasks are grouped by user story to enable independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- **Source**: `src/tds/` (TDS protocol layer)
- **Integration tests**: `test/sql/` (SQLLogicTest format, requires SQL Server)
- **Test fixtures**: `docker/init/init.sql` (SQL Server container initialization)

---

## Phase 1: Setup

**Purpose**: No setup needed — this is a bug fix in an existing codebase. The branch `040-fix-datetimeoffset-nbc` is already created and checked out.

---

## Phase 2: Foundational (Test Infrastructure)

**Purpose**: Add test table to docker init SQL that forces NBCROW encoding with all scale-dependent datetime types. This MUST be complete before integration tests can run.

- [x] T001 Add `NullableDatetimeScales` test table to `docker/init/init.sql` with TIME(0), TIME(3), TIME(7), DATETIME2(0), DATETIME2(3), DATETIME2(7), DATETIMEOFFSET(0), DATETIMEOFFSET(3), DATETIMEOFFSET(7) as nullable columns, plus 12 padding nullable INT columns to guarantee NBCROW encoding. Insert 5 rows: (1) all non-null with known values, (2) all datetime columns null, (3) mixed null/non-null alternating, (4) only DATETIMEOFFSET columns non-null, (5) all non-null with different timezone offsets (+05:30, -08:00, +00:00). See `specs/040-fix-datetimeoffset-nbc/data-model.md` for exact schema and test values.

**Checkpoint**: Docker container rebuild (`make docker-down && make docker-up`) succeeds and test table is queryable via SSMS or `mssql_scan`.

---

## Phase 3: User Story 1 — Fix DATETIMEOFFSET in NBCROW (Priority: P1) MVP

**Goal**: Users can query tables with DATETIMEOFFSET columns when SQL Server uses NBCROW encoding, without getting "Unsupported type in NBC RowReader: DATETIMEOFFSET" error.

**Independent Test**: ATTACH a SQL Server database, query a table with many nullable columns including DATETIMEOFFSET — values are returned correctly instead of throwing an exception.

### Implementation for User Story 1

- [x] T002 [US1] Add `case TDS_TYPE_DATETIMEOFFSET` to `ReadValueNBC()` in `src/tds/tds_row_reader.cpp` between the `TDS_TYPE_DATETIME2` case (line ~726) and `TDS_TYPE_UNIQUEIDENTIFIER` case (line ~728). Use the identical 1-byte-length-prefix pattern: read `data[0]` as `data_length`, then `value.assign(data + 1, data + 1 + data_length)`, return `1 + data_length`. See `specs/040-fix-datetimeoffset-nbc/contracts/readvalue-nbc-contract.md` for exact contract.

- [x] T003 [US1] Build the extension with `GEN=ninja make` and verify compilation succeeds with no errors or warnings in `tds_row_reader.cpp`.

- [x] T004 [US1] Run existing unit tests with `./build/release/test/unittest` to verify no regressions from the code change.

- [x] T005 [US1] Create integration test `test/sql/catalog/datetimeoffset_nbc.test` that: (a) ATTACHes the test database, (b) creates an inline test table via `mssql_exec` with 20+ nullable columns including DATETIMEOFFSET(7), DATETIMEOFFSET(3), DATETIMEOFFSET(0), (c) inserts rows with known values including NULLs, (d) queries the table and verifies DATETIMEOFFSET values are returned correctly as TIMESTAMP_TZ with proper UTC conversion, (e) verifies NULL handling, (f) cleans up the test table. Use `statement ok` / `query` / `statement error` format per existing test patterns in `test/sql/catalog/datetimeoffset.test`.

**Checkpoint**: `GEN=ninja make && ./build/release/test/unittest` passes. The DATETIMEOFFSET NBC error from issue #78 is resolved.

---

## Phase 4: User Story 2 — All Datetime Scales in ROW and NBCROW (Priority: P2)

**Goal**: All scale-dependent datetime types (TIME, DATETIME2, DATETIMEOFFSET) work correctly at scales 0, 3, 7 in both standard ROW and NBCROW encoding paths.

**Independent Test**: Query the `NullableDatetimeScales` table and verify correct values for all datetime types at all scales, including proper handling of NULLs in NBCROW.

### Implementation for User Story 2

- [x] T006 [P] [US2] Create integration test `test/sql/integration/datetimeoffset_scales.test` that: (a) ATTACHes the test database, (b) creates an inline test table via `mssql_exec` with DATETIMEOFFSET(0), DATETIMEOFFSET(3), DATETIMEOFFSET(7) columns (NOT NULL to test ROW path), (c) inserts rows with known values at each scale including positive/negative/zero offsets, (d) queries each scale column individually and verifies correct UTC conversion, (e) verifies DATETIMEOFFSET(0) returns second-level precision, DATETIMEOFFSET(3) millisecond, DATETIMEOFFSET(7) microsecond, (f) cleans up. Follow the pattern from `test/sql/integration/datetime2_scale.test`.

- [x] T007 [P] [US2] Create integration test `test/sql/integration/time_scales.test` that: (a) ATTACHes the test database, (b) creates an inline test table via `mssql_exec` with TIME(0), TIME(3), TIME(7) columns, (c) inserts rows with known time values at each scale, (d) queries each column and verifies correct values (TIME(0) = seconds, TIME(3) = millis, TIME(7) = micros), (e) cleans up. Follow the pattern from `test/sql/integration/datetime2_scale.test`.

- [x] T008 [US2] Create integration test `test/sql/integration/datetime_nbc_scales.test` that: (a) ATTACHes the test database, (b) queries the `NullableDatetimeScales` table created in T001, (c) verifies all TIME, DATETIME2, and DATETIMEOFFSET values at all scales via NBCROW path, (d) verifies NULL handling for each datetime type, (e) verifies mixed NULL/non-NULL rows. This test depends on T001 (test table) and T002 (DATETIMEOFFSET fix).

**Checkpoint**: All new scale tests pass. Existing `datetime2_scale.test` and `datetimeoffset.test` continue to pass (regression check).

---

## Phase 5: User Story 3 — Verify All Types in NBCROW (Priority: P2)

**Goal**: Confirm that all supported data types work correctly in NBCROW encoding, not just datetime types.

**Independent Test**: Query a table with all supported column types via NBCROW and verify every column returns correct values.

### Implementation for User Story 3

- [x] T009 [US3] Create integration test `test/sql/integration/nbc_all_types.test` that: (a) ATTACHes the test database, (b) creates an inline test table via `mssql_exec` with at least one column of every supported type (TINYINT, SMALLINT, INT, BIGINT, BIT, REAL, FLOAT, SMALLMONEY, MONEY, DECIMAL, NUMERIC, UNIQUEIDENTIFIER, CHAR, VARCHAR, NCHAR, NVARCHAR, BINARY, VARBINARY, DATE, TIME, DATETIME, DATETIME2, SMALLDATETIME, DATETIMEOFFSET) — all nullable plus padding columns to force NBCROW, (c) inserts: row with all non-null values, row with all null values, (d) verifies all non-null values are correct, (e) verifies all-null row returns NULLs, (f) cleans up.

**Checkpoint**: All supported types confirmed working in NBCROW. This completes the comprehensive audit from the spec.

---

## Phase 6: Polish & Validation

**Purpose**: Final build validation and integration test suite run

- [x] T010 Full build with `GEN=ninja make` — verify clean compilation
- [x] T011 Run unit tests with `./build/release/test/unittest` — verify all pass
- [x] T012 Run full integration test suite with `make integration-test` — verify all tests pass including new tests (requires `make docker-up`)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 (Setup)**: N/A — nothing to do
- **Phase 2 (Foundational)**: T001 must complete before T008 and T009 (test table needed)
- **Phase 3 (US1)**: T002 is the core fix; T003-T004 validate it; T005 is the NBC-specific integration test
- **Phase 4 (US2)**: T006 and T007 are independent (different files, no deps). T008 depends on T001 + T002
- **Phase 5 (US3)**: T009 depends on T001 + T002
- **Phase 6 (Polish)**: Depends on all previous phases

### User Story Dependencies

- **US1 (P1)**: No dependencies — can start immediately. This is the MVP.
- **US2 (P2)**: T006 and T007 can start immediately (no deps on T001 or T002). T008 depends on T001 + T002.
- **US3 (P2)**: T009 depends on T001 + T002.

### Within Each User Story

- US1: T002 (code fix) → T003 (build) → T004 (unit tests) → T005 (integration test)
- US2: T006 [P] + T007 [P] can run in parallel. T008 after T001 + T002.
- US3: T009 after T001 + T002.

### Parallel Opportunities

- T002 (US1 code fix) and T001 (test table) can run in parallel — different files
- T006 (DATETIMEOFFSET scales) and T007 (TIME scales) can run in parallel — different test files
- T005 (NBC test), T006, T007 can all run in parallel after T002 completes

---

## Parallel Example: Maximum Parallelism

```text
# Wave 1: Code fix + test infrastructure (parallel, different files)
T001: Add NullableDatetimeScales table to docker/init/init.sql
T002: Add DATETIMEOFFSET case to ReadValueNBC in src/tds/tds_row_reader.cpp

# Wave 2: Build and validate fix
T003: Build with GEN=ninja make
T004: Run unit tests

# Wave 3: All integration tests (parallel, different test files)
T005: datetimeoffset_nbc.test
T006: datetimeoffset_scales.test
T007: time_scales.test
T008: datetime_nbc_scales.test
T009: nbc_all_types.test

# Wave 4: Final validation
T010-T012: Full build + test suite
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. T002: Add the missing `case` block (5 minutes)
2. T003-T004: Build + unit tests (2 minutes)
3. T005: NBC integration test (10 minutes)
4. **STOP and VALIDATE**: The issue #78 bug is fixed

### Incremental Delivery

1. **MVP**: T002 → T003 → T004 → T005 (US1 complete — bug fix shipped)
2. **Scale coverage**: T001 + T006 + T007 + T008 (US2 complete — all scales tested)
3. **Full audit**: T009 (US3 complete — all types verified in NBC)
4. **Polish**: T010 → T011 → T012 (full validation)

---

## Notes

- T002 is the entire code fix — a single 7-line `case` block addition
- T001 requires rebuilding the Docker container (`make docker-down && make docker-up`)
- Integration tests (T005-T009) require a running SQL Server (`make docker-up`)
- The [P] marker on T006 and T007 means they edit different files and can be written simultaneously
- All test files use SQLLogicTest format — follow patterns from existing `test/sql/catalog/datetimeoffset.test` and `test/sql/integration/datetime2_scale.test`

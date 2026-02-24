# Tasks: XML Data Type Support

**Input**: Design documents from `/specs/041-xml-type-support/`
**Prerequisites**: plan.md (required), spec.md (required), research.md, data-model.md, quickstart.md

**Tests**: Integration tests are included as specified in plan.md.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3, US4)
- Include exact file paths in descriptions

## Phase 1: Setup (Test Data Infrastructure)

**Purpose**: Add XML test data to the Docker SQL Server container

- [x] T001 Add `dbo.XmlTestTable` with XML column and sample data (simple, NULL, empty element, Unicode, large, empty string) in `docker/init/init.sql`

---

## Phase 2: Foundational (Type System Support)

**Purpose**: Core type mapping and protocol support that MUST be complete before ANY user story tests can pass

**CRITICAL**: No user story work can begin until this phase is complete

- [x] T002 Map XML to VARCHAR in `GetDuckDBType()`, add XML to `IsSupported()` whitelist, and route XML through `ConvertString()` with UTF-16LE decode in `src/tds/encoding/type_converter.cpp`
- [x] T003 Add `TDS_TYPE_XML` case in `ParseTypeInfo()` — set `max_length = 0xFFFF`, no extra metadata reading in `src/tds/tds_column_metadata.cpp`
- [x] T004 Add `TDS_TYPE_XML` case in `ReadValue()`, `SkipValue()`, `ReadValueNBC()`, `SkipValueNBC()` — route through `ReadPLPType`/`SkipPLPType` in `src/tds/tds_row_reader.cpp`

**Checkpoint**: Foundation ready — XML columns can now be read from SQL Server. User story implementation can begin.

---

## Phase 3: User Story 1 — Read XML Columns (Priority: P1) MVP

**Goal**: Users can query SQL Server tables with XML columns; XML values appear as readable VARCHAR strings

**Independent Test**: SELECT from `dbo.XmlTestTable` returns all XML values correctly, including NULL, Unicode, large documents, and NBC rows

### Integration Tests for User Story 1

- [x] T005 [P] [US1] Create read test file `test/sql/xml/xml_read.test` — SELECT simple XML, NULL, empty element, Unicode, large PLP, mixed types, WHERE filter, COUNT(*), mssql_scan() (9 tests covering FR-001 through FR-004, SC-001, SC-005, SC-006)
- [x] T006 [P] [US1] Create NBC test file `test/sql/xml/xml_nbc.test` — SELECT XML from table with many nullable columns in NBC row format, NULL XML in NBC row (2 tests covering FR-005)

**Checkpoint**: XML read path fully functional and tested. Users can query tables with XML columns.

---

## Phase 4: User Story 2 & 3 — Write XML via BCP/COPY TO and CTAS (Priority: P2)

**Goal**: Users can write XML data to SQL Server via COPY TO (BCP) and CTAS; data round-trips correctly

**Independent Test**: COPY TO with XML column writes data, read-back verifies round-trip integrity for simple XML, NULL, and Unicode values

### Implementation for User Stories 2 & 3

- [x] T007 [P] [US2] Add `TDS_TYPE_XML` case in `BuildColmetadataToken()` — no additional metadata (like DATE) in `src/copy/bcp_writer.cpp`
- [x] T008 [P] [US2] Add `"xml"` mapping in `SQLServerTypeToTDSToken()` → `TDS_TYPE_XML` and `SQLServerTypeMaxLength()` → `0xFFFF` in `src/copy/target_resolver.cpp`

### Integration Tests for User Stories 2 & 3

- [x] T009 [US2] Create BCP test file `test/sql/xml/xml_copy_bcp.test` — COPY TO existing table with XML column, NULL round-trip, Unicode round-trip, read-back verification (4 tests covering FR-006, FR-007, SC-002, SC-003)

**Checkpoint**: XML write path via BCP/COPY TO and CTAS fully functional and tested. Data round-trips correctly.

---

## Phase 5: User Story 4 — DML Guards for INSERT/UPDATE (Priority: P3)

**Goal**: Users attempting INSERT with RETURNING or UPDATE on XML columns receive a clear error message recommending COPY TO with BCP

**Independent Test**: INSERT/UPDATE targeting XML columns return specific error messages; INSERT/UPDATE on non-XML columns in the same table succeed normally

### Implementation for User Story 4

- [x] T010 [P] [US4] Add XML column guard in `SerializeRow()` — check `col.mssql_type == "xml"` and throw error with column name and BCP recommendation in `src/dml/insert/mssql_batch_builder.cpp`
- [x] T011 [P] [US4] Add XML column guard in `Build()` — check `col.mssql_type == "xml"` and throw error with column name and BCP recommendation in `src/dml/update/mssql_update_statement.cpp`

### Integration Tests for User Story 4

- [x] T012 [US4] Create DML error test file `test/sql/xml/xml_dml_error.test` — INSERT with RETURNING targeting XML column errors, error mentions column name, error recommends COPY TO, UPDATE setting XML column errors, INSERT into non-XML columns succeeds, UPDATE on non-XML columns succeeds (6 tests covering FR-008 through FR-011, SC-004)

**Checkpoint**: All user stories complete. XML columns properly guarded in DML paths with actionable error messages.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Regression verification and validation

- [ ] T013 Verify existing tests pass unchanged — `test/sql/catalog/data_types.test`, `test/sql/copy/copy_types.test`, `test/sql/insert/insert_errors.test`, `test/sql/integration/max_types.test` (regression check for SC-005)
- [ ] T014 Run quickstart.md build & test validation (`GEN=ninja make && GEN=ninja make test && make docker-up && GEN=ninja make integration-test`)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — can start immediately
- **Foundational (Phase 2)**: T002 must complete first (type converter unlocks everything), then T003 and T004 can run in parallel
- **US1 Read Tests (Phase 3)**: Depends on Phase 2 completion (T002 + T003 + T004)
- **US2/US3 BCP Write (Phase 4)**: Depends on Phase 2 completion; independent of Phase 3
- **US4 DML Guards (Phase 5)**: Depends on Phase 2 completion; independent of Phase 3 and Phase 4
- **Polish (Phase 6)**: Depends on all user story phases being complete

### User Story Dependencies

- **User Story 1 (P1)**: Depends on Foundational (Phase 2) — no other story dependencies
- **User Story 2 & 3 (P2)**: Depends on Foundational (Phase 2) — independent of US1
- **User Story 4 (P3)**: Depends on Foundational (Phase 2) — independent of US1, US2, US3

### Within Each User Story

- Implementation tasks before integration tests (tests validate implementation)
- For Phase 4: T007 and T008 can run in parallel (different files), then T009 after both complete
- For Phase 5: T010 and T011 can run in parallel (different files), then T012 after both complete

### Parallel Opportunities

- T003 and T004 can run in parallel after T002 completes (different files)
- T005 and T006 can run in parallel (different test files)
- T007 and T008 can run in parallel (different source files)
- T010 and T011 can run in parallel (different source files)
- Phase 3, Phase 4, and Phase 5 can all run in parallel after Phase 2 completes

---

## Parallel Example: After Foundational Phase

```bash
# After T002 + T003 + T004 complete, launch all user story phases in parallel:

# US1 (read tests):
Task: "Create xml_read.test in test/sql/xml/xml_read.test"
Task: "Create xml_nbc.test in test/sql/xml/xml_nbc.test"

# US2/US3 (BCP write):
Task: "Add TDS_TYPE_XML in BuildColmetadataToken in src/copy/bcp_writer.cpp"
Task: "Add xml mapping in SQLServerTypeToTDSToken in src/copy/target_resolver.cpp"

# US4 (DML guards):
Task: "Add XML guard in SerializeRow in src/dml/insert/mssql_batch_builder.cpp"
Task: "Add XML guard in Build in src/dml/update/mssql_update_statement.cpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (Docker init)
2. Complete Phase 2: Foundational (type converter + metadata + row reader)
3. Complete Phase 3: User Story 1 (read tests)
4. **STOP and VALIDATE**: Run `xml_read.test` and `xml_nbc.test`
5. Users can now query tables with XML columns

### Incremental Delivery

1. Setup + Foundational → XML type recognized in protocol
2. Add User Story 1 → Test read path → XML readable (MVP!)
3. Add User Story 2 & 3 → Test BCP write → XML writable via COPY TO/CTAS
4. Add User Story 4 → Test DML guards → Clear errors for unsupported paths
5. Polish → Regression verification → Ready for merge

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story is independently completable and testable
- Commit after each phase or logical group
- Stop at any checkpoint to validate story independently
- Total: 14 tasks (1 setup + 3 foundational + 2 US1 tests + 2 US2/3 impl + 1 US2/3 test + 2 US4 impl + 1 US4 test + 2 polish)

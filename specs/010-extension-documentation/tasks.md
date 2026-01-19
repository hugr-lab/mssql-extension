# Tasks: Extension Documentation

**Input**: Design documents from `/specs/010-extension-documentation/`
**Prerequisites**: plan.md (required), spec.md (required), research.md, data-model.md, quickstart.md

**Tests**: Not applicable - documentation feature. Manual verification against SQL Server.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each documentation section.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different sections, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- **Output file**: `README.md` at repository root
- **Reference docs**: `specs/010-extension-documentation/` (research.md, data-model.md, quickstart.md)

---

## Phase 1: Setup (Documentation Structure)

**Purpose**: Create README.md skeleton with section headers

- [x] T001 Create README.md skeleton with all section headers per data-model.md hierarchy in README.md
- [x] T002 Add header section with title, one-line description, and badges in README.md

---

## Phase 2: Foundational (Overview Section)

**Purpose**: Overview section that provides context for all other sections

**CRITICAL**: This establishes the extension's purpose and feature summary

- [x] T003 Write Overview section explaining native TDS connectivity without ODBC/JDBC in README.md
- [x] T004 Add features bullet list (5-7 key capabilities) in README.md Overview section
- [x] T005 Add current status note (spec 009 - INSERT support) in README.md Overview section

**Checkpoint**: Overview complete - user story sections can now be written in parallel

---

## Phase 3: User Story 1 - Quick Start Guide (Priority: P1) MVP

**Goal**: Enable new users to install, connect, and run first query within 5 minutes

**Independent Test**: Follow Quick Start section alone and successfully execute SELECT query against SQL Server

### Implementation for User Story 1

- [x] T006 [US1] Write Prerequisites subsection (DuckDB, SQL Server requirements) in README.md Quick Start
- [x] T007 [US1] Write Installation subsection (INSTALL/LOAD commands) in README.md Quick Start
- [x] T008 [US1] Write Connect to SQL Server subsection with secret and connection string options in README.md Quick Start
- [x] T009 [US1] Write First Query subsection with SHOW SCHEMAS, SHOW TABLES, SELECT examples in README.md Quick Start
- [x] T010 [US1] Add expected output examples in README.md Quick Start
- [x] T011 [US1] Write basic Troubleshooting subsection (connection refused, login failed, TLS) in README.md Troubleshooting

**Checkpoint**: Quick Start complete - users can get started with extension

---

## Phase 4: User Story 2 - Function Reference Lookup (Priority: P1)

**Goal**: Provide complete function reference with signatures, parameters, return types, and examples

**Independent Test**: Look up any function and find complete documentation with working example

### Implementation for User Story 2

- [x] T012 [P] [US2] Document mssql_version() function (signature, return type, example) in README.md Function Reference
- [x] T013 [P] [US2] Document mssql_execute() function (signature, parameters, return table, example) in README.md Function Reference
- [x] T014 [P] [US2] Document mssql_scan() function (signature, parameters, streaming behavior, example) in README.md Function Reference
- [x] T015 [P] [US2] Document mssql_exec() scalar function (signature, parameters, return type, example) in README.md Function Reference
- [x] T016 [P] [US2] Document mssql_open() function (signature, parameters, return type, example) in README.md Function Reference
- [x] T017 [P] [US2] Document mssql_close() function (signature, parameters, return type, example) in README.md Function Reference
- [x] T018 [P] [US2] Document mssql_ping() function (signature, parameters, return type, example) in README.md Function Reference
- [x] T019 [P] [US2] Document mssql_pool_stats() function (signature, parameters, return columns, example) in README.md Function Reference

**Checkpoint**: All 8 functions documented with examples

---

## Phase 5: User Story 3 - Connection Configuration (Priority: P1)

**Goal**: Document all connection methods (secrets, connection strings, TLS)

**Independent Test**: Configure connection using either method and successfully attach

### Implementation for User Story 3

- [x] T020 [US3] Write Using Secrets subsection with CREATE SECRET syntax in README.md Connection Configuration
- [x] T021 [US3] Add secret fields table (host, port, database, user, password, use_encrypt) in README.md Connection Configuration
- [x] T022 [US3] Write ATTACH with secret syntax in README.md Connection Configuration
- [x] T023 [US3] Write Using Connection Strings subsection with ADO.NET format in README.md Connection Configuration
- [x] T024 [US3] Add ADO.NET key aliases table (Server/Data Source, Database/Initial Catalog, etc.) in README.md Connection Configuration
- [x] T025 [US3] Write URI format (mssql://) with query parameters in README.md Connection Configuration
- [x] T026 [US3] Write TLS/SSL Configuration subsection (secret vs connection string options) in README.md Connection Configuration
- [x] T027 [US3] Add TLS build requirement note (loadable extension only) in README.md Connection Configuration

**Checkpoint**: All connection methods documented

---

## Phase 6: User Story 4 - Catalog Integration Usage (Priority: P2)

**Goal**: Document schema browsing, three-part naming, and cross-catalog joins

**Independent Test**: Attach database and use SHOW SCHEMAS, SHOW TABLES, three-part naming

### Implementation for User Story 4

- [x] T028 [US4] Write ATTACH/DETACH syntax section in README.md Catalog Integration
- [x] T029 [US4] Write Schema Browsing subsection (SHOW SCHEMAS, SHOW TABLES, DESCRIBE) in README.md Catalog Integration
- [x] T030 [US4] Write Three-Part Naming subsection (context.schema.table) in README.md Catalog Integration
- [x] T031 [US4] Write Cross-Catalog Joins subsection with example joining SQL Server and local table in README.md Catalog Integration
- [x] T032 [US4] Write Query Execution section (streaming SELECT, filter/projection pushdown) in README.md

**Checkpoint**: Catalog integration documented

---

## Phase 7: User Story 5 - INSERT and DML Operations (Priority: P2)

**Goal**: Document INSERT syntax, RETURNING clause, and batch configuration

**Independent Test**: Follow INSERT examples and successfully insert data with RETURNING

### Implementation for User Story 5

- [x] T033 [US5] Write Basic INSERT subsection (single row, multiple rows) in README.md Data Modification
- [x] T034 [US5] Write INSERT from SELECT subsection in README.md Data Modification
- [x] T035 [US5] Write INSERT with RETURNING subsection (OUTPUT INSERTED behavior) in README.md Data Modification
- [x] T036 [US5] Write Batch Configuration subsection (mssql_insert_batch_size, max_sql_bytes) in README.md Data Modification
- [x] T037 [US5] Add identity column handling note in README.md Data Modification

**Checkpoint**: INSERT documentation complete

---

## Phase 8: User Story 6 - Type Mapping Reference (Priority: P2)

**Goal**: Document all SQL Server to DuckDB type mappings

**Independent Test**: Look up any SQL Server type and find DuckDB equivalent with notes

### Implementation for User Story 6

- [x] T038 [P] [US6] Write Numeric Types table (TINYINT, SMALLINT, INT, BIGINT, DECIMAL, MONEY) in README.md Type Mapping
- [x] T039 [P] [US6] Write String Types table (CHAR, VARCHAR, NCHAR, NVARCHAR) in README.md Type Mapping
- [x] T040 [P] [US6] Write Binary Types table (BINARY, VARBINARY) in README.md Type Mapping
- [x] T041 [P] [US6] Write Date/Time Types table (DATE, TIME, DATETIME, DATETIME2, DATETIMEOFFSET) in README.md Type Mapping
- [x] T042 [P] [US6] Write Special Types table (BIT, UNIQUEIDENTIFIER) in README.md Type Mapping
- [x] T043 [US6] Write Unsupported Types list (XML, UDT, SQL_VARIANT, IMAGE, TEXT, NTEXT) in README.md Type Mapping

**Checkpoint**: All 20+ type mappings documented

---

## Phase 9: User Story 9 - Configuration Reference (Priority: P2)

**Goal**: Document all DuckDB settings with defaults and ranges

**Independent Test**: Look up any setting and find purpose, default, and valid range

### Implementation for User Story 9

- [x] T044 [P] [US9] Write Connection Pool Settings table (7 settings) in README.md Configuration Reference
- [x] T045 [P] [US9] Write Statistics Settings table (4 settings) in README.md Configuration Reference
- [x] T046 [P] [US9] Write INSERT Settings table (4 settings) in README.md Configuration Reference
- [x] T047 [US9] Add usage examples for common tuning scenarios in README.md Configuration Reference

**Checkpoint**: All 15 settings documented

---

## Phase 10: User Story 7 - Building from Source (Priority: P3)

**Goal**: Document build instructions for contributors

**Independent Test**: Follow build instructions on fresh environment and produce working extension

### Implementation for User Story 7

- [x] T048 [US7] Write Prerequisites subsection (CMake, Ninja, vcpkg, C++17 compiler) in README.md Building from Source
- [x] T049 [US7] Write Build Commands subsection (make, make debug) in README.md Building from Source
- [x] T050 [US7] Write TLS Support subsection (split TLS build, loadable vs static) in README.md Building from Source
- [x] T051 [US7] Write Running Tests subsection (make test, Docker requirements) in README.md Building from Source

**Checkpoint**: Build documentation complete

---

## Phase 11: User Story 8 - IDE and Development Configuration (Priority: P3)

**Goal**: Document IDE setup for contributors

**Independent Test**: Configure VS Code or CLion with provided settings and get code completion

### Implementation for User Story 8

- [x] T052 [P] [US8] Write VS Code Configuration subsection (C++ extension, compile_commands.json) in README.md Building from Source
- [x] T053 [P] [US8] Write CLion Configuration subsection (CMake import) in README.md Building from Source
- [x] T054 [US8] Write Test Configuration subsection (unit tests, integration tests setup) in README.md Building from Source

**Checkpoint**: IDE configuration documented

---

## Phase 12: Polish & Cross-Cutting Concerns

**Purpose**: Complete troubleshooting, limitations, and final review

- [x] T055 Expand Troubleshooting section with TLS errors, type errors, performance issues in README.md
- [x] T056 Write Limitations section (unsupported types, UPDATE/DELETE, Windows auth, transactions) in README.md
- [x] T057 Add cross-reference links between sections per data-model.md requirements in README.md
- [x] T058 Review all code examples for accuracy and copy-paste readiness in README.md
- [x] T059 Validate all internal markdown links work correctly in README.md
- [ ] T060 Run quickstart.md validation against actual SQL Server

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup - BLOCKS all user stories
- **User Stories (Phase 3-11)**: All depend on Foundational phase
  - P1 stories (US1, US2, US3) can proceed in parallel after Phase 2
  - P2 stories (US4, US5, US6, US9) can proceed in parallel after Phase 2
  - P3 stories (US7, US8) can proceed in parallel after Phase 2
- **Polish (Phase 12)**: Depends on all user stories being complete

### User Story Dependencies

- **US1 (Quick Start)**: Core - provides foundation for all users
- **US2 (Function Reference)**: Independent - can parallelize with US1, US3
- **US3 (Connection Config)**: Referenced by Quick Start but can be written in parallel
- **US4 (Catalog Integration)**: References Connection Config (soft dependency)
- **US5 (INSERT/DML)**: References Catalog Integration (soft dependency)
- **US6 (Type Mapping)**: Independent - referenced by US4, US5
- **US7 (Build)**: Independent
- **US8 (IDE Config)**: Depends on US7 (same section)
- **US9 (Config Reference)**: Independent - referenced by US3, US5

### Parallel Opportunities

- **Phase 4 (US2)**: All 8 function documentation tasks (T012-T019) can run in parallel
- **Phase 8 (US6)**: All 5 type category tasks (T038-T042) can run in parallel
- **Phase 9 (US9)**: All 3 settings category tasks (T044-T046) can run in parallel
- **Phase 11 (US8)**: VS Code and CLion tasks (T052-T053) can run in parallel
- **Cross-story**: US1, US2, US3 can all start after Phase 2 completes

---

## Parallel Example: Function Reference (US2)

```bash
# Launch all function documentation tasks together:
Task: "Document mssql_version() function in README.md Function Reference"
Task: "Document mssql_execute() function in README.md Function Reference"
Task: "Document mssql_scan() function in README.md Function Reference"
Task: "Document mssql_exec() function in README.md Function Reference"
Task: "Document mssql_open() function in README.md Function Reference"
Task: "Document mssql_close() function in README.md Function Reference"
Task: "Document mssql_ping() function in README.md Function Reference"
Task: "Document mssql_pool_stats() function in README.md Function Reference"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (skeleton)
2. Complete Phase 2: Foundational (overview)
3. Complete Phase 3: User Story 1 (Quick Start + basic troubleshooting)
4. **STOP and VALIDATE**: Test Quick Start against actual SQL Server
5. Users can start using extension with minimal documentation

### Incremental Delivery

1. MVP: Quick Start (US1) → Users can get started
2. Add Function Reference (US2) → Users can look up functions
3. Add Connection Config (US3) → Users understand all connection options
4. Add Catalog Integration (US4) → Users understand schema browsing
5. Add INSERT (US5) + Type Mapping (US6) + Config (US9) → Complete usage docs
6. Add Build (US7) + IDE (US8) → Contributors can develop
7. Polish → Production-ready documentation

### Parallel Team Strategy

With multiple writers:

1. Complete Setup + Foundational together
2. Once Foundational is done:
   - Writer A: US1 (Quick Start) + US4 (Catalog)
   - Writer B: US2 (Function Reference)
   - Writer C: US3 (Connection) + US5 (INSERT)
   - Writer D: US6 (Type Mapping) + US9 (Config Reference)
3. Sections complete and integrate independently

---

## Notes

- [P] tasks = different sections, no dependencies
- [Story] label maps task to specific user story for traceability
- Each user story section is independently readable
- Use research.md for verified function signatures and settings
- Use quickstart.md as reference for Quick Start section content
- Use data-model.md for section hierarchy and content requirements
- Commit after each major section complete
- Stop at any checkpoint to validate section independently

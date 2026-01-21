# Tasks: README DDL Examples

**Input**: Design documents from `/specs/017-readme-ddl-examples/`
**Prerequisites**: spec.md (required)
**Type**: Documentation only (no code changes)

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1)
- Include exact file paths in descriptions

---

## Phase 1: Research (Understand What's Implemented)

**Purpose**: Understand existing DDL implementation from spec 008 before writing documentation

- [x] T001 Read spec 008 to understand DDL scope (what's IN vs OUT of scope) in specs/008-catalog-ddl-statistics/spec.md
- [x] T002 [P] Review DDL translator implementation in src/catalog/mssql_ddl_translator.cpp
- [x] T003 [P] Review existing README structure in README.md

**Checkpoint**: Clear understanding of implemented vs mssql_exec-only operations

---

## Phase 2: User Story 1 - DDL Documentation Section (Priority: P1)

**Goal**: Add comprehensive DDL examples to README that accurately reflect implemented functionality

**Independent Test**: User can read README and understand how to use DuckDB DDL syntax vs mssql_exec() for various operations

### Implementation for User Story 1

- [x] T004 [US1] Add "DDL Operations" section introduction in README.md (line ~320)
- [x] T005 [P] [US1] Add CREATE TABLE examples (DuckDB syntax + mssql_exec for IDENTITY/constraints) in README.md
- [x] T006 [P] [US1] Add DROP TABLE examples in README.md
- [x] T007 [P] [US1] Add ALTER TABLE examples (ADD/DROP/RENAME COLUMN) in README.md
- [x] T008 [P] [US1] Add RENAME TABLE example in README.md
- [x] T009 [P] [US1] Add CREATE/DROP SCHEMA examples in README.md
- [x] T010 [US1] Add Indexes section (mssql_exec only, explain not implemented via DDL hooks) in README.md
- [x] T011 [US1] Add note about mssql_refresh_cache() after mssql_exec DDL operations in README.md

**Checkpoint**: README has complete DDL section with accurate examples

---

## Phase 3: Finalize

**Purpose**: Commit and push documentation changes

- [x] T012 Update spec.md status to Complete in specs/017-readme-ddl-examples/spec.md
- [x] T013 Commit changes with descriptive message
- [x] T014 Push branch to remote

---

## Dependencies & Execution Order

### Phase Dependencies

- **Research (Phase 1)**: No dependencies - understand before documenting
- **User Story 1 (Phase 2)**: Depends on Research - write accurate docs
- **Finalize (Phase 3)**: Depends on all documentation being complete

### Parallel Opportunities

- T002, T003 can run in parallel (reading different source files)
- T005-T009 can run in parallel (different subsections, same file but non-overlapping)

---

## Implementation Strategy

### Single Story MVP

This feature has only one user story - add DDL documentation to README.

1. Complete Phase 1: Research
2. Complete Phase 2: Write documentation
3. Complete Phase 3: Commit and push
4. **DONE**: Create PR for review

---

## Completion Summary

| Task | Status | Description |
|------|--------|-------------|
| T001-T003 | ✅ Complete | Research phase |
| T004-T011 | ✅ Complete | Documentation written |
| T012-T014 | ✅ Complete | Committed and pushed |

**Total Tasks**: 14
**Completed**: 14
**Branch**: `017-readme-ddl-examples`
**Commit**: `d07c94d` - "docs: Add DDL operation examples to README"

---

## Notes

- This was a documentation-only feature with no code changes
- All DDL examples accurately reflect spec 008 implementation:
  - DuckDB DDL syntax: CREATE/DROP TABLE, CREATE/DROP SCHEMA, ALTER TABLE (columns), RENAME TABLE
  - mssql_exec() required: Indexes, constraints, IDENTITY, IF EXISTS
- Tasks marked [x] indicate completion (feature was implemented before task generation)

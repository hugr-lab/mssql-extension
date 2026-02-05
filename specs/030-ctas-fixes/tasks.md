# Tasks: CTAS Fixes - IF NOT EXISTS and Auto-TABLOCK

**Input**: Design documents from `/specs/030-ctas-fixes/`
**Prerequisites**: plan.md ✓, spec.md ✓, research.md ✓, data-model.md ✓, contracts/ ✓

**Tests**: Integration tests included as they are essential for validating SQL behavior.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1 = IF NOT EXISTS, US2 = Auto-TABLOCK)
- Include exact file paths in descriptions

---

## Phase 1: Setup (Shared Infrastructure) ✅

**Purpose**: Add shared data structure fields that both user stories depend on

- [x] T001 [P] Add `if_not_exists` field to CTASTarget struct in src/include/dml/ctas/mssql_ctas_types.hpp
- [x] T002 [P] Add `is_new_table` and `bcp_tablock_explicit` fields to CTASConfig in src/include/dml/ctas/mssql_ctas_config.hpp
- [x] T003 [P] Add `is_new_table` and `tablock_explicit` fields to BCPCopyConfig in src/include/copy/bcp_config.hpp

---

## Phase 2: Foundational (Blocking Prerequisites) ✅

**Purpose**: Core infrastructure changes required before user story implementation

**⚠️ CRITICAL**: User story work can begin after this phase

- [x] T004 Update CTASConfig::Load() to track explicit tablock setting in src/connection/mssql_settings.cpp
- [x] T005 Update BCPCopyConfig initialization to track explicit tablock setting in src/copy/bcp_config.cpp

**Checkpoint**: Data structures ready - user story implementation can begin

---

## Phase 3: User Story 1 - Idempotent Table Creation (Priority: P1) 🎯 MVP ✅

**Goal**: Fix `CREATE TABLE IF NOT EXISTS` to silently succeed when table exists (Issue #44)

**Independent Test**: Run `CREATE TABLE IF NOT EXISTS` twice on same table - second run should succeed with 0 rows

### Tests for User Story 1

- [x] T006 [P] [US1] Create IF NOT EXISTS integration test in test/sql/ctas/ctas_if_not_exists.test

### Implementation for User Story 1

- [x] T007 [US1] Extract if_not_exists flag in CTASPlanner::ExtractTarget() in src/dml/ctas/mssql_ctas_planner.cpp
- [x] T008 [US1] Handle IF NOT EXISTS in GetGlobalSinkState() in src/dml/ctas/mssql_physical_ctas.cpp - skip DDL and data phases when table exists
- [x] T009 [US1] Add `SKIPPED` phase to CTASPhase enum in src/include/dml/ctas/mssql_ctas_executor.hpp
- [x] T010 [US1] Handle IF NOT EXISTS in non-CTAS CreateTable() in src/catalog/mssql_schema_entry.cpp
- [x] T011 [US1] Skip catalog cache invalidation when IF NOT EXISTS skips creation in src/dml/ctas/mssql_physical_ctas.cpp

**Checkpoint**: `CREATE TABLE IF NOT EXISTS` works correctly - can test independently

---

## Phase 4: User Story 2 - Automatic TABLOCK for New Tables (Priority: P2) ✅

**Goal**: Auto-enable TABLOCK hint for BCP when creating new tables for 15-30% performance improvement (Issue #45)

**Independent Test**: Create new table via CTAS and verify TABLOCK hint is applied (via debug logging)

### Tests for User Story 2

- [x] T012 [P] [US2] Create auto-TABLOCK verification test in test/sql/ctas/ctas_auto_tablock.test and test/sql/copy/copy_auto_tablock.test

### Implementation for User Story 2

- [x] T013 [US2] Set is_new_table=true in MSSQLPhysicalCreateTableAs::GetGlobalSinkState() when table doesn't exist or OR REPLACE drops it in src/dml/ctas/mssql_physical_ctas.cpp
- [x] T014 [US2] Set is_new_table=true in ValidateTarget() when CREATE_TABLE creates a new table in src/copy/target_resolver.cpp
- [x] T015 [US2] Implement TABLOCK decision logic (explicit setting OR is_new_table) in BCP initialization in src/dml/ctas/mssql_ctas_executor.cpp
- [x] T016 [US2] Update COPY TO INSERT BULK hint logic to use auto-TABLOCK in src/copy/copy_function.cpp
- [x] T017 [US2] Add debug logging for TABLOCK decision in src/dml/ctas/mssql_ctas_executor.cpp

**Checkpoint**: Auto-TABLOCK works for both CTAS and COPY TO

---

## Phase 5: Polish & Cross-Cutting Concerns ✅

**Purpose**: Verification and cleanup

- [x] T018 Run existing CTAS tests to verify backward compatibility
- [x] T019 Run existing COPY tests to verify backward compatibility
- [x] T020 Run clang-format on all modified files
- [ ] T021 Update CLAUDE.md if new settings or behavior changes need documentation

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion
- **User Story 1 (Phase 3)**: Depends on Foundational completion
- **User Story 2 (Phase 4)**: Depends on Foundational completion (can run parallel to US1)
- **Polish (Phase 5)**: Depends on both user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational (Phase 2) - No dependencies on US2
- **User Story 2 (P2)**: Can start after Foundational (Phase 2) - No dependencies on US1

### Within Each User Story

- Test file can be written first (will fail until implementation complete)
- Header changes before source file changes
- Physical operator before executor
- All files within a story should be committed together

### Parallel Opportunities

```
Phase 1 (Setup):
  T001 ─┬─ (parallel)
  T002 ─┤
  T003 ─┘

Phase 2 (Foundational):
  T004 ─┬─ (parallel)
  T005 ─┘

Phase 3+4 (User Stories):
  US1: T006─T007─T008─T009─T010─T011 ─┬─ (can run in parallel)
  US2: T012─T013─T014─T015─T016─T017 ─┘

Phase 5 (Polish):
  T018─T019─T020─T021 (sequential verification)
```

---

## Parallel Example: Setup Phase

```bash
# Launch all Setup tasks together:
Task: "Add if_not_exists field to CTASTarget in src/include/dml/ctas/mssql_ctas_types.hpp"
Task: "Add is_new_table and bcp_tablock_explicit to CTASConfig in src/include/dml/ctas/mssql_ctas_config.hpp"
Task: "Add is_new_table and tablock_explicit to BCPCopyConfig in src/include/copy/bcp_config.hpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001-T003) ✅
2. Complete Phase 2: Foundational (T004-T005) ✅
3. Complete Phase 3: User Story 1 (T006-T011) ✅
4. **STOP and VALIDATE**: Test IF NOT EXISTS behavior ✅
5. Can deploy/merge with just Issue #44 fixed ✅

### Full Feature

1. Complete Phases 1-3 (MVP) ✅
2. Complete Phase 4: User Story 2 (T012-T017) ✅
3. Complete Phase 5: Polish (T018-T021) ✅
4. Full feature ready with both issues fixed ✅

### Incremental Delivery

- **After Phase 3**: Issue #44 (bug fix) complete - high value, can merge immediately ✅
- **After Phase 4**: Issue #45 (enhancement) complete - adds performance improvement ✅
- Each story adds value without breaking the other ✅

---

## File Change Summary

| File | US1 | US2 | Changes |
|------|-----|-----|---------|
| `src/include/dml/ctas/mssql_ctas_types.hpp` | ✓ | | Add `if_not_exists` field |
| `src/include/dml/ctas/mssql_ctas_config.hpp` | | ✓ | Add `is_new_table`, `bcp_tablock_explicit` |
| `src/include/copy/bcp_config.hpp` | | ✓ | Add `is_new_table`, `tablock_explicit` |
| `src/include/copy/target_resolver.hpp` | | ✓ | Update ValidateTarget signature |
| `src/include/dml/ctas/mssql_ctas_executor.hpp` | ✓ | | Add SKIPPED phase to enum |
| `src/connection/mssql_settings.cpp` | | ✓ | Track explicit tablock setting |
| `src/copy/bcp_config.cpp` | | ✓ | Track explicit tablock setting |
| `src/dml/ctas/mssql_ctas_planner.cpp` | ✓ | | Extract `if_not_exists` flag |
| `src/dml/ctas/mssql_physical_ctas.cpp` | ✓ | ✓ | Handle IF NOT EXISTS, set is_new_table |
| `src/dml/ctas/mssql_ctas_executor.cpp` | ✓ | ✓ | Add skipped state, TABLOCK decision |
| `src/catalog/mssql_schema_entry.cpp` | ✓ | | Handle IF NOT EXISTS for non-CTAS |
| `src/copy/target_resolver.cpp` | | ✓ | Set is_new_table on create |
| `src/copy/copy_function.cpp` | | ✓ | Auto-TABLOCK for COPY TO |
| `test/sql/ctas/ctas_if_not_exists.test` | ✓ | | NEW: IF NOT EXISTS tests |
| `test/sql/ctas/ctas_auto_tablock.test` | | ✓ | NEW: CTAS Auto-TABLOCK tests |
| `test/sql/copy/copy_auto_tablock.test` | | ✓ | NEW: COPY Auto-TABLOCK tests |
| `test/sql/catalog/ddl_if_not_exists.test` | ✓ | | NEW: DDL IF NOT EXISTS tests |

---

## Notes

- [P] tasks = different files, no dependencies
- [US1] = Issue #44 (IF NOT EXISTS bug fix)
- [US2] = Issue #45 (Auto-TABLOCK enhancement)
- Both user stories can be implemented independently
- Commit after each logical task group
- Run `make test` after each user story phase to verify no regression

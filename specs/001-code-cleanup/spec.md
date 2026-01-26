# Feature Specification: Code Cleanup and Directory Reorganization

**Feature Branch**: `001-code-cleanup`
**Created**: 2026-01-26
**Status**: Draft
**Input**: User description: "Refactoring: rename table_scan files, remove unused code, consolidate DML directories, update documentation"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - File Rename for Consistency (Priority: P1)

Rename the table_scan source files to follow consistent naming conventions. The files `mssql_table_scan.cpp` and `mssql_table_scan.hpp` should be renamed to `table_scan.cpp` and `table_scan.hpp` to match the directory name pattern used elsewhere.

**Why this priority**: Foundation for clean codebase - file names should match their containing directory pattern for discoverability and consistency.

**Independent Test**: Build and run all existing tests after rename to verify no breakage.

**Acceptance Scenarios**:

1. **Given** the files `src/table_scan/mssql_table_scan.cpp` and `src/include/table_scan/mssql_table_scan.hpp` exist, **When** renamed to `table_scan.cpp` and `table_scan.hpp`, **Then** all tests pass and build succeeds.
2. **Given** the renamed files, **When** CMakeLists.txt is updated, **Then** build system correctly references new file names.

---

### User Story 2 - Remove Unused Code (Priority: P1)

Identify and remove unused functions and fields from the codebase to reduce maintenance burden and improve code clarity.

**Why this priority**: Dead code creates confusion and maintenance overhead. Removing it improves code quality immediately.

**Independent Test**: After each removal, run full test suite to ensure no regressions.

**Acceptance Scenarios**:

1. **Given** an unused function exists in the codebase, **When** it is removed, **Then** all tests still pass.
2. **Given** an unused field exists in a class/struct, **When** it is removed, **Then** all tests still pass.
3. **Given** multiple unused items, **When** they are removed incrementally (one at a time), **Then** each removal is verified by passing tests.

---

### User Story 3 - DML Directory Consolidation (Priority: P2)

Consolidate the separate `insert`, `update`, and `delete` directories into a unified `dml` directory structure for better organization.

**Why this priority**: Improves codebase organization by grouping related DML operations together, making navigation easier.

**Independent Test**: Build and run all tests after directory restructuring to verify no breakage.

**Acceptance Scenarios**:

1. **Given** separate directories `src/insert`, `src/update`, `src/delete`, **When** moved to `src/dml/`, **Then** build succeeds and all tests pass.
2. **Given** separate include directories `src/include/insert`, `src/include/update`, `src/include/delete`, **When** moved to `src/include/dml/`, **Then** all include paths work correctly.
3. **Given** the consolidated structure, **When** CMakeLists.txt is updated, **Then** build system correctly references new paths.

---

### User Story 4 - Comment Cleanup (Priority: P3)

Remove unnecessary or outdated comments that don't add value to code understanding.

**Why this priority**: Clean comments improve readability, but this is lower priority than structural changes.

**Independent Test**: Build succeeds after comment removal (comments don't affect functionality).

**Acceptance Scenarios**:

1. **Given** commented-out code blocks, **When** they are removed, **Then** build succeeds.
2. **Given** redundant comments that state the obvious, **When** they are removed, **Then** code readability improves.

---

### User Story 5 - Documentation Update (Priority: P2)

Update documentation to reflect the new directory structure and any removed functionality.

**Why this priority**: Documentation must stay synchronized with code changes to remain useful.

**Independent Test**: Documentation accurately describes current codebase structure.

**Acceptance Scenarios**:

1. **Given** the new directory structure, **When** README.md is updated, **Then** it reflects the current project layout.
2. **Given** the new structure, **When** DEVELOPMENT.md is updated, **Then** it provides accurate build and contribution instructions.
3. **Given** the new structure, **When** docs/TESTING.md is updated, **Then** it provides accurate testing instructions.

---

### Edge Cases

- What happens if a "unused" function is actually called via template instantiation or macros?
- How to handle circular include dependencies when restructuring directories?
- What if removing a field breaks binary compatibility with external consumers?

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST rename `src/table_scan/mssql_table_scan.cpp` to `src/table_scan/table_scan.cpp`
- **FR-002**: System MUST rename `src/include/table_scan/mssql_table_scan.hpp` to `src/include/table_scan/table_scan.hpp`
- **FR-003**: Build MUST succeed after file renames with updated CMakeLists.txt
- **FR-004**: All existing tests MUST pass after each code removal
- **FR-005**: System MUST move `src/insert/`, `src/update/`, `src/delete/` contents to `src/dml/`
- **FR-006**: System MUST move `src/include/insert/`, `src/include/update/`, `src/include/delete/` contents to `src/include/dml/`
- **FR-007**: All include paths MUST be updated to reflect new directory structure
- **FR-008**: CMakeLists.txt MUST be updated to reference new file locations
- **FR-009**: README.md MUST be updated to reflect current project structure
- **FR-010**: DEVELOPMENT.md MUST be updated with accurate build instructions
- **FR-011**: docs/TESTING.md MUST be updated with accurate testing instructions

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: All 1371+ test assertions continue to pass after refactoring
- **SC-002**: Build succeeds on all supported platforms (macOS ARM64, Linux x86_64)
- **SC-003**: No new compiler warnings introduced by the refactoring
- **SC-004**: Documentation accurately reflects the new directory structure
- **SC-005**: Code coverage remains unchanged after unused code removal
- **SC-006**: Zero regressions in existing functionality

## Assumptions

- Unused code detection will be done through static analysis and compiler warnings
- The existing test suite provides sufficient coverage to detect regressions
- No external consumers depend on specific file paths (internal extension only)
- The `dml` consolidation groups logically related operations (INSERT, UPDATE, DELETE)

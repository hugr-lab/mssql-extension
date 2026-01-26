# Research: Code Cleanup and Directory Reorganization

## Design Decisions

### D1: File Renaming Strategy

**Decision**: Rename `mssql_table_scan.cpp/hpp` to `table_scan.cpp/hpp`

**Rationale**: The `mssql_` prefix is redundant since the files are already in a directory named `table_scan/`. Other directories in the codebase (catalog, connection, query) don't use this prefix pattern. Consistency improves navigation.

**Alternatives Considered**:
- Keep current names: Rejected - inconsistent with other directories
- Remove directory, keep prefixed names: Rejected - loses organizational structure

### D2: Unused Code Detection Approach

**Decision**: Use compiler warnings and manual inspection to identify unused code

**Rationale**:
- Compiler `-Wunused-function` and `-Wunused-variable` flags catch most obvious cases
- Manual inspection needed for:
  - Functions called only through templates
  - Functions referenced in macros
  - Functions intended for future use (review comments)

**Process**:
1. Enable additional warnings in build
2. Review each warning
3. Remove one item at a time
4. Run `make test` after each removal
5. Commit incrementally for easy revert if needed

**Alternatives Considered**:
- Static analysis tools (cppcheck, clang-tidy): Could add later but not needed for initial cleanup
- Code coverage analysis: Test coverage doesn't indicate production usage patterns

### D3: DML Directory Consolidation Structure

**Decision**: Create `src/dml/` and `src/include/dml/` with subdirectories for insert/update/delete

**Rationale**:
- Groups related DML operations together
- Preserves original subdirectory structure within dml/
- CMakeLists.txt can use glob patterns for dml/**/*.cpp
- Include paths remain logical: `#include "dml/insert/..."` or `#include "dml/..."` depending on what is included

**Alternatives Considered**:
- Flatten into single dml/ directory: Rejected - too many files in one directory
- Keep separate but add dml/ parent: Chosen approach
- Merge all files into one per operation type: Rejected - loses file-level organization

### D4: Include Path Update Strategy

**Decision**: Use git mv to preserve history, then update all #include directives

**Process**:
1. `git mv` source files to new locations
2. Update CMakeLists.txt with new paths
3. Update all `#include` statements in affected files
4. Build to verify all includes resolve
5. Run tests

**Rationale**: git mv preserves file history for blame and log operations.

### D5: Comment Cleanup Criteria

**Decision**: Remove comments that fall into these categories:
1. Commented-out code blocks (not documentation)
2. "TODO" comments for completed work
3. Obvious comments like `// increment counter` before `counter++`
4. Debug trace comments that are disabled

**Preserve comments that**:
1. Explain "why" not "what"
2. Document non-obvious behavior
3. Provide context for protocol/API requirements
4. Mark intentional fallthrough in switch statements

**Rationale**: Clean comments improve readability; excessive comments become noise.

## File Inventory

### Files to Rename

| Current Path | New Path |
|--------------|----------|
| `src/table_scan/mssql_table_scan.cpp` | `src/table_scan/table_scan.cpp` |
| `src/include/table_scan/mssql_table_scan.hpp` | `src/include/table_scan/table_scan.hpp` |

### Directories to Move

| Current Path | New Path |
|--------------|----------|
| `src/insert/` | `src/dml/insert/` |
| `src/update/` | `src/dml/update/` |
| `src/delete/` | `src/dml/delete/` |
| `src/include/insert/` | `src/include/dml/insert/` |
| `src/include/update/` | `src/include/dml/update/` |
| `src/include/delete/` | `src/include/dml/delete/` |

### Files Requiring Include Updates

After directory moves, these files will need `#include` path updates:
- All files in src/dml/**
- All files that include headers from insert/, update/, delete/
- CMakeLists.txt source lists and include directories

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| Breaking includes | Build after each change; compiler catches missing includes |
| Removing needed code | Run tests after each removal; git revert if tests fail |
| Circular dependencies | Careful ordering of directory moves; build between each |
| External tooling dependencies | Check IDE configurations, clang-format, etc. still work |

## Documentation Updates Required

1. **README.md**: Update Project Structure section with new dml/ directory
2. **DEVELOPMENT.md**: Update any file path references
3. **docs/TESTING.md**: Update if test locations referenced
4. **CLAUDE.md**: Will be auto-updated by agent context script

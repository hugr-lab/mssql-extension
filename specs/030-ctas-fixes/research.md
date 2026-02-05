# Research: CTAS Fixes - IF NOT EXISTS and Auto-TABLOCK

## Overview

This document captures research findings for implementing Issue #44 (IF NOT EXISTS bug) and Issue #45 (Auto-TABLOCK enhancement).

## Research Topic 1: DuckDB OnCreateConflict Handling

### Question
How does DuckDB represent the `IF NOT EXISTS` clause and where is it stored in the CreateTableInfo structure?

### Findings

**Decision**: Use `OnCreateConflict::IGNORE_ON_CONFLICT` to detect IF NOT EXISTS

**Rationale**:
- DuckDB uses the `OnCreateConflict` enum in `CreateInfo` (base class of `CreateTableInfo`)
- Three values are relevant:
  - `ERROR_ON_CONFLICT` - Default, throw error if table exists
  - `IGNORE_ON_CONFLICT` - IF NOT EXISTS, silently succeed if table exists
  - `REPLACE_ON_CONFLICT` - OR REPLACE, drop and recreate

**Code Location**: `duckdb/parser/parsed_data/create_info.hpp`

```cpp
enum class OnCreateConflict : uint8_t {
    ERROR_ON_CONFLICT,    // Default - throw error
    IGNORE_ON_CONFLICT,   // IF NOT EXISTS - silently succeed
    REPLACE_ON_CONFLICT,  // OR REPLACE - drop and recreate
    ALTER_ON_CONFLICT,    // For internal use
    ...
};
```

**Current Implementation Gap**:
In `src/dml/ctas/mssql_physical_ctas.cpp` line 47-54:
```cpp
// Current code only handles or_replace, not if_not_exists
if (gstate->state.target.or_replace) {
    // Handle OR REPLACE
} else {
    // Non-OR REPLACE: fail if table exists (FR-014)
    if (gstate->state.TableExists(context)) {
        throw InvalidInputException("CTAS failed: table '%s' already exists...");
    }
}
```

**Fix Required**: Add handling for `IGNORE_ON_CONFLICT` before the error check.

### Alternatives Considered
- Using a separate boolean flag instead of checking `on_conflict` - Rejected because it duplicates information already available in `on_conflict`

---

## Research Topic 2: TABLOCK Hint for New Tables

### Question
When is it safe to automatically apply TABLOCK, and how should we detect "new table" scenarios?

### Findings

**Decision**: Apply auto-TABLOCK when `is_new_table = true` and user hasn't explicitly set `mssql_copy_tablock = false`

**Rationale**:
1. **Safety**: TABLOCK blocks other readers/writers. For a new table (just created), there are no concurrent readers, so TABLOCK is always safe.
2. **Performance**: TABLOCK provides 15-30% improvement by:
   - Reducing lock overhead (table lock vs row locks)
   - Enabling minimal logging in simple/bulk-logged recovery
   - Allowing more parallel server-side processing

**New Table Detection Points**:

| Operation | Detection Point | is_new_table |
|-----------|----------------|--------------|
| CTAS (table doesn't exist) | After `TableExists()` check returns false | true |
| CTAS with OR REPLACE | After DROP, before CREATE | true |
| CTAS with IF NOT EXISTS (table exists) | After `TableExists()` check returns true | false |
| COPY TO with CREATE_TABLE=true | After `ValidateTarget()` creates table | true |
| COPY TO (append to existing) | After `ValidateTarget()` validates schema | false |

**User Override Logic**:
```cpp
// Pseudo-code for TABLOCK decision
bool use_tablock;
if (user_explicitly_set_tablock) {
    use_tablock = config.bcp_tablock;  // Respect user setting
} else {
    use_tablock = is_new_table;  // Auto-enable for new tables
}
```

### Alternatives Considered
- Always use TABLOCK for CTAS - Rejected because user might want to create empty tables with concurrent access planned
- Use a new setting `mssql_ctas_auto_tablock` - Rejected as overly complex; leveraging existing setting with auto-detection is simpler

---

## Research Topic 3: Code Change Locations

### Question
What files need to be modified and what is the minimal change set?

### Findings

**Issue #44 (IF NOT EXISTS) - 4 files**:

1. `src/include/dml/ctas/mssql_ctas_types.hpp`
   - Add `bool if_not_exists = false` to `CTASTarget` struct

2. `src/dml/ctas/mssql_ctas_planner.cpp`
   - In `ExtractTarget()`, set `target.if_not_exists` based on `on_conflict`

3. `src/dml/ctas/mssql_physical_ctas.cpp`
   - In `GetGlobalSinkState()`, add handling for `if_not_exists`:
     - If table exists and `if_not_exists` is true, return early with 0 rows
     - This requires a new state flag to skip data insertion

4. `src/catalog/mssql_schema_entry.cpp`
   - In `CreateTable()`, check `base_info.on_conflict` for `IGNORE_ON_CONFLICT`
   - If table exists and IF NOT EXISTS, return existing entry without error

**Issue #45 (Auto-TABLOCK) - 4 files**:

1. `src/include/dml/ctas/mssql_ctas_config.hpp`
   - Add `bool is_new_table = false` to track auto-TABLOCK eligibility
   - Add `bool user_set_tablock = false` to track explicit user setting

2. `src/dml/ctas/mssql_ctas_executor.cpp`
   - Modify BCP initialization to use `is_new_table || config.bcp_tablock`

3. `src/include/copy/bcp_config.hpp`
   - Add `bool is_new_table = false` field

4. `src/copy/copy_function.cpp`
   - Set `is_new_table = true` when `CREATE_TABLE` option creates the table
   - Modify INSERT BULK hint logic to auto-enable TABLOCK for new tables

### Data Flow

```
IF NOT EXISTS flow:
DuckDB Parser → CreateTableInfo.on_conflict = IGNORE_ON_CONFLICT
             → CTASPlanner.ExtractTarget() → CTASTarget.if_not_exists = true
             → MSSQLPhysicalCreateTableAs.GetGlobalSinkState()
               → if (table_exists && if_not_exists) → skip DDL and data phases

Auto-TABLOCK flow:
CTAS/COPY start
  → TableExists() check
  → if (!exists || or_replace) → is_new_table = true
  → CreateTable() DDL
  → BCP initialization: use_tablock = is_new_table || explicit_setting
  → INSERT BULK ... WITH (TABLOCK)
```

---

## Research Topic 4: Backward Compatibility

### Question
How do we ensure existing behavior is preserved for users not using IF NOT EXISTS?

### Findings

**Compatibility Matrix**:

| Scenario | Before Fix | After Fix |
|----------|------------|-----------|
| `CREATE TABLE ... AS SELECT` (table doesn't exist) | Creates table | Creates table (unchanged) |
| `CREATE TABLE ... AS SELECT` (table exists) | Error 2714 | Error 2714 (unchanged) |
| `CREATE TABLE IF NOT EXISTS ... AS SELECT` (table doesn't exist) | Creates table | Creates table (unchanged) |
| `CREATE TABLE IF NOT EXISTS ... AS SELECT` (table exists) | **Error 2714 (BUG)** | **Succeeds, returns 0** (FIXED) |
| `CREATE OR REPLACE TABLE ... AS SELECT` | Drops and recreates | Drops and recreates (unchanged) |
| CTAS with BCP (new table, default settings) | No TABLOCK | **With TABLOCK** (15-30% faster) |
| CTAS with BCP (existing table, append) | No TABLOCK | No TABLOCK (unchanged) |
| `SET mssql_copy_tablock = false; CREATE TABLE ...` | No TABLOCK | No TABLOCK (user respected) |

**Risk Assessment**: Low
- IF NOT EXISTS fix only affects error handling path
- Auto-TABLOCK only applies to new tables where TABLOCK is always safe
- User explicit settings always take precedence

---

## Summary of Decisions

| Topic | Decision | Rationale |
|-------|----------|-----------|
| IF NOT EXISTS detection | Use `OnCreateConflict::IGNORE_ON_CONFLICT` | Standard DuckDB pattern |
| IF NOT EXISTS behavior | Skip DDL and return 0 rows | Matches standard SQL semantics |
| Auto-TABLOCK trigger | `is_new_table = true` | New tables have no concurrent readers |
| User override | Explicit setting takes precedence | Preserve user control |
| Implementation scope | 6-8 files, ~100-150 lines changed | Minimal, focused changes |

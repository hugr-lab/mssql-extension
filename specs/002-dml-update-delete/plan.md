# Implementation Plan: DML UPDATE/DELETE using PK-based rowid

**Branch**: `002-dml-update-delete` | **Date**: 2026-01-25 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/002-dml-update-delete/spec.md`

## Summary

Implement UPDATE and DELETE operations on MSSQL tables through DuckDB by extending the existing catalog DML planning infrastructure. Operations use two-phase execution: DuckDB identifies affected rows via rowid (PK mapping from Spec 001), then the extension executes batched SQL statements using VALUES join patterns for efficient bulk operations.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB main branch (catalog API, PhysicalOperator, DataChunk), existing TDS layer (specs 001-009)
**Storage**: In-memory (no intermediate buffering per Streaming First principle)
**Testing**: DuckDB SQLLogicTest framework (`make integration-test`), requires SQL Server container
**Target Platform**: Linux (primary), macOS, Windows (MSVC/MinGW)
**Project Type**: Single project - DuckDB extension
**Performance Goals**: Batch operations process N rows in ceil(N/batch_size) SQL statements, not N statements
**Constraints**: SQL Server parameter limit ~2100; batch_size default 500 ensures safety margin
**Scale/Scope**: Support 100K+ row UPDATE/DELETE operations via batching

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Evidence |
|-----------|--------|----------|
| I. Native and Open | PASS | Uses existing TDS layer, no ODBC/JDBC |
| II. Streaming First | PASS | Batched execution, no full result buffering |
| III. Correctness over Convenience | PASS | PK-based identity only, no %%physloc%% |
| IV. Explicit State Machines | PASS | Uses existing TDS connection state machine |
| V. DuckDB-Native UX | PASS | Standard SQL UPDATE/DELETE syntax via catalog |
| VI. Incremental Delivery | PASS | Builds on completed INSERT (Spec 009) and rowid (Spec 001) |

**Row Identity Model Compliance:**
- DuckDB rowid maps to primary key values (single-column PK → scalar, composite PK → STRUCT)
- Tables without PK reject UPDATE/DELETE with clear error message
- Physical row locators (%%physloc%%) are NOT used

## Project Structure

### Documentation (this feature)

```text
specs/002-dml-update-delete/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
├── contracts/           # Phase 1 output (SQL patterns)
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
src/
├── include/
│   ├── dml/                          # NEW: Shared DML infrastructure
│   │   ├── mssql_dml_statement.hpp   # Base class for UPDATE/DELETE SQL generation
│   │   └── mssql_dml_config.hpp      # Shared DML configuration
│   ├── update/                       # NEW: UPDATE implementation
│   │   ├── mssql_physical_update.hpp # Physical operator
│   │   ├── mssql_update_executor.hpp # Batch execution orchestration
│   │   ├── mssql_update_target.hpp   # Target table metadata
│   │   └── mssql_update_statement.hpp# SQL generation
│   └── delete/                       # NEW: DELETE implementation
│       ├── mssql_physical_delete.hpp # Physical operator
│       ├── mssql_delete_executor.hpp # Batch execution orchestration
│       ├── mssql_delete_target.hpp   # Target table metadata
│       └── mssql_delete_statement.hpp# SQL generation
├── dml/                              # NEW: Shared DML implementation
│   ├── mssql_dml_statement.cpp
│   └── mssql_dml_config.cpp
├── update/                           # NEW: UPDATE implementation
│   ├── mssql_physical_update.cpp
│   ├── mssql_update_executor.cpp
│   └── mssql_update_statement.cpp
├── delete/                           # NEW: DELETE implementation
│   ├── mssql_physical_delete.cpp
│   ├── mssql_delete_executor.cpp
│   └── mssql_delete_statement.cpp
├── catalog/
│   └── mssql_catalog.cpp             # MODIFY: Implement PlanUpdate/PlanDelete
├── connection/
│   └── mssql_settings.cpp            # MODIFY: Add DML batch settings
└── insert/                           # EXISTING: Reference for patterns
    ├── mssql_value_serializer.cpp    # REUSE: Value→T-SQL conversion
    └── ...

test/
└── sql/
    └── dml/                          # NEW: DML tests
        ├── update_scalar_pk.test
        ├── update_composite_pk.test
        ├── delete_scalar_pk.test
        ├── delete_composite_pk.test
        ├── update_no_pk_error.test
        ├── delete_no_pk_error.test
        ├── update_pk_column_error.test
        └── batching.test
```

**Structure Decision**: Single project structure following existing extension layout. New `dml/`, `update/`, and `delete/` modules mirror the existing `insert/` module pattern.

## Complexity Tracking

> No constitution violations requiring justification.

## Key Implementation Details

### 1. Catalog Integration (PlanUpdate/PlanDelete)

Currently, `MSSQLCatalog::PlanUpdate` and `MSSQLCatalog::PlanDelete` throw `NotImplementedException`. Implementation will:

1. Check write access (existing `CheckWriteAccess()`)
2. Extract table entry and verify PK exists
3. Check for PK column modifications (UPDATE only) - reject if attempted
4. Build target metadata with PK info for rowid-to-PK mapping
5. Load DML config from settings
6. Create physical operator via `planner.Make<T>()`

### 2. Physical Operators (Sink Pattern)

Follow `MSSQLPhysicalInsert` pattern:
- `IsSink() = true`
- `Sink()`: Receive chunks with rowid + (for UPDATE) new values
- `Finalize()`: Flush pending batch
- `GetData()`: Return row count (or RETURNING data in future)

Key difference from INSERT: Input chunks contain rowid column identifying rows to modify.

### 3. Batched SQL Generation

**UPDATE pattern (VALUES join):**
```sql
UPDATE t
SET t.[col1] = v.[col1], t.[col2] = v.[col2]
FROM [schema].[table] AS t
JOIN (VALUES
  (@pk1_1, @v1_1, @v2_1),
  (@pk1_2, @v1_2, @v2_2)
) AS v([pk1], [col1], [col2])
ON t.[pk1] = v.[pk1]
```

**DELETE pattern (VALUES join):**
```sql
DELETE t
FROM [schema].[table] AS t
JOIN (VALUES
  (@pk1_1),
  (@pk1_2)
) AS v([pk1])
ON t.[pk1] = v.[pk1]
```

### 4. Rowid-to-PK Extraction

For scalar PK: Direct value from rowid column
For composite PK: Extract STRUCT fields in PK column order

### 5. Settings (Reuse/Extend from INSERT)

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `mssql_dml_batch_size` | INT | 500 | Max rows per batch |
| `mssql_dml_use_prepared` | BOOL | true | Use prepared statements |
| `mssql_dml_max_parameters` | INT | 2000 | Max params per statement |

### 6. Error Handling

| Scenario | Error Message |
|----------|---------------|
| No PK | "MSSQL: UPDATE/DELETE requires a primary key" |
| PK update attempt | "MSSQL: updating primary key columns is not supported" |
| Batch failure | "MSSQL UPDATE/DELETE failed: [operation] batch [N]: [SQL Server error]" |

## Reusable Components from INSERT

| Component | Location | Reuse Pattern |
|-----------|----------|---------------|
| `MSSQLValueSerializer` | `src/insert/` | Direct reuse for value→T-SQL |
| `MSSQLInsertConfig` loading | `src/catalog/` | Adapt for DML config |
| `MSSQLPhysicalInsert` | `src/insert/` | Pattern for Sink operators |
| `MSSQLInsertExecutor` | `src/insert/` | Pattern for batch execution |
| `PrimaryKeyInfo` | `src/catalog/` | Direct reuse for rowid mapping |

## Dependencies

1. **Spec 001 (pk-rowid-semantics)**: COMPLETED - provides `PrimaryKeyInfo`, `GetRowIdType()`, `HasPrimaryKey()`
2. **Spec 009 (dml-insert)**: COMPLETED - provides value serialization, config loading patterns
3. **Existing catalog infrastructure**: `MSSQLCatalog`, `MSSQLTableEntry`, `CheckWriteAccess()`
4. **TDS connection layer**: Query execution, parameter binding

---

## Constitution Check (Post-Design)

*Re-evaluated after Phase 1 design completion.*

| Principle | Status | Post-Design Evidence |
|-----------|--------|---------------------|
| I. Native and Open | PASS | Design uses existing TDS layer exclusively |
| II. Streaming First | PASS | Batched execution with bounded memory per batch |
| III. Correctness over Convenience | PASS | PK-only identity; clear errors for edge cases |
| IV. Explicit State Machines | PASS | Batch state machine: ACCEPTING → EXECUTE → FINALIZE |
| V. DuckDB-Native UX | PASS | Standard UPDATE/DELETE syntax, catalog integration |
| VI. Incremental Delivery | PASS | Extends existing INSERT/rowid infrastructure |

**Row Identity Model Verification:**
- Data model uses `PrimaryKeyInfo` from Spec 001
- STRUCT extraction for composite PK documented
- Physical locators explicitly forbidden in contracts

**All constitution gates PASS. Ready for `/speckit.tasks`.**

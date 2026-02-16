# Implementation Plan: ORDER BY Pushdown with TOP N

**Branch**: `039-order-pushdown` | **Date**: 2026-02-16 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/039-order-pushdown/spec.md`

## Summary

Push ORDER BY clauses to SQL Server via a custom `OptimizerExtension` that detects `LogicalOrder → LogicalGet` patterns in DuckDB's logical plan. Support simple column references, simple function expressions, and NULL order validation. When ORDER BY + LIMIT are both fully pushable, combine into `SELECT TOP N ... ORDER BY ...`. Feature controlled by `mssql_order_pushdown` setting (default false) and `order_pushdown` ATTACH option.

## Technical Context

**Language/Version**: C++17 (C++11-compatible for ODR on Linux)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (vcpkg), existing TDS protocol layer
**Storage**: N/A (remote SQL Server via TDS protocol)
**Testing**: DuckDB SQLLogicTest framework (integration tests), `./build/release/test/unittest`
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW)
**Project Type**: DuckDB extension (single project)
**Performance Goals**: ORDER BY on indexed columns should complete faster with pushdown vs without on 100K+ row tables
**Constraints**: Must not affect non-MSSQL queries; disabled by default; identical result ordering
**Scale/Scope**: 2 new source files, modifications to 4-5 existing files

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
| --------- | ------ | ----- |
| I. Native and Open | PASS | Uses native TDS protocol, no external drivers |
| II. Streaming First | PASS | ORDER BY pushdown doesn't change streaming behavior — results still stream row-by-row |
| III. Correctness over Convenience | PASS | Feature disabled by default; partial pushdown preserves correctness by keeping DuckDB sort; NULL ordering validated |
| IV. Explicit State Machines | PASS | No connection state changes; optimizer is stateless |
| V. DuckDB-Native UX | PASS | Transparent optimization — users write standard DuckDB SQL, pushdown happens internally |
| VI. Incremental Delivery | PASS | Delivered behind opt-in setting; can be enabled per-database |

**Post-Phase 1 Re-check**: All principles still pass. The optimizer extension is a read-only plan transformation with no side effects on connection state or streaming.

## Project Structure

### Documentation (this feature)

```text
specs/039-order-pushdown/
├── plan.md              # This file
├── spec.md              # Feature specification
├── research.md          # Phase 0 research
├── data-model.md        # Data model
├── quickstart.md        # Usage guide
└── tasks.md             # Task list (created by /speckit.tasks)
```

### Source Code (repository root)

```text
src/
├── mssql_extension.cpp                       # [MODIFY] Register OptimizerExtension
├── connection/
│   └── mssql_settings.cpp                    # [MODIFY] Add mssql_order_pushdown setting
├── mssql_storage.cpp                         # [MODIFY] Add order_pushdown ATTACH option
├── include/
│   ├── mssql_functions.hpp                   # [MODIFY] Add fields to MSSQLCatalogScanBindData
│   └── table_scan/
│       └── mssql_optimizer.hpp               # [NEW] Optimizer callback declaration
├── table_scan/
│   ├── mssql_optimizer.cpp                   # [NEW] OptimizerExtension implementation
│   └── table_scan.cpp                        # [MODIFY] Append ORDER BY / TOP N to SQL query
CMakeLists.txt                                # [MODIFY] Add new source file

test/sql/
└── catalog/
    └── order_pushdown.test                   # [NEW] Integration tests
```

**Structure Decision**: Follows existing extension layout. New optimizer file in `src/table_scan/` alongside existing scan infrastructure. Single new header in `src/include/table_scan/`.

## Key Design Decisions

### 1. OptimizerExtension (not table function callback)

DuckDB's `TableFunction` API has no `pushdown_order_by` callback. The only way to intercept ORDER BY is via `OptimizerExtension::optimize_function` which receives the full logical plan after built-in optimizers run.

### 2. Post-built-in optimizer timing

Use `optimize_function` (runs after built-in optimizers), not `pre_optimize_function`. DuckDB's TOP_N optimizer may merge `LogicalOrder + LogicalLimit → LogicalTop` — we need to see the final plan shape.

### 3. Setting + ATTACH option precedence

ATTACH option > global setting. If ATTACH option not specified, fall back to global setting. Default: disabled.

### 4. NULL ordering strategy

SQL Server defaults differ from DuckDB defaults:
- SQL Server ASC: NULLs first | DuckDB ASC: NULLs last
- SQL Server DESC: NULLs last | DuckDB DESC: NULLs first

Strategy: Skip pushdown for columns where NULL ordering doesn't match SQL Server behavior, UNLESS the column is NOT NULL (no difference).

### 5. Partial pushdown benefit

Even when not all ORDER BY columns can be pushed, the pushable prefix is still sent to SQL Server. Pre-sorted input benefits DuckDB's merge sort. DuckDB's `LogicalOrder` is kept in the plan for final correctness.

## File Change Summary

| File | Change | Description |
| ---- | ------ | ----------- |
| `src/connection/mssql_settings.cpp` | Modify | Add `mssql_order_pushdown` boolean setting (default false) |
| `src/mssql_storage.cpp` | Modify | Parse `order_pushdown` ATTACH option, store in catalog |
| `src/include/mssql_functions.hpp` | Modify | Add `order_by_clause` and `top_n` fields to `MSSQLCatalogScanBindData` |
| `src/include/table_scan/mssql_optimizer.hpp` | New | `MSSQLOptimizer` class with `Optimize()` static method |
| `src/table_scan/mssql_optimizer.cpp` | New | Pattern detection, expression resolution, NULL validation, plan modification |
| `src/table_scan/table_scan.cpp` | Modify | Append ORDER BY clause and TOP N to generated SQL |
| `src/mssql_extension.cpp` | Modify | Register `OptimizerExtension` in `LoadInternal()` |
| `CMakeLists.txt` | Modify | Add `src/table_scan/mssql_optimizer.cpp` to source list |
| `test/sql/catalog/order_pushdown.test` | New | Integration tests for all pushdown scenarios |

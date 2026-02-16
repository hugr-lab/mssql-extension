# Research: ORDER BY Pushdown

**Branch**: `039-order-pushdown`
**Date**: 2026-02-16

## R1: Pushdown Mechanism — OptimizerExtension vs Table Function Callback

**Decision**: Use `OptimizerExtension` (post-built-in optimizer hook).

**Rationale**: DuckDB's `TableFunction` API has no `pushdown_order_by` callback. The only callbacks are `filter_pushdown`, `pushdown_complex_filter`, `projection_pushdown`, and `sampling_pushdown`. To intercept and rewrite ORDER BY in the logical plan, we must register a custom optimizer extension that runs after DuckDB's built-in optimizers.

**Alternatives considered**:
- Table function callback: Does not exist for ORDER BY in DuckDB API.
- Pre-optimizer hook (`pre_optimize_function`): Rejected because DuckDB's TOP_N optimizer runs during built-in optimization and may merge `LogicalOrder + LogicalLimit → LogicalTop`. We need to see the final plan shape.

**Reference**: postgres_scanner uses the same `OptimizerExtension` mechanism for LIMIT pushdown (`src/storage/postgres_optimizer.cpp`). It is the only DuckDB extension using this API.

## R2: Settings Registration Pattern

**Decision**: Register `mssql_order_pushdown` as a boolean setting in `src/connection/mssql_settings.cpp`.

**Rationale**: Follows the existing pattern used by all other MSSQL settings (connection_limit, idle_timeout, etc.). Settings use `config.AddExtensionOption()` with `SetScope::GLOBAL`.

**Pattern** (from `mssql_settings.cpp`):
```cpp
config.AddExtensionOption("mssql_order_pushdown",
    "Enable ORDER BY pushdown to SQL Server (default: false)",
    LogicalType::BOOLEAN, Value::BOOLEAN(false), nullptr, SetScope::GLOBAL);
```

**Loading**: Use `context.TryGetCurrentSetting("mssql_order_pushdown", val)` in the optimizer callback.

## R3: ATTACH Option Parsing Pattern

**Decision**: Add `order_pushdown` ATTACH option in `src/mssql_storage.cpp` `MSSQLAttach()`.

**Rationale**: Follows the existing pattern used by `schema_filter` and `table_filter`. Options are parsed in the for-loop (lines 818-845), extracted, and erased from the options map.

**Storage**: The effective value (ATTACH option > global setting) will be stored in the catalog or bind data, accessible to the optimizer callback.

## R4: Bind Data Extension

**Decision**: Add ORDER BY and TOP N fields to `MSSQLCatalogScanBindData` in `src/include/mssql_functions.hpp`.

**Rationale**: `MSSQLCatalogScanBindData` is the data bridge between optimizer/planner and table scan execution. The existing `complex_filter_where_clause` field demonstrates this exact pattern — the optimizer stores pushdown info, and `TableScanInitGlobal` reads it to build the SQL query.

**New fields**:
- `string order_by_clause` — T-SQL ORDER BY fragment (e.g., `[col1] ASC, YEAR([col2]) DESC`)
- `int64_t top_n` — TOP N value when ORDER BY + LIMIT are both pushed (0 = no TOP)

## R5: SQL Generation Integration Point

**Decision**: Append ORDER BY clause after WHERE clause in `src/table_scan/table_scan.cpp` query assembly (around line 350).

**Rationale**: The query is built incrementally: `SELECT columns FROM table [WHERE ...] [ORDER BY ...]`. The ORDER BY clause is appended last (before execution), exactly like the WHERE clause is today.

**For TOP N**: Modify the SELECT to `SELECT TOP N columns FROM table WHERE ... ORDER BY ...`.

## R6: Function Mapping for ORDER BY Expressions

**Decision**: Reuse existing `function_mapping.cpp` for translating ORDER BY function expressions.

**Rationale**: The function mapping table already contains entries for `year`, `month`, `day`, `upper`, `lower`, `date_diff`, etc. The `GetFunctionMapping()` API returns the T-SQL template for a DuckDB function name.

**No new mappings needed**: The existing set covers the simple functions we plan to support in ORDER BY.

## R7: Column Nullability for NULL Order Validation

**Decision**: Use `MSSQLColumnInfo.is_nullable` from `src/include/catalog/mssql_column_info.hpp`.

**Rationale**: The metadata cache already stores nullability per column. When `is_nullable == false`, NULL ordering differences between DuckDB and SQL Server are irrelevant (no NULLs exist), so pushdown is always safe for that column.

**Access path**: `bind_data.mssql_columns[i].is_nullable` — the `mssql_columns` vector is populated during bind and carries full column metadata.

## R8: Plan Node Types

**Decision**: Handle `LOGICAL_ORDER_BY`, `LOGICAL_LIMIT`, and `LOGICAL_TOP` node types.

**Rationale**:
- `LOGICAL_ORDER_BY` → `LogicalOrder`: Standard ORDER BY node with `orders` vector
- `LOGICAL_LIMIT` → `LogicalLimit`: LIMIT node with `limit_val`/`offset_val` (only relevant when above `LogicalOrder`)
- `LOGICAL_TOP` → `LogicalTop`: Merged ORDER BY + LIMIT (DuckDB's TOP_N optimizer may produce this)

DuckDB's TOP_N optimizer runs before extension optimizers (lines 274-277 in `optimizer.cpp`), so by the time our callback runs, ORDER BY + LIMIT may already be merged into `LogicalTop`.

## R9: Existing Extension Architecture — No OptimizerExtension Yet

**Decision**: This will be the first `OptimizerExtension` registered by the MSSQL extension.

**Rationale**: Currently the extension only uses `pushdown_complex_filter` (table function callback) for filter pushdown. Adding `OptimizerExtension` is a new capability. Registration happens in `LoadInternal()` in `mssql_extension.cpp`.

**Pattern** (from postgres_scanner):
```cpp
OptimizerExtension mssql_optimizer;
mssql_optimizer.optimize_function = MSSQLOptimizer::Optimize;
OptimizerExtension::Register(config, std::move(mssql_optimizer));
```

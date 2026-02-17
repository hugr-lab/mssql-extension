# ORDER BY Pushdown (with TOP N)

## Problem

When a user writes `SELECT * FROM mssql_db.dbo.MyTable ORDER BY col1`, DuckDB fetches all rows unsorted from SQL Server and sorts them locally. For large tables this is inefficient — SQL Server can leverage its indexes to return pre-sorted data.

**Note on standalone LIMIT:** Standalone `LIMIT` without `ORDER BY` is already handled well enough by the existing ATTENTION packet mechanism — DuckDB stops pulling rows and cancels the SQL Server query mid-stream. TOP N pushdown (ORDER BY + LIMIT) is valuable because SQL Server can optimize its execution plan knowing both the sort order and row count upfront.

## Goals

- Push ORDER BY clauses to SQL Server when possible
- Improve DuckDB sort performance via pre-sorted input even when full pushdown isn't possible
- Push TOP N when ORDER BY + LIMIT are both fully pushable

## Design Decisions

### Configuration

ORDER BY pushdown is controlled by a DuckDB setting and an ATTACH parameter:

| Control | Name | Default | Description |
| ------- | ---- | ------- | ----------- |
| Setting | `mssql_order_pushdown` | `false` | Global default for all attached MSSQL databases |
| ATTACH option | `order_pushdown` | (uses setting) | Per-database override, takes precedence over setting |

```sql
-- Enable globally
SET mssql_order_pushdown = true;

-- Or per-database via ATTACH
ATTACH 'Server=...' AS db (TYPE mssql, order_pushdown true);

-- Per-database override: disable even when setting is true
ATTACH 'Server=...' AS db (TYPE mssql, order_pushdown false);
```

The optimizer checks the effective value (ATTACH option > setting) before attempting any pushdown. When disabled, the optimizer callback is a no-op for that database.

### What Gets Pushed Down

- **Simple column references**: `ORDER BY col1 ASC, col2 DESC`
- **Simple function expressions**: `ORDER BY YEAR(date_col)` — small set of known DuckDB→T-SQL function mappings (reuse existing `function_mapping.cpp`)
- **Default NULL ordering only**: SQL Server sorts NULLs first in ASC, last in DESC. If the user's NULL order matches these defaults, push down. If not (explicit `NULLS LAST` on ASC), skip that column.

### What Does NOT Get Pushed Down

- Complex expressions, nested functions, casts
- Non-default NULL ordering (T-SQL has no `NULLS FIRST/LAST` syntax)
- Expressions referencing columns not in the table scan

### Plan Modification Rules

#### ORDER BY

| Scenario | SQL Server | DuckDB Plan |
|----------|-----------|-------------|
| Full pushdown (all ORDER BY columns handled) | `ORDER BY col1 ASC, col2 DESC` | Remove `LogicalOrder` |
| Partial pushdown (some columns can't be pushed) | `ORDER BY col1 ASC` (pushable subset) | Keep `LogicalOrder` (pre-sorted data = faster DuckDB sort) |
| Full pushdown + LIMIT | `SELECT TOP N ... ORDER BY ...` | Remove `LogicalOrder` + `LogicalLimit` |
| Partial pushdown + LIMIT | `ORDER BY ...` (pushable subset) | Keep both operators |

### Pattern Detection

Only handle the simple direct pattern — no recursive tree walk:

**ORDER BY pushdown:**
```
LogicalOrder
  └── LogicalGet (function: "mssql_catalog_scan")
```

**TOP N pushdown (ORDER BY + LIMIT combined):**
```
LogicalLimit
  └── LogicalOrder
        └── LogicalGet (function: "mssql_catalog_scan")
```

Or `LogicalTop` if DuckDB merges them.

## Research: DuckDB API

### OptimizerExtension API

File: `duckdb/src/include/duckdb/optimizer/optimizer_extension.hpp`

```cpp
class OptimizerExtension {
public:
    optimize_function_t optimize_function = nullptr;        // Runs after built-in optimizers
    pre_optimize_function_t pre_optimize_function = nullptr; // Runs before built-in optimizers
    shared_ptr<OptimizerExtensionInfo> optimizer_info;

    static void Register(DBConfig &config, OptimizerExtension extension);
};

// Callback signature:
typedef void (*optimize_function_t)(OptimizerExtensionInput &input,
                                    unique_ptr<LogicalOperator> &plan);
```

Registration: call `OptimizerExtension::Register(config, ext)` in extension's `LoadInternal()`.

Extension optimizer hooks execute at two points in `optimizer.cpp`:
- `pre_optimize_function` — before built-in optimizers (line 329-336)
- `optimize_function` — after built-in optimizers (line 340-347)

We should use `optimize_function` (post-built-in) since DuckDB's TOP_N optimizer (line 274-277) runs first and may merge ORDER BY + LIMIT into LogicalTop.

### LogicalOrder Structure

File: `duckdb/src/include/duckdb/planner/operator/logical_order.hpp`

```cpp
class LogicalOrder : public LogicalOperator {
public:
    vector<BoundOrderByNode> orders;    // The ORDER BY clauses
    vector<idx_t> projection_map;
    vector<unique_ptr<LogicalOperator>> children;  // Usually 1 child
};
```

### BoundOrderByNode

File: `duckdb/src/include/duckdb/planner/bound_result_modifier.hpp` (lines 46-68)

```cpp
struct BoundOrderByNode {
    OrderType type;                      // ASCENDING or DESCENDING
    OrderByNullType null_order;          // NULLS_FIRST or NULLS_LAST
    unique_ptr<Expression> expression;   // Column/expression being ordered
};
```

### Expression Resolution

ORDER BY expressions are typically `BoundReferenceExpression` with an `index` field (output column position).

File: `duckdb/src/include/duckdb/planner/expression/bound_reference_expression.hpp`

```cpp
class BoundReferenceExpression : public Expression {
    storage_t index;  // Physical index into output columns
};
```

Resolution path: `BoundReferenceExpression.index` → `LogicalGet.GetColumnIds()[index]` → column name.

### LogicalGet — Where Data Lives

File: `duckdb/src/include/duckdb/planner/operator/logical_get.hpp`

Key fields:
- `bind_data` (line 34) — `unique_ptr<FunctionData>`, our `MSSQLCatalogScanBindData`
- `table_filters` (line 44) — filter pushdown storage
- `function` (line 31) — the `TableFunction` struct
- `GetColumnIds()` — maps output positions to table column IDs

### Data Flow: Optimizer → Table Function

1. Optimizer callback receives `plan` (the full logical plan tree)
2. Cast `LogicalGet.bind_data` to `MSSQLCatalogScanBindData`
3. Store order info (and TOP N limit) in bind_data fields
4. Optionally remove `LogicalOrder` (and `LogicalLimit`) from the plan
5. In `TableScanInitGlobal`, read info from `bind_data` and append `ORDER BY` / `TOP N` to SQL

### NULL Ordering in SQL Server

SQL Server default behavior (no syntax to change):
- ASC: NULLs sort **first**
- DESC: NULLs sort **last**

DuckDB defaults:
- ASC: NULLs sort **last** (`NULLS_LAST`)
- DESC: NULLs sort **first** (`NULLS_FIRST`)

**These defaults differ!** So by default, DuckDB `ORDER BY col ASC` has `NULLS LAST`, but SQL Server puts NULLs first. This means:
- Only push down when explicit null order matches SQL Server behavior
- Or when the column is NOT NULL (no difference)
- Need to check column nullability from metadata

## Research: External Extensions

### postgres_scanner (duckdb-postgres)

**ORDER BY pushdown: NOT implemented.** But postgres_scanner is the closest reference — it's the only DuckDB extension using `OptimizerExtension` for pushdown.

**What it does implement:**
- **LIMIT/OFFSET pushdown** via `OptimizerExtension` — the exact same mechanism we'd use
- **Filter pushdown** via `pushdown_complex_filter` callback

**Key implementation pattern** (`src/storage/postgres_optimizer.cpp`):

1. Registers optimizer in extension load:
   ```cpp
   OptimizerExtension postgres_optimizer;
   postgres_optimizer.optimize_function = PostgresOptimizer::Optimize;
   OptimizerExtension::Register(config, std::move(postgres_optimizer));
   ```

2. `Optimize()` traverses the plan looking for `LOGICAL_LIMIT` above PostgreSQL scans
3. `OptimizePostgresScanLimitPushdown()` extracts constant LIMIT/OFFSET values
4. Stores as string in `PostgresBindData::limit` field (e.g., `" LIMIT 5 OFFSET 10"`)
5. Appended to generated SELECT query in the scanner

**Conditions for LIMIT pushdown:**
- Values must be constants (not expressions)
- Single-threaded execution (`max_threads == 1`)

**Takeaway:** Our ORDER BY pushdown would follow the same pattern — register `OptimizerExtension`, detect the operator pattern, store info in bind data, generate SQL. postgres_scanner proves the approach works but only implemented the simpler LIMIT case.

### ducklake

**ORDER BY pushdown: NOT implemented. No OptimizerExtension used.**

DuckLake optimizes at the storage layer (file-level pruning via zone maps and partition metadata), not via plan rewriting. This makes sense for lakehouse formats where data is local/object storage.

### delta, iceberg

**No OptimizerExtension used.** Both use scanner-based pushdown (filter/projection) through DuckDB's native `MultiFileReader` infrastructure.

### Summary: Only postgres_scanner Uses OptimizerExtension

| Extension | OptimizerExtension | ORDER BY Pushdown | LIMIT Pushdown |
|-----------|-------------------|-------------------|----------------|
| **postgres_scanner** | Yes | No | Yes (via optimizer) |
| **ducklake** | No | No | DuckDB built-in |
| **delta** | No | No | DuckDB built-in |
| **iceberg** | No | No | DuckDB built-in |
| **mssql (ours)** | No (planned) | No (planned) | No (planned) |

We would be the **first DuckDB extension to implement ORDER BY pushdown** via `OptimizerExtension`.

## Implementation Plan

### Phase 1: Optimizer Extension Infrastructure
- Add `mssql_order_pushdown` setting (bool, default `false`) in `mssql_extension.cpp`
- Add `order_pushdown` ATTACH option parsing in `mssql_storage.cpp`
- Store effective value in `MSSQLCatalogScanBindData`
- Register `OptimizerExtension` in `mssql_extension.cpp` LoadInternal
- Create `src/table_scan/mssql_optimizer.cpp` + `src/include/table_scan/mssql_optimizer.hpp`
- Add `OrderByColumnInfo` struct and TOP N field to `MSSQLCatalogScanBindData`
- Add new source file to `CMakeLists.txt`

### Phase 2: ORDER BY Pattern Detection & Validation
- Check `plan->type == LOGICAL_ORDER_BY` and `child->type == LOGICAL_GET`
- Verify `get_op.function.name == "mssql_catalog_scan"`
- Iterate `order_op.orders` and classify each as pushable or not

### Phase 3: Expression Resolution
- Map `BoundReferenceExpression.index` → column name via `LogicalGet.GetColumnIds()`
- Support simple function calls (reuse/extend `function_mapping.cpp`)
- Reject complex expressions
- Track full vs partial pushdown

### Phase 4: NULL Order Validation
- Check `BoundOrderByNode.null_order` vs SQL Server defaults
- Check column nullability from `MSSQLColumnInfo` metadata
- Mark non-matching as not pushable

### Phase 5: SQL Generation
- In `TableScanInitGlobal`, read `bind_data.order_by_columns`
- Build `ORDER BY [col1] ASC, [YEAR]([col2]) DESC` clause
- Append after WHERE clause

### Phase 6: Plan Modification
- Full pushdown: remove `LogicalOrder` from plan
- Partial pushdown: keep `LogicalOrder` (pre-sort benefit)

### Phase 7: TOP N Pushdown (ORDER BY + LIMIT combined)
- Detect `LogicalLimit → LogicalOrder → LogicalGet` or `LogicalTop → LogicalGet`
- Full ORDER BY + LIMIT: `SELECT TOP N ... ORDER BY ...`, remove both operators
- Partial ORDER BY + LIMIT: keep DuckDB operators, still pre-sort

### Phase 8: Tests
- ORDER BY on various column types
- ORDER BY + LIMIT → verify TOP N pushdown
- Partial pushdown (mixed pushable/non-pushable columns)
- ORDER BY with functions (`YEAR(date_col)`)
- Regression: non-MSSQL tables unaffected
- NULL ordering edge cases

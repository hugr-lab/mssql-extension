# Query Execution & DML

This document covers table scanning with filter/projection pushdown, the INSERT/UPDATE/DELETE pipelines, and the `mssql_scan`/`mssql_exec` utility functions.

## Table Scan

Table scans are implemented as DuckDB table functions. When a query references an attached MSSQL table, the catalog returns a scan function that generates a `SELECT` statement with pushed-down filters and projections.

### Filter Pushdown

`FilterEncoder` (`src/table_scan/filter_encoder.cpp`) converts DuckDB's `TableFilterSet` into T-SQL WHERE clauses.

**Supported filter types**:

| DuckDB Filter Type | T-SQL Output | Notes |
|---|---|---|
| CONSTANT_COMPARISON | `col OP value` | =, <>, <, >, <=, >= |
| IS_NULL | `col IS NULL` | |
| IS_NOT_NULL | `col IS NOT NULL` | |
| IN_FILTER | `col IN (v1, v2, ...)` | |
| CONJUNCTION_AND | `f1 AND f2 AND ...` | Partial pushdown: unsupported children skipped |
| CONJUNCTION_OR | `f1 OR f2 OR ...` | All-or-nothing: if any child unsupported, entire OR skipped |
| EXPRESSION_FILTER | Arbitrary expressions | Recursive encoding with depth limit (100) |

**Partial pushdown**: When a filter cannot be fully pushed down, `FilterEncoderResult.needs_duckdb_filter` is set to `true`, and DuckDB re-filters locally.

**Rowid filter pushdown**: Conditions like `rowid = value` are translated into PK column conditions (e.g., `pk_col = value` for scalar PK, or multi-column conditions for composite PK).

### Function Mapping

`FunctionMapping` (`src/table_scan/function_mapping.cpp`) maps DuckDB functions to SQL Server equivalents for filter pushdown.

**Mapped functions**:

| Category | DuckDB Functions | SQL Server Equivalent |
|---|---|---|
| String | `lower`, `upper`, `length`, `trim`, `ltrim`, `rtrim` | `LOWER`, `UPPER`, `LEN`, `TRIM`, `LTRIM`, `RTRIM` |
| Date extraction | `year`, `month`, `day`, `hour`, `minute`, `second` | `YEAR`, `MONTH`, `DAY`, `DATEPART(...)` |
| Date arithmetic | `date_diff(part, start, end)` | `DATEDIFF(part, start, end)` |
| Date arithmetic | `date_add(date, part, amount)` | `DATEADD(part, amount, date)` (reordered) |
| Arithmetic | `+`, `-`, `*`, `/`, `%` | Same operators |
| Pattern | prefix, suffix, contains | `LIKE 'pattern%'`, `LIKE '%pattern'`, `LIKE '%pattern%'` |

**LIKE pattern escaping**: `%` → `[%]`, `_` → `[_]`, `[` → `[[]` (SQL Server bracket syntax).

### Projection Pushdown

Only requested columns are included in the generated SELECT statement. The column list is determined during the bind phase from DuckDB's column requirements.

### ORDER BY Pushdown (Experimental)

`MSSQLOptimizer` (`src/table_scan/mssql_optimizer.cpp`) is a custom `OptimizerExtension` that detects ORDER BY and TOP N patterns above MSSQL catalog scans and pushes them down to SQL Server.

**Controlled by:**
- Global setting `mssql_order_pushdown` (default: `false`) — checked first; if `true`, pushdown is enabled
- ATTACH option `order_pushdown` — checked second; `true` enables pushdown, `false` is a no-op

**Supported patterns:**

| DuckDB Plan Pattern | SQL Server Output | Notes |
|---|---|---|
| `LogicalOrder → LogicalGet` | `SELECT ... ORDER BY col ASC/DESC` | Simple ORDER BY |
| `LogicalTopN → LogicalGet` | `SELECT TOP N ... ORDER BY col` | Merged ORDER BY + LIMIT |
| `LogicalLimit → LogicalOrder → LogicalGet` | `SELECT TOP N ... ORDER BY col` | Separate LIMIT + ORDER |

Projections between ORDER BY and GET are handled transparently (column references are resolved through the projection).

**Supported ORDER BY expressions:**
- Simple column references (e.g., `ORDER BY name`)
- Single-argument function expressions with known mappings (e.g., `ORDER BY year(created_date)`)

**NULL ordering validation:** SQL Server defaults to NULLS FIRST for ASC and NULLS LAST for DESC. If the DuckDB query requests a different null ordering on a nullable column, pushdown is skipped for that column and all subsequent columns (prefix-only pushdown).

**Partial pushdown:** When only a prefix of ORDER BY columns can be pushed, the extension pushes the prefix to SQL Server and keeps the full ORDER BY in DuckDB for correctness.

### Scan Execution Flow

```
DuckDB query: SELECT a, b FROM mydb.dbo.users WHERE age > 30
  │
  ▼
Bind phase (table_scan_bind.cpp)
  ├─ Resolve table in MSSQLCatalog
  ├─ Determine projected columns (a, b)
  └─ Return TableScanBindData

Init phase (table_scan_execute.cpp)
  ├─ Encode filters → WHERE clause
  ├─ Build: SELECT [a], [b] FROM [dbo].[users] WHERE [age] > 30
  ├─ Acquire connection
  ├─ Execute SQL_BATCH
  └─ Create MSSQLResultStream

Execute phase (table_scan_execute.cpp)
  ├─ Stream rows via MSSQLResultStream::FillChunk()
  ├─ TypeConverter converts TDS values → DuckDB vectors
  └─ Return DataChunks until DONE token
```

## MSSQLResultStream

**Files**: `src/query/mssql_result_stream.cpp`, `src/include/query/mssql_result_stream.hpp`

Streams query results from SQL Server into DuckDB DataChunks.

**State machine**:
- `Initializing` → `Streaming` → `Complete`
- `Initializing` → `Complete` (DML-only batch, no result set)
- `Initializing` → `Streaming` → `Draining` (on cancel) → `Complete`
- `Initializing` → `Streaming` → `Error` (multiple result sets detected)

**Key methods**:

| Method | Purpose |
|---|---|
| `Initialize()` | Send query, wait for COLMETADATA token. Skips non-final DONE tokens (DONE_MORE flag) to support multi-statement batches where intermediate statements don't return results |
| `FillChunk()` | Read rows into DataChunk, return row count. Detects second COLMETADATA token (multiple result sets) and throws a clear error |
| `Cancel()` | Send ATTENTION, drain remaining data |
| `DrainRemainingTokens()` | Drain remaining TDS tokens after error (e.g., multiple result sets). Similar to cancel drain but without ATTENTION signal |
| `SetColumnsToFill()` | Partial column filling (e.g., skip virtual columns) |
| `SetOutputColumnMapping()` | Map SQL result indices to output chunk indices |

### Multi-Statement Batch Support

`mssql_scan()` supports multi-statement SQL batches where intermediate statements don't return result sets. For example:

```sql
-- Create temp table, then query it
FROM mssql_scan('db', 'SELECT * INTO #t FROM dbo.test; SELECT * FROM #t');
```

`Initialize()` handles this by checking the `DONE_MORE` flag on DONE tokens. Non-final DONE tokens (from intermediate DML/DDL statements) are skipped, and the parser continues looking for COLMETADATA from the next result-producing statement.

**Constraint**: Only one statement in the batch may produce a result set. If a second COLMETADATA token is encountered during `FillChunk()`, the system throws a clear error:

```
MSSQL Error: The SQL batch produced multiple result sets.
Only one result-producing statement is allowed per mssql_scan() call.
```

The connection is left in a clean state after this error (remaining tokens are drained).

## INSERT Workflow

INSERT is implemented as a DuckDB `PhysicalOperator` (sink pattern).

### Pipeline

```
DuckDB INSERT plan
  │
  ▼
MSSQLPhysicalInsert (PhysicalOperator)
  ├─ Sink(chunk) → MSSQLInsertExecutor::Execute(chunk)
  │     ├─ MSSQLBatchBuilder::AddRow()
  │     │     └─ MSSQLValueSerializer::Serialize(value) per column
  │     ├─ Batch full? → FlushBatch()
  │     │     ├─ MSSQLInsertStatement::Build() → SQL
  │     │     │   INSERT INTO [schema].[table] ([col1], [col2])
  │     │     │   OUTPUT INSERTED.[col1], ...  -- if RETURNING
  │     │     │   VALUES (lit1, lit2), (lit3, lit4);
  │     │     └─ ExecuteBatch(sql) via TDS layer
  │     └─ With RETURNING: MSSQLReturningParser parses OUTPUT results
  ├─ Finalize() → Flush remaining batch
  └─ GetData() → Return row count or RETURNING chunks
```

### Batching Strategy

- **Max rows per statement**: 1000 (SQL Server VALUES clause limit)
- **Max SQL bytes**: 8MB (configurable via `mssql_insert_max_sql_bytes`)
- **Batch size**: configurable via `mssql_insert_batch_size`

### INSERT with RETURNING (OUTPUT INSERTED)

When `INSERT ... RETURNING` is used, the generated SQL includes an `OUTPUT INSERTED` clause. The response is parsed by `MSSQLReturningParser` which extracts the returned rows using `TypeConverter`.

### INSERT Configuration

```sql
SET mssql_insert_batch_size = 1000;
SET mssql_insert_max_rows_per_statement = 1000;
SET mssql_insert_max_sql_bytes = 8388608;
SET mssql_insert_use_returning_output = true;
```

## UPDATE Workflow

UPDATE uses a rowid-based approach: DuckDB scans the table to get rowid values, then the extension generates batch UPDATE statements using a VALUES join pattern.

### Pipeline

```
DuckDB UPDATE plan
  ├─ Scan table → get [rowid, update_col1, update_col2, ...]
  │
  ▼
MSSQLPhysicalUpdate (PhysicalOperator)
  ├─ Sink(chunk)
  │     ├─ ExtractPKFromRowid(rowid_vector) → PK values
  │     ├─ Extract update values from remaining columns
  │     └─ Accumulate in executor
  ├─ Finalize() → FlushBatch()
  │     └─ MSSQLUpdateStatement::Build() → SQL
  │         UPDATE t
  │         SET t.[col1] = v.[col1], t.[col2] = v.[col2]
  │         FROM [schema].[table] AS t
  │         JOIN (VALUES
  │           (pk1, val1, val2),
  │           (pk2, val3, val4)
  │         ) AS v([pk], [col1], [col2])
  │         ON t.[pk] = v.[pk]
  └─ GetData() → Return row count
```

### Transaction-Aware Deferred Execution

When inside an explicit DuckDB transaction, the UPDATE executor defers all SQL execution to `Finalize()`. This is because the pinned connection may be in `Executing` state while streaming rowid values during the scan phase. All rows are buffered and executed in batches after the scan completes.

### Batch Size Calculation

```
effective_batch_size = min(
    mssql_dml_batch_size,
    mssql_dml_max_parameters / params_per_row
)
```

Where `params_per_row = pk_column_count + update_column_count`. This respects SQL Server's ~2100 parameter limit.

## DELETE Workflow

DELETE follows the same pattern as UPDATE but only needs PK values (no update columns).

### Pipeline

```
DuckDB DELETE plan
  ├─ Scan table → get [...filter_cols, rowid]
  │
  ▼
MSSQLPhysicalDelete (PhysicalOperator)
  ├─ Sink(chunk)
  │     ├─ ExtractPKFromRowid(rowid_vector) → PK values
  │     └─ Accumulate in executor
  ├─ Finalize() → FlushBatch()
  │     └─ MSSQLDeleteStatement::Build() → SQL
  │         DELETE t FROM [schema].[table] AS t
  │         JOIN (VALUES
  │           (pk1),
  │           (pk2)
  │         ) AS v([pk])
  │         ON t.[pk] = v.[pk]
  └─ GetData() → Return row count
```

DELETE also uses deferred execution in explicit transactions.

## Rowid Extraction

`MSSQLRowidExtractor` (`src/dml/mssql_rowid_extractor.cpp`) converts DuckDB rowid values back to PK values for UPDATE/DELETE.

- **Scalar PK** (single column): rowid is the PK value directly
- **Composite PK** (multiple columns): rowid is a STRUCT with fields matching PK columns in key_ordinal order

`GetPKValueAsString()` converts PK values to T-SQL literals using `MSSQLValueSerializer::Serialize()`.

## mssql_scan Function

`mssql_scan(context_name, query)` executes raw T-SQL and streams results as a DuckDB table function.

```sql
SELECT * FROM mssql_scan('mydb', 'SELECT TOP 10 * FROM users');
```

**Implementation** (`src/mssql_functions.cpp`):
1. **Bind**: Executes the query once to discover column types (via COLMETADATA token), stores result stream for reuse
2. **InitGlobal**: Retrieves the stored result stream (avoids double execution)
3. **Execute**: Calls `MSSQLResultStream::FillChunk()` to stream rows

A singleton result stream registry prevents the query from being executed twice (once for schema inference, once for data).

## mssql_exec Function

`mssql_exec(context_name, sql)` executes a T-SQL statement and returns the affected row count.

```sql
SELECT mssql_exec('mydb', 'DELETE FROM users WHERE id = 1');
-- Returns: 1
```

**Implementation**:
1. Acquire connection from pool
2. Execute SQL_BATCH
3. Parse response for DONE token with DONE_COUNT flag
4. Return row count (0 for DDL/SELECT)

## DML Configuration Summary

| Setting | Default | Applies To |
|---|---|---|
| `mssql_insert_batch_size` | 1000 | INSERT row batching |
| `mssql_insert_max_rows_per_statement` | 1000 | INSERT VALUES clause limit |
| `mssql_insert_max_sql_bytes` | 8MB | INSERT SQL size limit |
| `mssql_insert_use_returning_output` | true | INSERT RETURNING via OUTPUT |
| `mssql_dml_batch_size` | 500 | UPDATE/DELETE row batching |
| `mssql_dml_max_parameters` | 2000 | UPDATE/DELETE parameter limit |

## Key Design Decisions

1. **VALUES join pattern** for UPDATE/DELETE: efficient batch matching without temp tables
2. **Multi-row VALUES** for INSERT: reduces round-trips to SQL Server
3. **Deferred execution in transactions**: avoids conflict with pinned connection streaming state
4. **Partial filter pushdown**: maximizes server-side filtering while maintaining correctness
5. **LIKE pattern encoding**: uses SQL Server's bracket escaping syntax (`[%]`, `[_]`, `[[]`)
6. **Multi-statement batch support**: `Initialize()` skips non-final DONE tokens to reach COLMETADATA from later statements
7. **Single result set enforcement**: `FillChunk()` detects second COLMETADATA and throws clear error instead of crashing

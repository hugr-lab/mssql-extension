# Research: Table Scan and Filter Pushdown Refactoring

**Feature**: 013-table-scan-filter-refactor
**Date**: 2026-01-20

## Research Questions

1. How does DuckDB pass filter expressions to table functions?
2. How are LIKE patterns, functions, CASE, and arithmetic represented?
3. What SQL Server T-SQL equivalents exist for DuckDB functions?
4. What is the best pattern for expression tree traversal?

---

## 1. DuckDB Filter API Architecture

### Decision: Use `EXPRESSION_FILTER` handling for advanced filters

### Rationale

DuckDB's filter system uses `TableFilterSet` with 10 filter types. The existing implementation handles 5 types (CONSTANT_COMPARISON, IS_NULL, IS_NOT_NULL, IN_FILTER, CONJUNCTION_AND/OR). Advanced filters (LIKE, functions, CASE, arithmetic) are passed as `EXPRESSION_FILTER` containing arbitrary `Expression` trees.

### Filter Types (from `table_filter.hpp`)

| Type | Enum Value | Current Support | Required Action |
|------|------------|-----------------|-----------------|
| CONSTANT_COMPARISON | 0 | Supported | Keep as-is |
| IS_NULL | 1 | Supported | Keep as-is |
| IS_NOT_NULL | 2 | Supported | Keep as-is |
| CONJUNCTION_OR | 3 | Supported | Keep as-is |
| CONJUNCTION_AND | 4 | Supported | Keep as-is |
| STRUCT_EXTRACT | 5 | Not supported | Skip (out of scope) |
| OPTIONAL_FILTER | 6 | Not supported | Skip (optional) |
| IN_FILTER | 7 | Supported | Keep as-is |
| DYNAMIC_FILTER | 8 | Not supported | Skip (runtime) |
| EXPRESSION_FILTER | 9 | Not supported | **NEW: Parse expression tree** |

### Alternatives Considered

1. **Wait for DuckDB to add dedicated LIKE/function filter types**: Rejected - no timeline, blocks feature delivery
2. **Use DuckDB's optimizer hooks**: Rejected - overly complex, extension API doesn't expose this well
3. **Parse EXPRESSION_FILTER expressions**: Chosen - direct access to expression tree, full control

---

## 2. Expression Tree Structure

### Decision: Implement recursive expression visitor for EXPRESSION_FILTER

### Expression Types to Handle

When `TableFilterType::EXPRESSION_FILTER` is encountered, the `ExpressionFilter::expr` contains one of:

| Expression Class | DuckDB Type | Maps To | Example |
|-----------------|-------------|---------|---------|
| `BoundFunctionExpression` | Function call | T-SQL function | `LOWER(col)` → `LOWER([col])` |
| `BoundComparisonExpression` | Comparison | T-SQL comparison | `a = b` → `[a] = [b]` |
| `BoundOperatorExpression` | Operator | T-SQL operator | `a + b` → `([a] + [b])` |
| `BoundCaseExpression` | CASE | T-SQL CASE | CASE WHEN ... |
| `BoundColumnRefExpression` | Column reference | Bracketed identifier | `[column_name]` |
| `BoundConstantExpression` | Constant | SQL literal | `N'value'`, `123` |
| `BoundConjunctionExpression` | AND/OR | T-SQL AND/OR | `(...) AND (...)` |

### Expression Tree Traversal Pattern

```
ExpressionFilter
  └── expr: Expression*
        ├── BoundFunctionExpression
        │     ├── function.name: string ("lower", "length", etc.)
        │     └── children: vector<Expression>
        ├── BoundComparisonExpression
        │     ├── type: ExpressionType (COMPARE_EQUAL, etc.)
        │     ├── left: Expression*
        │     └── right: Expression*
        ├── BoundOperatorExpression
        │     ├── type: ExpressionType (OPERATOR_ADD, etc.)
        │     └── children: vector<Expression>
        └── BoundCaseExpression
              ├── case_checks: vector<BoundCaseCheck>
              │     ├── when_expr: Expression*
              │     └── then_expr: Expression*
              └── else_expr: Expression*
```

### Rationale

Recursive visitor pattern matches DuckDB's expression tree structure. Each expression type maps cleanly to a T-SQL construct. The visitor returns `(sql_string, is_supported)` tuple to track partial pushdown.

---

## 3. DuckDB to SQL Server Function Mapping

### Decision: Implement explicit function mapping table

### Function Mapping Table

| DuckDB Function | SQL Server Equivalent | Notes |
|-----------------|----------------------|-------|
| `lower(x)` | `LOWER(x)` | Direct mapping |
| `upper(x)` | `UPPER(x)` | Direct mapping |
| `length(x)` | `LEN(x)` | Name differs |
| `trim(x)` | `LTRIM(RTRIM(x))` | Composite |
| `ltrim(x)` | `LTRIM(x)` | Direct mapping |
| `rtrim(x)` | `RTRIM(x)` | Direct mapping |
| `prefix(x, p)` | `x LIKE p + '%'` | Pattern transformation |
| `suffix(x, p)` | `x LIKE '%' + p` | Pattern transformation |
| `contains(x, p)` | `x LIKE '%' + p + '%'` | Pattern transformation |

### Unsupported Functions (return not-pushable)

- `substring` (different parameter semantics)
- `replace` (different behavior for NULL)
- `concat` (use + operator instead in T-SQL)
- Aggregate functions (out of scope)
- See Section 10 for supported date/time functions

### Rationale

Explicit mapping table allows:
1. Clear documentation of supported functions
2. Easy addition of new functions
3. Safe rejection of unsupported functions
4. Correct handling of semantic differences

### Alternatives Considered

1. **Automatic function name passthrough**: Rejected - SQL Server function names differ, semantics may differ
2. **Whitelist + blacklist**: Rejected - blacklist can miss new DuckDB functions
3. **Explicit whitelist with mapping**: Chosen - safest, most maintainable

---

## 4. LIKE Pattern Handling

### Decision: Handle prefix/suffix/contains functions, escape special characters

### Pattern Recognition

DuckDB decomposes LIKE into function calls:
- `col LIKE 'abc%'` → `prefix(col, 'abc')`
- `col LIKE '%abc'` → `suffix(col, 'abc')`
- `col LIKE '%abc%'` → `contains(col, 'abc')`

These appear as `BoundFunctionExpression` with function names `prefix`, `suffix`, `contains`.

### T-SQL Generation

```sql
-- prefix(col, 'abc')
[col] LIKE N'abc%'

-- suffix(col, 'abc')
[col] LIKE N'%abc'

-- contains(col, 'abc')
[col] LIKE N'%abc%'
```

### Special Character Escaping

SQL Server LIKE special characters must be escaped in the pattern:
- `%` → `[%]`
- `_` → `[_]`
- `[` → `[[]`

### ILIKE (Case-Insensitive)

DuckDB ILIKE is decomposed to `iprefix`, `isuffix`, `icontains`. Map to:
```sql
LOWER([col]) LIKE LOWER(N'pattern%')
```

### Rationale

Pattern functions give us structured access to the pattern string, making escaping straightforward. Applying LOWER() to both sides for ILIKE ensures case-insensitive matching regardless of SQL Server collation.

---

## 5. Arithmetic Expression Handling

### Decision: Support +, -, *, / with proper parenthesization

### Operator Mapping

| DuckDB ExpressionType | SQL Server | Precedence Handling |
|----------------------|------------|---------------------|
| OPERATOR_ADD | `+` | Wrap in parentheses |
| OPERATOR_SUBTRACT | `-` | Wrap in parentheses |
| OPERATOR_MULTIPLY | `*` | Wrap in parentheses |
| OPERATOR_DIVIDE | `/` | Wrap in parentheses |
| OPERATOR_MOD | `%` | Wrap in parentheses |

### Example

DuckDB: `(price * quantity) > 100`
T-SQL: `(([price] * [quantity]) > 100)`

### Rationale

Always parenthesizing arithmetic expressions ensures correct precedence regardless of context. Minor SQL verbosity is acceptable for correctness.

---

## 6. CASE Expression Handling

### Decision: Support both simple and searched CASE expressions

### Structure Mapping

DuckDB `BoundCaseExpression`:
```
case_checks: [
  { when_expr, then_expr },
  { when_expr, then_expr },
  ...
]
else_expr: Expression (optional)
```

T-SQL output:
```sql
CASE
  WHEN <when_expr_1> THEN <then_expr_1>
  WHEN <when_expr_2> THEN <then_expr_2>
  ELSE <else_expr>
END
```

### Rationale

CASE expressions map directly between DuckDB and T-SQL. Both support searched CASE (WHEN condition THEN result). Simple CASE (CASE expr WHEN value) is internally represented as searched CASE by DuckDB.

---

## 7. OR Condition Semantics

### Decision: Reject entire OR branch if any child is unsupported

### Rationale

OR semantics require all branches to be evaluated. If we push down only some branches:
- `(A OR B)` where B is unsupported
- Pushing only `A` would return rows matching A but not B
- This is incorrect - we'd miss rows that match B but not A

For AND semantics, partial pushdown is safe:
- `(A AND B)` where B is unsupported
- Pushing only `A` filters some rows on SQL Server
- DuckDB re-applies both A and B, ensuring correctness

### Implementation

```cpp
EncodeResult EncodeOrExpression(vector<Expression*> children) {
    vector<string> parts;
    for (auto& child : children) {
        auto result = EncodeExpression(child);
        if (!result.supported) {
            // Any unsupported child → entire OR is unsupported
            return {.sql = "", .supported = false};
        }
        parts.push_back(result.sql);
    }
    return {.sql = "(" + join(parts, " OR ") + ")", .supported = true};
}
```

---

## 8. Filter Encoder Result Structure

### Decision: Return SQL string + needs_refilter boolean

### Structure

```cpp
struct FilterEncoderResult {
    string where_clause;      // T-SQL WHERE clause (empty if nothing pushable)
    bool needs_duckdb_filter; // true if DuckDB must re-apply filters
};
```

### Logic

- `needs_duckdb_filter = true` if ANY expression was not fully pushed down
- When `needs_duckdb_filter = true`, DuckDB re-applies ALL original filters
- This ensures correctness at the cost of some redundant filtering

### Rationale

This "correctness first" approach aligns with Constitution principle III (Correctness over Convenience). Users get correct results; optimization can be added later.

---

## 9. Column Reference Resolution

### Decision: Use column name lookup from bind data

### Challenge

Filter expressions reference columns by index. The filter encoder needs to:
1. Resolve column index to column name
2. Handle projection pushdown (column_ids mapping)
3. Escape column names as T-SQL identifiers

### Solution

```cpp
string ResolveColumnRef(idx_t column_idx, const BindData& bind_data) {
    // Map from projected index to table column index
    idx_t table_idx = bind_data.column_ids[column_idx];
    // Get column name
    const string& col_name = bind_data.all_column_names[table_idx];
    // Escape as T-SQL identifier
    return "[" + EscapeBracketIdentifier(col_name) + "]";
}
```

### Rationale

Reuses existing column mapping logic from current implementation. Ensures correct column reference regardless of projection.

---

## 10. Date/Time Function Mapping

### Decision: Support core date/time extraction and arithmetic functions

### Function Mapping Table

| DuckDB Function | SQL Server Equivalent | Notes |
|-----------------|----------------------|-------|
| `year(x)` | `YEAR(x)` | Direct mapping |
| `month(x)` | `MONTH(x)` | Direct mapping |
| `day(x)` | `DAY(x)` | Direct mapping |
| `hour(x)` | `DATEPART(HOUR, x)` | Uses DATEPART |
| `minute(x)` | `DATEPART(MINUTE, x)` | Uses DATEPART |
| `second(x)` | `DATEPART(SECOND, x)` | Uses DATEPART |
| `date_diff(part, start, end)` | `DATEDIFF(part, start, end)` | Direct mapping |
| `date_add(date, interval, amount)` | `DATEADD(interval, amount, date)` | Parameter reorder |
| `date_part(part, date)` | `DATEPART(part, date)` | Direct mapping |
| `current_date` | `CAST(GETDATE() AS DATE)` | Constant expression |
| `current_timestamp` | `GETDATE()` | SQL Server 2008+ |

### Date Part Mapping

DuckDB date parts map to SQL Server DATEPART identifiers:

| DuckDB Part | SQL Server Part |
|-------------|-----------------|
| `'year'` | `year` |
| `'month'` | `month` |
| `'day'` | `day` |
| `'hour'` | `hour` |
| `'minute'` | `minute` |
| `'second'` | `second` |
| `'millisecond'` | `millisecond` |
| `'week'` | `week` |
| `'quarter'` | `quarter` |
| `'dayofweek'` | `weekday` |
| `'dayofyear'` | `dayofyear` |

### Example Transformations

```sql
-- DuckDB: year(order_date) = 2024
-- T-SQL: YEAR([order_date]) = 2024

-- DuckDB: date_diff('day', created_at, updated_at) > 30
-- T-SQL: DATEDIFF(day, [created_at], [updated_at]) > 30

-- DuckDB: date_add(due_date, 'day', 7)
-- T-SQL: DATEADD(day, 7, [due_date])

-- DuckDB: hour(timestamp_col) >= 9 AND hour(timestamp_col) < 17
-- T-SQL: DATEPART(HOUR, [timestamp_col]) >= 9 AND DATEPART(HOUR, [timestamp_col]) < 17
```

### Unsupported Date/Time Functions

- `epoch` / `epoch_ms` (different semantics)
- `make_date` / `make_timestamp` (construction, not filtering)
- `strftime` / `strptime` (format string differences)
- Timezone-aware functions (SQL Server has limited TZ support)

### Rationale

Date/time functions are commonly used in filter predicates for time-based queries. The core extraction functions (year, month, day, hour, minute, second) and DATEDIFF/DATEADD map directly to SQL Server equivalents, enabling efficient server-side filtering on temporal data.

---

## Summary

| Research Area | Decision | Confidence |
|--------------|----------|------------|
| Filter API | Parse EXPRESSION_FILTER expressions | High |
| Expression Traversal | Recursive visitor pattern | High |
| Function Mapping | Explicit whitelist table | High |
| LIKE Patterns | Handle prefix/suffix/contains | High |
| Arithmetic | Support +, -, *, / with parens | High |
| CASE | Support searched CASE syntax | High |
| OR Semantics | Reject entire OR if any child unsupported | High |
| Result Structure | SQL + needs_refilter boolean | High |
| Column Resolution | Use bind data column mapping | High |
| Date/Time Functions | Support core extraction and arithmetic | High |

All research questions resolved. Proceed to Phase 1: Design & Contracts.

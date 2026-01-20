# Quickstart: Table Scan and Filter Pushdown

**Feature**: 013-table-scan-filter-refactor

## Overview

This feature refactors the table scan implementation into a dedicated module and extends filter pushdown to support LIKE patterns, function expressions, CASE expressions, and arithmetic operations.

## Module Structure

After implementation, the table scan code will be organized as:

```
src/table_scan/
├── mssql_table_scan.hpp      # Public interface
├── mssql_table_scan.cpp      # Function registration
├── table_scan_bind.hpp/cpp   # Bind data and function
├── table_scan_state.hpp/cpp  # Execution state
├── table_scan_execute.cpp    # Scan execution
├── filter_encoder.hpp/cpp    # Filter → T-SQL conversion
└── function_mapping.hpp      # DuckDB → SQL Server function map
```

## Key Components

### 1. Filter Encoder

The `FilterEncoder` class converts DuckDB filter expressions to T-SQL WHERE clauses.

**Input**: `TableFilterSet` from DuckDB optimizer
**Output**: `FilterEncoderResult` with WHERE clause and re-filter flag

```cpp
// Usage in InitGlobal
FilterEncoderResult result = FilterEncoder::Encode(
    input.filters,
    bind_data.column_ids,
    bind_data.all_column_names
);

// Build query with WHERE clause
string query = "SELECT ... FROM [schema].[table]";
if (!result.where_clause.empty()) {
    query += " WHERE " + result.where_clause;
}

// Track if DuckDB needs to re-filter
global_state.needs_duckdb_filter = result.needs_duckdb_filter;
```

### 2. Supported Filter Types

| Filter Type | Support Level | Notes |
|-------------|---------------|-------|
| `col = value` | Full | All comparison operators |
| `col IS NULL` | Full | |
| `col IS NOT NULL` | Full | |
| `col IN (...)` | Full | |
| `col LIKE 'pattern%'` | Full | prefix, suffix, contains |
| `col ILIKE 'pattern'` | Full | Uses LOWER() on both sides |
| `LOWER(col) = value` | Full | And UPPER, LEN, TRIM |
| `CASE WHEN ... END` | Full | Searched CASE only |
| `col + val > 10` | Full | +, -, *, / operators |
| `AND` combinations | Partial | Unsupported children skipped |
| `OR` combinations | All-or-none | One unsupported = skip all |

### 3. Function Mapping

DuckDB functions are mapped to SQL Server equivalents:

| DuckDB | SQL Server |
|--------|------------|
| `lower(x)` | `LOWER(x)` |
| `upper(x)` | `UPPER(x)` |
| `length(x)` | `LEN(x)` |
| `trim(x)` | `LTRIM(RTRIM(x))` |
| `prefix(x, p)` | `x LIKE p + '%'` |
| `suffix(x, p)` | `x LIKE '%' + p` |
| `contains(x, p)` | `x LIKE '%' + p + '%'` |
| `year(x)` | `YEAR(x)` |
| `month(x)` | `MONTH(x)` |
| `day(x)` | `DAY(x)` |
| `hour(x)` | `DATEPART(HOUR, x)` |
| `date_diff(part, start, end)` | `DATEDIFF(part, start, end)` |
| `date_add(date, interval, amount)` | `DATEADD(interval, amount, date)` |

## Usage Examples

### Basic Filter Pushdown

```sql
-- DuckDB query
SELECT * FROM mssql.dbo.customers WHERE name LIKE 'John%';

-- Generated T-SQL
SELECT [id], [name], [email] FROM [dbo].[customers] WHERE [name] LIKE N'John%'
```

### Case-Insensitive Search (ILIKE)

```sql
-- DuckDB query
SELECT * FROM mssql.dbo.products WHERE name ILIKE '%widget%';

-- Generated T-SQL
SELECT ... FROM [dbo].[products] WHERE LOWER([name]) LIKE LOWER(N'%widget%')
```

### Function in Filter

```sql
-- DuckDB query
SELECT * FROM mssql.dbo.items WHERE LOWER(category) = 'electronics';

-- Generated T-SQL
SELECT ... FROM [dbo].[items] WHERE LOWER([category]) = N'electronics'
```

### Arithmetic Expression

```sql
-- DuckDB query
SELECT * FROM mssql.dbo.orders WHERE quantity * price > 1000;

-- Generated T-SQL
SELECT ... FROM [dbo].[orders] WHERE (([quantity] * [price]) > 1000)
```

### CASE Expression

```sql
-- DuckDB query
SELECT * FROM mssql.dbo.users
WHERE CASE WHEN status = 1 THEN 'active' ELSE 'inactive' END = 'active';

-- Generated T-SQL
SELECT ... FROM [dbo].[users]
WHERE (CASE WHEN ([status] = 1) THEN N'active' ELSE N'inactive' END) = N'active'
```

### Date/Time Filtering

```sql
-- DuckDB query (filter by year)
SELECT * FROM mssql.dbo.orders WHERE year(order_date) = 2024;

-- Generated T-SQL
SELECT ... FROM [dbo].[orders] WHERE YEAR([order_date]) = 2024

-- DuckDB query (date range with DATEDIFF)
SELECT * FROM mssql.dbo.tasks WHERE date_diff('day', created_at, current_date) < 30;

-- Generated T-SQL
SELECT ... FROM [dbo].[tasks] WHERE DATEDIFF(day, [created_at], CAST(GETDATE() AS DATE)) < 30
```

### Complex Conditions

```sql
-- DuckDB query (partial pushdown)
SELECT * FROM mssql.dbo.data
WHERE a = 1 AND (b = 2 OR unsupported_func(c) = 3) AND d = 4;

-- Generated T-SQL (only a=1 AND d=4 pushed down)
SELECT ... FROM [dbo].[data] WHERE ([a] = 1) AND ([d] = 4)

-- DuckDB re-applies ALL original filters on results
```

## Debugging

Enable debug logging with `MSSQL_DEBUG` environment variable:

```bash
MSSQL_DEBUG=2 duckdb
```

Log output includes:
- Original DuckDB filter expression
- Generated T-SQL WHERE clause
- Whether DuckDB re-filtering is required

## Testing

### Unit Tests (no SQL Server required)

```bash
# Test filter encoder in isolation
make test TEST_FILTER="filter_encoder"
```

### Integration Tests (SQL Server required)

```bash
# Test end-to-end filter pushdown
MSSQL_TEST_HOST=localhost make test TEST_FILTER="filter_pushdown"
```

## Common Issues

### Unsupported Function

If a DuckDB function is not in the mapping table, the filter falls back to DuckDB:

```sql
-- substring() not supported for pushdown
SELECT * FROM mssql.dbo.items WHERE substring(code, 1, 3) = 'ABC';
-- Filter applied by DuckDB, not SQL Server
```

### OR with Unsupported Branch

If any OR branch contains unsupported expressions, the entire OR is not pushed:

```sql
-- unsupported_func makes entire OR unpushable
SELECT * FROM mssql.dbo.data WHERE a = 1 OR unsupported_func(b) = 2;
-- No WHERE clause sent to SQL Server; DuckDB filters all results
```

## Migration Notes

### From Previous Implementation

1. `mssql_functions.cpp` retains `mssql_scan()` for ad-hoc queries
2. Catalog scan function moved to `src/table_scan/`
3. `MSSQLCatalogScanBindData` renamed to `TableScanBindData` (in `duckdb::mssql` namespace)
4. Filter conversion logic moved to `FilterEncoder` class
5. `src/pushdown/` directory removed (was unused)

### API Changes

- `GetMSSQLCatalogScanFunction()` renamed to `GetCatalogScanFunction()` (in `duckdb::mssql` namespace)
- Import from `table_scan/mssql_table_scan.hpp` instead of `mssql_functions.hpp`

### Naming Convention

- Types in `duckdb::mssql` namespace do NOT use MSSQL prefix (e.g., `TableScanBindData`, `FilterEncoder`)
- Types directly in `duckdb` namespace MUST use MSSQL prefix (e.g., `MSSQLCatalog`, `MSSQLResultStream`)

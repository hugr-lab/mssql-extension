# Contracts: VARCHAR to NVARCHAR Conversion

This feature does not introduce new APIs. It modifies internal query generation behavior.

## Internal Contracts

### Query Generation Contract

**Input**: Table scan request with column projection
**Output**: SQL SELECT statement with VARCHAR columns wrapped in CAST

**Contract**:
- VARCHAR/CHAR columns with non-UTF8 collation → `CAST([col] AS NVARCHAR(n)) AS [col]`
- NVARCHAR/NCHAR columns → `[col]` (unchanged)
- UTF-8 VARCHAR columns → `[col]` (unchanged)
- Non-string columns → `[col]` (unchanged)

### Length Mapping Contract

| Input (VARCHAR) | Output (NVARCHAR) |
|-----------------|-------------------|
| 1-4000 | Same length |
| 4001-8000 | 4000 (truncated) |
| MAX (-1) | MAX |

### Scope Contract

| Operation | Conversion Applied |
|-----------|-------------------|
| Table scan (`SELECT * FROM table`) | YES |
| `mssql_scan(context, query)` | NO |
| Filter pushdown | NO (comparisons work regardless) |
| INSERT/UPDATE/DELETE | NO |
| COPY TO | NO |

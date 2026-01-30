# Contract: COPY TO MSSQL Syntax

**Feature**: 024-mssql-copy-bcp
**Date**: 2026-01-29

## SQL Syntax

### URL-Based Target

```sql
COPY (source_query) TO 'mssql://<attach_alias>/<schema>/<table>'
    (FORMAT 'bcp' [, option = value, ...]);

COPY (source_query) TO 'mssql://<attach_alias>/#temp_table'
    (FORMAT 'bcp' [, option = value, ...]);

COPY (source_query) TO 'mssql://<attach_alias>/##global_temp'
    (FORMAT 'bcp' [, option = value, ...]);
```

### Catalog-Based Target

```sql
COPY (source_query) TO <catalog>.<schema>.<table>
    (FORMAT 'bcp' [, option = value, ...]);
```

---

## Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `FORMAT` | string | (required) | Must be `'bcp'` |
| `CREATE_TABLE` | boolean | `true` | Create table if it doesn't exist |
| `OVERWRITE` | boolean | `false` | Drop and recreate existing table |

---

## Examples

### Basic COPY to New Table

```sql
-- Create and populate new table
COPY (SELECT * FROM local_data)
TO 'mssql://db/dbo/imported_data' (FORMAT 'bcp');
```

### COPY to Existing Table (Append)

```sql
-- Append to existing table
COPY (SELECT * FROM new_records)
TO 'mssql://db/dbo/existing_table' (FORMAT 'bcp', CREATE_TABLE false);
```

### COPY with Overwrite

```sql
-- Replace existing table
COPY (SELECT * FROM fresh_data)
TO 'mssql://db/dbo/target_table' (FORMAT 'bcp', OVERWRITE true);
```

### COPY to Temp Table

```sql
-- Create session-scoped temp table
COPY (SELECT id, name FROM source)
TO 'mssql://db/#staging_data' (FORMAT 'bcp');

-- Query temp table on same session
SELECT * FROM mssql_scan('db', 'SELECT * FROM #staging_data');
```

### Catalog Syntax

```sql
-- Assuming 'sqlsrv' is an attached MSSQL catalog
COPY (SELECT * FROM local_table)
TO sqlsrv.dbo.target_table (FORMAT 'bcp');
```

### Filtered COPY

```sql
-- Only copy active records
COPY (SELECT * FROM customers WHERE status = 'active')
TO 'mssql://db/dbo/active_customers' (FORMAT 'bcp');
```

### Join COPY

```sql
-- Copy joined result
COPY (
    SELECT o.id, o.total, c.name
    FROM orders o
    JOIN customers c ON o.customer_id = c.id
)
TO 'mssql://db/dbo/order_summary' (FORMAT 'bcp');
```

---

## Error Conditions

| Condition | Error Message |
|-----------|---------------|
| Invalid attach alias | `MSSQL: Unknown MSSQL catalog '<alias>'` |
| Table exists, CREATE_TABLE=true, OVERWRITE=false | `MSSQL: Table [schema].[table] already exists. Use OVERWRITE=true to replace.` |
| Table doesn't exist, CREATE_TABLE=false | `MSSQL: Table [schema].[table] does not exist and CREATE_TABLE=false` |
| Target is a VIEW | `MSSQL: COPY TO views is not supported` |
| Schema mismatch | `MSSQL: Column count mismatch: source has N columns, target has M` |
| Type incompatibility | `MSSQL: Type mismatch for column 'name': cannot convert DuckDB TYPE to SQL Server TYPE` |
| Connection busy | `MSSQL: cannot start COPY/BCP on a connection that is currently streaming results` |
| Transaction conflict | `MSSQL: COPY TO is not supported inside DuckDB transactions when the source requires scanning rows (mssql_scan)` |

---

## Return Value

COPY returns the number of rows written:

```sql
D COPY (SELECT * FROM data) TO 'mssql://db/dbo/target' (FORMAT 'bcp');
┌───────────────┐
│ rows_affected │
├───────────────┤
│        100000 │
└───────────────┘
```

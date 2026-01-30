# COPY TO Syntax Contract

**Feature**: 025-bcp-improvements
**Date**: 2026-01-30

## URL Format Syntax

### Current (maintained for backward compatibility)

```sql
-- Two-part: catalog/table (schema defaults to 'dbo')
COPY data TO 'mssql://catalog/table' (FORMAT 'bcp');

-- Three-part: catalog/schema/table
COPY data TO 'mssql://catalog/schema/table' (FORMAT 'bcp');

-- Temp table: catalog/#table (no schema)
COPY data TO 'mssql://catalog/#temp' (FORMAT 'bcp');
```

### New (empty schema syntax)

```sql
-- Empty schema for temp tables: catalog//#table
COPY data TO 'mssql://catalog//#temp' (FORMAT 'bcp');

-- Empty schema for global temp: catalog//##table
COPY data TO 'mssql://catalog//##global_temp' (FORMAT 'bcp');
```

### Invalid (rejected with error)

```sql
-- Empty schema for regular tables: ERROR
COPY data TO 'mssql://catalog//regular_table' (FORMAT 'bcp');
-- Error: "Empty schema only valid for temp tables"

-- Triple slash: ERROR
COPY data TO 'mssql://catalog///table' (FORMAT 'bcp');
-- Error: "Invalid URL format"
```

## Catalog Format Syntax

### Current (maintained for backward compatibility)

```sql
-- Two-part: catalog.table (schema defaults to 'dbo')
COPY data TO 'catalog.table' (FORMAT 'bcp');

-- Three-part: catalog.schema.table
COPY data TO 'catalog.schema.table' (FORMAT 'bcp');

-- Temp table: catalog.#table (no schema)
COPY data TO 'catalog.#temp' (FORMAT 'bcp');
```

### New (empty schema syntax)

```sql
-- Empty schema for temp tables: catalog..#table
COPY data TO 'catalog..#temp' (FORMAT 'bcp');

-- Empty schema for global temp: catalog..##table
COPY data TO 'catalog..##global_temp' (FORMAT 'bcp');
```

### Invalid (rejected with error)

```sql
-- Empty schema for regular tables: ERROR
COPY data TO 'catalog..regular_table' (FORMAT 'bcp');
-- Error: "Empty schema only valid for temp tables"
```

## COPY Options

### Existing Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| FORMAT | VARCHAR | required | Must be 'bcp' |
| CREATE_TABLE | BOOLEAN | true | Auto-create target table |
| REPLACE | BOOLEAN | false | Drop and recreate table |
| FLUSH_ROWS | BIGINT | 100000 | Rows before flushing |
| TABLOCK | BOOLEAN | true | Use TABLOCK hint |

### New Option

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| METHOD | VARCHAR | 'bcp' | Copy method: 'bcp' or 'insert' |

### Method Option Usage

```sql
-- Default: BCP protocol (high performance)
COPY data TO 'catalog.schema.table' (FORMAT 'bcp');
COPY data TO 'catalog.schema.table' (FORMAT 'bcp', METHOD 'bcp');

-- Alternative: INSERT statements (debugging, compatibility)
COPY data TO 'catalog.schema.table' (FORMAT 'bcp', METHOD 'insert');
```

## Error Messages

### Type Mismatch

```text
Column 'column_name' type mismatch: target expects INT, source provides VARCHAR
```

### Empty Schema Invalid

```text
Empty schema only valid for temp tables (table name must start with '#')
```

### Connection Error

```text
COPY failed: connection error during {phase}. {original_error}
```

### Invalid URL

```text
Invalid COPY target URL format. Expected:
  mssql://catalog/table
  mssql://catalog/schema/table
  mssql://catalog/#temp_table
  mssql://catalog//#temp_table
```

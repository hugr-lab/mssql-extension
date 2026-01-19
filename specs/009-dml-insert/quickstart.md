# Quickstart: High-Performance DML INSERT

**Feature**: 009-dml-insert
**Date**: 2026-01-19

## Prerequisites

- DuckDB with mssql extension loaded
- SQL Server 2019+ database accessible
- Target table exists in SQL Server

## Basic Usage

### 1. Attach SQL Server with Write Access

```sql
-- Attach with READ_WRITE mode to enable INSERT
ATTACH 'mssql://user:password@server/database' AS mssql (TYPE mssql, READ_WRITE);
```

### 2. Simple INSERT

```sql
-- Insert single row
INSERT INTO mssql.schema.table (col1, col2) VALUES (1, 'hello');

-- Insert multiple rows
INSERT INTO mssql.schema.table (col1, col2) VALUES
    (1, 'hello'),
    (2, 'world'),
    (3, 'test');
```

### 3. INSERT from DuckDB Query

```sql
-- Insert from local DuckDB table
INSERT INTO mssql.dbo.target_table
SELECT * FROM local_duckdb_table;

-- Insert with transformation
INSERT INTO mssql.dbo.customers (name, email, created_at)
SELECT
    upper(name),
    lower(email),
    current_timestamp
FROM staging_customers
WHERE status = 'active';
```

### 4. INSERT with RETURNING

```sql
-- Get inserted rows back (including identity values)
INSERT INTO mssql.dbo.orders (customer_id, amount)
VALUES (123, 99.99)
RETURNING *;

-- Returns:
-- | order_id | customer_id | amount | created_at          |
-- |----------|-------------|--------|---------------------|
-- | 1001     | 123         | 99.99  | 2026-01-19 10:30:00 |

-- Return specific columns
INSERT INTO mssql.dbo.orders (customer_id, amount)
VALUES (123, 99.99)
RETURNING order_id, created_at;
```

### 5. Bulk Insert (10M+ rows)

```sql
-- Configure batch size for large inserts
SET mssql_insert_batch_size = 5000;

-- Insert large dataset
INSERT INTO mssql.dbo.fact_table
SELECT * FROM generate_series(1, 10000000) AS t(id);
```

## Configuration Options

```sql
-- View current settings
SELECT current_setting('mssql_insert_batch_size');
SELECT current_setting('mssql_insert_max_sql_bytes');

-- Adjust batch size (default: 2000)
SET mssql_insert_batch_size = 1000;

-- Adjust max SQL size (default: 8MB)
SET mssql_insert_max_sql_bytes = 4194304;  -- 4MB

-- Disable RETURNING optimization (if triggers interfere)
SET mssql_insert_use_returning_output = false;
```

## Type Mapping Examples

```sql
-- All supported types
INSERT INTO mssql.dbo.all_types (
    bool_col,      -- BOOLEAN → BIT
    int_col,       -- INTEGER → INT
    bigint_col,    -- BIGINT → BIGINT
    float_col,     -- DOUBLE → FLOAT
    decimal_col,   -- DECIMAL(10,2) → DECIMAL(10,2)
    text_col,      -- VARCHAR → NVARCHAR (Unicode)
    uuid_col,      -- UUID → UNIQUEIDENTIFIER
    blob_col,      -- BLOB → VARBINARY
    date_col,      -- DATE → DATE
    time_col,      -- TIME → TIME
    ts_col,        -- TIMESTAMP → DATETIME2
    tstz_col       -- TIMESTAMP_TZ → DATETIMEOFFSET
) VALUES (
    true,
    42,
    9223372036854775807,
    3.14159,
    123.45,
    'Hello Unicode: 你好 🎉',
    '550e8400-e29b-41d4-a716-446655440000',
    '\x48454C4C4F',
    '2026-01-19',
    '14:30:00.123456',
    '2026-01-19 14:30:00.123456',
    '2026-01-19 14:30:00.123456+05:30'
);
```

## Error Handling

### Constraint Violation

```sql
-- Duplicate key error
INSERT INTO mssql.dbo.users (id, name) VALUES (1, 'test');
-- Error: INSERT failed at statement 0 (rows 0-0): [2627] Violation of PRIMARY KEY constraint

-- Foreign key error
INSERT INTO mssql.dbo.orders (customer_id) VALUES (99999);
-- Error: INSERT failed at statement 0 (rows 0-0): [547] The INSERT statement conflicted with the FOREIGN KEY constraint
```

### Identity Column

```sql
-- Cannot specify identity values (MVP limitation)
INSERT INTO mssql.dbo.users (id, name) VALUES (100, 'test');
-- Error: Cannot insert explicit value for identity column 'id'

-- Correct: omit identity column
INSERT INTO mssql.dbo.users (name) VALUES ('test') RETURNING id;
```

### Size Limits

```sql
-- Row too large
INSERT INTO mssql.dbo.docs (content) VALUES (repeat('x', 10000000));
-- Error: Row at offset 0 exceeds maximum SQL size (8388608 bytes)
```

## Performance Tips

1. **Batch size tuning**: Start with default 2000, increase if rows are small, decrease if rows are large
2. **Disable RETURNING**: If you don't need returned values, avoid RETURNING for better throughput
3. **Transaction batching**: Group related inserts in a single INSERT ... SELECT for best performance
4. **Network latency**: Larger batches reduce round-trips but increase memory; balance for your network

```sql
-- Optimal for small rows
SET mssql_insert_batch_size = 5000;

-- Optimal for large rows (text/binary)
SET mssql_insert_batch_size = 500;
SET mssql_insert_max_sql_bytes = 4194304;
```

## Unicode and Collation

### Unicode String Handling

All string values are automatically serialized with the `N'...'` Unicode prefix to ensure proper handling of international characters:

```sql
-- These all work correctly with Unicode:
INSERT INTO mssql.dbo.messages (content) VALUES
    ('Hello World'),                    -- ASCII
    ('你好世界'),                         -- Chinese
    ('مرحبا بالعالم'),                   -- Arabic
    ('Привет мир'),                     -- Russian
    ('こんにちは世界'),                    -- Japanese
    ('Hello 😀🎉🚀 Emoji');              -- Emoji
```

The extension:
- **Always uses `N'...'` literals**: Ensures SQL Server interprets strings as Unicode (NVARCHAR)
- **Preserves UTF-8 encoding**: DuckDB strings are UTF-8; converted to UTF-16 by SQL Server
- **Escapes single quotes**: `'` becomes `''` to prevent SQL injection

### Collation Behavior

SQL Server performs collation conversion server-side:

1. **Source**: DuckDB VARCHAR (UTF-8)
2. **Wire format**: T-SQL `N'...'` literal (Unicode)
3. **Storage**: SQL Server converts to column's collation

```sql
-- Column with specific collation
CREATE TABLE mssql.dbo.multilingual (
    content NVARCHAR(100) COLLATE Latin1_General_CI_AS
);

-- Insert works regardless of column collation
INSERT INTO mssql.dbo.multilingual (content) VALUES ('Ελληνικά');
```

**Note**: If inserting into non-Unicode columns (`VARCHAR` without `N` prefix on server), SQL Server may perform lossy conversion. For full Unicode support, ensure target columns are `NVARCHAR`.

### SQL Injection Prevention

All string values are properly escaped:

```sql
-- This is safe - the single quote is escaped
INSERT INTO mssql.dbo.logs (message) VALUES ('''; DROP TABLE users; --');
-- Results in: N'''; DROP TABLE users; --'
-- Stored as literal text, NOT executed as SQL
```

The escaping rules:
- Single quotes (`'`) are doubled (`''`)
- No other escaping needed for T-SQL string literals
- Backslashes, percent signs, underscores are preserved as-is

## Limitations (MVP)

- No `SET IDENTITY_INSERT ON` support
- No partial row error reporting (all-or-nothing per batch)
- No BCP/bulk copy protocol (future spec)
- No MERGE/UPSERT operations
- RETURNING supports column references only (no expressions)

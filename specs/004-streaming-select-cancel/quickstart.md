# Quickstart: Streaming SELECT and Query Cancellation

**Feature**: 004-streaming-select-cancel
**Date**: 2026-01-15

## Prerequisites

- DuckDB with mssql extension loaded
- SQL Server 2019+ instance with SQL Server authentication enabled
- Valid credentials stored in a secret

## Setup

### 1. Create a Secret

```sql
-- Store SQL Server credentials
CREATE SECRET my_sqlserver (
    TYPE mssql,
    HOST 'localhost',
    PORT 1433,
    DATABASE 'testdb',
    USER 'sa',
    PASSWORD 'YourPassword123!'
);
```

### 2. Attach the Database

```sql
-- Create a connection context
ATTACH '' AS sqlserver (TYPE mssql, SECRET my_sqlserver);
```

## Basic Usage

### Execute a SELECT Query

```sql
-- Query data from SQL Server
SELECT * FROM mssql_scan('sqlserver', 'SELECT id, name, created_at FROM users');

-- With filtering (filter happens on SQL Server)
SELECT * FROM mssql_scan('sqlserver', 'SELECT * FROM orders WHERE status = ''pending''');

-- With aggregation
SELECT * FROM mssql_scan('sqlserver', 'SELECT category, COUNT(*) as cnt FROM products GROUP BY category');
```

### Query Large Tables

```sql
-- Stream 10 million rows (results arrive in chunks)
SELECT * FROM mssql_scan('sqlserver', 'SELECT * FROM large_table');

-- Combine with DuckDB processing
SELECT category, SUM(amount) as total
FROM mssql_scan('sqlserver', 'SELECT category, amount FROM transactions')
GROUP BY category
ORDER BY total DESC
LIMIT 10;
```

### Data Type Examples

```sql
-- Integer types
SELECT * FROM mssql_scan('sqlserver', 'SELECT tinyint_col, smallint_col, int_col, bigint_col FROM type_test');

-- Decimal types
SELECT * FROM mssql_scan('sqlserver', 'SELECT price, quantity, total FROM orders');

-- String types
SELECT * FROM mssql_scan('sqlserver', 'SELECT name, description, unicode_text FROM products');

-- Date/time types
SELECT * FROM mssql_scan('sqlserver', 'SELECT created_at, updated_at, birth_date FROM records');

-- Binary and UUID
SELECT * FROM mssql_scan('sqlserver', 'SELECT id, file_data FROM attachments');
```

## Query Cancellation

### Interactive Cancellation

When running a long query in the DuckDB CLI, press `Ctrl+C` to cancel:

```
D SELECT * FROM mssql_scan('sqlserver', 'SELECT * FROM huge_table');
-- Press Ctrl+C to cancel
-- Query cancelled. Connection returned to pool.
```

### Programmatic Cancellation

In applications using DuckDB's API, use the interrupt mechanism:

```cpp
// C++ example
auto result = connection.SendQuery("SELECT * FROM mssql_scan('ctx', 'SELECT * FROM big_table')");

// From another thread:
connection.Interrupt();
```

```python
# Python example
import signal

def cancel_handler(signum, frame):
    conn.interrupt()

signal.signal(signal.SIGINT, cancel_handler)
result = conn.execute("SELECT * FROM mssql_scan('ctx', 'SELECT * FROM big_table')")
```

## Error Handling

### SQL Server Errors

```sql
-- Invalid syntax (SQL Server error propagated)
SELECT * FROM mssql_scan('sqlserver', 'SELEC * FROM users');
-- Error: SQL Server Error 102 (Severity 15): Incorrect syntax near '*'.

-- Missing table
SELECT * FROM mssql_scan('sqlserver', 'SELECT * FROM nonexistent_table');
-- Error: SQL Server Error 208 (Severity 16): Invalid object name 'nonexistent_table'.

-- Permission denied
SELECT * FROM mssql_scan('sqlserver', 'SELECT * FROM restricted_table');
-- Error: SQL Server Error 229 (Severity 14): The SELECT permission was denied...
```

### Unsupported Types

```sql
-- Query with unsupported XML column
SELECT * FROM mssql_scan('sqlserver', 'SELECT id, xml_data FROM documents');
-- Error: Unsupported SQL Server type 'XML' (0xF1) for column 'xml_data'.
-- Consider casting to VARCHAR or excluding this column.

-- Workaround: Cast to supported type
SELECT * FROM mssql_scan('sqlserver', 'SELECT id, CAST(xml_data AS NVARCHAR(MAX)) as xml_text FROM documents');
```

## Debugging

### Enable Debug Logging

```sql
-- Enable DuckDB debug logging
CALL enable_logging(level = 'debug');

-- Now queries will show TDS protocol details:
SELECT * FROM mssql_scan('sqlserver', 'SELECT TOP 10 * FROM users');
-- Debug: TDS: Sending SQL_BATCH (128 bytes)
-- Debug: TDS: Received COLMETADATA with 5 columns
-- Debug: TDS: Row 1 parsed, 64 bytes
-- ...
-- Debug: TDS: Received DONE (10 rows)
```

### Check Connection Pool

```sql
-- View pool statistics
SELECT * FROM mssql_pool_stats('sqlserver');
-- ┌──────────────┬───────┬────────┬─────────┬────────────────┬──────────────────┐
-- │ context_name │ total │ active │   idle  │ created_count  │  closed_count    │
-- ├──────────────┼───────┼────────┼─────────┼────────────────┼──────────────────┤
-- │ sqlserver    │     3 │      1 │       2 │             5  │               2  │
-- └──────────────┴───────┴────────┴─────────┴────────────────┴──────────────────┘
```

## Integration Test Setup

### Create Test Table with 10M Rows

```sql
-- On SQL Server (run directly in SSMS or sqlcmd)
USE testdb;

-- Create test table
CREATE TABLE streaming_test (
    id INT IDENTITY PRIMARY KEY,
    value1 INT,
    value2 VARCHAR(100),
    value3 DECIMAL(18,2),
    value4 DATETIME2
);

-- Generate 10M rows
SET NOCOUNT ON;
DECLARE @i INT = 0;
WHILE @i < 10000000
BEGIN
    INSERT INTO streaming_test (value1, value2, value3, value4)
    VALUES (
        @i % 1000,
        CONCAT('Value_', @i),
        (@i % 10000) * 1.23,
        DATEADD(SECOND, @i, '2020-01-01')
    );
    SET @i = @i + 1;

    IF @i % 100000 = 0
        PRINT CONCAT('Inserted ', @i, ' rows');
END;
```

Or use a faster bulk approach:

```sql
-- Faster: Generate with cross join
;WITH Numbers AS (
    SELECT TOP 10000000 ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) - 1 AS n
    FROM master..spt_values a, master..spt_values b, master..spt_values c
)
INSERT INTO streaming_test (value1, value2, value3, value4)
SELECT
    n % 1000,
    CONCAT('Value_', n),
    (n % 10000) * 1.23,
    DATEADD(SECOND, n % 86400, '2020-01-01')
FROM Numbers;
```

### Run Streaming Test

```sql
-- In DuckDB
SELECT COUNT(*) FROM mssql_scan('sqlserver', 'SELECT * FROM streaming_test');
-- Should return 10000000, streaming completes in < 60 seconds

-- Test with aggregation
SELECT value1, AVG(value3) as avg_value
FROM mssql_scan('sqlserver', 'SELECT value1, value3 FROM streaming_test')
GROUP BY value1
ORDER BY value1;
```

### Test Cancellation

```sql
-- Start a query and cancel after a few seconds
SELECT * FROM mssql_scan('sqlserver', 'SELECT * FROM streaming_test');
-- Press Ctrl+C after seeing some output

-- Verify connection is still usable
SELECT * FROM mssql_scan('sqlserver', 'SELECT TOP 10 * FROM streaming_test');
-- Should work without reconnection
```

## Best Practices

1. **Filter on SQL Server**: Push WHERE clauses into the SQL query to reduce data transfer
   ```sql
   -- Good: Filter on server
   SELECT * FROM mssql_scan('ctx', 'SELECT * FROM orders WHERE date > ''2024-01-01''');

   -- Less efficient: Filter in DuckDB
   SELECT * FROM mssql_scan('ctx', 'SELECT * FROM orders') WHERE date > '2024-01-01';
   ```

2. **Select only needed columns**: Reduce bandwidth by selecting specific columns
   ```sql
   -- Good: Select specific columns
   SELECT * FROM mssql_scan('ctx', 'SELECT id, name FROM users');

   -- Less efficient: Select all
   SELECT id, name FROM mssql_scan('ctx', 'SELECT * FROM users');
   ```

3. **Use connection pooling**: Let the pool manage connections
   ```sql
   -- Don't worry about connection management
   -- Multiple queries will reuse pooled connections automatically
   ```

4. **Handle large results incrementally**: Use LIMIT or pagination for exploration
   ```sql
   -- Preview data first
   SELECT * FROM mssql_scan('ctx', 'SELECT TOP 100 * FROM big_table');

   -- Then run full query if needed
   ```

# MSSQL Extension - Data Types and Query Features

This document describes the SQL Server data types supported by the DuckDB MSSQL Extension and the query optimization features available.

## Table of Contents

1. [Supported Data Types](#supported-data-types)
2. [Filter Pushdown](#filter-pushdown)
3. [Projection Pushdown](#projection-pushdown)
4. [Collation Support](#collation-support)
5. [Known Limitations](#known-limitations)

---

## Supported Data Types

The extension maps SQL Server data types to their DuckDB equivalents during query execution.

### Integer Types

| SQL Server Type | DuckDB Type | Range | Notes |
|-----------------|-------------|-------|-------|
| `BIT` | `BOOLEAN` | 0, 1 | Maps to true/false |
| `TINYINT` | `UTINYINT` | 0-255 | Unsigned 8-bit |
| `SMALLINT` | `SMALLINT` | -32,768 to 32,767 | Signed 16-bit |
| `INT` | `INTEGER` | -2^31 to 2^31-1 | Signed 32-bit |
| `BIGINT` | `BIGINT` | -2^63 to 2^63-1 | Signed 64-bit |

### Floating-Point Types

| SQL Server Type | DuckDB Type | Precision | Notes |
|-----------------|-------------|-----------|-------|
| `REAL` | `FLOAT` | ~7 digits | 32-bit IEEE 754 |
| `FLOAT` | `DOUBLE` | ~15 digits | 64-bit IEEE 754 |

### Decimal/Numeric Types

| SQL Server Type | DuckDB Type | Range | Notes |
|-----------------|-------------|-------|-------|
| `DECIMAL(p,s)` | `DECIMAL(p,s)` | Up to 38 digits | Exact numeric |
| `NUMERIC(p,s)` | `DECIMAL(p,s)` | Up to 38 digits | Same as DECIMAL |
| `MONEY` | `DECIMAL(19,4)` | -922,337,203,685,477.5808 to 922,337,203,685,477.5807 | 8-byte currency |
| `SMALLMONEY` | `DECIMAL(10,4)` | -214,748.3648 to 214,748.3647 | 4-byte currency |

### Character/String Types

| SQL Server Type | DuckDB Type | Encoding | Notes |
|-----------------|-------------|----------|-------|
| `CHAR(n)` | `VARCHAR` | Single-byte | Fixed-length, trailing spaces trimmed |
| `VARCHAR(n)` | `VARCHAR` | Single-byte | Variable-length |
| `VARCHAR(MAX)` | `VARCHAR` | Single-byte | Up to 2GB |
| `NCHAR(n)` | `VARCHAR` | UTF-16 → UTF-8 | Fixed-length Unicode |
| `NVARCHAR(n)` | `VARCHAR` | UTF-16 → UTF-8 | Variable-length Unicode |
| `NVARCHAR(MAX)` | `VARCHAR` | UTF-16 → UTF-8 | Up to 2GB Unicode |

### Binary Types

| SQL Server Type | DuckDB Type | Notes |
|-----------------|-------------|-------|
| `BINARY(n)` | `BLOB` | Fixed-length binary |
| `VARBINARY(n)` | `BLOB` | Variable-length binary |
| `VARBINARY(MAX)` | `BLOB` | Up to 2GB binary |

### Date/Time Types

| SQL Server Type | DuckDB Type | Range | Precision |
|-----------------|-------------|-------|-----------|
| `DATE` | `DATE` | 0001-01-01 to 9999-12-31 | Day |
| `TIME` | `TIME` | 00:00:00 to 23:59:59 | Up to 100ns |
| `SMALLDATETIME` | `TIMESTAMP` | 1900-01-01 to 2079-06-06 | Minute |
| `DATETIME` | `TIMESTAMP` | 1753-01-01 to 9999-12-31 | ~3.33ms |
| `DATETIME2` | `TIMESTAMP` | 0001-01-01 to 9999-12-31 | Up to 100ns |
| `DATETIMEOFFSET` | `TIMESTAMP_TZ` | 0001-01-01 to 9999-12-31 | With timezone |

### Special Types

| SQL Server Type | DuckDB Type | Notes |
|-----------------|-------------|-------|
| `UNIQUEIDENTIFIER` | `UUID` | 16-byte GUID |

### Unsupported Types

The following SQL Server types are **not supported** and will cause an error if queried:

| SQL Server Type | Workaround |
|-----------------|------------|
| `XML` | Cast to `NVARCHAR(MAX)` |
| `TEXT` | Use `VARCHAR(MAX)` instead |
| `NTEXT` | Use `NVARCHAR(MAX)` instead |
| `IMAGE` | Use `VARBINARY(MAX)` instead |
| `SQL_VARIANT` | Cast to specific type |
| `UDT` (User-Defined Types) | Not supported |
| `GEOGRAPHY` | Not supported |
| `GEOMETRY` | Not supported |
| `HIERARCHYID` | Cast to `VARCHAR` |

---

## Filter Pushdown

Filter pushdown optimizes queries by sending WHERE clause conditions to SQL Server, reducing the amount of data transferred over the network.

### How It Works

When you execute a query with a WHERE clause on an attached MSSQL table, the extension analyzes the filters and pushes supported conditions to SQL Server:

```sql
-- DuckDB query
SELECT id, name FROM testdb.dbo.LargeTable WHERE id = 75000;

-- Pushed to SQL Server as:
-- SELECT [id], [name] FROM [dbo].[LargeTable] WHERE [id] = 75000
```

This means only 1 row is transferred instead of all 150,000 rows.

### Supported Filter Types

#### Comparison Operators

| Operator | Example | Pushed SQL |
|----------|---------|------------|
| `=` | `WHERE id = 5` | `WHERE [id] = 5` |
| `<>` or `!=` | `WHERE id <> 5` | `WHERE [id] <> 5` |
| `<` | `WHERE id < 10` | `WHERE [id] < 10` |
| `>` | `WHERE id > 10` | `WHERE [id] > 10` |
| `<=` | `WHERE id <= 10` | `WHERE [id] <= 10` |
| `>=` | `WHERE id >= 10` | `WHERE [id] >= 10` |

#### NULL Checks

| Filter | Example | Pushed SQL |
|--------|---------|------------|
| `IS NULL` | `WHERE value IS NULL` | `WHERE [value] IS NULL` |
| `IS NOT NULL` | `WHERE value IS NOT NULL` | `WHERE [value] IS NOT NULL` |

#### IN Lists

| Filter | Example | Pushed SQL |
|--------|---------|------------|
| `IN (...)` | `WHERE id IN (1, 2, 3)` | `WHERE [id] IN (1, 2, 3)` |

#### BETWEEN Ranges

BETWEEN is converted to a conjunction of comparisons:

| Filter | Example | Pushed SQL |
|--------|---------|------------|
| `BETWEEN` | `WHERE id BETWEEN 100 AND 200` | `WHERE ([id] >= 100 AND [id] <= 200)` |

#### Combined Filters (AND/OR)

| Filter | Example | Pushed SQL |
|--------|---------|------------|
| `AND` | `WHERE id > 5 AND id < 10` | `WHERE ([id] > 5 AND [id] < 10)` |
| `OR` | `WHERE id = 1 OR id = 2` | `WHERE ([id] = 1 OR [id] = 2)` |

### Supported Data Types for Filters

Filter pushdown works with all supported data types:

| Type Category | Example Filter |
|---------------|----------------|
| **Integers** | `WHERE id = 42` |
| **Decimals** | `WHERE price > 99.99` |
| **Strings** | `WHERE name = 'John'` |
| **Dates** | `WHERE created_date = '2024-01-15'` |
| **Timestamps** | `WHERE updated_at >= '2024-01-01 00:00:00'` |
| **UUIDs** | `WHERE guid = '550e8400-e29b-41d4-a716-446655440000'` |
| **Booleans** | `WHERE is_active = true` |

### Date/Time Filter Examples

```sql
-- Date equality
SELECT * FROM testdb.dbo.Orders
WHERE order_date = '2024-01-15';

-- Date range
SELECT * FROM testdb.dbo.Orders
WHERE order_date BETWEEN '2024-01-01' AND '2024-01-31';

-- Timestamp comparison
SELECT * FROM testdb.dbo.Events
WHERE event_time >= '2024-06-15 13:00:00';

-- Combined date and timestamp
SELECT * FROM testdb.dbo.AllDataTypes
WHERE col_date = '2024-06-15'
  AND col_datetime >= '2024-06-15 13:00:00';
```

### String Filter Examples

```sql
-- Exact match (case-sensitivity depends on collation)
SELECT * FROM testdb.dbo.Customers
WHERE name = 'John';

-- Note: LIKE patterns are NOT pushed down
-- This filter stays in DuckDB:
SELECT * FROM testdb.dbo.Customers
WHERE name LIKE 'John%';
```

### Performance Impact

Filter pushdown can dramatically improve query performance:

| Scenario | Without Pushdown | With Pushdown |
|----------|------------------|---------------|
| Select 1 row from 150k | Transfer 150k rows | Transfer 1 row |
| Date range (6 rows) | Transfer 150k rows | Transfer 6 rows |
| ID range (1000 rows) | Transfer 150k rows | Transfer 1000 rows |

### Verifying Filter Pushdown

Enable debug logging to see the actual SQL sent to SQL Server:

```bash
export MSSQL_DEBUG=1
```

Then run your query. You'll see output like:

```
[MSSQL FN] MSSQLCatalogScanInitGlobal: filter pushdown with 1 filter(s)
[MSSQL FN] MSSQLCatalogScanInitGlobal: generated query: SELECT [id], [name] FROM [dbo].[TestTable] WHERE [id] = 5
```

---

## Projection Pushdown

Projection pushdown ensures only the columns you SELECT are retrieved from SQL Server.

### How It Works

```sql
-- DuckDB query (only needs 2 columns)
SELECT id, name FROM testdb.dbo.AllDataTypes;

-- Pushed to SQL Server as:
-- SELECT [id], [name] FROM [dbo].[AllDataTypes]
-- (instead of SELECT * which would fetch all 20+ columns)
```

### Benefits

- **Reduced network traffic**: Only requested columns are transferred
- **Faster queries**: Less data to serialize/deserialize
- **Lower memory usage**: Smaller result sets in memory

---

## Collation Support

The extension detects and respects SQL Server collation settings for string columns.

### Collation Detection

When connecting to a database, the extension queries:
- Database default collation
- Column-specific collations

### Collation Properties

| Property | Detected From | Effect |
|----------|---------------|--------|
| Case Sensitivity | `_CS_` or `_CI_` in collation name | Affects string comparisons |
| Accent Sensitivity | `_AS_` or `_AI_` in collation name | Affects accent handling |
| UTF-8 | `_UTF8` suffix | Encoding handling |

### Examples

| Collation | Case Sensitive | Accent Sensitive |
|-----------|----------------|------------------|
| `SQL_Latin1_General_CP1_CI_AS` | No | Yes |
| `SQL_Latin1_General_CP1_CS_AS` | Yes | Yes |
| `Latin1_General_100_CI_AI_SC_UTF8` | No | No |

---

## Known Limitations

### 1. TINYINT Type

SQL Server TINYINT is unsigned (0-255), mapping to DuckDB's `UTINYINT`. Some operations may expect signed integers:

```sql
-- If you get type mismatch errors, cast to INTEGER:
SELECT CAST(col_tinyint AS INTEGER) FROM table;
```

### 2. COUNT(*) Not Supported

Virtual columns require projection pushdown. Use a specific column instead:

```sql
-- Won't work
SELECT COUNT(*) FROM testdb.dbo.table;

-- Works
SELECT COUNT(id) FROM testdb.dbo.table;
```

### 3. LIKE Patterns Not Pushed

LIKE, ILIKE, and regex patterns are not pushed to SQL Server:

```sql
-- Filter stays in DuckDB (all rows fetched first):
SELECT * FROM testdb.dbo.Customers WHERE name LIKE 'J%';
```

### 4. Functions Not Pushed

Expressions and functions in filters are not pushed:

```sql
-- Not pushed (YEAR function applied in DuckDB):
SELECT * FROM testdb.dbo.Orders WHERE YEAR(order_date) = 2024;

-- Better: Use date range (pushed to SQL Server):
SELECT * FROM testdb.dbo.Orders
WHERE order_date >= '2024-01-01' AND order_date < '2025-01-01';
```

### 5. Unsupported Column Types

Tables with unsupported column types (XML, geography, etc.) cannot be queried directly. Either:
- Exclude those columns in your SELECT
- Create a view in SQL Server that casts the columns

```sql
-- Create a view in SQL Server
CREATE VIEW dbo.MyTableClean AS
SELECT id, name, CAST(xml_col AS NVARCHAR(MAX)) AS xml_text
FROM dbo.MyTable;

-- Query the view from DuckDB
SELECT * FROM testdb.dbo.MyTableClean;
```

---

## Summary

| Feature | Status | Notes |
|---------|--------|-------|
| **Data Types** | 25+ types supported | See type mapping table |
| **Filter Pushdown** | Full support | Comparisons, NULL, IN, AND/OR |
| **Projection Pushdown** | Full support | Only requested columns fetched |
| **Collation** | Detected | Case/accent sensitivity tracked |

For additional details, see:
- [TESTING.md](TESTING.md) - Testing guide with examples
- [../MEMORY.md](../MEMORY.md) - Quick reference

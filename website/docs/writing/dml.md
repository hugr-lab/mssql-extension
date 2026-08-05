---
title: INSERT / UPDATE / DELETE
sidebar_position: 4
---

# INSERT

### Basic INSERT

```sql
-- Single row
INSERT INTO sqlserver.dbo.my_table (name, value)
VALUES ('test', 42);

-- Multiple rows
INSERT INTO sqlserver.dbo.my_table (name, value)
VALUES ('first', 1), ('second', 2), ('third', 3);
```

### INSERT from SELECT

```sql
INSERT INTO sqlserver.dbo.target_table (name, value)
SELECT name, value FROM local_source_table;
```

### INSERT with RETURNING

Get inserted values back (uses SQL Server's OUTPUT INSERTED):

```sql
INSERT INTO sqlserver.dbo.my_table (name)
VALUES ('test')
RETURNING id, name;
```

```sql
INSERT INTO sqlserver.dbo.my_table (name, value)
VALUES ('a', 1), ('b', 2)
RETURNING *;
```

### Batch Configuration

Large inserts are automatically batched. Configure batch size:

```sql
-- Set batch size (default: 1000, SQL Server limit)
SET mssql_insert_batch_size = 500;

-- Maximum SQL statement size (default: 8MB)
SET mssql_insert_max_sql_bytes = 4194304;
```

### Identity Columns

Identity (auto-increment) columns are automatically excluded from INSERT statements. The generated values are returned via RETURNING clause.

## UPDATE

UPDATE operations are supported for tables with primary keys. The extension uses rowid-based targeting for efficient updates.

### Basic UPDATE

```sql
-- Update single row
UPDATE sqlserver.dbo.products SET price = 19.99 WHERE id = 1;

-- Update multiple rows
UPDATE sqlserver.dbo.products SET status = 'discontinued' WHERE category = 'legacy';

-- Update with expressions
UPDATE sqlserver.dbo.products SET price = price * 1.10 WHERE category = 'premium';
```

### UPDATE with Multiple Columns

```sql
UPDATE sqlserver.dbo.customers
SET name = 'John Doe', email = 'john@example.com', updated_at = NOW()
WHERE id = 42;
```

### Batch Configuration

Large updates are automatically batched:

```sql
-- Set batch size (default: 500)
SET mssql_dml_batch_size = 500;
```

### Limitations

- **RETURNING clause is not supported** for UPDATE operations
- Tables must have a primary key (uses rowid for row identification)
- Updates use a single `UPDATE ... FROM target JOIN (VALUES ...)` statement per batch, joining on the primary key (scalar or composite)

## DELETE

DELETE operations are supported for tables with primary keys.

### Basic DELETE

```sql
-- Delete single row
DELETE FROM sqlserver.dbo.products WHERE id = 1;

-- Delete multiple rows
DELETE FROM sqlserver.dbo.products WHERE status = 'discontinued';

-- Delete all rows (use with caution)
DELETE FROM sqlserver.dbo.products;
```

### DELETE with Complex Conditions

```sql
DELETE FROM sqlserver.dbo.order_items
WHERE order_id IN (SELECT id FROM sqlserver.dbo.orders WHERE status = 'cancelled');
```

### Batch Configuration

Large deletes are automatically batched:

```sql
-- Set batch size (default: 500)
SET mssql_dml_batch_size = 500;
```

### Limitations

- **RETURNING clause is not supported** for DELETE operations
- Tables must have a primary key (uses rowid for row identification)


# Quickstart: DML UPDATE/DELETE

**Feature**: 002-dml-update-delete
**Date**: 2026-01-25

## Prerequisites

1. DuckDB with MSSQL extension loaded
2. SQL Server with tables containing primary keys
3. Write access to target tables

## Basic Usage

### Attach MSSQL Database

```sql
-- Using DSN
ATTACH 'Server=localhost,1433;Database=TestDB;User Id=sa;Password=TestPassword1' AS mssql (TYPE mssql);

-- Or using secret
CREATE SECRET mssql_secret (
    TYPE mssql,
    SERVER 'localhost',
    PORT 1433,
    DATABASE 'TestDB',
    USER 'sa',
    PASSWORD 'TestPassword1'
);
ATTACH '' AS mssql (TYPE mssql, SECRET mssql_secret);
```

### UPDATE Operations

```sql
-- Single row update
UPDATE mssql.dbo.products
SET price = 99.99
WHERE id = 1;

-- Bulk update with filter
UPDATE mssql.dbo.orders
SET status = 'archived', updated_at = NOW()
WHERE created_date < '2024-01-01';

-- Update with expression
UPDATE mssql.dbo.products
SET price = price * 1.1
WHERE category = 'electronics';

-- Update with NULL
UPDATE mssql.dbo.users
SET middle_name = NULL
WHERE id = 42;
```

### DELETE Operations

```sql
-- Single row delete
DELETE FROM mssql.dbo.logs
WHERE id = 12345;

-- Bulk delete with filter
DELETE FROM mssql.dbo.audit_logs
WHERE log_date < '2023-01-01';

-- Delete with subquery filter
DELETE FROM mssql.dbo.orders
WHERE customer_id IN (
    SELECT id FROM mssql.dbo.customers WHERE inactive = true
);
```

## Configuration

### Batch Size Settings

```sql
-- Set batch size (default: 500)
SET mssql_dml_batch_size = 1000;

-- Set max parameters (default: 2000, SQL Server limit ~2100)
SET mssql_dml_max_parameters = 1500;

-- Enable/disable prepared statements (default: true)
SET mssql_dml_use_prepared = true;
```

### View Current Settings

```sql
SELECT * FROM duckdb_settings()
WHERE name LIKE 'mssql_dml%';
```

## Error Handling

### Tables Without Primary Key

```sql
-- This will fail with clear error message
UPDATE mssql.dbo.heap_table SET col = 'value';
-- Error: MSSQL: UPDATE/DELETE requires a primary key
```

### PK Column Modification

```sql
-- This will fail before any modification
UPDATE mssql.dbo.products SET id = id + 1000;
-- Error: MSSQL: updating primary key columns is not supported
```

## Performance Tips

### Large-Scale Operations

For updating or deleting many rows:

```sql
-- Increase batch size for fewer round-trips
SET mssql_dml_batch_size = 1000;

-- Execute the operation
DELETE FROM mssql.dbo.large_logs
WHERE created_date < '2020-01-01';
```

### Monitor Progress

DML operations report row counts:

```sql
D UPDATE mssql.dbo.products SET status = 'active' WHERE status = 'pending';
-- 15000 rows affected
```

## Testing Setup

### Start Test Environment

```bash
# Start SQL Server container
make docker-up

# Verify connection
make docker-status
```

### Run Integration Tests

```bash
# All integration tests
make integration-test

# Specific DML tests
build/release/test/unittest "test/sql/dml/*" --force-reload
```

## Troubleshooting

### Debug Mode

```bash
# Enable debug logging
export MSSQL_DEBUG=1
./build/release/duckdb

# Then run your DML operations
```

### Common Issues

| Issue | Cause | Solution |
|-------|-------|----------|
| "requires a primary key" | Table has no PK | Add PK to table or use direct SQL |
| "updating primary key columns" | SET clause includes PK | Remove PK from SET clause |
| Batch timeout | Very large batch | Reduce `mssql_dml_batch_size` |
| Parameter limit exceeded | Too many columns × rows | Reduce batch size |

---
title: Catalog Integration
sidebar_position: 1
---

# Catalog Integration

### Attaching and Detaching

```sql
-- Attach with secret
ATTACH '' AS sqlserver (TYPE mssql, SECRET my_secret);

-- Attach with connection string
ATTACH 'Server=localhost,1433;Database=master;User Id=sa;Password=pass'
    AS sqlserver (TYPE mssql);

-- Detach when done
DETACH sqlserver;
```

### Schema Browsing

```sql
-- List all schemas
SELECT schema_name FROM duckdb_schemas() WHERE database_name = 'sqlserver';

-- List tables in a schema
SELECT table_name FROM duckdb_tables() WHERE database_name = 'sqlserver' AND schema_name = 'dbo';

-- Describe table structure (list columns)
SELECT column_name, data_type, is_nullable
FROM duckdb_columns()
WHERE database_name = 'sqlserver' AND schema_name = 'dbo' AND table_name = 'my_table';
```

### Three-Part Naming

Access SQL Server tables using `context.schema.table` naming:

```sql
SELECT id, name, created_at
FROM sqlserver.dbo.customers
WHERE status = 'active'
LIMIT 100;
```

### Cross-Catalog Joins

Join SQL Server tables with local DuckDB tables:

```sql
-- Create local table
CREATE TABLE local_data (customer_id INTEGER, extra_info VARCHAR);

-- Join with SQL Server
SELECT c.id, c.name, l.extra_info
FROM sqlserver.dbo.customers c
JOIN local_data l ON c.id = l.customer_id;
```


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

### The default schema is `dbo`

After `USE <catalog>`, unqualified names resolve against `dbo`:

```sql
USE sqlserver;

SELECT current_schema();
-- dbo

-- resolves as sqlserver.dbo.customers
SELECT id FROM customers;
```

This matters beyond convenience. DuckDB's own default is `main`, which no SQL
Server database has, so anything that asks the catalog for its default schema —
generic tooling, or an extension layering its own catalog on top of this one —
used to fail with `Schema 'main' not found in MSSQL database` before it could
read anything. The workaround in the wild was to create a schema literally named
`main` on the server.

`dbo` is a constant, not a per-login lookup: it is the default for every login
that has not been given another, and resolving it per connection would cost a
round trip on every `ATTACH`. If your login's default schema is something else,
address those tables by their qualified name — `catalog.schema.table` works
regardless, and is what the three-part naming above is for.

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


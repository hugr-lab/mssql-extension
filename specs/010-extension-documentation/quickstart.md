# Quick Start Reference: Extension Documentation

**Date**: 2026-01-19
**Feature**: 010-extension-documentation

## Minimal Quick Start Flow

This is the content that will appear in the Quick Start section of README.md.

### Prerequisites

- DuckDB v1.0+ installed
- SQL Server 2019+ accessible on network

### Step 1: Install Extension

```sql
INSTALL mssql FROM community;
LOAD mssql;
```

### Step 2: Connect to SQL Server

**Option A: Using a Secret (Recommended)**

```sql
CREATE SECRET my_sqlserver (
    TYPE mssql,
    host 'localhost',
    port 1433,
    database 'master',
    user 'sa',
    password 'YourPassword123'
);

ATTACH '' AS sqlserver (TYPE mssql, SECRET my_sqlserver);
```

**Option B: Using Connection String**

```sql
ATTACH 'Server=localhost,1433;Database=master;User Id=sa;Password=YourPassword123'
    AS sqlserver (TYPE mssql);
```

### Step 3: Query Data

```sql
-- List schemas
SHOW SCHEMAS FROM sqlserver;

-- List tables in dbo schema
SHOW TABLES FROM sqlserver.dbo;

-- Query a table
SELECT * FROM sqlserver.dbo.my_table LIMIT 10;
```

### Expected Output

```
┌──────────────┐
│ schema_name  │
│   varchar    │
├──────────────┤
│ dbo          │
│ sys          │
│ ...          │
└──────────────┘
```

## Common Errors and Solutions

### Connection Refused

```
Error: Failed to connect to SQL Server: Connection refused
```

**Solution**: Verify SQL Server is running and accessible:

- Check hostname and port
- Verify firewall allows TCP/1433
- Ensure SQL Server is configured for TCP/IP connections

### Login Failed

```
Error: Login failed for user 'sa'
```

**Solution**: Verify credentials:

- Check username and password
- Ensure SQL Server authentication is enabled (not Windows-only)
- Check user has access to specified database

### TLS Required

```
Error: Server requires encryption but TLS is not available
```

**Solution**: Use the loadable extension with TLS support:

```sql
-- Unload static extension if loaded
-- Load TLS-enabled extension
INSTALL mssql FROM community;
LOAD mssql;

-- Use encryption in connection
CREATE SECRET my_sqlserver (
    TYPE mssql,
    host 'localhost',
    port 1433,
    database 'master',
    user 'sa',
    password 'YourPassword123',
    use_encrypt true
);
```

## Quick Reference Examples

### Insert Data

```sql
INSERT INTO sqlserver.dbo.my_table (name, value)
VALUES ('test', 42);
```

### Insert with RETURNING

```sql
INSERT INTO sqlserver.dbo.my_table (name)
VALUES ('test')
RETURNING id, name;
```

### Execute Raw SQL

```sql
SELECT * FROM mssql_execute('sqlserver', 'EXEC sp_who2');
```

### Check Connection Pool

```sql
SELECT * FROM mssql_pool_stats('sqlserver');
```

### Disconnect

```sql
DETACH sqlserver;
DROP SECRET my_sqlserver;
```

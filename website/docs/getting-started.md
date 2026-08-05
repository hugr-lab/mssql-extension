---
title: Getting Started
sidebar_position: 2
---

# Quick Start

### Prerequisites

- DuckDB v1.4.1 or later (minimum supported version)
- SQL Server 2019 or later accessible on network

### Step 1: Install Extension

```sql
INSTALL mssql FROM community;
LOAD mssql;
```

### Step 2: Connect to SQL Server

#### Option A: Using a Secret (Recommended)

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

#### Option B: Using Connection String

```sql
ATTACH 'Server=localhost,1433;Database=master;User Id=sa;Password=YourPassword123'
    AS sqlserver (TYPE mssql);
```

### Step 3: Query Data

```sql
-- List schemas
SELECT schema_name FROM duckdb_schemas() WHERE database_name = 'sqlserver';

-- List tables in dbo schema
SELECT table_name FROM duckdb_tables() WHERE database_name = 'sqlserver' AND schema_name = 'dbo';

-- Query a table
FROM sqlserver.dbo.my_table LIMIT 10;
```

### Step 4: Disconnect

```sql
DETACH sqlserver;
DROP SECRET my_sqlserver;
```


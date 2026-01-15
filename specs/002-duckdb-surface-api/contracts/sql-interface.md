# SQL Interface Contract: MSSQL Extension

**Feature**: 002-duckdb-surface-api
**Date**: 2026-01-15

This document defines the SQL-level interface for the MSSQL extension.

---

## 1. Secret Management

### CREATE SECRET

Creates a new MSSQL secret with connection credentials.

```sql
CREATE SECRET <name> (
    TYPE mssql,
    host '<hostname>',
    port <port_number>,
    database '<database_name>',
    user '<username>',
    password '<password>',
    use_ssl <boolean>  -- Optional
);
```

**Parameters**:

| Parameter | Type | Required | Constraints | Description |
|-----------|------|----------|-------------|-------------|
| name | identifier | Yes | Unique | Secret name for reference |
| host | VARCHAR | Yes | Non-empty | SQL Server hostname or IP address |
| port | INTEGER | Yes | 1-65535 | TCP port number (default: 1433) |
| database | VARCHAR | Yes | Non-empty | Target database name |
| user | VARCHAR | Yes | Non-empty | SQL Server username |
| password | VARCHAR | Yes | - | SQL Server password |
| use_ssl | BOOLEAN | No | true/false | Enable SSL/TLS encryption (default: false) |

**Examples**:

```sql
-- Basic secret creation
CREATE SECRET my_sqlserver (
    TYPE mssql,
    host 'db.example.com',
    port 1433,
    database 'production',
    user 'app_user',
    password 'secure_password'
);

-- Using non-standard port
CREATE SECRET dev_server (
    TYPE mssql,
    host 'localhost',
    port 14330,
    database 'dev_db',
    user 'sa',
    password 'DevPassword123!'
);

-- With SSL enabled
CREATE SECRET secure_server (
    TYPE mssql,
    host 'secure.example.com',
    port 1433,
    database 'secure_db',
    user 'admin',
    password 'SecurePass!',
    use_ssl true
);
```

**Error Conditions**:

| Condition | Error Message |
|-----------|---------------|
| Missing host | `MSSQL Error: Missing required field 'host'. Provide host parameter when creating secret.` |
| Missing port | `MSSQL Error: Missing required field 'port'. Provide port parameter when creating secret.` |
| Missing database | `MSSQL Error: Missing required field 'database'. Provide database parameter when creating secret.` |
| Missing user | `MSSQL Error: Missing required field 'user'. Provide user parameter when creating secret.` |
| Missing password | `MSSQL Error: Missing required field 'password'. Provide password parameter when creating secret.` |
| Invalid port | `MSSQL Error: Port must be between 1 and 65535. Got: <value>` |
| Empty host | `MSSQL Error: Field 'host' cannot be empty.` |
| Duplicate name | `Secret with name '<name>' already exists` |

### DROP SECRET

Removes an existing secret.

```sql
DROP SECRET <name>;
```

**Notes**: Standard DuckDB secret management. No MSSQL-specific behavior.

### Query Secrets

View registered secrets (passwords redacted).

```sql
SELECT * FROM duckdb_secrets() WHERE type = 'mssql';
```

**Output Columns**:

| Column | Type | Description |
|--------|------|-------------|
| name | VARCHAR | Secret name |
| type | VARCHAR | Always 'mssql' |
| provider | VARCHAR | Always 'config' |
| scope | VARCHAR[] | Empty for MSSQL secrets |
| secret_string | VARCHAR | Redacted output (password hidden) |

---

## 2. Database Attachment

### ATTACH

Attaches a SQL Server database as a named context. Two methods are supported:

#### Method 1: Using a Secret

```sql
ATTACH '' AS <context_name> (TYPE mssql, SECRET <secret_name>);
```

#### Method 2: Using a Connection String

```sql
ATTACH '<connection_string>' AS <context_name> (TYPE mssql);
```

**Connection String Format**:
```
Server=<host>[,<port>];Database=<database>;User Id=<user>;Password=<password>[;Encrypt=yes/no]
```

**Connection String Keys** (case-insensitive):

| Key | Aliases | Description |
|-----|---------|-------------|
| Server | Data Source | Hostname, optionally with port (e.g., `host,1433`) |
| Database | Initial Catalog | Target database name |
| User Id | UID, User | SQL Server username |
| Password | PWD | SQL Server password |
| Encrypt | Use Encryption for Data | Enable SSL (yes/no, default: no) |

**Parameters**:

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| context_name | identifier | Yes | Name for the attached database |
| secret_name | identifier | Conditional | Reference to existing MSSQL secret (if not using connection string) |
| connection_string | VARCHAR | Conditional | Connection string (if not using secret) |

**Notes**:
- Either SECRET or connection string must be provided, not both
- When using SECRET, the path (first parameter) must be empty string `''`
- Connection is established lazily (on first data access)
- Context name must be unique among all attached databases
- Default port is 1433 if not specified in connection string

**Examples**:

```sql
-- Attach using a secret
ATTACH '' AS sqlserver (TYPE mssql, SECRET my_sqlserver);

-- Attach with different context name
ATTACH '' AS production_db (TYPE mssql, SECRET prod_secret);

-- Attach using connection string
ATTACH 'Server=localhost,1433;Database=mydb;User Id=sa;Password=pass' AS conn_db (TYPE mssql);

-- Connection string with default port
ATTACH 'Server=db.example.com;Database=prod;User Id=admin;Password=secret' AS prod_db (TYPE mssql);

-- Connection string with SSL enabled
ATTACH 'Server=secure.example.com;Database=db;User Id=user;Password=pass;Encrypt=yes' AS secure_db (TYPE mssql);

-- Connection string with alternative key names
ATTACH 'Data Source=localhost;Initial Catalog=testdb;UID=testuser;PWD=testpass' AS alt_db (TYPE mssql);
```

**Error Conditions**:

| Condition | Error Message |
|-----------|---------------|
| Neither secret nor connection string | `MSSQL Error: Either SECRET or connection string is required for ATTACH.` |
| Secret not found | `Secret '<name>' not found` |
| Invalid secret type | `Secret '<name>' is not of type 'mssql'` |
| Duplicate context | `Database '<name>' already exists` |
| Missing Server in connection string | `MSSQL Error: Missing 'Server' in connection string.` |
| Missing Database in connection string | `MSSQL Error: Missing 'Database' in connection string.` |
| Missing User Id in connection string | `MSSQL Error: Missing 'User Id' in connection string.` |
| Missing Password in connection string | `MSSQL Error: Missing 'Password' in connection string.` |
| Invalid port in connection string | `MSSQL Error: Port must be between 1 and 65535. Got: <value>` |

### DETACH

Removes an attached SQL Server database.

```sql
DETACH <context_name>;
```

**Behavior**:
- Immediately aborts any in-progress queries on the context
- Closes any open network connections
- Removes the context from the registry
- Cleans up all associated metadata

**Examples**:

```sql
-- Detach a database
DETACH sqlserver;
```

**Error Conditions**:

| Condition | Error Message |
|-----------|---------------|
| Context not found | `Database '<name>' not found` |

---

## 3. Table Functions

### mssql_execute

Executes a raw SQL statement against SQL Server.

```sql
SELECT * FROM mssql_execute('<context_name>', '<sql_statement>');
```

**Parameters**:

| Parameter | Type | Description |
|-----------|------|-------------|
| context_name | VARCHAR | Name of attached MSSQL database |
| sql_statement | VARCHAR | SQL statement to execute |

**Return Schema**:

| Column | Type | Description |
|--------|------|-------------|
| success | BOOLEAN | `true` if execution succeeded |
| affected_rows | BIGINT | Number of rows affected, or -1 if not applicable |
| message | VARCHAR | Status message or error description |

**Examples**:

```sql
-- Execute an UPDATE statement
SELECT * FROM mssql_execute('sqlserver', 'UPDATE users SET active = 1 WHERE id = 5');

-- Execute a DDL statement
SELECT * FROM mssql_execute('sqlserver', 'CREATE INDEX idx_name ON users(name)');

-- Check result
SELECT success, affected_rows, message
FROM mssql_execute('sqlserver', 'DELETE FROM logs WHERE created_at < ''2024-01-01''');
```

**Return Values**:

| Scenario | success | affected_rows | message |
|----------|---------|---------------|---------|
| Successful DML | true | N (rows affected) | "Query executed successfully" |
| Successful DDL | true | -1 | "Query executed successfully" |
| SQL Error | false | -1 | Error message from SQL Server |
| Stub mode | true | 1 | "Query executed successfully (stub)" |

**Error Conditions**:

| Condition | Error Message |
|-----------|---------------|
| Context not found | `MSSQL Error: Unknown context '<name>'. Attach a database first with: ATTACH '' AS <name> TYPE mssql (SECRET ...)` |

### mssql_scan

Executes a SELECT query and returns results as a relation.

```sql
SELECT * FROM mssql_scan('<context_name>', '<select_query>');
```

**Parameters**:

| Parameter | Type | Description |
|-----------|------|-------------|
| context_name | VARCHAR | Name of attached MSSQL database |
| select_query | VARCHAR | SELECT statement to execute |

**Return Schema** (stub implementation):

| Column | Type | Description |
|--------|------|-------------|
| id | INTEGER | Sample row identifier |
| name | VARCHAR | Sample name value |

**Notes**:
- In stub mode, returns 3 hardcoded sample rows regardless of query
- Future implementation will return actual query results with dynamic schema

**Examples**:

```sql
-- Basic scan
SELECT * FROM mssql_scan('sqlserver', 'SELECT id, name FROM users');

-- With filtering (applied after scan in stub mode)
SELECT * FROM mssql_scan('sqlserver', 'SELECT * FROM products')
WHERE id > 1;

-- Join with local data
SELECT s.id, s.name, l.extra
FROM mssql_scan('sqlserver', 'SELECT id, name FROM users') s
JOIN local_table l ON s.id = l.user_id;
```

**Stub Return Data**:

| id | name |
|----|------|
| 1 | Name 1 |
| 2 | Name 2 |
| 3 | Name 3 |

**Error Conditions**:

| Condition | Error Message |
|-----------|---------------|
| Context not found | `MSSQL Error: Unknown context '<name>'. Attach a database first with: ATTACH '' AS <name> TYPE mssql (SECRET ...)` |

---

## 4. Information Functions

### mssql_version

Returns the extension version string.

```sql
SELECT mssql_version();
```

**Return Type**: VARCHAR

**Example Output**: `"a1b2c3d"` (DuckDB commit hash)

---

## 5. Complete Usage Example

```sql
-- 1. Create a secret with connection credentials
CREATE SECRET prod_db (
    TYPE mssql,
    host 'sql.example.com',
    port 1433,
    database 'production',
    user 'readonly_user',
    password 'SecurePass123!'
);

-- 2. Attach the database
ATTACH '' AS prod TYPE mssql (SECRET prod_db);

-- 3. Query data (stub returns sample data)
SELECT * FROM mssql_scan('prod', 'SELECT id, name FROM customers');

-- 4. Execute a statement (stub returns success)
SELECT * FROM mssql_execute('prod', 'UPDATE metrics SET last_access = GETDATE()');

-- 5. Clean up
DETACH prod;
DROP SECRET prod_db;
```

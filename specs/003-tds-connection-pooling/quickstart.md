# Quickstart: TDS Connection, Authentication, and Pooling

**Branch**: `003-tds-connection-pooling`
**Date**: 2026-01-15

## Prerequisites

- DuckDB with mssql extension loaded
- SQL Server 2019+ instance with TCP/IP enabled
- SQL Server authentication credentials

## 1. Create a Secret (from Spec 002)

```sql
CREATE SECRET my_sqlserver (
    TYPE mssql,
    host 'sqlserver.example.com',
    port 1433,
    database 'mydb',
    user 'myuser',
    password 'mypassword'
);
```

## 2. Configure Pool Settings (Optional)

```sql
-- Adjust connection pool behavior
SET mssql_connection_limit = 10;       -- Max 10 connections per context
SET mssql_connection_timeout = 30;     -- 30 second connect timeout
SET mssql_idle_timeout = 300;          -- Close idle connections after 5 minutes
SET mssql_acquire_timeout = 30;        -- Wait up to 30s for connection
```

## 3. Test Connection with Diagnostic Functions

```sql
-- Open a test connection (scalar function)
SELECT mssql_open('my_sqlserver') AS handle;
-- Returns: 1

-- Check if connection is alive
SELECT mssql_ping(1);  -- Use handle from above
-- Returns: true

-- Close the connection
SELECT mssql_close(1);
-- Returns: true

-- Chain operations
SELECT mssql_close(mssql_open('my_sqlserver'));
-- Returns: true
```

## 4. View Pool Statistics

```sql
-- After attaching a database (spec 002)
ATTACH '' AS mydb (TYPE mssql, SECRET my_sqlserver);

-- Check pool health
SELECT * FROM mssql_pool_stats('mydb');
```

Returns:

| Column | Type | Description |
|--------|------|-------------|
| total_connections | BIGINT | Current pool size |
| idle_connections | BIGINT | Available connections |
| active_connections | BIGINT | In-use connections |
| connections_created | BIGINT | Lifetime created |
| connections_closed | BIGINT | Lifetime closed |

## Connection States

Connections progress through these states:

```
disconnected → authenticating → idle ⇄ executing
                                        ↓
                                   cancelling
```

- **disconnected**: No TCP connection
- **authenticating**: PRELOGIN/LOGIN7 handshake in progress
- **idle**: Ready for queries
- **executing**: Query in progress
- **cancelling**: Attention signal sent, awaiting acknowledgment

## Error Handling

| Error | Cause | Solution |
|-------|-------|----------|
| Connection timeout | Server unreachable | Check network, firewall, port |
| Authentication failed | Invalid credentials | Verify username/password |
| Pool exhausted | All connections in use | Increase `mssql_connection_limit` or wait |
| Acquire timeout | Pool exhausted too long | Increase `mssql_acquire_timeout` |

## Development Testing

For local development with SQL Server in Docker:

```bash
docker run -e "ACCEPT_EULA=Y" -e "MSSQL_SA_PASSWORD=YourStrong!Password" \
    -p 1433:1433 --name sql1 \
    -d mcr.microsoft.com/mssql/server:2019-latest
```

Then connect:

```sql
CREATE SECRET local_sql (
    TYPE mssql,
    host 'localhost',
    port 1433,
    database 'master',
    user 'sa',
    password 'YourStrong!Password'
);

SELECT mssql_open('local_sql');
-- Returns: 1
```

# Quickstart: Pool Stats and Connection Validation

**Feature**: 023-pool-stats-validation
**Date**: 2026-01-28

## What's New

This release adds five improvements to the mssql-extension:

1. **TLS by Default** - Connections now use TLS encryption by default
2. **TLS Validation** - TLS configuration is validated at ATTACH time
3. **Query Timeout** - Configurable query timeout via `mssql_query_timeout` setting
4. **Catalog-Free Mode** - Connect without catalog integration for restricted databases
5. **Extended Pool Stats** - Track active and pinned connections

---

## TLS by Default

Connections now use TLS encryption by default. If you need to connect to a server without TLS:

```sql
-- Old behavior (no longer default)
ATTACH 'Server=myserver;Database=mydb;User Id=user;Password=pass' AS ctx (TYPE mssql);

-- Explicitly disable TLS for legacy servers
ATTACH 'Server=myserver;Database=mydb;User Id=user;Password=pass;Encrypt=no' AS ctx (TYPE mssql);
```

If the server requires TLS and you've explicitly disabled it, you'll see:

```
Error: Server requires encryption (ENCRYPT_REQ) but use_encrypt=false.
Set use_encrypt=true or Encrypt=yes in connection string.
```

---

## Query Timeout Configuration

Configure how long queries can run before timing out:

```sql
-- Increase timeout for long-running queries (120 seconds)
SET mssql_query_timeout = 120;

-- Disable timeout entirely (infinite wait)
SET mssql_query_timeout = 0;

-- Reset to default (30 seconds)
RESET mssql_query_timeout;
```

When a query times out:

```
Error: MSSQL query timed out after 30 seconds.
Use SET mssql_query_timeout to increase the timeout limit.
```

---

## Catalog-Free Mode

Connect to databases without querying system tables (useful for serverless or restricted permissions):

### Using Secret

```sql
CREATE SECRET mssql_serverless (
    TYPE mssql,
    host 'myserver.database.windows.net',
    port 1433,
    database 'mydb',
    user 'admin',
    password 'secret',
    catalog false  -- Disable catalog integration
);

ATTACH '' AS ctx (TYPE mssql, SECRET mssql_serverless);

-- These work:
SELECT * FROM mssql_scan('ctx', 'SELECT * FROM dbo.mytable');
SELECT mssql_exec('ctx', 'INSERT INTO dbo.mytable VALUES (1)');

-- This fails with clear error:
SELECT * FROM ctx.dbo.mytable;
-- Error: Catalog integration is disabled. Use mssql_scan() instead.
```

### Using Connection String

```sql
ATTACH 'Server=host;Database=db;User Id=user;Password=pass;Catalog=no' AS ctx (TYPE mssql);
```

---

## Extended Pool Statistics

Monitor connection usage with new columns:

```sql
SELECT
    db,
    idle_connections,
    active_connections,
    pinned_connections  -- NEW: connections held by transactions
FROM mssql_pool_stats();
```

### Understanding pinned_connections

Pinned connections are held for the duration of a transaction:

```sql
BEGIN;
-- pinned_connections increases by 1

SELECT * FROM mydb.dbo.orders;
-- Connection stays pinned

COMMIT;
-- pinned_connections decreases by 1
```

### Diagnosing Issues

```sql
-- Check for connection leaks
SELECT * FROM mssql_pool_stats()
WHERE active_connections - pinned_connections > 5;
-- Many non-pinned active connections may indicate leaks

-- Check pool exhaustion
SELECT * FROM mssql_pool_stats()
WHERE active_connections = total_connections;
```

---

## Migration Guide

### For Existing Users

**Breaking Change**: TLS is now enabled by default.

If your SQL Server doesn't support TLS:

```sql
-- Add Encrypt=no to your connection string
ATTACH 'Server=host;Database=db;User Id=user;Password=pass;Encrypt=no' AS ctx (TYPE mssql);

-- Or add use_encrypt=false to your secret
CREATE SECRET mysecret (
    TYPE mssql,
    ...
    use_encrypt false
);
```

### For New Users

No changes needed - TLS will work automatically with servers that support it.

---

## Common Errors and Solutions

| Error | Cause | Solution |
|-------|-------|----------|
| "Server requires encryption but use_encrypt=false" | Server needs TLS but you disabled it | Remove `Encrypt=no` from connection string |
| "TLS handshake failed" | Certificate or network issue | Check server certificate, firewall rules |
| "Query timed out after N seconds" | Query took too long | Increase `mssql_query_timeout` |
| "Catalog integration is disabled" | Using `catalog=false` | Use `mssql_scan()` instead of direct table access |

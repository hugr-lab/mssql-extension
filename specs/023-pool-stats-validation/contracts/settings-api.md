# Settings API Contract

**Feature**: 023-pool-stats-validation
**Date**: 2026-01-28

## New Setting: mssql_query_timeout

### Configuration

```sql
-- Set query timeout to 120 seconds
SET mssql_query_timeout = 120;

-- Set infinite timeout (no timeout)
SET mssql_query_timeout = 0;

-- Reset to default (30 seconds)
RESET mssql_query_timeout;
```

### Query Current Value

```sql
SELECT current_setting('mssql_query_timeout');
-- Returns: 30 (default)
```

### Behavior

| Value | Behavior |
|-------|----------|
| `> 0` | Query times out after specified seconds |
| `= 0` | No timeout (infinite wait) |
| `< 0` | Invalid, rejected with error |

### Error Messages

```
-- Timeout error
Error: MSSQL query timed out after 30 seconds. Use SET mssql_query_timeout to increase.

-- Invalid value
Error: Invalid value for mssql_query_timeout: must be >= 0
```

---

## Secret API Changes

### Create Secret with Catalog Disabled

```sql
CREATE SECRET mssql_serverless (
    TYPE mssql,
    host 'serverless.database.windows.net',
    port 1433,
    database 'mydb',
    user 'admin',
    password 'secret',
    use_encrypt true,
    catalog false  -- NEW: Disable catalog integration
);
```

### Secret Field Validation

| Field | Type | Required | Default | Notes |
|-------|------|----------|---------|-------|
| `host` | STRING | Yes | - | SQL Server hostname |
| `port` | INTEGER | No | 1433 | TCP port |
| `database` | STRING | Yes | - | Database name |
| `user` | STRING | Yes | - | Username |
| `password` | STRING | Yes | - | Password |
| `use_encrypt` | BOOLEAN | No | `true` | **CHANGED**: Default from false to true |
| `catalog` | BOOLEAN | No | `true` | **NEW**: Enable/disable catalog integration |

---

## Connection String API Changes

### Catalog Parameter

```sql
-- With catalog enabled (default)
ATTACH 'Server=host;Database=db;User Id=user;Password=pass' AS ctx (TYPE mssql);

-- With catalog disabled
ATTACH 'Server=host;Database=db;User Id=user;Password=pass;Catalog=no' AS ctx (TYPE mssql);
```

### Supported Parameter Aliases

| Parameter | Aliases | Values |
|-----------|---------|--------|
| `Catalog` | `catalog`, `use_catalog` | `yes`/`true`/`1` (enabled) or `no`/`false`/`0` (disabled) |
| `Encrypt` | `use encryption for data` | `yes`/`true`/`1` (TLS) or `no`/`false`/`0` (no TLS) |

### Encryption Default Change

```sql
-- Old behavior (pre-023): No encryption unless specified
ATTACH 'Server=host;Database=db;User Id=user;Password=pass' AS ctx (TYPE mssql);
-- → use_encrypt = false

-- New behavior (023+): TLS enabled by default
ATTACH 'Server=host;Database=db;User Id=user;Password=pass' AS ctx (TYPE mssql);
-- → use_encrypt = true

-- Explicit disable for legacy servers
ATTACH 'Server=host;Database=db;User Id=user;Password=pass;Encrypt=no' AS ctx (TYPE mssql);
-- → use_encrypt = false
```

---

## Error Contracts

### TLS Validation Errors

```
-- Server requires TLS but client disabled it
Error: MSSQL connection validation failed: Server requires encryption (ENCRYPT_REQ) but use_encrypt=false. Set use_encrypt=true or Encrypt=yes in connection string.

-- TLS handshake failed
Error: MSSQL connection validation failed: TLS handshake failed to host:1433 - check TLS configuration. Details: [OpenSSL error]

-- Certificate validation failed
Error: MSSQL connection validation failed: TLS certificate validation failed - server certificate not trusted

-- TLS validation query failed
Error: MSSQL connection validation failed: TLS connection established but validation query failed. The server may have network issues or TLS may be misconfigured.
```

### Catalog Disabled Errors

```
-- Attempt to access catalog when disabled
Error: Catalog integration is disabled for this connection. Use mssql_scan() or mssql_exec() for raw queries.

-- Specific table access
Error: Cannot access 'ctx.dbo.mytable' - catalog integration is disabled. Use mssql_scan(ctx, 'SELECT * FROM dbo.mytable') instead.
```

### Query Timeout Errors

```
-- Query exceeded timeout
Error: MSSQL query timed out after 30 seconds. Use SET mssql_query_timeout to increase the timeout limit.
```

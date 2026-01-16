# Quickstart: TLS Connection Support

**Feature Branch**: `005-tls-connection-support`
**Date**: 2026-01-16

## Overview

This feature adds optional TLS encryption to SQL Server connections via the `use_encrypt` secret option.

## Usage

### Method 1: Create a TLS-Enabled Secret

```sql
CREATE SECRET mssql_secure (
    TYPE mssql,
    host 'sql-server.example.com',
    port 1433,
    database 'MyDatabase',
    user 'sa',
    password 'MyPassword123!',
    use_encrypt true
);

-- Attach using the secret
ATTACH '' AS mydb (TYPE mssql, SECRET mssql_secure);

-- Query Over Encrypted Connection
SELECT * FROM mssql_scan('mydb', 'SELECT * FROM customers');
```

### Method 2: ADO.NET Connection String with Encrypt

```sql
-- ADO.NET format with Encrypt=yes
ATTACH 'Server=sql-server.example.com,1433;Database=MyDatabase;User Id=sa;Password=MyPassword123!;Encrypt=yes' AS mydb (TYPE mssql);

-- Also supports: Encrypt=true, Encrypt=1, Use Encryption For Data=yes
```

### Method 3: URI Format with encrypt Parameter

```sql
-- URI format with ?encrypt=true
ATTACH 'mssql://sa:MyPassword123!@sql-server.example.com:1433/MyDatabase?encrypt=true' AS mydb (TYPE mssql);
```

## Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `use_encrypt` | boolean | `false` | Enable TLS encryption |

## Backward Compatibility

Existing secrets without `use_encrypt` continue to work as plaintext connections:

```sql
-- This still works (plaintext, no TLS)
CREATE SECRET mssql_legacy (
    TYPE mssql,
    host 'localhost',
    database 'TestDB',
    user 'sa',
    password 'test'
);
```

## Error Messages

| Error | Meaning | Solution |
|-------|---------|----------|
| "TLS requested but server does not support encryption" | Server has encryption disabled | Configure SQL Server to accept encrypted connections |
| "TLS handshake timed out" | Network or server issue | Check connectivity, increase timeout |
| "TLS handshake failed: [details]" | Protocol error | Check server certificate, TLS version support |

## Testing TLS Connection

### Verify Encryption Status

After connecting, you can verify TLS is active by checking SQL Server's DMV:

```sql
SELECT
    encrypt_option,
    client_net_address
FROM sys.dm_exec_connections
WHERE session_id = @@SPID;
```

Expected result when `use_encrypt=true`:
- `encrypt_option` = `TRUE`

### Local Testing with Docker

Use the project's Docker Compose setup:

```bash
# Start SQL Server (from project root)
make docker-up

# Run TLS integration tests
make integration-test
```

Or run SQL Server directly:

```bash
docker run -e 'ACCEPT_EULA=Y' -e 'SA_PASSWORD=TestPassword1!' \
    -p 1433:1433 \
    mcr.microsoft.com/mssql/server:2022-latest
```

Note: SQL Server 2022+ includes TLS support by default with a self-signed certificate. The extension's "trust server certificate" mode accepts this without additional configuration.

## Limitations

- Certificate verification is not yet supported (trust-server-certificate mode only)
- TLS 1.2 is the maximum version (TLS 1.3 not yet supported)
- Client certificate authentication not supported

These limitations may be addressed in future versions.

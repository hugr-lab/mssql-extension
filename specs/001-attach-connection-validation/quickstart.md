# Quickstart: Attach Connection Validation

## What This Feature Does

This feature adds connection validation to the ATTACH command. When you attach a SQL Server database with invalid credentials or an unreachable server, you'll get an immediate, clear error message instead of discovering the problem later during queries.

## Usage Examples

### Before (Current Behavior)

```sql
-- ATTACH succeeds even with wrong password
D ATTACH 'Server=localhost;Database=master;User Id=sa;Password=WRONG' AS mssql (TYPE mssql);
-- No error shown

-- Error only appears on first query
D SELECT * FROM mssql.dbo.test;
-- IO Error: Failed to acquire connection for metadata refresh
```

### After (New Behavior)

```sql
-- ATTACH fails immediately with clear error
D ATTACH 'Server=localhost;Database=master;User Id=sa;Password=WRONG' AS mssql (TYPE mssql);
-- Error: Authentication failed for user 'sa' - check credentials
```

## New Parameter: TrustServerCertificate

For compatibility with ADO.NET connection strings, `TrustServerCertificate` is now accepted as an alias for `Encrypt`:

```sql
-- These are equivalent:
ATTACH 'Server=localhost;Database=master;User Id=sa;Password=pass;Encrypt=true' AS mssql (TYPE mssql);
ATTACH 'Server=localhost;Database=master;User Id=sa;Password=pass;TrustServerCertificate=true' AS mssql (TYPE mssql);

-- Conflicting values produce an error:
ATTACH 'Server=localhost;...;Encrypt=true;TrustServerCertificate=false' AS mssql (TYPE mssql);
-- Error: Conflicting values for Encrypt and TrustServerCertificate
```

## Error Messages

| Scenario | Error Message |
|----------|---------------|
| Wrong password | "Authentication failed for user 'X' - check credentials" |
| Server not found | "Cannot resolve hostname 'X'" |
| Server not running | "Connection refused to X:Y - check if SQL Server is running" |
| Network timeout | "Connection timed out to X:Y after Z seconds" |
| Wrong database | "Cannot access database 'X' - check database name and permissions" |
| TLS failure | "TLS handshake failed - check TLS configuration" |

## Testing Connection Before ATTACH

You can use the existing `mssql_ping` function to test connectivity:

```sql
D SELECT mssql_ping('localhost', 1433, 'sa', 'password', 'master', false);
-- Returns true if connection succeeds, throws error if not
```

## Configuration

The connection validation uses the existing timeout settings:

```sql
-- Set connection timeout (default: 30 seconds)
SET mssql_connection_timeout = 60;
```

## Verification Checklist

After implementing this feature, verify:

- [ ] Invalid password produces immediate error on ATTACH
- [ ] Invalid hostname produces immediate error on ATTACH
- [ ] Unreachable server times out within configured timeout
- [ ] Valid credentials still work without noticeable delay
- [ ] `TrustServerCertificate=true` enables TLS
- [ ] Conflicting Encrypt/TrustServerCertificate produces error
- [ ] No catalog entry exists after failed ATTACH

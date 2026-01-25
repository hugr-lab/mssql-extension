# Data Model: Attach Connection Validation

## Overview

This feature does not introduce new data entities. It modifies the ATTACH flow behavior and adds a connection string parameter alias.

## Entities Modified

### MSSQLConnectionInfo

**File**: `src/include/mssql_storage.hpp`

**Current fields**:
- `host`: string - SQL Server hostname
- `port`: uint16_t - SQL Server port (default 1433)
- `database`: string - Database name
- `user`: string - Username
- `password`: string - Password
- `use_encrypt`: bool - Enable TLS encryption
- `connected`: bool - Connection status flag

**No new fields required**. The `TrustServerCertificate` parameter is resolved to `use_encrypt` during parsing.

## Connection String Parameters

### Current Parameters

| Parameter | Aliases | Type | Default | Description |
|-----------|---------|------|---------|-------------|
| Server | server, host | string | (required) | Host,port or host |
| Database | database, initial catalog | string | (required) | Database name |
| User Id | user id, uid, user | string | (required) | Username |
| Password | password, pwd | string | (required) | Password |
| Encrypt | encrypt | bool | false | Enable TLS |

### New Parameter

| Parameter | Aliases | Type | Default | Description |
|-----------|---------|------|---------|-------------|
| TrustServerCertificate | trustservercertificate | bool | false | Alias for Encrypt |

## State Transitions

### ATTACH Flow (Updated)

```
┌─────────────────┐
│ Parse Options   │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Parse ConnInfo  │──────► InvalidInputException (parse error)
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Validate        │──────► IOException (connection failed)
│ Connection      │──────► InvalidInputException (auth failed)
└────────┬────────┘
         │ (success)
         ▼
┌─────────────────┐
│ Register        │
│ Context         │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Create Pool     │
│ & Catalog       │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Return Catalog  │
└─────────────────┘
```

## Error Categories

| Error Code | Category | Cause | Resolution |
|------------|----------|-------|------------|
| E_RESOLVE | Network | DNS resolution failed | Check hostname spelling |
| E_CONNECT | Network | TCP connection failed | Check server is running, firewall |
| E_TIMEOUT | Network | Connection timed out | Check network path, increase timeout |
| E_AUTH | Authentication | Invalid credentials | Check username/password |
| E_DATABASE | Authorization | Database inaccessible | Check database name, permissions |
| E_TLS | TLS | Handshake failed | Check TLS configuration |
| E_CONFLICT | Configuration | Conflicting parameters | Remove conflicting options |

## Validation Rules

1. **Parameter conflicts**: If both `Encrypt` and `TrustServerCertificate` are specified with different boolean values, throw error.

2. **Connection validation**: Before registering context, create a test connection to verify:
   - TCP connectivity
   - TLS handshake (if enabled)
   - Authentication
   - Database access

3. **Resource cleanup**: If validation fails, no context, pool, or catalog is created.

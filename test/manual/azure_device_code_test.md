# Manual Test: Azure Device Code Authentication

This document describes how to manually test the Device Code Flow (RFC 8628) for Azure AD authentication.

## Prerequisites

- DuckDB with MSSQL extension built and installed
- DuckDB Azure extension installed (`INSTALL azure; LOAD azure;`)
- Access to an Azure AD tenant (can use personal Microsoft account with `TENANT_ID 'common'`)
- A web browser to complete authentication

## Test Steps

### Step 1: Load Extensions

```sql
INSTALL azure;
LOAD azure;
LOAD mssql;  -- or: INSTALL mssql FROM local_build; LOAD mssql;
```

### Step 2: Create Azure Secret with Interactive Chain

```sql
-- Option A: Using 'common' tenant (works with any Microsoft account)
CREATE SECRET my_interactive_azure (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'interactive'
);

-- Option B: Using specific tenant (for organizational accounts)
CREATE SECRET my_interactive_azure (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'interactive',
    TENANT_ID 'your-tenant-id-here'
);
```

### Step 3: Test Authentication

```sql
SELECT mssql_azure_auth_test('my_interactive_azure');
```

### Expected Output

1. A message will appear in the console:
   ```
   To sign in, visit https://microsoft.com/devicelogin and enter code ABC123XYZ
   ```

2. Open the URL in your web browser (can be on any device - phone, another computer, same machine)

3. Enter the code shown in the message (e.g., `ABC123XYZ`)

4. Sign in with your Microsoft account (you may be prompted for MFA)

5. Click "Continue" to grant access to the application

6. The DuckDB query will return a truncated token:
   ```
   ┌───────────────────────────────────────────┐
   │ mssql_azure_auth_test('my_interactive_azure') │
   ├───────────────────────────────────────────┤
   │ eyJ0eXAi...xyz [1847 chars]               │
   └───────────────────────────────────────────┘
   ```

## Error Scenarios to Test

### 1. Device Code Expired (15-minute timeout)

- Run `SELECT mssql_azure_auth_test('my_interactive_azure');`
- Wait 15 minutes without completing authentication
- Expected: `Error: Device code expired. Please try again.`

### 2. User Declines Authorization

- Run `SELECT mssql_azure_auth_test('my_interactive_azure');`
- Visit the URL, enter the code, but click "Cancel" or "Deny" when prompted
- Expected: `Error: Authorization was declined by user`

### 3. Create MSSQL Secret with Azure Auth

After successful token acquisition:

```sql
CREATE SECRET my_azure_sql (
    TYPE mssql,
    HOST 'myserver.database.windows.net',
    PORT 1433,
    DATABASE 'mydb',
    AZURE_SECRET 'my_interactive_azure'
);
```

Note: Actual database connection requires Phase 2 (TDS FEDAUTH integration).

## Cleanup

```sql
DROP SECRET IF EXISTS my_interactive_azure;
DROP SECRET IF EXISTS my_azure_sql;
```

## Notes

- Device Code Flow works in headless environments (SSH, containers, WSL)
- No browser opening or localhost server required
- User can complete authentication on any device (mobile, another computer)
- The 15-minute timeout is an Azure AD default and cannot be changed
- Tokens are cached and reused until they expire (approximately 1 hour)

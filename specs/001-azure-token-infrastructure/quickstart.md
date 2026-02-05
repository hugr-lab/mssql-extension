# Quickstart: Azure Token Infrastructure (Phase 1)

**Feature**: 001-azure-token-infrastructure
**Created**: 2026-02-05

## Overview

This feature adds Azure AD token acquisition infrastructure to the MSSQL extension. After implementation, users can:

1. Create MSSQL secrets that reference Azure credentials
2. Validate Azure credentials before attempting database connections
3. Continue using existing SQL Server authentication unchanged

> **Note**: Actual database connections with Azure auth require Phase 2 (TDS FEDAUTH integration).

## Prerequisites

- DuckDB with MSSQL extension
- DuckDB Azure extension (`INSTALL azure; LOAD azure;`)
- Azure credentials (one of: service principal, Azure CLI, managed identity)

## Usage

### Step 1: Load Extensions

```sql
INSTALL azure;
LOAD azure;

INSTALL mssql FROM local_build;  -- or community repository
LOAD mssql;
```

### Step 2: Create Azure Secret

**Option A: Service Principal** (recommended for production)

```sql
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
    CLIENT_ID 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
    CLIENT_SECRET 'your-client-secret'
);
```

**Option B: Azure CLI** (recommended for development)

```bash
# First, login with Azure CLI
az login
```

```sql
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'cli'
);
```

**Option C: Interactive Device Code** (for analysts with MFA - no CLI required, works in SSH)

```sql
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'interactive'
);

-- Or with specific tenant
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'interactive',
    TENANT_ID 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx'
);
```

When you call `mssql_azure_auth_test('my_azure')`:
1. A message appears: "To sign in, visit https://microsoft.com/devicelogin and enter code ABC123"
2. Open the URL on any device (phone, another computer, same machine)
3. Enter the code and complete sign-in (including MFA if required)
4. The token is returned automatically

**Option D: Managed Identity** (for Azure-hosted apps)

```sql
-- System-assigned managed identity
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'managed_identity'
);

-- User-assigned managed identity
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER managed_identity,
    CLIENT_ID 'identity-client-id'
);
```

### Step 3: Test Azure Credentials

Validate that token acquisition works:

```sql
SELECT mssql_azure_auth_test('my_azure');
```

**Expected output** (on success):
```
┌─────────────────────────────────────┐
│ mssql_azure_auth_test('my_azure')   │
├─────────────────────────────────────┤
│ eyJ0eXAi...xyz [1847 chars]         │
└─────────────────────────────────────┘
```

**Example errors**:
- `"Azure AD error AADSTS7000215: Invalid client secret provided"`
- `"Azure CLI credentials expired. Run 'az login' to refresh."`
- `"Azure secret 'my_azure' not found"`

### Step 4: Create MSSQL Secret with Azure Auth

```sql
CREATE SECRET my_sql (
    TYPE mssql,
    HOST 'myserver.database.windows.net',
    PORT 1433,
    DATABASE 'mydb',
    AZURE_SECRET 'my_azure'
);
```

Note: `user` and `password` are not required when `AZURE_SECRET` is specified.

### Step 5: Verify Secret (Optional)

```sql
SELECT * FROM duckdb_secrets() WHERE name = 'my_sql';
```

## Backward Compatibility

Existing SQL Server authentication continues to work unchanged:

```sql
-- Still works exactly as before
CREATE SECRET my_sql_auth (
    TYPE mssql,
    HOST 'my-sql-server.internal',
    PORT 1433,
    DATABASE 'mydb',
    USER 'sa',
    PASSWORD 'my-password'
);
```

## Troubleshooting

### "Azure extension required"

Load the Azure extension before creating Azure secrets:

```sql
INSTALL azure;
LOAD azure;
```

### "Azure secret 'name' not found"

Verify the Azure secret exists:

```sql
SELECT * FROM duckdb_secrets() WHERE name = 'my_azure';
```

### "Azure CLI credentials expired"

Refresh Azure CLI credentials:

```bash
az login
```

### "Invalid client secret"

Verify service principal credentials in Azure Portal:
1. Go to Azure AD → App registrations → Your app
2. Certificates & secrets → Verify secret hasn't expired
3. Create new secret if needed and update DuckDB secret

### "Neither user/password nor azure_secret provided"

MSSQL secret requires one authentication method:

```sql
-- Either this:
CREATE SECRET ... ( USER 'x', PASSWORD 'y' );

-- Or this:
CREATE SECRET ... ( AZURE_SECRET 'azure_secret_name' );
```

### "Device code expired"

The device code expires after 15 minutes. If you see this error:
1. Run the authentication again
2. Complete the login more quickly (visit URL, enter code, complete MFA)

### "Authorization was declined by user"

You clicked "Cancel" or "Deny" on the Microsoft login page. Run the authentication again and click "Accept" or "Continue" to grant access.

### "Error during authentication"

A network or server error occurred. Check your internet connection and try again. If the problem persists, try using Azure CLI (`CHAIN 'cli'`) or a service principal instead.

## What's Next

This feature provides infrastructure only. To actually connect to Azure SQL:

1. Wait for Phase 2: TDS FEDAUTH Integration
2. Then: `ATTACH '' AS azuredb (TYPE mssql, SECRET my_sql);`

## API Reference

### mssql_azure_auth_test(secret_name VARCHAR) → VARCHAR

Tests Azure token acquisition for the specified Azure secret.

**Parameters**:
- `secret_name`: Name of a DuckDB Azure secret (TYPE azure)

**Returns**:
- On success: Truncated token with length (e.g., `"eyJ0eXAi...xyz [1847 chars]"`)
- On failure: Error message with Azure AD error code if available

**Example**:
```sql
SELECT mssql_azure_auth_test('my_azure');
```

### MSSQL Secret Fields

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `host` | VARCHAR | Yes | - | SQL Server hostname |
| `port` | INTEGER | Yes | 1433 | TCP port |
| `database` | VARCHAR | Yes | - | Database name |
| `user` | VARCHAR | Conditional* | - | SQL auth username |
| `password` | VARCHAR | Conditional* | - | SQL auth password |
| `use_encrypt` | BOOLEAN | No | true | Enable TLS |
| `catalog` | BOOLEAN | No | true | Enable catalog integration |
| `azure_secret` | VARCHAR | Conditional* | - | Azure secret name |

*Either `user`+`password` OR `azure_secret` must be provided.

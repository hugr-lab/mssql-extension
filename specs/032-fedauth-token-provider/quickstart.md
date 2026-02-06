# Quickstart: FEDAUTH Token Provider Enhancements

**Feature**: 032-fedauth-token-provider

## Overview

This feature adds two new ways to authenticate with Azure SQL Database and Microsoft Fabric:

1. **Manual Token Authentication** - Use a pre-obtained Azure AD access token directly
2. **Environment-Based Service Principal** - Use Azure SDK environment variables

## Manual Token Authentication

### Use Case

You have an access token from:
- Microsoft Fabric notebook (`mssparkutils.credentials.getToken()`)
- Azure CLI (`az account get-access-token`)
- Any OAuth2 token provider

### Quick Start

```sql
-- Get a token (example using Azure CLI)
-- az account get-access-token --resource https://database.windows.net/ --query accessToken -o tsv

-- Use token directly in ATTACH
ATTACH 'Server=myserver.database.windows.net;Database=mydb' AS azuredb (
    TYPE mssql,
    ACCESS_TOKEN 'eyJ0eXAiOiJKV1QiLCJhbGciOiJSUzI1NiIs...'
);

-- Query your data
SELECT * FROM azuredb.dbo.MyTable;
```

### Using an MSSQL Secret

```sql
-- Create a secret for reuse
CREATE SECRET my_azure_token (
    TYPE mssql,
    HOST 'myserver.database.windows.net',
    DATABASE 'mydb',
    ACCESS_TOKEN 'eyJ0eXAiOiJKV1QiLCJhbGciOiJSUzI1NiIs...'
);

-- Attach using the secret
ATTACH '' AS azuredb (TYPE mssql, SECRET my_azure_token);
```

### Token Expiration

Manual tokens cannot be refreshed automatically. When your token expires, you'll see:

```
Access token expired at 2026-02-06 14:30:00 UTC. Please provide a new token.
```

To fix: Obtain a new token and re-attach.

## Environment-Based Service Principal

### Use Case

You want to use Azure SDK standard environment variables in:
- CI/CD pipelines
- Docker containers
- Kubernetes deployments

### Quick Start

```bash
# Set environment variables
export AZURE_TENANT_ID="your-tenant-id"
export AZURE_CLIENT_ID="your-app-id"
export AZURE_CLIENT_SECRET="your-client-secret"
```

```sql
-- Create an Azure secret with env provider
CREATE SECRET azure_env (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'env'
);

-- Attach using the secret
ATTACH 'Server=myserver.database.windows.net;Database=mydb' AS azuredb (
    TYPE mssql,
    AZURE_SECRET 'azure_env'
);

-- Query your data
SELECT * FROM azuredb.dbo.MyTable;
```

### Token Refresh

Environment-based tokens refresh automatically. No action needed for long-running sessions.

## Error Handling

### Invalid Token Format

```
Invalid access token format: unable to parse JWT. Ensure token is a valid Azure AD access token.
```

**Fix**: Verify your token is a valid JWT (three base64url-encoded parts separated by dots).

### Wrong Audience

```
Access token audience 'https://graph.microsoft.com' does not match expected 'https://database.windows.net/'.
```

**Fix**: Request token for the correct resource:
```bash
az account get-access-token --resource https://database.windows.net/
```

### Missing Environment Variable

```
Environment variable AZURE_CLIENT_ID not set. Required for credential_chain with 'env' provider.
```

**Fix**: Set the missing environment variable before running DuckDB.

## Comparison: When to Use What

| Method | Best For | Token Refresh | Setup Complexity |
|--------|----------|---------------|------------------|
| ACCESS_TOKEN in ATTACH | Quick one-off queries | Manual | Lowest |
| ACCESS_TOKEN in secret | Repeated short sessions | Manual | Low |
| env chain | CI/CD, containers | Automatic | Medium |
| cli chain | Development | Automatic | Low |
| service_principal secret | Production apps | Automatic | Medium |

## Microsoft Fabric Example

```python
# In a Fabric notebook
from mssparkutils import credentials

# Get token for SQL Database resource
token = credentials.getToken("https://database.windows.net/")

# Use in DuckDB
import duckdb
conn = duckdb.connect()
conn.execute(f"""
    ATTACH 'Server=xxx.datawarehouse.fabric.microsoft.com;Database=mywarehouse'
    AS wh (TYPE mssql, ACCESS_TOKEN '{token}')
""")

# Query your warehouse
result = conn.execute("SELECT * FROM wh.dbo.MyTable").fetchall()
```

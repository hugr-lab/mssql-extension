# Azure AD Authentication for MSSQL Extension

This document describes how to use Azure Active Directory (Azure AD) authentication with the DuckDB MSSQL extension to connect to Azure SQL Database and Microsoft Fabric.

## Prerequisites

1. **DuckDB Azure Extension** - Required for Azure secret management
2. **Azure Credentials** - Service principal, Azure CLI login, or interactive authentication

## Quick Start

```sql
-- Install and load required extensions
INSTALL azure;
LOAD azure;
INSTALL mssql FROM community;
LOAD mssql;

-- Create Azure secret with service principal
CREATE SECRET my_azure_secret (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID 'your-tenant-id',
    CLIENT_ID 'your-client-id',
    CLIENT_SECRET 'your-client-secret'
);

-- Test the credentials
SELECT mssql_azure_auth_test('my_azure_secret');
-- Returns: eyJ0eXAiOi...xyz [1634 chars]

-- Attach to Azure SQL Database
ATTACH 'Server=myserver.database.windows.net;Database=mydb' AS azuredb (
    TYPE mssql,
    AZURE_SECRET 'my_azure_secret'
);
```

## Authentication Methods

### 1. Service Principal (Recommended for Automation)

Best for CI/CD pipelines, scheduled jobs, and server applications.

```sql
-- Create service principal secret
CREATE SECRET azure_sp (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID 'your-tenant-id',
    CLIENT_ID 'your-client-id',
    CLIENT_SECRET 'your-client-secret'
);

-- Test credentials
SELECT mssql_azure_auth_test('azure_sp');
```

**Required Azure Setup:**

1. Register an application in Azure AD
2. Create a client secret for the application
3. Grant the application access to your Azure SQL Database:

   ```sql
   -- Run in Azure SQL Database
   CREATE USER [your-app-name] FROM EXTERNAL PROVIDER;
   ALTER ROLE db_datareader ADD MEMBER [your-app-name];
   ALTER ROLE db_datawriter ADD MEMBER [your-app-name];
   ```

### 2. Azure CLI (Recommended for Development)

Uses your existing Azure CLI login credentials.

```sql
-- Create credential chain secret with CLI
CREATE SECRET azure_cli (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'cli'
);

-- Test credentials (requires 'az login' first)
SELECT mssql_azure_auth_test('azure_cli');
```

**Prerequisites:**

```bash
# Install Azure CLI and login
az login
az account set --subscription "Your Subscription Name"
```

### 3. Environment Variables (For CI/CD)

Best for CI/CD pipelines using Azure SDK standard environment variables.

```sql
-- Create credential chain secret with env provider
CREATE SECRET azure_env (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'env'
);

-- Test credentials (requires AZURE_TENANT_ID, AZURE_CLIENT_ID, AZURE_CLIENT_SECRET)
SELECT mssql_azure_auth_test('azure_env');
```

**Prerequisites:**

```bash
# Set Azure SDK standard environment variables
export AZURE_TENANT_ID="your-tenant-id"
export AZURE_CLIENT_ID="your-client-id"
export AZURE_CLIENT_SECRET="your-client-secret"
```

The `env` provider uses the same service principal credentials as Azure SDK's `DefaultAzureCredential`. This makes it easy to use the same credentials across different Azure tools and SDKs.

### 4. Interactive / Device Code Flow (For MFA)

Best for interactive sessions where MFA is required.

```sql
-- Create credential chain secret with interactive auth
CREATE SECRET azure_interactive (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'interactive'
);

-- Test with tenant_id (required for interactive auth)
SELECT mssql_azure_auth_test('azure_interactive', 'your-tenant-id');
-- Output: To sign in, use a web browser to open https://microsoft.com/devicelogin
--         and enter the code ABCD1234 to authenticate.
```

> **Note:** Interactive auth requires a `tenant_id` to be specified. You can either:
>
> 1. Pass it as the second argument to `mssql_azure_auth_test()`
> 2. Use `azure_tenant_id` in the MSSQL secret (see below)
> 3. Use Azure CLI (`az login`) which establishes tenant context automatically

### 5. Manual Access Token (For External Token Management)

Best when you have your own token management system or need to use tokens from external sources.

```sql
-- Attach using a pre-obtained access token
ATTACH 'Server=myserver.database.windows.net;Database=mydb' AS azuredb (
    TYPE mssql,
    ACCESS_TOKEN 'eyJ0eXAiOi...your-jwt-token'
);
```

**Prerequisites:**

```bash
# Obtain a token using Azure CLI
az account get-access-token --resource https://database.windows.net/ --query accessToken -o tsv
```

**Token Requirements:**
- Must be a valid JWT token for Azure SQL Database
- Audience (`aud` claim) must be `https://database.windows.net/`
- Token must not be expired (checked with 5-minute margin)

**Using ACCESS_TOKEN in MSSQL Secret:**

```sql
-- Create MSSQL secret with access token
CREATE SECRET mssql_token_auth (
    TYPE mssql,
    HOST 'myserver.database.windows.net',
    DATABASE 'mydb',
    ACCESS_TOKEN 'eyJ0eXAiOi...your-jwt-token'
);

-- Attach using the secret
ATTACH '' AS mydb (TYPE mssql, SECRET mssql_token_auth);
```

> **Note:** When using `ACCESS_TOKEN`, the token is validated at ATTACH time. If the token is expired or has an invalid audience, you'll get a clear error message.

## Connection Examples

### Azure SQL Database

```sql
-- Using service principal
ATTACH 'Server=myserver.database.windows.net;Database=mydb' AS azuresql (
    TYPE mssql,
    AZURE_SECRET 'azure_sp'
);

-- Query data
SELECT * FROM azuresql.dbo.customers LIMIT 10;
```

### Microsoft Fabric Data Warehouse

```sql
-- Fabric connection string format
ATTACH 'Server=xyz.datawarehouse.fabric.microsoft.com;Database=my_warehouse' AS fabric (
    TYPE mssql,
    AZURE_SECRET 'azure_sp'
);

-- Query Fabric tables
SELECT * FROM fabric.dbo.sales_data;
```

### Azure SQL with TLS

```sql
-- Azure SQL always uses TLS, trust the certificate
ATTACH 'Server=myserver.database.windows.net,1433;Database=mydb;Encrypt=True' AS azuresql (
    TYPE mssql,
    AZURE_SECRET 'azure_sp',
    TRUST_SERVER_CERTIFICATE true
);
```

## Testing Credentials

The `mssql_azure_auth_test()` function validates Azure credentials without connecting to a database:

```sql
-- Test service principal
SELECT mssql_azure_auth_test('my_secret_name');

-- Successful output (truncated token):
-- eyJ0eXAiOi...gJw [1634 chars]

-- Error outputs:
-- Error: Secret name required
-- Error: Azure secret 'xyz' not found
-- Error: Secret 'xyz' is not an Azure secret (type: postgres)
-- Azure AD error: AADSTS7000215: Invalid client secret provided
-- Azure CLI credentials expired. Run 'az login' to refresh.
```

## Token Caching

Tokens are cached automatically with a 5-minute refresh margin:

- Tokens are cached per secret name
- Cache is thread-safe for concurrent queries
- Tokens are refreshed automatically before expiration
- Cache survives across multiple ATTACH/DETACH cycles

```sql
-- First call acquires token from Azure AD (~200ms)
SELECT mssql_azure_auth_test('azure_sp');

-- Subsequent calls use cached token (~0ms)
SELECT mssql_azure_auth_test('azure_sp');
```

## Combining with MSSQL Secrets

You can reference an Azure secret from an MSSQL secret:

```sql
-- Create Azure secret
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID 'your-tenant-id',
    CLIENT_ID 'your-client-id',
    CLIENT_SECRET 'your-client-secret'
);

-- Create MSSQL secret that references Azure secret
CREATE SECRET azure_sql_conn (
    TYPE mssql,
    HOST 'myserver.database.windows.net',
    DATABASE 'mydb',
    AZURE_SECRET 'my_azure'
);

-- Attach using the MSSQL secret
ATTACH '' AS mydb (TYPE mssql, SECRET azure_sql_conn);
```

### Interactive Auth with MSSQL Secret

For interactive authentication, you must provide a tenant ID. Since the Azure extension's
credential_chain provider doesn't support tenant_id directly, use `azure_tenant_id` in the
MSSQL secret:

```sql
-- Create Azure secret for interactive auth (no tenant_id needed here)
CREATE SECRET my_azure_interactive (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'interactive'
);

-- Create MSSQL secret with tenant_id for interactive auth
CREATE SECRET azure_sql_interactive (
    TYPE mssql,
    HOST 'myserver.database.windows.net',
    DATABASE 'mydb',
    AZURE_SECRET 'my_azure_interactive',
    AZURE_TENANT_ID 'your-tenant-id'  -- Required for interactive auth
);

-- Attach using the MSSQL secret (will prompt for device code login)
ATTACH '' AS mydb (TYPE mssql, SECRET azure_sql_interactive);
```

## Troubleshooting

### "Azure extension required for Azure authentication"

```sql
-- Install and load Azure extension first
INSTALL azure;
LOAD azure;
```

### "Azure secret 'xyz' not found"

```sql
-- Check existing secrets
SELECT * FROM duckdb_secrets();

-- Ensure secret was created with TYPE azure
CREATE SECRET my_secret (
    TYPE azure,  -- Must be 'azure'
    PROVIDER service_principal,
    ...
);
```

### "AADSTS7000215: Invalid client secret provided"

The client secret is incorrect or expired. Generate a new secret in Azure Portal:

1. Go to Azure AD > App registrations > Your app
2. Certificates & secrets > New client secret

### "AADSTS700016: Application not found"

The client ID (application ID) is incorrect:

1. Go to Azure AD > App registrations
2. Copy the correct Application (client) ID

### "Azure CLI credentials expired"

```bash
# Re-authenticate with Azure CLI
az login

# Or for a specific tenant
az login --tenant your-tenant-id
```

### "Connection reset by peer" with Azure CLI

This error can occur if you're using an older version of the extension that doesn't support large token fragmentation. Azure CLI tokens (~2091 chars → ~4182 bytes UTF-16LE) exceed the default TDS packet size (4096 bytes) and require packet fragmentation.

**Solution**: Update to version 0.1.11+ which includes the `BuildFedAuthTokenMultiPacket()` fix.

**Token sizes by authentication method**:

| Method            | Token Size (chars) | UTF-16LE Size | Packets Needed |
|-------------------|-------------------|---------------|----------------|
| Service Principal | ~1632             | ~3264 bytes   | 1              |
| Azure CLI         | ~2091             | ~4182 bytes   | 2              |
| Interactive       | ~2000+            | ~4000+ bytes  | 1-2            |

### "Device code flow timeout"

The device code flow times out after 15 minutes. Start again:

```sql
-- Invalidate cached token and retry
SELECT mssql_azure_auth_test('azure_interactive');
```

## Security Best Practices

1. **Never commit secrets to source control** - Use environment variables or secret managers
2. **Use service principals for production** - Avoid interactive auth in automated pipelines
3. **Rotate secrets regularly** - Azure recommends rotating every 90 days
4. **Use least privilege** - Grant only required database permissions
5. **Enable Azure AD audit logs** - Monitor authentication attempts

## Environment Variables

For CI/CD, set credentials via Azure SDK standard environment variables:

```bash
export AZURE_TENANT_ID="your-tenant-id"
export AZURE_CLIENT_ID="your-client-id"
export AZURE_CLIENT_SECRET="your-client-secret"
```

```sql
-- Use in DuckDB with credential_chain 'env' provider
CREATE SECRET azure_env (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'env'
);

-- Attach to Azure SQL using env-based credentials
ATTACH 'Server=myserver.database.windows.net;Database=mydb' AS azuresql (
    TYPE mssql,
    AZURE_SECRET 'azure_env'
);
```

The `env` provider reads the standard Azure SDK environment variables, making it compatible with:
- Azure SDK for Python, JavaScript, Go, Java, .NET
- Azure CLI (when using service principal login)
- GitHub Actions with Azure credentials
- Azure DevOps pipelines

**Chain Priority:**

When using multiple providers in a chain (e.g., `CHAIN 'env;cli'`), the extension tries them in order:

1. `env` — Environment variables
2. `cli` — Azure CLI credentials
3. `interactive` — Device code flow

This matches Azure SDK's `DefaultAzureCredential` behavior.

## Microsoft Fabric Limitations

Microsoft Fabric Data Warehouses have some limitations compared to Azure SQL Database:

### No BCP/INSERT BULK Support

Fabric doesn't support the TDS `INSERT BULK` command (BCP protocol). The extension handles this automatically:

- **CTAS (CREATE TABLE AS SELECT)**: Auto-falls back to batched INSERT statements
- **COPY TO MSSQL**: Not supported on Fabric - use CTAS instead

```sql
-- This works on Fabric (auto-fallback to INSERT mode)
CREATE TABLE fabric.dbo.new_table AS SELECT * FROM local_table;

-- This will fail on Fabric with a clear error message
COPY (SELECT * FROM local_table) TO 'fabric.dbo.new_table' (FORMAT 'bcp');
-- Error: Microsoft Fabric does not support INSERT BULK (BCP protocol).
-- Use CREATE TABLE AS SELECT (CTAS) instead.
```

### Performance Note

Due to the INSERT fallback, bulk data transfers to Fabric are slower than to Azure SQL Database. For large data loads, consider:

1. Loading to Azure SQL Database first, then syncing to Fabric
2. Using Fabric's native data ingestion tools (Data Factory, Dataflows)
3. Breaking large loads into smaller batches

## Supported Azure Services

| Service | Connection String Format | BCP Support |
| ------- | ------------------------ | ----------- |
| Azure SQL Database | `Server=name.database.windows.net;Database=dbname` | ✅ Full |
| Azure SQL Managed Instance | `Server=name.public.xyz.database.windows.net,3342;Database=dbname` | ✅ Full |
| Microsoft Fabric DW | `Server=xyz.datawarehouse.fabric.microsoft.com;Database=warehouse` | ❌ INSERT fallback |
| Azure Synapse Serverless | `Server=name-ondemand.sql.azuresynapse.net;Database=dbname` | ⚠️ Limited |

## See Also

- [DuckDB Azure Extension](https://duckdb.org/docs/extensions/azure)
- [Azure AD Authentication for Azure SQL](https://docs.microsoft.com/azure/azure-sql/database/authentication-aad-overview)
- [Microsoft Fabric Documentation](https://docs.microsoft.com/fabric/)

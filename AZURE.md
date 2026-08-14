# Azure AD Authentication for MSSQL Extension

Connect to Azure SQL Database and Microsoft Fabric using Azure Active Directory authentication.

## Table of Contents

- [Quick Start](#quick-start)
- [Authentication Methods](#authentication-methods)
  - [Service Principal](#1-service-principal-recommended-for-automation)
  - [Azure CLI](#2-azure-cli-recommended-for-development)
  - [Environment Variables](#3-environment-variables-for-cicd)
  - [Interactive (Device Code)](#4-interactive-device-code-flow)
  - [Manual Access Token](#5-manual-access-token)
- [Connection Examples](#connection-examples)
- [Using MSSQL Secrets](#using-mssql-secrets)
- [Troubleshooting](#troubleshooting)
- [Microsoft Fabric](#microsoft-fabric)
- [Reference](#reference)

---

## Quick Start

```sql
-- 1. Install and load required extensions
INSTALL azure;
LOAD azure;
INSTALL mssql FROM community;
LOAD mssql;

-- 2. Create Azure secret (service principal example)
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID 'your-tenant-id',
    CLIENT_ID 'your-client-id',
    CLIENT_SECRET 'your-client-secret'
);

-- 3. Connect to Azure SQL Database
ATTACH 'Server=myserver.database.windows.net;Database=mydb' AS azuredb (
    TYPE mssql,
    AZURE_SECRET 'my_azure'
);

-- 4. Query data
SELECT * FROM azuredb.dbo.customers LIMIT 10;
```

---

## Authentication Methods

### 1. Service Principal (Recommended for Automation)

Best for CI/CD pipelines, scheduled jobs, and server applications.

```sql
CREATE SECRET azure_sp (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID 'your-tenant-id',
    CLIENT_ID 'your-client-id',
    CLIENT_SECRET 'your-client-secret'
);

ATTACH 'Server=myserver.database.windows.net;Database=mydb' AS db (
    TYPE mssql,
    AZURE_SECRET 'azure_sp'
);
```

**Azure Setup Required:**

1. Register an application in Azure AD (Entra ID)
2. Create a client secret for the application
3. Grant database access:
   ```sql
   -- Run in Azure SQL Database
   CREATE USER [your-app-name] FROM EXTERNAL PROVIDER;
   ALTER ROLE db_datareader ADD MEMBER [your-app-name];
   ALTER ROLE db_datawriter ADD MEMBER [your-app-name];
   ```

### 2. Azure CLI (Recommended for Development)

Uses your existing `az login` credentials.

```sql
CREATE SECRET azure_cli (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'cli'
);

ATTACH 'Server=myserver.database.windows.net;Database=mydb' AS db (
    TYPE mssql,
    AZURE_SECRET 'azure_cli'
);
```

**Prerequisites:**

```bash
az login
az account set --subscription "Your Subscription Name"
```

### 3. Environment Variables (For CI/CD)

Uses Azure SDK standard environment variables—compatible with GitHub Actions, Azure DevOps, and all Azure SDKs.

```bash
# Set environment variables
export AZURE_TENANT_ID="your-tenant-id"
export AZURE_CLIENT_ID="your-client-id"
export AZURE_CLIENT_SECRET="your-client-secret"
```

```sql
CREATE SECRET azure_env (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'env'
);

ATTACH 'Server=myserver.database.windows.net;Database=mydb' AS db (
    TYPE mssql,
    AZURE_SECRET 'azure_env'
);
```

### 4. Interactive (Device Code Flow)

Best for interactive sessions, MFA-enabled accounts, and personal accounts.

```sql
CREATE SECRET azure_interactive (
    TYPE azure,
    PROVIDER credential_chain,
    CHAIN 'interactive'
);

ATTACH 'Server=myserver.database.windows.net;Database=mydb' AS db (
    TYPE mssql,
    AZURE_SECRET 'azure_interactive'
);
-- Output: To sign in, use a web browser to open https://microsoft.com/devicelogin
--         and enter the code ABCD1234 to authenticate.
```

**Choosing the tenant.** With no tenant the device-code flow signs in against
the multi-tenant `/common/` endpoint, which is what you want for a personal
account and wrong for a single-tenant organization. `TENANT_ID` does **not**
work on the azure secret — duckdb-azure rejects it on
`provider='credential_chain'` — so pass it on the MSSQL side, either as the
`azure_tenant_id` ATTACH option or as the secret field of the same name. The
ATTACH option wins over the secret.

```sql
ATTACH 'Server=myserver.database.windows.net;Database=mydb' AS db (
    TYPE mssql,
    AZURE_SECRET 'azure_interactive',
    AZURE_TENANT_ID 'contoso.onmicrosoft.com'
);
```

The tenant is part of the token cache key, so two ATTACHes of the same secret
against different tenants do not share a token.

**Example Session:**

```text
D> ATTACH 'Server=xyz.datawarehouse.fabric.microsoft.com;Database=my_warehouse' AS wh (
       TYPE mssql, AZURE_SECRET 'azure_interactive'
   );
To sign in, use a web browser to open https://microsoft.com/devicelogin
and enter the code LYBT74YQB to authenticate.
100% ▕████████████████████████████████████▏ (00:00:20.85 elapsed)

D> SHOW ALL TABLES;
┌──────────┬─────────┬────────────┬──────────────────┐
│ database │ schema  │    name    │   column_names   │
├──────────┼─────────┼────────────┼──────────────────┤
│ wh       │ dbo     │ Date       │ [DateID, Date..] │
│ wh       │ dbo     │ Geography  │ [GeographyID..]  │
│ wh       │ dbo     │ Trip       │ [DateID, Medal.] │
└──────────┴─────────┴────────────┴──────────────────┘
```

### 5. Manual Access Token

For external token management or pre-obtained tokens.

```sql
-- Direct token in ATTACH
ATTACH 'Server=myserver.database.windows.net;Database=mydb' AS db (
    TYPE mssql,
    ACCESS_TOKEN 'eyJ0eXAiOi...your-jwt-token'
);
```

**Get a token:**

```bash
az account get-access-token --resource https://database.windows.net/ --query accessToken -o tsv
```

**Token Requirements:**
- Audience must be `https://database.windows.net/`
- Token must not be expired (validated with 5-minute margin)

### 6. Reusable Access Token (via Azure Secret)

Store a pre-obtained token in an Azure secret and reuse it across multiple connections.

```sql
-- Create Azure secret with pre-provided token
CREATE SECRET my_token (
    TYPE azure,
    PROVIDER access_token,
    ACCESS_TOKEN 'eyJ0eXAiOi...your-jwt-token'
);

-- Reuse the same token for multiple databases
ATTACH 'Server=server1.database.windows.net;Database=db1' AS db1 (
    TYPE mssql,
    AZURE_SECRET 'my_token'
);
ATTACH 'Server=server2.database.windows.net;Database=db2' AS db2 (
    TYPE mssql,
    AZURE_SECRET 'my_token'
);
```

You can also combine it with an MSSQL secret:

```sql
CREATE SECRET my_token (
    TYPE azure,
    PROVIDER access_token,
    ACCESS_TOKEN 'eyJ0eXAiOi...your-jwt-token'
);

CREATE SECRET azure_sql_conn (
    TYPE mssql,
    HOST 'myserver.database.windows.net',
    DATABASE 'mydb',
    AZURE_SECRET 'my_token'
);

ATTACH '' AS mydb (TYPE mssql, SECRET azure_sql_conn);
```

**When to use this over direct ACCESS_TOKEN:**
- You need to connect to multiple servers/databases with the same token
- You want to manage the token separately from connection details

---

## Connection Examples

### Azure SQL Database

```sql
ATTACH 'Server=myserver.database.windows.net;Database=mydb' AS azuresql (
    TYPE mssql,
    AZURE_SECRET 'my_azure'
);
```

### Microsoft Fabric Data Warehouse

```sql
ATTACH 'Server=xyz.datawarehouse.fabric.microsoft.com;Database=my_warehouse' AS fabric (
    TYPE mssql,
    AZURE_SECRET 'my_azure'
);
```

### Azure SQL Managed Instance

```sql
ATTACH 'Server=myinstance.public.abc123.database.windows.net,3342;Database=mydb' AS mi (
    TYPE mssql,
    AZURE_SECRET 'my_azure'
);
```

---

## Using MSSQL Secrets

You can store connection details and Azure credentials together in an MSSQL secret:

```sql
-- Create Azure secret
CREATE SECRET my_azure (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID 'your-tenant-id',
    CLIENT_ID 'your-client-id',
    CLIENT_SECRET 'your-client-secret'
);

-- Create MSSQL secret referencing Azure secret
CREATE SECRET azure_sql_conn (
    TYPE mssql,
    HOST 'myserver.database.windows.net',
    DATABASE 'mydb',
    AZURE_SECRET 'my_azure'
);

-- Attach using just the MSSQL secret
ATTACH '' AS mydb (TYPE mssql, SECRET azure_sql_conn);
```

**With ACCESS_TOKEN:**

```sql
CREATE SECRET mssql_token (
    TYPE mssql,
    HOST 'myserver.database.windows.net',
    DATABASE 'mydb',
    ACCESS_TOKEN 'eyJ0eXAiOi...your-jwt-token'
);

ATTACH '' AS mydb (TYPE mssql, SECRET mssql_token);
```

---

## Troubleshooting

### Common Errors

| Error | Cause | Solution |
|-------|-------|----------|
| `Azure extension required` | Azure extension not loaded | `INSTALL azure; LOAD azure;` |
| `Azure secret 'xyz' not found` | Secret doesn't exist | Check `SELECT * FROM duckdb_secrets();` |
| `AADSTS7000215: Invalid client secret` | Wrong or expired secret | Generate new secret in Azure Portal |
| `AADSTS700016: Application not found` | Wrong client ID | Verify Application (client) ID in Azure AD |
| `Azure CLI credentials expired` | CLI token expired | Run `az login` |
| `Connection reset by peer` | Token too large (older versions) | Update to v0.1.11+ |
| `Invalid access token format` | Malformed JWT | Verify token format |
| `Access token audience` | Wrong token audience | Token must be for `https://database.windows.net/` |
| `Access token expired` | Token has expired | Get a fresh token |

### Testing Credentials

Test your Azure credentials without connecting to a database:

```sql
SELECT mssql_azure_auth_test('my_azure_secret');
-- Returns: eyJ0eXAiOi...gJw [1634 chars]

-- For interactive auth, pass tenant_id:
SELECT mssql_azure_auth_test('azure_interactive', 'your-tenant-id');
```

### Token Caching

Tokens are cached automatically:
- Cache is per secret name, thread-safe
- Tokens refresh automatically 5 minutes before expiration
- First call: ~200ms (acquires from Azure AD)
- Subsequent calls: ~0ms (uses cache)

---

## Microsoft Fabric

Microsoft Fabric Data Warehouses have some differences from Azure SQL Database:

### What a Warehouse Is, and What Follows

A Fabric Data Warehouse stores its tables as Delta Parquet. Almost every
difference below follows from that one fact, and the extension enforces them
before generating any DDL rather than letting the server reject it.

| Operation | Azure SQL | Microsoft Fabric |
|-----------|-----------|------------------|
| CTAS | BCP | BCP |
| COPY TO | BCP | BCP |
| INSERT / SELECT | Full | Full |

**BCP works on Fabric.** Earlier releases forced CTAS onto an INSERT fallback
and refused `COPY TO` outright; both are gone. Microsoft still labels bcp on
Warehouse a preview, so if you hit a tenant where it is unavailable, the escape
hatch is `SET mssql_ctas_use_bcp = false`.

### No NVARCHAR

There is no UTF-16 type in Parquet, so a warehouse has no `nvarchar` or `nchar`
at all — `CREATE TABLE (v nvarchar(50))` is refused with *"not supported in this
edition of SQL Server"*. Since `nvarchar(max)` is the extension's default for
string columns everywhere else, CTAS and COPY force `VARCHAR` on a Fabric
catalog whatever `mssql_ctas_text_type` says. Nothing is lost: a warehouse
collation is UTF-8, so a `varchar` column holds every character an `nvarchar`
would — counting bytes rather than UTF-16 units.

`MSSQL_NVARCHAR(n)` is refused with an error naming `MSSQL_VARCHAR(n)` as the
alternative.

`varchar(max)` **is** supported (16 MB), contrary to older guidance.

### Two collations, both UTF-8

A warehouse accepts only `Latin1_General_100_BIN2_UTF8` (the default) and
`Latin1_General_100_CI_AS_KS_WS_SC_UTF8`, and the choice is fixed when the
warehouse is created. The extension emits no `COLLATE` clause for a permanent
table — inheriting the warehouse's own is right — and refuses any other
collation named in `MSSQL_VARCHAR(n, 'collation')` before the DDL is built.

Because every string column on a warehouse is a UTF-8 `varchar`, BCP sends UTF-8
bytes there rather than transcoding to UTF-16, which halves the wire for
ASCII-ish data.

### No indexes, no page compression

`CREATE TABLE ... WITH (table_kind = ...)`, `clustered_index` and
`data_compression` are all refused on a Fabric catalog: Delta Parquet has no
indexes, and carries its own compression. A warehouse is already columnar.

### Types with no warehouse equivalent

Beyond `nvarchar`/`nchar`: `datetimeoffset`, `datetime`, `smalldatetime`,
`money`, `smallmoney`, `tinyint`, `text`, `ntext`, `image`, `xml`, `geometry`
and `geography`. Note `datetimeoffset` in particular — the extension maps
DuckDB's `TIMESTAMP WITH TIME ZONE` onto it, so such a column cannot be created
on a warehouse.

### Temp tables

A `#temp` table works on a warehouse but **not** as a BCP target: the load fails
inside Fabric with an I/O error against a parquet file. Load into a permanent
table instead. Note also that a temp table lives in tempdb and does not inherit
the warehouse's UTF-8 collation, so the extension names it explicitly on any
temp target it creates.

### Performance Tips for Fabric

1. Size your string columns — `MSSQL_VARCHAR(n)`, or `mssql_default_string_length`
2. Break large loads into smaller batches
3. Keep `mssql_copy_tablock` in mind for large single-writer loads

---

## Reference

### Supported Azure Services

| Service | Connection Format | BCP Support |
|---------|-------------------|-------------|
| Azure SQL Database | `name.database.windows.net` | ✅ Full |
| Azure SQL Managed Instance | `name.public.xyz.database.windows.net,3342` | ✅ Full |
| Microsoft Fabric DW | `xyz.datawarehouse.fabric.microsoft.com` | ✅ Full (preview upstream) |
| Azure Synapse Serverless | `name-ondemand.sql.azuresynapse.net` | ⚠️ Limited |

### Chain Priority

When using multiple providers (e.g., `CHAIN 'env;cli'`), they're tried in order:

1. `env` — Environment variables
2. `cli` — Azure CLI credentials
3. `interactive` — Device code flow

This matches Azure SDK's `DefaultAzureCredential` behavior.

### Token Sizes

| Method | Token Size | UTF-16LE Size | TDS Packets |
|--------|-----------|---------------|-------------|
| Service Principal | ~1632 chars | ~3264 bytes | 1 |
| Azure CLI | ~2091 chars | ~4182 bytes | 2 |
| Interactive | ~2000+ chars | ~4000+ bytes | 1-2 |

### Security Best Practices

1. **Never commit secrets to source control**
2. **Use service principals for production** (not interactive auth)
3. **Rotate secrets every 90 days**
4. **Grant least privilege** database permissions
5. **Enable Azure AD audit logs**

### See Also

- [DuckDB Azure Extension](https://duckdb.org/docs/extensions/azure)
- [Azure AD Authentication for Azure SQL](https://docs.microsoft.com/azure/azure-sql/database/authentication-aad-overview)
- [Microsoft Fabric Documentation](https://docs.microsoft.com/fabric/)

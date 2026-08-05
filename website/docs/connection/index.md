---
title: Connections & Authentication
sidebar_position: 1
---

# Connection Configuration

### Using Secrets

Create a secret to store connection credentials securely:

```sql
CREATE SECRET secret_name (
    TYPE mssql,
    host 'hostname',
    port 1433,
    database 'database_name',
    user 'username',
    password 'password',
    use_encrypt true  -- TLS enabled by default
);
```

#### Secret Fields

| Field         | Type    | Required | Description                          |
| ------------- | ------- | -------- | ------------------------------------ |
| `host`        | VARCHAR | Yes      | SQL Server hostname or IP address    |
| `port`        | INTEGER | Yes      | TCP port (1-65535, default: 1433)    |
| `database`    | VARCHAR | Yes      | Database name                        |
| `user`        | VARCHAR | Yes\*    | SQL Server username (\*not required for `authenticator='krb5'` ccache mode or Azure AD) |
| `password`    | VARCHAR | Yes\*    | Password (hidden in `duckdb_secrets()`; required only for SQL auth + Kerberos raw mode) |
| `use_encrypt` | BOOLEAN | No       | Enable TLS encryption (default: true) |
| `catalog`     | BOOLEAN | No       | Enable catalog integration (default: true). Set to false for serverless/restricted databases that don't support catalog queries |
| `schema_filter` | VARCHAR | No     | Regex pattern to filter visible schemas (case-insensitive partial match) |
| `table_filter`  | VARCHAR | No     | Regex pattern to filter visible tables/views (case-insensitive partial match) |
| `azure_secret`  | VARCHAR | No     | Name of an Azure secret (DuckDB Azure extension) for Azure AD auth — see [AZURE.md](/connection/azure/) |
| `access_token`  | VARCHAR | No     | Pre-acquired Azure AD JWT (hidden in `duckdb_secrets()`) — see [AZURE.md](/connection/azure/) |
| `authenticator` | VARCHAR | No     | `krb5` (POSIX) or `winsspi` (Windows; pending) — Kerberos / SSPI integrated auth, see [Kerberos.md](/connection/kerberos/) |
| `krb5_configfile`    | VARCHAR | No | Per-secret `/etc/krb5.conf` override (Linux only) |
| `krb5_keytabfile`    | VARCHAR | No | Path to a keytab — selects keytab credential mode (Linux only) |
| `krb5_credcachefile` | VARCHAR | No | ccache path override (Linux only) |
| `krb5_realm`         | VARCHAR | No | AD realm (UPPERCASE) — required for keytab and raw modes |
| `service_principal_name` | VARCHAR | No | SPN override, e.g. `MSSQLSvc/sqlhost.example.com:1433` |
| `application_name`   | VARCHAR | No | LOGIN7 `program_name` propagated to SQL Server (visible via `APP_NAME()` / `sys.dm_exec_sessions.program_name`). Empty → `"DuckDB MSSQL Extension"` default. Clamped client-side to 128 UTF-16 code units. Fallback secret key: `applicationname`. |

Attach using the secret:

```sql
ATTACH '' AS context_name (TYPE mssql, SECRET secret_name);
```

### Using Connection Strings

#### ADO.NET Format

```sql
ATTACH 'Server=host,port;Database=db;User Id=user;Password=pass;Encrypt=yes'
    AS context_name (TYPE mssql);
```

#### Key Aliases (case-insensitive)

| Key                         | Aliases                              |
| --------------------------- | ------------------------------------ |
| `Server`                    | `Data Source`                        |
| `Database`                  | `Initial Catalog`                    |
| `User Id`                   | `Uid`, `User`                        |
| `Password`                  | `Pwd`                                |
| `Encrypt`                   | `Use Encryption for Data`, `TrustServerCertificate` |
| `Trusted_Connection`        | `Trusted Connection`, `TrustedConnection` (yes/true/SSPI/1 -> Kerberos on POSIX, SSPI on Windows; see [Kerberos.md](/connection/kerberos/)) |
| `Integrated Security`       | `IntegratedSecurity`, `Integrated_Security` (same resolution as `Trusted_Connection`) |
| `authenticator`             | `krb5` or `winsspi` (see [Kerberos.md](/connection/kerberos/)) |
| `krb5-keytabfile`           | `krb5_keytabfile` (path to keytab; selects keytab mode, Linux only) |
| `krb5-configfile`           | `krb5_configfile` (per-connection `/etc/krb5.conf` override, Linux only) |
| `krb5-credcachefile`        | `krb5_credcachefile` (ccache path override, Linux only) |
| `krb5-realm`                | `krb5_realm` (AD realm, UPPERCASE) |
| `service_principal_name`    | `service-principal-name`, `serviceprincipalname` (SPN override) |
| `Application Name`          | `ApplicationName`, `App Name`, `application_name` (LOGIN7 `program_name`; visible as `APP_NAME()`. URI query form: `applicationname`. Empty → `"DuckDB MSSQL Extension"`. Clamped to 128 UTF-16 code units.) |

#### URI Format

```sql
ATTACH 'mssql://user:password@host:port/database?encrypt=true'
    AS context_name (TYPE mssql);
```

URI format supports URL-encoded components for special characters in credentials.

### Integrated Authentication (Kerberos / SSPI)

POSIX users with an Active-Directory-joined SQL Server can authenticate via
Kerberos after running `kinit`. The simplest form (pyodbc-compatible alias):

```sql
ATTACH 'Server=sqlhost.corp.example.com;Database=YourDB;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes'
    AS db (TYPE mssql);
```

Or the explicit `microsoft/go-mssqldb` form:

```sql
ATTACH 'Server=sqlhost.corp.example.com;Database=YourDB;authenticator=krb5;Encrypt=yes'
    AS db (TYPE mssql);
```

Three credential modes are supported on POSIX:

- **Credential cache** (default) — uses a `kinit` ticket. Works on Linux and macOS.
- **Keytab** — `krb5-keytabfile=/path` + `User Id=svc@REALM`. Linux only.
- **Raw credentials** — username + password + realm via `CREATE SECRET` only (not connection string, to keep cleartext out of logs). Linux only.

On Windows, **SSPI** (`authenticator=winsspi` or `Trusted_Connection=yes`) authenticates with the current Windows logon session via `secur32.dll`'s Negotiate package — no `kinit` needed. The connection-string surface is identical to POSIX; `Trusted_Connection=yes` / `Integrated Security=SSPI` resolve to `winsspi` automatically on Windows hosts.

See [Kerberos.md](/connection/kerberos/) for prerequisites, full connection-string
reference, the bundled docker-compose test stack (no real AD required),
troubleshooting (including WSL2 specifics), and SPN verification.

### TLS/SSL Configuration

To enable encrypted connections:

#### Using Secret

```sql
CREATE SECRET secure_conn (
    TYPE mssql,
    host 'sql-server.example.com',
    port 1433,
    database 'MyDatabase',
    user 'sa',
    password 'Password123',
    use_encrypt true
);
```

#### Using Connection String

```sql
ATTACH 'Server=sql-server.example.com,1433;Database=MyDatabase;User Id=sa;Password=Password123;Encrypt=yes'
    AS db (TYPE mssql);
```

#### Using URI

```sql
ATTACH 'mssql://sa:Password123@sql-server.example.com:1433/MyDatabase?encrypt=true'
    AS db (TYPE mssql);
```

> **Note**: TLS is enabled by default for security. Use `use_encrypt=false` or `Encrypt=no` to disable. TLS support is available in both static and loadable extension builds (using OpenSSL).

#### TrustServerCertificate Parameter

For compatibility with ADO.NET connection strings, `TrustServerCertificate` is supported as an alias for `Encrypt`:

```sql
-- Using TrustServerCertificate (equivalent to Encrypt=yes)
ATTACH 'Server=localhost,1433;Database=master;User Id=sa;Password=pass;TrustServerCertificate=true'
    AS db (TYPE mssql);
```

> **Note**: If both `Encrypt` and `TrustServerCertificate` are specified with conflicting values (e.g., `Encrypt=true;TrustServerCertificate=false`), ATTACH will fail with an error. Either omit one parameter or ensure they have the same value.

### Catalog-Free Mode

For serverless databases (like Azure SQL Serverless) or databases with restricted permissions where catalog queries fail, disable catalog integration:

#### Using Secret

```sql
CREATE SECRET serverless_db (
    TYPE mssql,
    host 'myserver.database.windows.net',
    port 1433,
    database 'mydb',
    user 'sa',
    password 'Password123',
    catalog false  -- Disable catalog integration
);

ATTACH '' AS serverless (TYPE mssql, SECRET serverless_db);
```

#### Using Connection String

```sql
ATTACH 'Server=myserver.database.windows.net,1433;Database=mydb;User Id=sa;Password=Password123;Catalog=false'
    AS serverless (TYPE mssql);
```

With catalog disabled:
- `mssql_scan()` and `mssql_exec()` work normally for raw SQL queries
- Schema browsing via `duckdb_schemas()`, `duckdb_tables()` is not available
- Three-part naming (`db.schema.table`) is not available
- Use `mssql_scan()` for all queries instead

### Catalog Filters

For large databases with thousands of schemas or tables, you can filter which objects are visible to DuckDB using regex patterns. This significantly reduces metadata loading time and memory usage.

#### Using Secret

```sql
CREATE SECRET erp_db (
    TYPE mssql,
    host 'erp-server.example.com',
    port 1433,
    database 'ERP',
    user 'readonly',
    password 'Password123',
    schema_filter '^(dbo|sales|inventory)$',  -- Only these schemas
    table_filter '^(Order|Product|Customer)'   -- Tables starting with these prefixes
);

ATTACH '' AS erp (TYPE mssql, SECRET erp_db);
```

#### Using Connection String

```sql
ATTACH 'Server=erp-server,1433;Database=ERP;User Id=sa;Password=pass;SchemaFilter=^dbo$;TableFilter=^Order'
    AS erp (TYPE mssql);
```

#### Filter Behavior

- Filters use case-insensitive regex partial match (C++ `std::regex_search`)
- Use `^` and `$` anchors for exact matching: `^dbo$` matches only "dbo"
- Without anchors, `dbo` matches "dbo", "dbo_archive", "test_dbo", etc.
- Filters apply to catalog browsing, schema scans, and metadata loading
- `mssql_scan()` and `mssql_exec()` bypass filters (raw SQL access)

### Connection Validation

The extension validates connections at ATTACH time, providing immediate feedback on configuration errors:

```sql
-- Invalid hostname - fails immediately with clear error
ATTACH 'Server=nonexistent.host,1433;Database=master;User Id=sa;Password=pass'
    AS db (TYPE mssql);
-- Error: MSSQL connection validation failed: Cannot resolve hostname 'nonexistent.host'

-- Invalid credentials - fails immediately
ATTACH 'Server=localhost,1433;Database=master;User Id=wrong;Password=wrong'
    AS db (TYPE mssql);
-- Error: MSSQL connection validation failed: Authentication failed for user 'wrong'
```

This fail-fast behavior ensures that:

1. **No orphaned catalogs**: Failed ATTACH operations do not create catalog entries
2. **Clear error messages**: Connection errors are reported immediately with specific details
3. **Faster debugging**: Invalid configurations are caught at ATTACH time, not during first query
4. **Password never leaks**: error messages never include the password (audited)

**Opt out per-ATTACH** for container/orchestrator startup where the SQL Server may not yet be reachable:

```sql
ATTACH 'Server=...' AS db (TYPE mssql, lazy_validation true);
```

With `lazy_validation true`, ATTACH succeeds without the TCP+LOGIN7 round trip; the first query then pays the connection-establishment cost (pre-spec-047 behaviour). The eager-validation ceiling is bounded by `mssql_attach_validation_timeout` (default `0` inherits `mssql_connection_timeout`).

### ATTACH Options Reference

In addition to options propagated from the secret / connection string, the following ATTACH options are accepted directly:

| Option              | Type    | Description                                                                  |
| ------------------- | ------- | ---------------------------------------------------------------------------- |
| `SECRET`            | VARCHAR | Name of an MSSQL secret holding connection parameters                        |
| `azure_secret`      | VARCHAR | Override / supply Azure secret name for Azure AD auth                        |
| `access_token`      | VARCHAR | Pre-acquired Azure AD JWT (see [AZURE.md](/connection/azure/))                         |
| `catalog`           | BOOLEAN | Enable catalog integration (default `true`)                                  |
| `schema_filter`     | VARCHAR | Override secret schema_filter for this ATTACH                                |
| `table_filter`      | VARCHAR | Override secret table_filter for this ATTACH                                 |
| `order_pushdown`    | BOOLEAN | Per-ATTACH ORDER BY pushdown override (overrides `mssql_order_pushdown` setting) |
| `lazy_validation`   | BOOLEAN | Skip the eager ATTACH-time credential check (default `false`)                |
| `application_name`  | VARCHAR | Override LOGIN7 `program_name` for this ATTACH (also accepts `applicationname`) |

### Named Instances

`Server=host\instance` resolves the instance's dynamic TCP port through the
SQL Server Browser (UDP 1434) at ATTACH time:

```sql
ATTACH 'Server=myhost\SQLEXPRESS;Database=mydb;User Id=sa;Password=...' AS db (TYPE mssql);
```

In environments that strip outbound UDP 1434, set
`mssql_named_instance_resolution = false` — a named instance then errors
instead of silently trying port 1433 — and connect with an explicit
`Server=host,port`. The Browser query timeout is
`mssql_browser_timeout_seconds` (default 3 s, one retry).

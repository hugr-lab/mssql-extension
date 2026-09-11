---
title: Connections & Authentication
sidebar_position: 1
---

# Connection Configuration

There are three ways to tell `ATTACH` where and how to connect, and they
compose:

1. **ADO.NET connection string** — the familiar `Server=...;Database=...` form
2. **URI** — `mssql://user:pass@host:port/db?...`
3. **DuckDB secret** — credentials stored once, referenced by name

Values are resolved with a fixed precedence: **ATTACH options override
connection-string / URI values, which override secret values.** So a secret
can carry the credentials while an individual `ATTACH` overrides, say, the
filters or the application name.

### Using Connection Strings (ADO.NET style)

```sql
-- The basics: host, port, database, SQL authentication, TLS on
ATTACH 'Server=localhost,1433;Database=AdventureWorks;User Id=sa;Password=...;Encrypt=yes'
    AS mssql (TYPE mssql);

-- Named instance: the port is resolved through SQL Server Browser (UDP 1434)
ATTACH 'Server=myhost\SQLEXPRESS;Database=mydb;User Id=app;Password=...'
    AS db (TYPE mssql);

-- Integrated authentication (Kerberos on POSIX, SSPI on Windows)
ATTACH 'Server=sql.corp.example.com;Database=Sales;Trusted_Connection=yes'
    AS sales (TYPE mssql);

-- Identify the client to the server (visible in sys.dm_exec_sessions)
ATTACH 'Server=host;Database=db;User Id=u;Password=p;Application Name=nightly-etl'
    AS etl (TYPE mssql);
```

`Server=host` defaults the port to 1433; `Server=host,port` sets it
explicitly. Keys are case-insensitive and accept the usual ADO.NET aliases
(full table below).

### Using URIs

```sql
ATTACH 'mssql://user:password@host:1433/database?encrypt=true'
    AS db (TYPE mssql);
```

Structure: `mssql://[user[:password]@]host[:port]/database[?param=value&...]`.
User and password are URL-decoded, so special characters go in %-encoded.
Recognized query parameters:

| Parameter | Values | Description |
|---|---|---|
| `encrypt` | `true`/`false` | TLS (default: true) |
| `trustservercertificate` | `true`/`false` | Accept the server certificate without verifying it (default: `false`, verify) |
| `hostnameincertificate` | host name | Name the certificate must carry, when it differs from the host connected to |
| `catalog` | `true`/`false` | Catalog integration (default: true) |
| `schema_filter` / `table_filter` | regex | Limit visible schemas / tables |
| `applicationname` | string | LOGIN7 `program_name` (spaceless form in URIs) |

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
| `trust_server_certificate` | BOOLEAN | No | Accept the server certificate without verifying it (default: false, verify) |
| `host_name_in_certificate` | VARCHAR | No | Name the certificate must carry, when it differs from `host` |
| `catalog`     | BOOLEAN | No       | Enable catalog integration (default: true). Set to false for serverless/restricted databases that don't support catalog queries |
| `schema_filter` | VARCHAR | No     | Regex pattern to filter visible schemas (case-insensitive partial match) |
| `table_filter`  | VARCHAR | No     | Regex pattern to filter visible tables/views (case-insensitive partial match) |
| `azure_secret`  | VARCHAR | No     | Name of an Azure secret (DuckDB Azure extension) for Azure AD auth — see [AZURE.md](./azure.md) |
| `access_token`  | VARCHAR | No     | Pre-acquired Azure AD JWT (hidden in `duckdb_secrets()`) — see [AZURE.md](./azure.md) |
| `authenticator` | VARCHAR | No     | `krb5` (POSIX) or `winsspi` (Windows SSPI) — Kerberos / SSPI integrated auth, see [Kerberos.md](./kerberos.md) |
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

### Connection String Key Aliases (case-insensitive)

| Key                         | Aliases                              |
| --------------------------- | ------------------------------------ |
| `Server`                    | `Data Source`                        |
| `Database`                  | `Initial Catalog`                    |
| `User Id`                   | `Uid`, `User`                        |
| `Password`                  | `Pwd`                                |
| `Encrypt`                   | `Use Encryption for Data`            |
| `TrustServerCertificate`    | (no alias; see TLS/SSL Configuration) |
| `HostNameInCertificate`     | `HostnameInCertificate` (ODBC spelling) |
| `Trusted_Connection`        | `Trusted Connection`, `TrustedConnection` (yes/true/SSPI/1 -> Kerberos on POSIX, SSPI on Windows; see [Kerberos.md](./kerberos.md)) |
| `Integrated Security`       | `IntegratedSecurity`, `Integrated_Security` (same resolution as `Trusted_Connection`) |
| `authenticator`             | `krb5` or `winsspi` (see [Kerberos.md](./kerberos.md)) |
| `krb5-keytabfile`           | `krb5_keytabfile` (path to keytab; selects keytab mode, Linux only) |
| `krb5-configfile`           | `krb5_configfile` (per-connection `/etc/krb5.conf` override, Linux only) |
| `krb5-credcachefile`        | `krb5_credcachefile` (ccache path override, Linux only) |
| `krb5-realm`                | `krb5_realm` (AD realm, UPPERCASE) |
| `service_principal_name`    | `service-principal-name`, `serviceprincipalname` (SPN override) |
| `Application Name`          | `ApplicationName`, `App Name`, `application_name` (LOGIN7 `program_name`; visible as `APP_NAME()`. URI query form: `applicationname`. Empty → `"DuckDB MSSQL Extension"`. Clamped to 128 UTF-16 code units.) |

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

See [Kerberos.md](./kerberos.md) for prerequisites, full connection-string
reference, the bundled docker-compose test stack (no real AD required),
troubleshooting (including WSL2 specifics), and SPN verification.

### TLS/SSL Configuration

Three options, with the meanings and defaults of the Microsoft drivers (ODBC 18,
`Microsoft.Data.SqlClient` 4.0, JDBC 10.2, `go-mssqldb`):

| ADO.NET key / URI parameter / secret field | Default | Meaning |
|---|---|---|
| `Encrypt` / `encrypt` / `use_encrypt` | `true` | Encrypt the session. `false` sends no TLS at all, the login packet included, and the two options below are ignored. |
| `TrustServerCertificate` / `trustservercertificate` / `trust_server_certificate` | `false` | `false`: the server's certificate chain must validate against the platform trust store, and its subject must match the host connected to. `true`: any certificate is accepted; the channel is still encrypted, what is given up is knowing who is at the other end. |
| `HostNameInCertificate` / `hostnameincertificate` / `host_name_in_certificate` | the host connected to | The name the certificate must carry when it differs from the address you connect to: an IP, an SSH tunnel to `localhost`, an alias the certificate was not issued for. After a login-time routing hop (Azure SQL redirect, Fabric) the routed host is checked, unless this is set, in which case it applies to every hop. |

```sql
-- Production: encrypted and verified (both are the defaults)
ATTACH 'Server=sql.example.com,1433;Database=Sales;User Id=app;Password=...' AS db (TYPE mssql);

-- Through an SSH tunnel: the certificate says sql.example.com, the socket says localhost
ATTACH 'Server=localhost,14330;Database=Sales;User Id=app;Password=...;HostNameInCertificate=sql.example.com' AS db (TYPE mssql);

-- A self-signed certificate (docker, an on-prem instance with none installed)
ATTACH 'Server=localhost,1433;Database=master;User Id=sa;Password=...;TrustServerCertificate=yes' AS db (TYPE mssql);

-- URI and secret spellings
ATTACH 'mssql://app:...@localhost:14330/Sales?hostnameincertificate=sql.example.com' AS db (TYPE mssql);
CREATE SECRET dev (TYPE mssql, host 'localhost', port 1433, database 'master', user 'sa', password '...',
                   trust_server_certificate true);
```

#### Self-signed certificates

Every SQL Server without an installed certificate, the docker image included,
runs on a self-generated one. The default rejects it and says so:

```text
TLS handshake failed: certificate verification failed for localhost: self-signed certificate. Set TrustServerCertificate=yes to accept this server's certificate without verification, or HostNameInCertificate=<name> if the certificate is valid but issued for a different name
```

Add `TrustServerCertificate=yes`, which is what `sqlcmd -C` does. The reason in
the message is OpenSSL's own (`self-signed certificate`, `unable to get local
issuer certificate`, `hostname mismatch`, `certificate has expired`), so it can
be searched for.

#### Where the trusted roots come from

The platform's store, the same one the Azure AD token request uses: on Windows
the `ROOT` and `CA` system stores; on macOS the keychain trust settings plus
OpenSSL's default paths; on Linux OpenSSL's default paths (`/etc/ssl/certs`,
package `ca-certificates`). `SSL_CERT_FILE` / `SSL_CERT_DIR` override the
paths on every platform. A private CA goes into the platform store or into
`SSL_CERT_FILE`; there is no per-connection CA-file option.

> **Changed in spec 074**: `TrustServerCertificate` used to be an alias of
> `Encrypt` and nothing verified the server certificate. It is now its own
> option, `false` by default. A connection string that reached a self-signed
> server without it needs `TrustServerCertificate=yes` added.

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
| `access_token`      | VARCHAR | Pre-acquired Azure AD JWT (see [AZURE.md](./azure.md))                         |
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

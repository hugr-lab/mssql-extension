# Quickstart — Named-Instance Resolution (Spec 046)

End-to-end recipes for using the named-instance support shipped in
spec 046. Closes [#77](https://github.com/hugr-lab/mssql-extension/issues/77).

## TL;DR — connect to a named instance

After spec 046, `Server=host\instance` works the same way it does in
every other SQL Server client (pyodbc, sqlcmd, JDBC, go-mssqldb):

```sql
-- Default ATTACH form
ATTACH 'Server=localhost\SS2022;Database=AdventureWorks;User Id=sa;Password=...;TrustServerCertificate=yes'
  AS adw (TYPE mssql);
SELECT TOP 1 name FROM adw.sys.tables;
```

Under the hood:
1. Parser splits `localhost\SS2022` into `host="localhost"`,
   `instance="SS2022"`.
2. Instance name validated against `[A-Za-z0-9_$#]{1,16}` at parse time.
3. Resolver sends a `CLNT_UCAST_INST` query to `localhost:1434/udp`.
4. Browser replies with the dynamic TCP port for `SS2022`.
5. Extension connects to `localhost:<resolved-port>` via the existing
   TDS path. LOGIN7 `ServerName` carries `localhost\SS2022`.

## URI form

```sql
-- %5C is the URL-encoded backslash. Literal `\` is rejected at parse time.
ATTACH 'mssql://sa:secret@localhost%5CSS2022/AdventureWorks?trustservercertificate=yes'
  AS adw (TYPE mssql);
```

## Secret form

```sql
CREATE SECRET ss2022_secret (
    TYPE mssql,
    host 'localhost\SS2022',
    database 'AdventureWorks',
    user 'sa',
    password 'secret'
);
ATTACH '' AS adw (TYPE mssql, SECRET ss2022_secret);
```

## Skip SQL Browser (explicit port)

When the instance is configured to listen on a fixed port, include it
directly and the resolver is bypassed:

```sql
-- Goes straight to SS2022's known port (5022 in this example).
ATTACH 'Server=localhost\SS2022,5022;Database=AdventureWorks;User Id=sa;Password=...;TrustServerCertificate=yes'
  AS adw (TYPE mssql);
```

The instance name still flows through to LOGIN7 `ServerName` (visible
in `sys.dm_exec_sessions`), but no UDP query is sent. This is the
escape hatch for environments where SQL Server Browser is disabled or
UDP 1434 is firewalled.

## Settings

| Setting                            | Default | Purpose                                                              |
|------------------------------------|---------|----------------------------------------------------------------------|
| `mssql_browser_timeout_seconds`    | `3`     | UDP receive timeout per attempt (one retry on timeout, so worst-case wait is ~2×). |
| `mssql_named_instance_resolution`  | `true`  | Master switch. Set to `false` to reject `host\instance` at parse time (forces explicit-port form). |

```sql
SET mssql_browser_timeout_seconds = 5;
SET mssql_named_instance_resolution = false;  -- now host\instance fails fast
```

## Error categories

| Error message prefix                                            | What's wrong                                  | Fix |
|-----------------------------------------------------------------|-----------------------------------------------|-----|
| `SQL Browser unreachable at <host>:1434/udp after Ns`           | UDP blocked or Browser service stopped        | Check firewall (UDP 1434); start `SQL Server Browser` service on the host |
| `instance '<name>' not found on host '<host>'; available: ...`  | Misspelled name or instance not registered    | Check the available list in the error; verify with `SELECT @@SERVERNAME` on the instance |
| `instance '<name>' exists but TCP transport is disabled`        | Instance is reachable only via named pipes    | Enable TCP/IP in SQL Server Configuration Manager → instance protocols |
| `malformed SQL Browser response: ...`                           | Likely a middlebox mangling UDP 1434          | File a bug; hex dump in the error helps triage |
| `named-instance resolution is disabled`                         | `mssql_named_instance_resolution=false`       | Re-enable or use `Server=host,port` |
| `Instance name 'X' contains invalid character 'Y'`              | Out of `[A-Za-z0-9_$#]{1,16}` grammar         | Use a valid instance name |

## Manual reproduction of issue #77

Setup on a Windows host (per @Stan-RED's original report — three
coexisting instances):

```cmd
# Three SQL Server installs, each a named instance:
SS2019 (SQL Server 2019)
SS2022 (SQL Server 2022, dynamic port)
SQLEXPRESS (Express, dynamic port)
```

Before spec 046:

```sql
-- Fails with "Cannot resolve hostname 'localhost\SS2022'"
ATTACH 'Server=localhost\SS2022;Database=master;User Id=sa;Password=...;TrustServerCertificate=yes'
  AS s2022 (TYPE mssql);
```

After spec 046:

```sql
-- All three work without specifying ports
ATTACH 'Server=localhost\SS2019;Database=master;User Id=sa;Password=...;TrustServerCertificate=yes' AS s2019 (TYPE mssql);
ATTACH 'Server=localhost\SS2022;Database=master;User Id=sa;Password=...;TrustServerCertificate=yes' AS s2022 (TYPE mssql);
ATTACH 'Server=localhost\SQLEXPRESS;Database=master;User Id=sa;Password=...;TrustServerCertificate=yes' AS sqlx (TYPE mssql);

SELECT name FROM s2019.sys.databases;
SELECT name FROM s2022.sys.databases;
SELECT name FROM sqlx.sys.databases;
```

## Bringing up the docker test stack

For developing and validating the resolver without a real Windows
host, the project ships a self-contained mock-browser stack:

```bash
cd test/named-instance
docker compose up -d --build --wait
docker compose exec test-client /run-tests.sh
docker compose down -v
```

Three containers:

- `mock-browser` — Python UDP responder on `browser.example.com:1434`
  advertising `TESTINST → sql.example.com:11433`.
- `sql` — real `mcr.microsoft.com/mssql/server:2022-latest` listening
  on `sql.example.com:11433` (deliberately non-default port, so a
  passing test proves the resolver actually did the translation).
- `test-client` — Ubuntu with the freshly-built extension + DuckDB
  CLI; runs the smoke driver `run-tests.sh`.

Fault-injection modes via `MOCK_BROWSER_MODE`:

```bash
# Browser drops requests entirely
MOCK_BROWSER_MODE=silent docker compose up -d --build
# Browser returns random bytes
MOCK_BROWSER_MODE=garbage docker compose up -d --build
```

See `test/named-instance/README.md` for the full fault-mode table.

## Windows + SSPI + named-instance (manual test recipe)

The integrated-authentication path is not covered by the docker stack
(SQL Server's Linux image has no AD integration). Verify manually on a
domain-joined Windows host before each release:

```cmd
# Prereq: domain-joined Windows; SQL Server with a named instance like SS2022;
# the workstation user has a valid Kerberos TGT (use `klist` to check).

duckdb --unsigned -c "
INSTALL mssql FROM '/path/to/mssql.duckdb_extension';
LOAD mssql;
ATTACH 'Server=sqlserver.example.com\SS2022;Database=master;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes' AS s (TYPE mssql);
SELECT TOP 5 name FROM s.sys.databases;
"
```

Expected: query returns rows. Negative test: stop the SQL Server
Browser service, retry — should fail within ~6s (3s × 1 retry) with
*"SQL Browser unreachable at sqlserver.example.com:1434/udp after 6s"*.

The integrated-auth code path is unchanged from spec 042; spec 046
only changes the *target port* the auth flow uses (now the
Browser-discovered port instead of the explicit one). SPN derivation
under Kerberos uses `MSSQLSvc/<host.fqdn>:<discovered-port>`, which
matches AD's default SPN registration for named instances.

## Cross-references

- Issue: [#77](https://github.com/hugr-lab/mssql-extension/issues/77)
- Spec: `specs/046-named-instance-resolution/`
- Microsoft docs:
  [Database engine instances](https://learn.microsoft.com/en-us/sql/database-engine/configure-windows/database-engine-instances-sql-server),
  [MC-SQLR](https://learn.microsoft.com/en-us/openspecs/windows_protocols/mc-sqlr/)
- Related specs: spec 042 (Integrated Auth — SPN derivation against
  discovered port), spec 031 (`MSSQLConnectionInfo` plumbing).

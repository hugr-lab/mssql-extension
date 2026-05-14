# Kerberos / Integrated Authentication

End-user guide for connecting to Active-Directory-joined SQL Server via Kerberos
(POSIX) or SSPI (Windows). Spec 042. Mirrors the structure of [AZURE.md](AZURE.md).

## Table of Contents

- [Quick Start](#quick-start)
- [Prerequisites](#prerequisites)
- [Credential Modes](#credential-modes)
- [Connection Examples](#connection-examples)
- [Using MSSQL Secrets](#using-mssql-secrets)
- [Diagnostic function: `mssql_kerberos_auth_test`](#diagnostic-function-mssql_kerberos_auth_test)
- [Diagnostic function: `mssql_winsspi_auth_test` (Windows)](#diagnostic-function-mssql_winsspi_auth_test-windows)
- [Troubleshooting](#troubleshooting)
- [Running the test stack locally](#running-the-test-stack-locally)
- [Reference](#reference)

## Quick Start

On a POSIX host with a valid Kerberos ticket:

```sh
kinit alice@CORP.EXAMPLE.COM     # acquire TGT (password prompt)
klist                             # verify
```

Then in DuckDB:

```sql
LOAD mssql;
ATTACH 'Server=sqlhost.corp.example.com,1433;Database=AdventureWorks;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes'
    AS adw (TYPE mssql);

SELECT TOP 5 name FROM adw.sys.tables;
```

That's it. The extension picks up your `kinit` ticket from the credential cache,
negotiates SPNEGO with the SQL Server's SPN, and connects.

## Prerequisites

### POSIX (Linux, macOS)

| Step | Linux (Debian / Ubuntu) | Linux (RHEL / Fedora) | macOS |
|---|---|---|---|
| Install client tools | `sudo apt install krb5-user libgssapi-krb5-2` | `sudo dnf install krb5-workstation` | built into the OS |
| Build-time dep (only if you build the extension yourself) | `sudo apt install libkrb5-dev` | `sudo dnf install krb5-devel` | built into the OS |
| Configure `/etc/krb5.conf` | yes — point at your AD KDCs | yes | yes |
| Acquire ticket | `kinit user@REALM` | `kinit user@REALM` | `kinit user@REALM` |
| Verify | `klist` | `klist` | `klist` |

Minimal `/etc/krb5.conf`:

```ini
[libdefaults]
    default_realm = CORP.EXAMPLE.COM
    dns_lookup_kdc = true
    forwardable = true
    rdns = false

[realms]
    CORP.EXAMPLE.COM = {
        kdc = dc01.corp.example.com:88
        kdc = dc02.corp.example.com:88
    }

[domain_realm]
    .corp.example.com = CORP.EXAMPLE.COM
```

### Windows

Native Windows SSPI via `secur32.dll`'s Negotiate package. Uses the current
Windows logon session — no `kinit` needed. Same connection-string surface as
POSIX:

```sql
ATTACH 'Server=sqlhost.corp.example.com,1433;Database=YourDB;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes'
    AS db (TYPE mssql);
```

Or the explicit form:

```sql
ATTACH 'Server=sqlhost.corp.example.com,1433;Database=YourDB;authenticator=winsspi;Encrypt=yes;TrustServerCertificate=yes'
    AS db (TYPE mssql);
```

`Trusted_Connection=yes` and `Integrated Security=SSPI` both resolve to
`authenticator=winsspi` on Windows; on POSIX they resolve to
`authenticator=krb5`. The behavior is the same — your machine's existing
authentication credentials are used.

**Prerequisites:**

- Domain-joined Windows host (or sufficient privileges to acquire a Kerberos
  TGT for the target realm).
- SQL Server's SPN registered in AD with the canonical
  `MSSQLSvc/<fqdn>:<port>` form. Verify with `setspn -L DOMAIN\sqlservice`
  from an admin PowerShell.
- No build-time dependencies — `secur32.lib` is part of the Windows SDK.

**Limitations:**

- Only credential-cache mode (current logon session). Keytab and raw-credentials
  modes are POSIX-only.
- Negotiate falls back to NTLM transparently when the KDC isn't reachable for
  the target SPN. If you require Kerberos-only authentication, ensure the SPN
  is registered correctly so the fallback is never triggered.

### WSL2 (Ubuntu under Windows)

WSL2 is a fully supported testing path — it's a real Linux kernel with MIT
Kerberos.

```sh
# 1. Install Kerberos client + build deps (one-time)
sudo apt update
sudo apt install -y krb5-user libgssapi-krb5-2 libssl3 chrony

# 2. Configure /etc/krb5.conf (same as Linux above)
sudo nano /etc/krb5.conf

# 3. Acquire ticket
kinit alice@CORP.EXAMPLE.COM
klist

# 4. Use the extension (built or downloaded as a Linux .duckdb_extension)
duckdb --unsigned -c "
  LOAD '/path/to/mssql.duckdb_extension';
  ATTACH 'Server=sqlhost.corp.example.com;Database=YourDB;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes' AS p (TYPE mssql);
  SELECT TOP 5 name FROM p.sys.tables;
"
```

**WSL2-specific gotchas:**

1. **No SSO with Windows logon.** WSL2's Linux ccache lives at
   `/tmp/krb5cc_$UID`, separate from Windows' credential cache. You must `kinit`
   inside WSL2 even if your Windows session already has a TGT.

2. **Clock drift after laptop sleep.** WSL2's clock can drift far enough to
   trigger `KRB5KRB_AP_ERR_SKEW`. Fix:
   ```sh
   sudo apt install chrony     # persistent
   sudo hwclock -s             # one-shot sync from BIOS
   ```

3. **DNS to corporate DCs.** If your DCs aren't resolvable from WSL2 by default,
   add to `/etc/wsl.conf`:
   ```ini
   [network]
   generateResolvConf = false
   ```
   then write your own `/etc/resolv.conf`, or hardcode KDCs in `krb5.conf`.

4. **SPN verification from WSL2.** `setspn -L` is Windows-only, but you can ask
   the KDC directly:
   ```sh
   kvno -S MSSQLSvc sqlhost.corp.example.com   # asks for a service ticket
   ```
   If this succeeds, the SPN is registered correctly.

## Credential Modes

Three modes are supported on POSIX. The mode is selected by which connection-string
keys (or secret fields) are populated.

### 1. Credential Cache (recommended for interactive use)

Default mode when you have a `kinit` ticket. The extension uses
`GSS_C_NO_CREDENTIAL` so GSSAPI picks up the ticket from `KRB5CCNAME` or
`/tmp/krb5cc_<uid>`.

```sql
ATTACH 'Server=sqlhost;Database=db;Trusted_Connection=yes' AS k (TYPE mssql);
```

Works on **Linux + macOS**.

### 2. Keytab (recommended for service accounts / CI)

Provide a keytab file containing the service account's long-term keys.
GSSAPI acquires a TGT internally — no `kinit` needed.

```sql
ATTACH 'Server=sqlhost;Database=db;authenticator=krb5;krb5-keytabfile=/etc/keytabs/svc.keytab;User Id=svc@CORP.EXAMPLE.COM'
    AS k (TYPE mssql);
```

**Linux only** — macOS's bundled GSS framework lacks the required MIT
extensions (`gss_acquire_cred_from`).

### 3. Raw Credentials (SECRET-ONLY)

Domain user + password, no `kinit` required. **Available only via `CREATE
SECRET`** — `Password` in a connection string is unconditionally rejected to
keep cleartext passwords out of logs.

```sql
CREATE SECRET kerb_raw (
    TYPE mssql,
    host 'sqlhost.corp.example.com',
    port 1433,
    database 'AdventureWorks',
    authenticator 'krb5',
    user 'alice',
    password 'her-password',
    krb5_realm 'CORP.EXAMPLE.COM',
    use_encrypt true
);

ATTACH '' AS adw (TYPE mssql, SECRET kerb_raw);
```

**Linux only.** macOS lacks `krb5_get_init_creds_password` in the public SDK.

## Connection Examples

### ADO.NET style

```sql
-- Simplest (uses defaults; pyodbc-compatible)
ATTACH 'Server=sqlhost;Database=db;Trusted_Connection=yes' AS k (TYPE mssql);

-- ADO.NET canonical alias
ATTACH 'Server=sqlhost;Database=db;Integrated Security=SSPI' AS k (TYPE mssql);
ATTACH 'Server=sqlhost;Database=db;Integrated Security=true' AS k (TYPE mssql);

-- Explicit go-mssqldb form
ATTACH 'Server=sqlhost;Database=db;authenticator=krb5' AS k (TYPE mssql);

-- Keytab mode (service account)
ATTACH 'Server=sqlhost;Database=db;authenticator=krb5;krb5-keytabfile=/etc/keytabs/svc.keytab;User Id=svc@CORP.EXAMPLE.COM'
    AS k (TYPE mssql);

-- Override SPN when DNS doesn't match what's registered in AD
ATTACH 'Server=10.0.0.5;Database=db;Trusted_Connection=yes;service_principal_name=MSSQLSvc/sqlhost.corp.example.com:1433'
    AS k (TYPE mssql);

-- Per-connection krb5.conf override (Linux only)
ATTACH 'Server=sqlhost;Database=db;Trusted_Connection=yes;krb5-configfile=/opt/myapp/krb5.conf'
    AS k (TYPE mssql);
```

### URI style

```sql
ATTACH 'mssql://sqlhost/db?authenticator=krb5' AS k (TYPE mssql);

ATTACH 'mssql://sqlhost/db?authenticator=krb5&krb5-keytabfile=/etc/keytabs/svc.keytab&krb5-realm=CORP.EXAMPLE.COM'
    AS k (TYPE mssql);
```

### MSSQL Secret

```sql
CREATE SECRET kerb (
    TYPE mssql,
    host 'sqlhost.corp.example.com',
    port 1433,
    database 'AdventureWorks',
    authenticator 'krb5',
    use_encrypt true
);

ATTACH '' AS adw (TYPE mssql, SECRET kerb);
```

### service_principal_name override

When the host you `Server=` to is an IP address, alias, or doesn't match the
registered SPN's hostname:

```sql
ATTACH 'Server=sql-internal.local,1433;Database=db;Trusted_Connection=yes;service_principal_name=MSSQLSvc/sqlhost.corp.example.com:1433'
    AS k (TYPE mssql);
```

Accepts either the canonical Kerberos principal form (`MSSQLSvc/host:port`) or
the hostbased-service form (`MSSQLSvc@host`).

## Using MSSQL Secrets

Use a secret when you want to:

- Persist Kerberos config across DuckDB sessions
- Use raw credentials (the only path that accepts a Password)
- Keep connection strings clean

```sql
-- Credential cache (kinit ticket) -- minimal secret
CREATE SECRET kerb_ccache (
    TYPE mssql,
    host 'sqlhost.corp.example.com',
    port 1433,
    database 'AdventureWorks',
    authenticator 'krb5'
);

-- Keytab for a service account
CREATE SECRET kerb_keytab (
    TYPE mssql,
    host 'sqlhost.corp.example.com',
    port 1433,
    database 'AdventureWorks',
    authenticator 'krb5',
    user 'svc-etl@CORP.EXAMPLE.COM',
    krb5_keytabfile '/etc/keytabs/svc-etl.keytab',
    use_encrypt true
);

-- Raw credentials -- ONLY available via secret
CREATE SECRET kerb_raw (
    TYPE mssql,
    host 'sqlhost.corp.example.com',
    port 1433,
    database 'AdventureWorks',
    authenticator 'krb5',
    user 'alice',
    password 'her-password',
    krb5_realm 'CORP.EXAMPLE.COM'
);

-- All Kerberos secret fields:
--   authenticator               -- 'krb5' (or 'winsspi' on Windows, pending)
--   krb5_configfile             -- /etc/krb5.conf override (Linux only)
--   krb5_keytabfile             -- selects keytab mode
--   krb5_credcachefile          -- ccache path override (Linux only)
--   krb5_realm                  -- AD realm (UPPERCASE)
--   service_principal_name      -- SPN override
```

Note the underscore convention in secret fields vs hyphens in connection strings
— this matches the project's existing convention (`schema_filter` in secrets,
`schema-filter` in connection strings).

## Diagnostic function: `mssql_kerberos_auth_test`

Test the Kerberos auth path **without actually connecting to SQL Server**.
Parallels `mssql_azure_auth_test` for Azure AD. Useful for confirming that
`/etc/krb5.conf` is reachable from the DuckDB process, the ccache has a
valid TGT, the SPN resolves through the KDC, and `gss_init_sec_context`
produces a non-empty SPNEGO blob — none of which `kinit` + `klist` alone
verify.

Three overloads:

```sql
-- 1. Smoke test against a host, default port 1433, default ccache.
--    Derives SPN = MSSQLSvc/<host>:1433.
SELECT mssql_kerberos_auth_test('sqlhost.corp.example.com');

-- 2. Explicit port (named-instance / non-1433 deployments).
--    Derives SPN = MSSQLSvc/<host>:<port>.
SELECT mssql_kerberos_auth_test('sqlhost.corp.example.com', 1433);

-- 3. Against a configured MSSQL secret -- exercises the FULL Krb5Config
--    path (honors keytab, krb5-realm, service_principal_name override,
--    etc.) exactly the way an ATTACH against the same secret would.
SELECT mssql_kerberos_auth_test_secret('my_kerb_secret');
```

### Success output

One-line status string:

```text
OK: principal=alice@CORP.EXAMPLE.COM, spn=MSSQLSvc/sqlhost.corp.example.com:1433, mech=SPNEGO, token_size=1834 bytes
```

The values reveal the same things an experienced SRE would check manually:

- `principal=...` — what `klist` reports as the default principal in the
  ccache (or `<no ticket>` if the ccache is empty). On macOS this shows
  `<macOS: run klist>` because GSS.framework doesn't export the krb5_cc_*
  extensions; run `klist` from the shell to see the principal there.
- `spn=...` — the exact service principal name the KDC was asked for.
  Compare to `setspn -L DOMAIN\sqlservice` on a Windows admin host.
- `mech=SPNEGO` — confirms the mechanism is right (SQL Server expects
  SPNEGO, not raw Kerberos).
- `token_size=...` — sanity check that the blob isn't suspiciously small
  (a 100-byte token usually means SPNEGO couldn't establish the underlying
  Kerberos mech).

### Failure output

The function returns the verbatim error message — same wording that
ATTACH-time validation would produce, including the actionable hint from
the GSSAPI status taxonomy. Examples:

```text
-- No kinit yet
MSSQL Kerberos auth failed: gss_acquire_cred: ... No credentials cache file found.
(Hint: no credentials cache. Run 'kinit <user>@<REALM>' first.)

-- SPN missing from AD
MSSQL Kerberos auth failed: gss_init_sec_context: ... Server not found in Kerberos database
(Hint: server SPN not registered. Verify with 'setspn -L <account>' on Windows admin host.)

-- Clock skew
MSSQL Kerberos auth failed: gss_init_sec_context: ... Clock skew too great
(Hint: clock skew between client and KDC exceeds 5 minutes. Sync system clock via ntp/chrony.)

-- Wrong secret type
MSSQL Kerberos auth test: secret 'sql_pwd' is not configured for Kerberos (authenticator != 'krb5').
Add authenticator 'krb5' to the secret.

-- Build without Kerberos support
MSSQL Kerberos auth test: this build of the mssql extension was compiled without
Kerberos support (MSSQL_ENABLE_KRB5 was not defined). Rebuild with -DENABLE_KRB5=ON.
```

### When to use each variant

| Goal | Function |
|---|---|
| Quick "can I auth at all?" check | `mssql_kerberos_auth_test('host')` |
| Verifying a non-default port resolves the right SPN | `mssql_kerberos_auth_test('host', 12345)` |
| Validating a keytab / SPN override before ATTACH | `mssql_kerberos_auth_test_secret('secret_name')` |
| Pre-flighting a CI pipeline (service account + keytab) | `mssql_kerberos_auth_test_secret(...)` then ATTACH |

### Limitations

- Tests the **client-side** GSSAPI flow only. It does NOT confirm that
  SQL Server itself will accept the ticket (the server-side mapping
  `EXAMPLE.COM\testuser` → SQL login needs `setspn`-style work that's
  outside the extension's scope).
- A pass here + ATTACH failure → almost always a server-side login
  mapping issue (run `init.sql`-style `CREATE LOGIN [REALM\user] FROM
  WINDOWS` on the SQL Server side).
- A fail here + working `kinit` → `mssql_kerberos_auth_test` exposes a
  bug in your config that `kinit` didn't catch (wrong SPN, wrong realm
  resolution, etc.).

## Diagnostic function: `mssql_winsspi_auth_test` (Windows)

The Windows SSPI peer of `mssql_kerberos_auth_test`. Same idea: exercises
the client-side handshake (`AcquireCredentialsHandleW` +
`InitializeSecurityContextW`, Negotiate package) **without** connecting to
SQL Server. Useful for confirming the current logon session has a valid
ticket, the SPN resolves, and Negotiate produces a non-empty token.

Three overloads:

```sql
-- 1. Smoke test against a host, default port 1433.
SELECT mssql_winsspi_auth_test('sqlhost.corp.example.com');

-- 2. Explicit port.
SELECT mssql_winsspi_auth_test('sqlhost.corp.example.com', 1433);

-- 3. Explicit SPN -- override default MSSQLSvc/<host>:<port> derivation.
SELECT mssql_winsspi_auth_test_spn('MSSQLSvc/sqlcluster.corp.example.com:1433');
```

### Success output

```text
OK: principal=alice@CORP.EXAMPLE.COM, spn=MSSQLSvc/sqlhost.corp.example.com:1433, mech=Negotiate, token_size=1652 bytes
```

`principal` here is the current logon session's UPN (via
`GetUserNameEx(NameUserPrincipal)`); falls back to `DOMAIN\user` if the UPN
isn't set on the account. `mech=Negotiate` is the SSPI package name —
Negotiate transparently selects Kerberos when the SPN resolves through AD
and falls back to NTLM otherwise, which is why we report it explicitly.

### Failure output

The function returns the verbatim error message from
`winsspi_authenticator.cpp`, wrapping `FormatMessageW` plus the SSPI status
code. Common cases:

```text
-- SPN not registered for this server
MSSQL Kerberos auth failed: InitializeSecurityContext failed:
sspi_status=0x80090303, The specified target is unknown or unreachable.
(Hint: SPN not found in Active Directory. Verify with 'setspn -L domain\sqlsvc'.)

-- No domain credentials in the current session
MSSQL Kerberos auth failed: InitializeSecurityContext failed:
sspi_status=0x8009030E, No credentials are available in the security package.
(Hint: log in as a domain user, or run 'runas /netonly' against domain creds.)

-- Clock skew
MSSQL Kerberos auth failed: InitializeSecurityContext failed:
sspi_status=0x80090324, The clocks on the client and server machines are skewed.
(Hint: sync system clock; typical tolerance is ±5 minutes.)
```

### Function availability

- On Windows builds: real implementation, exercises SSPI Negotiate.
- On Linux / macOS builds: returns a clear "wrong platform — use
  `mssql_kerberos_auth_test` on POSIX" message. Registered unconditionally
  so the function exists on every platform and gives consistent diagnostics
  rather than "function does not exist".

## Troubleshooting

### Common Errors

Every error from the Kerberos path is prefixed with `MSSQL Kerberos auth
failed:` and includes the verbatim GSSAPI status text plus an actionable hint.

| Error contains | Cause | Fix |
|---|---|---|
| `no credentials cache` | Never ran `kinit` (or `kdestroy` cleared it) | `kinit user@REALM` |
| `ticket expired` | Ticket lifetime elapsed (typically 8-24 h) | `kinit` again |
| `Server principal '...' not registered in KDC` | SPN missing from AD | `setspn -L <account>` on Windows admin host; ask AD admin to register if missing |
| `Clock skew between client and KDC exceeds 5 minutes` | System clock drifted | `sudo chronyc tracking` / `sudo hwclock -s`; install `chrony` for persistence |
| `KDC unreachable` | DNS / network / firewall | Verify `/etc/krb5.conf`, try `kinit -V user@REALM` for verbose output |
| `preauthentication failed -- wrong password` | Bad password in raw mode | Check secret password |
| `no matching key in keytab` | Keytab doesn't have the requested principal | `klist -k /path/to.keytab` to inspect; re-export from KDC if stale |
| `'Password' cannot be combined with 'Trusted_Connection' / 'authenticator=krb5'` | Tried raw credentials via connection string | Use a `CREATE SECRET` instead |
| `'User Id' with authenticator=krb5 requires either 'krb5-keytabfile' or a Password supplied via MSSQL secret` | User Id with no keytab / password / ticket-mode info | Drop User Id (CredCache mode picks it up from `kinit`), or add a keytab |
| `extension was compiled without Kerberos support` | Built with `-DENABLE_KRB5=OFF` | Rebuild with `-DENABLE_KRB5=ON` (default), or use SQL auth |

### Verifying the SPN

The SQL Server must have an SPN registered in AD with name
`MSSQLSvc/<fqdn>:<port>` (or `MSSQLSvc/<fqdn>:<instance>` for named instances).

From a Windows admin host:

```powershell
setspn -L DOMAIN\sqlservice
# Should include: MSSQLSvc/sqlhost.corp.example.com:1433
```

From any POSIX host (asks the KDC for a service ticket):

```sh
kvno -S MSSQLSvc sqlhost.corp.example.com
# kvno = N indicates the SPN is registered and reachable
```

If the SPN is missing, an AD admin must register it:

```powershell
setspn -A MSSQLSvc/sqlhost.corp.example.com:1433 DOMAIN\sqlservice
```

### Combined TLS + auth failures

If the server requires encryption AND auth fails, you'll see both errors. Read
them in order:

```
MSSQL connection validation failed: MSSQL Kerberos auth failed: ...
```

The TLS handshake (`Encrypt=yes`) happens before LOGIN7, so a TLS error is
reported separately. Use `MSSQL_DEBUG=1` to see the PRELOGIN response and TLS
handshake details on stderr.

### WSL2 specifics

See the [WSL2 prerequisites section](#wsl2-ubuntu-under-windows) above. The
most common trip-ups on WSL2 are clock skew (after laptop sleep) and missing
ticket (because WSL2's ccache is separate from Windows logon).

## Running the test stack locally

The repo ships a self-contained Kerberos test stack at `test/kerberos/` — a
docker-compose with a KDC, a SQL Server, and a test client. No real Active
Directory required.

```sh
cd test/kerberos
docker compose up -d --build         # multi-stage build, ~5 min first time
docker compose exec test-client /run-tests.sh
docker compose down -v               # -v clears the keytabs volume
```

The stack provides:

- **kdc** — MIT Kerberos KDC, realm `EXAMPLE.COM`. Test principal
  `testuser@EXAMPLE.COM` (password `testpass`), service principal
  `MSSQLSvc/sql.example.com:1433`.
- **sql** — SQL Server 2022 joined-equivalent via the shared service keytab.
  `init.sql` creates `TestDB` and the `EXAMPLE.COM\testuser` login mapping.
- **test-client** — Ubuntu + krb5-user + the built `mssql.duckdb_extension`
  (built in a multi-stage Linux container so the stack works on macOS hosts).

For an interactive session inside the test client:

```sh
docker compose exec test-client bash

# Inside the container:
kinit testuser@EXAMPLE.COM   # password: testpass
klist
duckdb --unsigned
> LOAD '/home/tester/mssql.duckdb_extension';
> ATTACH 'Server=sql.example.com,1433;Database=TestDB;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes' AS k (TYPE mssql);
> SELECT * FROM k.dbo.test;
```

See `test/kerberos/README.md` for the full layout and troubleshooting.

## Reference

### Supported credential modes

| Mode | Trigger | POSIX Linux | POSIX macOS | Notes |
|---|---|---|---|---|
| CredCache | (default — no keytab, no password) | yes | yes | Uses `kinit` ticket from `KRB5CCNAME` |
| Keytab | `krb5-keytabfile=/path` + `User Id=svc@REALM` | yes | rejected at ATTACH | Service-account / CI pattern |
| Raw | secret with `user` + `password` + `krb5_realm` | yes | rejected at ATTACH | Secret-only — Password in connection string is rejected |

### Connection-string keys

Names verbatim from `microsoft/go-mssqldb`'s `integratedauth/` package.

| Key | Aliases | Purpose |
|---|---|---|
| `authenticator` | — | `krb5` (POSIX) or `winsspi` (Windows; pending) |
| `Trusted_Connection` | `Trusted Connection`, `TrustedConnection` | pyodbc alias — yes/true/SSPI/1 resolves to platform default |
| `Integrated Security` | `IntegratedSecurity`, `Integrated_Security` | ADO.NET alias — same resolution |
| `krb5-configfile` | `krb5_configfile` | `/etc/krb5.conf` override (Linux only) |
| `krb5-keytabfile` | `krb5_keytabfile` | Selects keytab mode |
| `krb5-credcachefile` | `krb5_credcachefile` | ccache path override (Linux only) |
| `krb5-realm` | `krb5_realm` | AD realm (UPPERCASE) |
| `service_principal_name` | `service-principal-name`, `serviceprincipalname` | Override default SPN derivation |

### Platform matrix

| Platform | CredCache | Keytab | Raw | Status |
|---|---|---|---|---|
| Linux x86_64 / ARM64 | yes | yes | yes (secret only) | Phase 3 shipped |
| macOS ARM64 | yes | rejected | rejected | Phase 3 shipped |
| Windows x64 | yes (logon session) | n/a (use logon session) | n/a (use logon session) | Phase 4 shipped |
| WSL2 Ubuntu | yes | yes | yes (secret only) | Same as Linux |

### Security notes

- Cleartext passwords are **never** accepted from a connection string when
  Kerberos is in use. Use `CREATE SECRET` for raw-credentials mode.
- Per-connection overrides (`krb5-configfile`, `krb5-credcachefile`) are
  applied through `gss_acquire_cred_from` cred_store elements, NOT through
  `setenv()`. Thread-safe and per-instance.
- The raw-mode MEMORY ccache is destroyed after `gss_acquire_cred_from`
  has its own internal copy, so cleartext credentials don't linger in the
  process-global MIT ccache registry.
- Kerberos itself provides mutual authentication: the server identity is
  cryptographically verified by the KDC. `TrustServerCertificate=yes` only
  affects TLS certificate validation — it does not weaken Kerberos.

#### Operational security checklist

| Item | Why | How to verify |
|---|---|---|
| Credential cache file is `0600` | The TGT in `/tmp/krb5cc_<uid>` is a bearer credential for your AD principal — anyone who can read the file can impersonate you for the TGT's lifetime. | `ls -la /tmp/krb5cc_$UID` (MIT default is `0600`; verify after any custom `KRB5CCNAME` config). For `DIR:` / `KEYRING:persistent:` ccache types, check the equivalent ACL. |
| Keytab files are `0600` and owned by the service account | A keytab is the password equivalent — anyone who can read it can impersonate the principal until the keys are rotated. | `find / -name '*.keytab' 2>/dev/null -exec ls -la {} \;` |
| `KRB5_TRACE` is not set in production | MIT Kerberos writes ticket-acquisition trace (principal names, key versions, ticket flags) to the path in `KRB5_TRACE`. Useful for debugging, dangerous if forgotten. | `env \| grep KRB5_TRACE` — should be empty |
| `MSSQL_DEBUG` is unset (or `0`) in production | Our extension logs auth flow timing, SPN, host, and pool factory errors to stderr at level 1–3. Sizes and metadata only (no token contents — verified by audit), but reveals operational signal. | `env \| grep MSSQL_DEBUG` — should be empty |
| Coredumps disabled for the DuckDB process | If the process crashes, the coredump contains in-memory cleartext tokens, ccache contents, and TGS session keys. | `ulimit -c 0` before launching DuckDB, or `prlimit --core=0 -p <pid>` on a running process. For systemd-managed services: `LimitCORE=0` in the unit file. |
| Connection strings with passwords don't reach shell history | `duckdb -c "ATTACH '...;Password=...' ..."` writes the password to `~/.bash_history` and exposes it in `ps auxw` to other users for the duration of the process. | Use `CREATE SECRET` + `ATTACH '' AS db (TYPE mssql, SECRET <name>)` instead, or wrap the ATTACH in a heredoc (`duckdb --unsigned <<'EOF'\n…\nEOF`). For Kerberos this is moot — `Trusted_Connection=yes` carries no secret. |
| `duckdb_secrets()` does not expose the password | The `password` field is registered as a redacted secret key in DuckDB's SecretManager and shows as `***` instead of the cleartext. | `SELECT name, secret_string FROM duckdb_secrets() WHERE type='mssql';` |

### See Also

- [AZURE.md](AZURE.md) — Azure AD authentication (Entra ID, Service Principal,
  device code, manual access token)
- [README.md](README.md) — main extension documentation
- [docs/architecture.md](docs/architecture.md) — IAuthenticator abstraction and
  auth strategy pattern
- [specs/042-integrated-authentication/](specs/042-integrated-authentication/) —
  design spec for this feature
- `test/kerberos/README.md` — test stack walkthrough
- [microsoft/go-mssqldb](https://github.com/microsoft/go-mssqldb) — reference
  driver whose `integratedauth/` package this implementation mirrors

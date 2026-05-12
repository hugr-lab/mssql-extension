# Quickstart: Integrated Authentication (Kerberos / SSPI)

**Feature**: 042-integrated-authentication
**Date**: 2026-05-12

This document gives end users the shortest viable path from "I want to use my AD credentials" to a working DuckDB ↔ SQL Server connection. For full reference material see [`docs/kerberos.md`](../../docs/kerberos.md) (created in T036).

## Prerequisites

### POSIX (Linux, macOS)

| Requirement | Linux | macOS |
|-------------|-------|-------|
| Kerberos client tools | `apt install krb5-user` (Debian/Ubuntu) or `dnf install krb5-workstation` (RHEL/Fedora) | Built into the OS |
| GSSAPI runtime library | `libgssapi-krb5-2` (installed with `krb5-user`) | Built-in (`GSS.framework`) |
| `/etc/krb5.conf` configured | Yes — point at your AD KDC(s) | Yes — same |
| Valid Kerberos ticket | Run `kinit user@REALM` | Run `kinit user@REALM` |
| SPN registered in AD | `MSSQLSvc/<fqdn>:<port>` for the SQL Server account | Same |

### Windows

| Requirement | Notes |
|-------------|-------|
| Domain-joined Windows session | The user's logon credentials are used automatically |
| Reachable AD domain controller | Standard AD networking |
| SPN registered in AD | Same as POSIX |
| No client-side install needed | SSPI is built into the OS (`secur32.dll`) |

## Step 1: Configure `/etc/krb5.conf` (POSIX only)

Minimal example for a realm `EXAMPLE.COM`:

```ini
[libdefaults]
    default_realm = EXAMPLE.COM
    dns_lookup_realm = false
    dns_lookup_kdc = true
    ticket_lifetime = 24h
    renew_lifetime = 7d
    forwardable = true

[realms]
    EXAMPLE.COM = {
        kdc = dc01.example.com
        kdc = dc02.example.com
        admin_server = dc01.example.com
    }

[domain_realm]
    .example.com = EXAMPLE.COM
    example.com = EXAMPLE.COM
```

## Step 2: Acquire a ticket (POSIX only)

```bash
kinit alice@EXAMPLE.COM
# Enter password when prompted
```

Verify:

```bash
klist
# Should show:
#   Default principal: alice@EXAMPLE.COM
#   Valid starting       Expires              Service principal
#   05/12/26 09:00:00   05/12/26 19:00:00   krbtgt/EXAMPLE.COM@EXAMPLE.COM
```

## Step 3: Verify the SQL Server SPN exists

From any Windows host that can talk to AD:

```powershell
setspn -L DOMAIN\sqlservice
# Should include:
#   MSSQLSvc/sqlserver.example.com:1433
```

If the SPN is missing, a domain admin must run:

```powershell
setspn -A MSSQLSvc/sqlserver.example.com:1433 DOMAIN\sqlservice
```

## Step 4: Attach in DuckDB

The simplest possible ATTACH (pyodbc-style):

```sql
LOAD mssql;

ATTACH 'Server=sqlserver.example.com;Database=AdventureWorks;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes'
    AS adv (TYPE mssql);

SELECT TOP 5 name FROM adv.sys.tables;
```

On POSIX, `Trusted_Connection=yes` resolves to `authenticator=krb5`. On Windows, it resolves to `authenticator=winsspi`. The behavior is identical from the user's perspective.

## Connection-string forms

### ADO.NET style (recommended)

```sql
-- Simplest (uses defaults)
ATTACH 'Server=sqlhost;Database=db;Trusted_Connection=yes' AS db (TYPE mssql);

-- Explicit authenticator name (go-mssqldb compatible)
ATTACH 'Server=sqlhost;Database=db;authenticator=krb5' AS db (TYPE mssql);

-- Keytab (service account, no kinit needed)
ATTACH 'Server=sqlhost;Database=db;authenticator=krb5;krb5-keytabfile=/etc/keytabs/svc.keytab;User Id=svc@EXAMPLE.COM'
    AS db (TYPE mssql);

-- Raw credentials (KDC reachable but no client config)
ATTACH 'Server=sqlhost;Database=db;authenticator=krb5;User Id=alice;Password=...;krb5-realm=EXAMPLE.COM'
    AS db (TYPE mssql);

-- Override SPN when DNS doesn't match what's registered in AD
ATTACH 'Server=10.0.0.5;Database=db;Trusted_Connection=yes;service_principal_name=MSSQLSvc/sqlhost.example.com:1433'
    AS db (TYPE mssql);
```

### URI style

```sql
ATTACH 'mssql://sqlhost/db?authenticator=krb5' AS db (TYPE mssql);

ATTACH 'mssql://sqlhost/db?authenticator=krb5&krb5-keytabfile=/etc/keytabs/svc.keytab&krb5-realm=EXAMPLE.COM'
    AS db (TYPE mssql);
```

### MSSQL Secret

```sql
CREATE SECRET kerb_secret (
    TYPE mssql,
    host 'sqlhost.example.com',
    port 1433,
    database 'AdventureWorks',
    authenticator 'krb5',
    use_encrypt true
);

ATTACH '' AS adv (TYPE mssql, SECRET kerb_secret);
```

## Aliases

All three of these resolve identically on POSIX (`krb5`) and Windows (`winsspi`):

| Connection-string fragment | Notes |
|----------------------------|-------|
| `Trusted_Connection=yes` | pyodbc / mssql-jdbc canonical |
| `Integrated Security=SSPI` | ADO.NET canonical |
| `Integrated Security=true` | ADO.NET legacy |

The explicit form `authenticator=krb5` (or `winsspi`) is supported in parallel and follows `microsoft/go-mssqldb`.

## Common Errors and Fixes

| Error message | Root cause | Fix |
|---------------|------------|-----|
| `MSSQL Kerberos auth failed: No credentials cache found. Run 'kinit <user>@<REALM>' first.` | No TGT in ccache | Run `kinit user@REALM` |
| `MSSQL Kerberos auth failed: Kerberos ticket expired. Run 'kinit' to refresh.` | TGT lifetime elapsed | Run `kinit` again |
| `MSSQL Kerberos auth failed: Server principal 'MSSQLSvc/sqlhost.example.com:1433' not registered in KDC. Verify with 'setspn -L <account>'.` | SPN not in AD, or DNS gives a different name than the SPN | Register SPN with `setspn -A`, OR pass `service_principal_name=...` to override |
| `MSSQL Kerberos auth failed: Clock skew between client and KDC exceeds 5 minutes. Sync system clock (ntp/chrony).` | Client clock drift | Install/start `chrony` or `ntpd`; verify with `chronyc tracking` |
| `MSSQL Kerberos auth failed: KDC unreachable.` | DNS or network path to KDC broken | Verify `/etc/krb5.conf` KDC entries; test with `kinit -V user@REALM` for verbose output |
| `MSSQL Kerberos auth failed: Kerberos preauthentication failed (bad password).` | Wrong password (raw-credentials mode) | Check password; consider keytab mode for non-interactive |
| `MSSQL Error: 'Trusted_Connection=yes' cannot be combined with 'User Id'. Use one or the other.` | Connection-string conflict | Remove `User Id` / `Password` when using `Trusted_Connection=yes` |
| `MSSQL Error: This build of the mssql extension was compiled without Kerberos support. Rebuild with -DENABLE_KRB5=ON or use SQL authentication.` | Distribution built with `ENABLE_KRB5=OFF` | Rebuild from source, or use SQL auth |

### Combined TLS + auth failures

If the server requires encryption AND auth fails, you'll see two distinct errors — read both:

```
MSSQL Error: TLS handshake completed successfully.
MSSQL Kerberos auth failed: Server principal '...' not registered in KDC.
```

The TLS message confirms the network and certificate path are healthy; the Kerberos message is the actual problem to fix.

## Verifying everything works end-to-end

```bash
# 1. Get a ticket
kinit alice@EXAMPLE.COM

# 2. Confirm it
klist

# 3. Test SPN resolution (optional, requires Windows admin host)
setspn -L DOMAIN\sqlservice | grep -i mssqlsvc

# 4. Connect from DuckDB
duckdb -c "LOAD mssql; ATTACH 'Server=sqlhost.example.com;Database=db;Trusted_Connection=yes' AS db (TYPE mssql); SELECT TOP 1 name FROM db.sys.tables;"
```

If step 4 fails, work backwards: confirm step 2 shows a TGT, confirm step 3 lists the SPN, then re-read the specific error message.

## Build (developers)

```bash
# Default (Kerberos enabled on POSIX, SSPI enabled on Windows)
GEN=ninja make

# Disable Kerberos (hermetic build)
GEN=ninja make CMAKE_ARGS="-DENABLE_KRB5=OFF"

# Run parser unit tests (no SQL Server, no KDC)
GEN=ninja make test

# Bring up Kerberos integration test infrastructure (self-contained, no AD required)
cd test/kerberos
docker-compose up -d
docker-compose exec test-client kinit testuser@EXAMPLE.COM <<< 'testpass'
docker-compose exec test-client ./run-tests.sh
docker-compose down
```

## File-by-file summary of what this feature adds

| File | Change | Complexity |
|------|--------|------------|
| `src/include/tds/auth/iauthenticator.hpp` | NEW — three-method interface | ~50 lines |
| `src/include/tds/auth/krb5_authenticator.hpp` | NEW — POSIX GSSAPI wrapper | ~60 lines |
| `src/include/tds/auth/winsspi_authenticator.hpp` | NEW — Windows SSPI wrapper | ~60 lines |
| `src/include/tds/auth/integrated_auth_strategy.hpp` | NEW — adapter to `AuthenticationStrategy` | ~50 lines |
| `src/tds/auth/krb5_authenticator.cpp` | NEW — GSSAPI calls, all three credential modes | ~350 lines |
| `src/tds/auth/winsspi_authenticator.cpp` | NEW — SSPI calls | ~200 lines |
| `src/tds/auth/integrated_auth_strategy.cpp` | NEW — wraps `IAuthenticator` | ~80 lines |
| `src/tds/auth/auth_strategy_factory.cpp` | MODIFY — add Kerberos/SSPI branches | ~30 lines |
| `src/tds/tds_protocol.cpp` | MODIFY — LOGIN7 `fIntSecurity` bit + SSPI field writer | ~40 lines |
| `src/tds/tds_token_parser.cpp` | MODIFY — recognize `0xED` SSPI token | ~20 lines |
| `src/tds/tds_connection.cpp` | MODIFY — SSPI continuation loop in `Login()` | ~80 lines |
| `src/mssql_storage.cpp` | MODIFY — parse new keys + aliases + conflict validation | ~120 lines |
| `src/mssql_secret.cpp` | MODIFY — extend secret schema | ~40 lines |
| `src/include/mssql_storage.hpp` | MODIFY — `AuthMethod` enum + krb5 fields | ~30 lines |
| `CMakeLists.txt` | MODIFY — `ENABLE_KRB5`, library discovery | ~25 lines |
| `test/kerberos/docker-compose.yml` | NEW — three-service compose (kdc, sql, test-client) | ~40 lines |
| `test/kerberos/kdc/` | NEW — Dockerfile + krb5.conf + init-kdc.sh | ~80 lines |
| `test/kerberos/sql/init.sql` | NEW — SQL logins mapped to AD principals | ~20 lines |
| `test/kerberos/test-client/` | NEW — Dockerfile + run-tests.sh | ~40 lines |
| `test/cpp/test_integrated_auth_parsing.cpp` | NEW — connection-string unit tests | ~200 lines |
| `test/sql/integrated_auth/*.test` | NEW — six integration test files | ~250 lines total |
| `README.md` | MODIFY — remove Windows-auth limitation; add section; add table rows | ~50 lines |
| `docs/kerberos.md` | NEW — end-user guide | ~400 lines |
| `docs/architecture.md` | MODIFY — update Authentication Strategy Pattern section | ~30 lines |

**Total**: roughly 2200 lines of new code + 250 lines of modifications across the existing codebase.

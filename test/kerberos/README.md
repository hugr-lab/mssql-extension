# Kerberos integration test stack (spec 042)

Self-contained `docker compose` stack for testing the mssql extension's
Integrated Authentication (Kerberos / SSPI) backend without a real Active
Directory.

## What's inside

| Service | Role |
|---|---|
| `kdc` | MIT Kerberos KDC, realm `EXAMPLE.COM`. Creates principals `testuser@EXAMPLE.COM` (password `testpass`) and `MSSQLSvc/sql.example.com:1433@EXAMPLE.COM`. Exports the service keytab to the shared `keytabs` volume so SQL Server can decrypt service tickets. |
| `sql` | `mcr.microsoft.com/mssql/server:2022-latest` joined-equivalent: the service keytab is mounted at `/var/opt/mssql/secrets/mssql.keytab` and a Windows-style login `EXAMPLE.COM\testuser` is created in TestDB. |
| `test-client` | Ubuntu + `krb5-user` + a DuckDB CLI + the locally-built `mssql.duckdb_extension`. Runs `kinit testuser@EXAMPLE.COM` then exercises the extension. |

All three services share an internal `kerb_net` bridge. The SQL Server is
reachable at `sql.example.com:1433` from inside `test-client`; the KDC is
reachable at `kdc.example.com:88`. Nothing is published to the host.

## Quick start

From this directory:

```bash
# 1. Rebuild the extension from the repo root first (the test-client image
#    bakes the .duckdb_extension into the image).
( cd ../.. && GEN=ninja make )

# 2. Bring up the stack.
docker compose up -d --build

# 3. Run the smoke tests.
docker compose exec test-client /run-tests.sh

# 4. Tear down.
docker compose down -v
```

The `-v` on teardown removes the `keytabs` volume so the next `up` produces
a fresh KDC. Leave `-v` off to preserve the KDC database between runs.

## What `run-tests.sh` exercises

1. `kinit testuser@EXAMPLE.COM` -- acquires a TGT in the user's credential
   cache.
2. `klist` -- shows the TGT for debugging.
3. ATTACH via `Trusted_Connection=yes` -- exercises the pyodbc-style alias.
4. ATTACH via the explicit `authenticator=krb5` -- exercises the go-mssqldb
   form.
5. `kdestroy` + ATTACH -- exercises the negative path; expects the GSSAPI
   "no credentials cache" error.

## Manual experimentation

After `docker compose up -d`:

```bash
# Open a shell in the test-client.
docker compose exec test-client bash

# Inside the container:
kinit testuser@EXAMPLE.COM   # password: testpass
klist                         # confirm TGT
duckdb --unsigned
> LOAD '/home/tester/mssql.duckdb_extension';
> ATTACH 'Server=sql.example.com,1433;Database=TestDB;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes' AS kdb (TYPE mssql);
> SELECT * FROM kdb.dbo.test;
```

## Troubleshooting

* **`kinit: Cannot find KDC`** -- The KDC container isn't healthy yet. Wait
  10s after `up -d` (the healthcheck verifies kadmin.local responds before
  declaring it ready).

* **`Server principal 'MSSQLSvc/sql.example.com:1433' not registered`** --
  The keytab in the SQL Server container is stale (e.g. you wiped the
  `keytabs` volume without restarting `sql`). Run
  `docker compose restart sql` to pick up the freshly-exported keytab.

* **`Clock skew too great`** -- The KDC and host clocks have drifted (>5
  minutes). This typically only affects sleeping laptops. Restart Docker
  Desktop or run `docker compose down && docker compose up -d`.

* **Login failed for user 'EXAMPLE.COM\testuser'** -- Kerberos auth itself
  succeeded but the SQL principal isn't mapped. Re-run `sql/init.sql`:
  `docker compose exec sql /opt/mssql-tools18/bin/sqlcmd -S localhost -U sa -P TestPassword1 -C -i /init/init.sql`.

## Files

```
test/kerberos/
├── docker-compose.yml          # The three-service stack
├── README.md                   # This file
├── kdc/
│   ├── Dockerfile              # MIT KDC on ubuntu:24.04
│   ├── kdc.conf                # Realm / enctype config
│   ├── krb5.conf               # Client config (mirrored into test-client)
│   └── init-kdc.sh             # Realm + principal + keytab bootstrap
├── sql/
│   ├── configure-kerberos.sh   # Helper that points mssql.conf at the keytab
│   └── init.sql                # TestDB + dbo.test + EXAMPLE.COM\testuser login
└── test-client/
    ├── Dockerfile              # ubuntu + krb5-user + duckdb + extension
    ├── krb5.conf               # Same as kdc/krb5.conf
    └── run-tests.sh            # End-to-end smoke test driver
```

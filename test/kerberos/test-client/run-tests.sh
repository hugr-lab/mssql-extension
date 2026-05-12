#!/bin/bash
# Spec 042 smoke test driver.
#
# Run inside the test-client container after `docker compose up -d`.
# Acquires a TGT for testuser@EXAMPLE.COM, loads the mssql extension, and
# attempts a Kerberos-authenticated ATTACH against sql.example.com.
#
# Exits non-zero on any failure so CI can gate on this.
set -euo pipefail

EXT=/home/tester/mssql.duckdb_extension
PRINCIPAL="${PRINCIPAL:-testuser@EXAMPLE.COM}"
PASSWORD="${PASSWORD:-testpass}"

if [[ ! -f "${EXT}" ]]; then
    echo "[run-tests] ERROR: extension not found at ${EXT}. Did you rebuild before 'docker compose up --build'?" >&2
    exit 2
fi

echo "[run-tests] step 1: kinit ${PRINCIPAL}"
echo -n "${PASSWORD}" | kinit "${PRINCIPAL}"

echo "[run-tests] step 2: klist"
klist

echo "[run-tests] step 3: ATTACH via Trusted_Connection=yes"
duckdb --unsigned <<EOF
LOAD '${EXT}';
ATTACH 'Server=sql.example.com,1433;Database=TestDB;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes' AS kdb (TYPE mssql);
SELECT '== kerberos query ==' AS marker;
SELECT id, name FROM kdb.dbo.test ORDER BY id;
SELECT '== mssql_version ==' AS marker, mssql_version() AS version;
DETACH kdb;
EOF

echo "[run-tests] step 4: ATTACH via explicit authenticator=krb5"
duckdb --unsigned <<EOF
LOAD '${EXT}';
ATTACH 'Server=sql.example.com,1433;Database=TestDB;authenticator=krb5;Encrypt=yes;TrustServerCertificate=yes' AS kdb2 (TYPE mssql);
SELECT COUNT(*) AS rows FROM kdb2.dbo.test;
DETACH kdb2;
EOF

echo "[run-tests] step 5: negative case -- destroy ccache, expect a clear error"
kdestroy
duckdb --unsigned 2>&1 <<EOF || true
LOAD '${EXT}';
ATTACH 'Server=sql.example.com,1433;Database=TestDB;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes' AS kdb3 (TYPE mssql);
EOF

echo "[run-tests] all integration steps completed"

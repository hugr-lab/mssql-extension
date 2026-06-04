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

echo "[run-tests] step 5: forced LOGIN7 fragmentation (issue #138)"
# Lower the LOGIN7 fragmentation boundary so the multi-packet send path is
# exercised end-to-end: with a 512-byte cap even the test KDC's small SPNEGO
# token produces a multi-packet LOGIN7. A real AD PAC does this naturally; the
# test KDC issues minimal tickets, so we force the boundary down instead.
# Success here proves SQL Server reassembles and accepts the fragmented LOGIN7.
frag_out=$(MSSQL_LOGIN7_MAX_PACKET=512 MSSQL_DEBUG=1 duckdb --unsigned 2>&1 <<EOF
LOAD '${EXT}';
ATTACH 'Server=sql.example.com,1433;Database=TestDB;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes' AS kfrag (TYPE mssql);
SELECT COUNT(*) AS frag_rows FROM kfrag.dbo.test;
DETACH kfrag;
EOF
)
echo "${frag_out}"
# Assert the LOGIN7 actually split into more than one TDS packet, and the query
# returned (i.e. the server accepted the fragmented login).
if ! echo "${frag_out}" | grep -Eq 'LOGIN7 payload=[0-9]+ bytes -> ([2-9]|[1-9][0-9]+) TDS packet'; then
    echo "[run-tests] ERROR: LOGIN7 was not fragmented into multiple packets (knob ineffective?)" >&2
    exit 3
fi
if ! echo "${frag_out}" | grep -q 'frag_rows'; then
    echo "[run-tests] ERROR: fragmented-LOGIN7 ATTACH/query did not succeed" >&2
    exit 3
fi
echo "[run-tests] step 5: OK -- multi-packet LOGIN7 accepted by SQL Server"

echo "[run-tests] step 6: negative case -- destroy ccache, expect a clear error"
kdestroy
duckdb --unsigned 2>&1 <<EOF || true
LOAD '${EXT}';
ATTACH 'Server=sql.example.com,1433;Database=TestDB;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes' AS kdb3 (TYPE mssql);
EOF

echo "[run-tests] all integration steps completed"

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

# --------------------------------------------------------------------------
# step 3: forced multi-packet LOGIN7 (issue #138) -- THE regression check.
#
# Run this BEFORE the happy-path ATTACHes: it validates the protocol-level
# framing fix and is independent of whether SQL Server's Kerberos *trust* lets
# the login fully succeed (that depends on keytab/realm wiring). MSSQL_DEBUG=1
# surfaces the packet-split line; the mssql_login7_max_packet=512 setting forces
# the multi-packet path even for the test KDC's small (PAC-less) token -- a real
# AD PAC pushes LOGIN7 past 4096 on its own.
#
# Pre-fix, an oversized single-packet LOGIN7 made SQL Server reset the TCP
# connection ("Connection reset by peer" / "TLS receive failed"). The fix is
# proven when (a) LOGIN7 splits into >1 TDS packet and (b) the server gives a
# graceful TDS-level response -- a successful login OR a login-level error like
# 18452 -- rather than a transport reset. Either means the server reassembled
# and parsed the fragmented login.
# --------------------------------------------------------------------------
echo "[run-tests] step 3: forced LOGIN7 fragmentation (issue #138)"
cat > /tmp/frag.sql <<EOF
LOAD '${EXT}';
SET mssql_login7_max_packet=512;
ATTACH 'Server=sql.example.com,1433;Database=TestDB;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes' AS kfrag (TYPE mssql);
SELECT COUNT(*) AS frag_rows FROM kfrag.dbo.test;
DETACH kfrag;
EOF
# A rejected login makes duckdb exit non-zero; we assert on the captured output,
# so shield the command substitution from `set -e`.
set +e
frag_out=$(MSSQL_DEBUG=1 duckdb --unsigned -f /tmp/frag.sql 2>&1)
set -e
echo "${frag_out}"
if ! echo "${frag_out}" | grep -Eq 'LOGIN7 payload=[0-9]+ bytes -> ([2-9]|[1-9][0-9]+) TDS packet'; then
    echo "[run-tests] ERROR: LOGIN7 did not fragment into multiple packets (knob ineffective?)" >&2
    exit 3
fi
if echo "${frag_out}" | grep -Eq 'Connection reset by peer|TLS receive failed|Failed to receive LOGIN7 response'; then
    echo "[run-tests] ERROR: fragmented LOGIN7 triggered a transport reset -- issue #138 NOT fixed" >&2
    exit 3
fi
echo "[run-tests] step 3: OK -- multi-packet LOGIN7 reassembled & answered by SQL Server"

echo "[run-tests] step 4: ATTACH via Trusted_Connection=yes (happy path; needs Kerberos trust)"
duckdb --unsigned <<EOF
LOAD '${EXT}';
ATTACH 'Server=sql.example.com,1433;Database=TestDB;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes' AS kdb (TYPE mssql);
SELECT '== kerberos query ==' AS marker;
SELECT id, name FROM kdb.dbo.test ORDER BY id;
SELECT '== mssql_version ==' AS marker, mssql_version() AS version;
DETACH kdb;
EOF

echo "[run-tests] step 5: ATTACH via explicit authenticator=krb5"
duckdb --unsigned <<EOF
LOAD '${EXT}';
ATTACH 'Server=sql.example.com,1433;Database=TestDB;authenticator=krb5;Encrypt=yes;TrustServerCertificate=yes' AS kdb2 (TYPE mssql);
SELECT COUNT(*) AS rows FROM kdb2.dbo.test;
DETACH kdb2;
EOF

echo "[run-tests] step 6: negative case -- destroy ccache, expect a clear error"
kdestroy
duckdb --unsigned 2>&1 <<EOF || true
LOAD '${EXT}';
ATTACH 'Server=sql.example.com,1433;Database=TestDB;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes' AS kdb3 (TYPE mssql);
EOF

echo "[run-tests] all integration steps completed"

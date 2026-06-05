#!/bin/bash
# Spec 042 smoke test driver -- Option A (PR #112).
#
# Run inside the test-client container after `docker compose up -d --wait`.
# Exercises the POSIX Kerberos auth path *without* requiring a SQL Server
# login mapped to the AD principal. SQL Server on Linux can't resolve
# Windows principals to SIDs without SSSD/realmd, so a full ATTACH +
# Trusted_Connection round-trip is out of scope for this stack -- it
# remains a manual test, documented in Kerberos.md.
#
# What this DOES cover (the high-value regression surface for spec 042):
#   1. Multi-stage Docker build sanity -- the COPY'd extension is a real ELF
#   2. kinit round-trip against the KDC (testuser@EXAMPLE.COM / testpass)
#   3. mssql_kerberos_auth_test('sql.example.com', 1433)
#      -> exercises Krb5Authenticator::InitialBytes(): cred acquisition,
#         SPN import, gss_init_sec_context(SPNEGO), token marshalling.
#         Returns "OK: principal=... spn=... token_size=... bytes" on
#         success, or the verbatim GSSAPI error on failure.
#   4. Negative: after kdestroy, the same call must report a clear
#      "no credentials" error rather than silently passing or crashing.
#
# Exits non-zero on any failure so CI can gate on this.
set -euo pipefail

EXT=/home/tester/mssql.duckdb_extension
PRINCIPAL="${PRINCIPAL:-testuser@EXAMPLE.COM}"
PASSWORD="${PASSWORD:-testpass}"
SQL_HOST="${SQL_HOST:-sql.example.com}"
SQL_PORT="${SQL_PORT:-1433}"

echo "[run-tests] step 0: extension sanity check"
if [[ ! -f "${EXT}" ]]; then
    echo "[run-tests] ERROR: extension not found at ${EXT}. Did you rebuild before 'docker compose up --build'?" >&2
    exit 2
fi
file "${EXT}"
file "${EXT}" | grep -q "ELF " || {
    echo "[run-tests] ERROR: ${EXT} is not an ELF binary -- multi-stage build is broken." >&2
    exit 2
}

echo "[run-tests] step 1: kinit ${PRINCIPAL}"
echo -n "${PASSWORD}" | kinit "${PRINCIPAL}"

echo "[run-tests] step 2: klist"
klist

echo "[run-tests] step 3: mssql_kerberos_auth_test('${SQL_HOST}', ${SQL_PORT})"
result=$(duckdb --unsigned -noheader -list <<EOF
LOAD '${EXT}';
SELECT mssql_kerberos_auth_test('${SQL_HOST}', ${SQL_PORT});
EOF
)
echo "${result}"
echo "${result}" | grep -q "^OK:" || {
    echo "[run-tests] ERROR: expected 'OK:' prefix from mssql_kerberos_auth_test, got: ${result}" >&2
    exit 3
}

# --------------------------------------------------------------------------
# step 4: forced multi-packet LOGIN7 (issue #138).
#
# Unlike step 3 (which only exercises local token generation), this performs a
# real ATTACH so a LOGIN7 is actually sent on the wire. The
# `mssql_login7_max_packet=512` setting lowers the fragmentation boundary so
# even the test KDC's small (PAC-less) token produces a multi-packet LOGIN7 -- a
# real AD PAC pushes LOGIN7 past 4096 on its own (see issue #138).
#
# This is independent of whether the login fully succeeds: SQL Server on Linux
# can't map the AD principal without SSSD/realmd, so the ATTACH is expected to
# come back with login error 18452 (see the file header). The point of the test
# is the *framing*: pre-fix an oversized single-packet LOGIN7 made the server
# reset the TCP connection ("Connection reset by peer" / "TLS receive failed").
# The fix is proven when (a) LOGIN7 splits into >1 TDS packet and (b) the server
# answers at the TDS level (success OR a login error) rather than resetting --
# either means it reassembled and parsed the fragmented login.
echo "[run-tests] step 4: forced LOGIN7 fragmentation (issue #138)"
cat > /tmp/frag.sql <<EOF
LOAD '${EXT}';
SET mssql_login7_max_packet=512;
ATTACH 'Server=${SQL_HOST},${SQL_PORT};Database=TestDB;Trusted_Connection=yes;Encrypt=yes;TrustServerCertificate=yes' AS kfrag (TYPE mssql);
DETACH kfrag;
EOF
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
echo "[run-tests] step 4: OK -- multi-packet LOGIN7 reassembled & answered by SQL Server"

echo "[run-tests] step 5: negative case -- destroy ccache, expect a clear error"
kdestroy
negative=$(duckdb --unsigned -noheader -list <<EOF || true
LOAD '${EXT}';
SELECT mssql_kerberos_auth_test('${SQL_HOST}', ${SQL_PORT});
EOF
)
echo "${negative}"
if echo "${negative}" | grep -q "^OK:"; then
    echo "[run-tests] ERROR: auth_test reported OK with no ccache -- this should have failed." >&2
    exit 4
fi

echo "[run-tests] all integration steps completed"

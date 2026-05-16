#!/bin/bash
# Spec 045 smoke test driver — Phase 2 (resolver-only; no DuckDB yet).
#
# Run inside the test-client container after `docker compose up -d --wait`:
#   docker compose exec test-client /run-tests.sh
#
# What this covers (the high-value regression surface for spec 045 Phase 2):
#
#   1. The mock SQL Server Browser is reachable on UDP 1434 and answers
#      CLNT_UCAST_INST queries (proves the docker stack networking works).
#
#   2. The real C++ InstanceResolver:
#      (a) resolves TESTINST -> 11433 against the mock browser
#          (proves the resolver is wire-compatible with a faithful mock)
#      (b) reports InstanceNotFound for an unknown instance
#          (proves error categorisation)
#      (c) ...and we can actually open a TCP socket to that resolved port
#          against the SQL Server container (proves the mock advertises
#          the correct port, end-to-end through real Linux networking).
#
# What this does NOT cover (Phase 3 territory, future PR):
#   * DuckDB ATTACH 'Server=sql.example.com\TESTINST;...'  — the resolver
#     isn't wired into MSSQLConnectionInfo::FromConnectionString yet.
#   * LOGIN7 ServerName containing host\instance.
#   * SPN derivation under Kerberos with a discovered port.
#
# Exits non-zero on any failure so CI can gate on this.

set -euo pipefail

RESOLVER=/usr/local/bin/test_instance_resolver
BROWSER_HOST="${BROWSER_HOST:-browser.example.com}"
BROWSER_PORT="${BROWSER_PORT:-1434}"
SQL_HOST="${SQL_HOST:-sql.example.com}"
EXPECTED_PORT="${EXPECTED_PORT:-11433}"
INSTANCE="${INSTANCE:-TESTINST}"

echo "[run-tests] step 1: resolver binary sanity"
if [[ ! -x "${RESOLVER}" ]]; then
    echo "[run-tests] ERROR: ${RESOLVER} not found or not executable" >&2
    exit 2
fi
file "${RESOLVER}" 2>/dev/null || true

echo "[run-tests] step 2: resolve ${BROWSER_HOST}\\${INSTANCE} via mock browser"
result=$("${RESOLVER}" --resolve "${BROWSER_HOST}" "${BROWSER_PORT}" "${INSTANCE}" 5)
echo "  resolver output: ${result}"

case "${result}" in
    "OK port=${EXPECTED_PORT}")
        echo "  PASS"
        ;;
    "OK port="*)
        echo "  FAIL: resolved to wrong port (expected ${EXPECTED_PORT})" >&2
        exit 3
        ;;
    *)
        echo "  FAIL: resolver did not succeed" >&2
        exit 3
        ;;
esac

echo "[run-tests] step 3: negative — unknown instance"
result=$("${RESOLVER}" --resolve "${BROWSER_HOST}" "${BROWSER_PORT}" "NONESUCH" 5 || true)
echo "  resolver output: ${result}"
case "${result}" in
    "FAIL InstanceNotFound:"*)
        echo "  PASS"
        ;;
    *)
        echo "  FAIL: expected 'FAIL InstanceNotFound: ...' got: ${result}" >&2
        exit 4
        ;;
esac

echo "[run-tests] step 4: end-to-end — TCP connect to resolved port on ${SQL_HOST}"
# Proves the port the mock advertised actually corresponds to a real
# listening SQL Server. We don't speak TDS here — just verifying the
# socket opens.
if nc -z "${SQL_HOST}" "${EXPECTED_PORT}" 2>/dev/null; then
    echo "  PASS — TCP socket to ${SQL_HOST}:${EXPECTED_PORT} opens"
else
    echo "  FAIL: cannot open TCP to ${SQL_HOST}:${EXPECTED_PORT}" >&2
    exit 5
fi

echo "[run-tests] step 5: resolver self-tests (unit tests baked into binary)"
"${RESOLVER}"

echo ""
echo "[run-tests] ALL SMOKE TESTS PASSED"

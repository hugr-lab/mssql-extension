#!/bin/bash
# Spec 046 sql container entrypoint.
#
# Backgrounds sqlservr (listening on 11433 via mssql.conf), polls until it
# accepts SA connections, then runs /init/init.sql exactly once. The init
# script is idempotent (IF NOT EXISTS guards) so re-running on a restart
# is safe.
#
# Why port 11433 and not 1433: the entire point of the spec 046 test stack
# is to verify the resolver translates an instance name to the correct
# port. Pinning SQL Server to a non-default port means a test that passes
# could only have passed via Browser resolution, not by lucky default.
#
# Modelled on test/kerberos/sql/entrypoint.sh (spec 042) — same idempotent
# init pattern, different port.

set -euo pipefail

INIT_SQL=/init/init.sql
SQLCMD=/opt/mssql-tools18/bin/sqlcmd
SA_PASSWORD="${MSSQL_SA_PASSWORD:-TestPassword1}"
SQL_PORT="${SQL_TCP_PORT:-11433}"
INIT_DONE=/var/opt/mssql/.spec046-init-done

run_init() {
    if [[ -f "${INIT_DONE}" ]]; then
        echo "[sql-entrypoint] init.sql already applied at $(cat ${INIT_DONE}); skipping"
        return 0
    fi

    echo "[sql-entrypoint] waiting for SQL Server to accept connections on localhost,${SQL_PORT}..."
    local tries=0
    until "${SQLCMD}" -S "localhost,${SQL_PORT}" -U sa -P "${SA_PASSWORD}" -C -l 1 -Q "SELECT 1" >/dev/null 2>&1; do
        tries=$((tries + 1))
        if [[ ${tries} -gt 60 ]]; then
            echo "[sql-entrypoint] SQL Server did not become reachable in 60s" >&2
            return 1
        fi
        sleep 1
    done

    echo "[sql-entrypoint] SQL Server up on localhost,${SQL_PORT}; applying ${INIT_SQL}"
    if "${SQLCMD}" -S "localhost,${SQL_PORT}" -U sa -P "${SA_PASSWORD}" -C -b -i "${INIT_SQL}"; then
        date -u +"%FT%TZ" > "${INIT_DONE}"
        echo "[sql-entrypoint] init.sql applied successfully"
    else
        echo "[sql-entrypoint] init.sql FAILED" >&2
        return 1
    fi
}

run_init &
exec /opt/mssql/bin/sqlservr

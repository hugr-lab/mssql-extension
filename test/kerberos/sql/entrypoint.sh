#!/bin/bash
# Spec 042 sql container entrypoint.
#
# Backgrounds sqlservr, polls until it accepts SA connections, then runs
# /init/init.sql exactly once. The init script is idempotent (uses IF NOT
# EXISTS guards) so re-running on a restart is safe.
#
# Kerberos keytab wiring happens via the baked-in /var/opt/mssql/mssql.conf
# from the Dockerfile -- by the time sqlservr starts, the conf already points
# at /var/opt/mssql/secrets/mssql.keytab. The KDC service uses the same volume
# to publish the keytab. We tolerate a small startup race where the keytab
# isn't visible yet by retrying connections in run-tests.sh on the client side.

set -euo pipefail

INIT_SQL=/init/init.sql
SQLCMD=/opt/mssql-tools18/bin/sqlcmd
SA_PASSWORD="${MSSQL_SA_PASSWORD:-TestPassword1}"
INIT_DONE=/var/opt/mssql/.kerb-init-done

run_init() {
    if [[ -f "${INIT_DONE}" ]]; then
        echo "[sql-entrypoint] init.sql already applied at $(cat ${INIT_DONE}); skipping"
        return 0
    fi

    echo "[sql-entrypoint] waiting for SQL Server to accept connections..."
    local tries=0
    until "${SQLCMD}" -S localhost -U sa -P "${SA_PASSWORD}" -C -l 1 -Q "SELECT 1" >/dev/null 2>&1; do
        tries=$((tries + 1))
        if [[ ${tries} -gt 60 ]]; then
            echo "[sql-entrypoint] SQL Server did not become reachable in 60s" >&2
            return 1
        fi
        sleep 1
    done

    echo "[sql-entrypoint] SQL Server up; applying ${INIT_SQL}"
    if "${SQLCMD}" -S localhost -U sa -P "${SA_PASSWORD}" -C -b -i "${INIT_SQL}"; then
        date -u +"%FT%TZ" > "${INIT_DONE}"
        echo "[sql-entrypoint] init.sql applied successfully"
    else
        echo "[sql-entrypoint] init.sql FAILED" >&2
        return 1
    fi
}

# Background the init poller and exec sqlservr in the foreground.
run_init &
exec /opt/mssql/bin/sqlservr

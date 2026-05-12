#!/bin/bash
# Enable Kerberos on the SQL Server container.
#
# Runs at container start time. Configures the mssql.conf file with the
# keytab path and Kerberos enctypes, then SQL Server picks it up on next
# restart. Idempotent.
set -euo pipefail

CONF=/var/opt/mssql/mssql.conf
KEYTAB=/var/opt/mssql/secrets/mssql.keytab

mkdir -p "$(dirname "${CONF}")"

if [[ ! -f "${KEYTAB}" ]]; then
    echo "[configure-kerberos] keytab not present yet: ${KEYTAB}"
    exit 0
fi

# Use the mssql-conf helper if available so we don't have to hand-edit ini.
if command -v /opt/mssql/bin/mssql-conf >/dev/null 2>&1; then
    /opt/mssql/bin/mssql-conf set network.kerberoskeytabfile "${KEYTAB}" || true
else
    cat >"${CONF}" <<EOF
[network]
kerberoskeytabfile = ${KEYTAB}
EOF
fi
echo "[configure-kerberos] keytab configured at ${KEYTAB}"

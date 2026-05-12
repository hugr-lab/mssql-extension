#!/bin/bash
# Initialize the KDC, create test principals, export the SQL Server keytab,
# then launch krb5kdc + kadmind in the foreground.
#
# Idempotent: re-running on an existing /var/lib/krb5kdc skips the realm
# creation step (useful when the keytabs volume has been recreated but the
# KDC database is fresh).

set -euo pipefail

REALM="${KRB5_REALM:-EXAMPLE.COM}"
ADMIN_PASSWORD="${ADMIN_PASSWORD:-adminpw}"
USER_PRINCIPAL="${USER_PRINCIPAL:-testuser}"
USER_PASSWORD="${USER_PASSWORD:-testpass}"
SERVICE_PRINCIPAL="${SERVICE_PRINCIPAL:-MSSQLSvc/sql.example.com:1433}"
KEYTAB_OUT="${KEYTAB_OUT:-/export/mssql.keytab}"

mkdir -p /var/log /var/lib/krb5kdc /export

if [[ ! -f /var/lib/krb5kdc/principal ]]; then
    echo "[init-kdc] creating realm ${REALM}"
    # -s = stash the master key on disk so we can autostart krb5kdc later
    # -P = master password (non-interactive)
    kdb5_util create -s -P "${ADMIN_PASSWORD}" -r "${REALM}"

    # Permit the */admin principal class to administer the realm (standard MIT default).
    echo '*/admin *' > /etc/krb5kdc/kadm5.acl

    echo "[init-kdc] creating user principal ${USER_PRINCIPAL}@${REALM}"
    kadmin.local -q "addprinc -pw ${USER_PASSWORD} ${USER_PRINCIPAL}@${REALM}"

    echo "[init-kdc] creating service principal ${SERVICE_PRINCIPAL}@${REALM}"
    # randkey: generate random long-term key; we never need a password for SPNs.
    kadmin.local -q "addprinc -randkey ${SERVICE_PRINCIPAL}@${REALM}"

    # Export the service principal's keytab so the SQL Server container can
    # decrypt service tickets. The shared 'keytabs' volume makes this visible
    # at /var/opt/mssql/secrets/mssql.keytab inside the sql container.
    echo "[init-kdc] exporting service keytab to ${KEYTAB_OUT}"
    kadmin.local -q "ktadd -k ${KEYTAB_OUT} ${SERVICE_PRINCIPAL}@${REALM}"
    chmod 644 "${KEYTAB_OUT}"
else
    echo "[init-kdc] realm already exists at /var/lib/krb5kdc; skipping create"
    # Re-export keytab in case the keytabs volume was wiped between runs but
    # the database was retained.
    if [[ ! -s "${KEYTAB_OUT}" ]]; then
        echo "[init-kdc] keytab missing; re-exporting"
        kadmin.local -q "ktadd -k ${KEYTAB_OUT} ${SERVICE_PRINCIPAL}@${REALM}"
        chmod 644 "${KEYTAB_OUT}"
    fi
fi

echo "[init-kdc] starting krb5kdc and kadmind"
# krb5kdc -n keeps it in the foreground; kadmind backgrounds itself.
kadmind &
exec krb5kdc -n

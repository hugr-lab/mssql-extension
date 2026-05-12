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

    # Also register the port-less hostbased-service variant. Clients using
    # GSS_C_NT_HOSTBASED_SERVICE (no port info in the SPN) end up here, while
    # clients using the principal-name form land on the port-suffixed variant.
    # Registering both makes the test KDC robust against both client paths.
    # spec 042 ultrareview bug_015.
    SERVICE_PRINCIPAL_HOSTBASED="${SERVICE_PRINCIPAL%:*}"  # strip ":1433"
    if [[ "${SERVICE_PRINCIPAL_HOSTBASED}" != "${SERVICE_PRINCIPAL}" ]]; then
        echo "[init-kdc] also creating hostbased variant ${SERVICE_PRINCIPAL_HOSTBASED}@${REALM}"
        kadmin.local -q "addprinc -randkey ${SERVICE_PRINCIPAL_HOSTBASED}@${REALM}"
    fi

    # Export both variants into a single keytab so the SQL Server container
    # can decrypt service tickets for either form. The shared 'keytabs' volume
    # makes this visible at /var/opt/mssql/secrets/mssql.keytab inside sql.
    echo "[init-kdc] exporting service keytab to ${KEYTAB_OUT}"
    kadmin.local -q "ktadd -k ${KEYTAB_OUT} ${SERVICE_PRINCIPAL}@${REALM}"
    if [[ "${SERVICE_PRINCIPAL_HOSTBASED}" != "${SERVICE_PRINCIPAL}" ]]; then
        kadmin.local -q "ktadd -k ${KEYTAB_OUT} ${SERVICE_PRINCIPAL_HOSTBASED}@${REALM}"
    fi
    chmod 644 "${KEYTAB_OUT}"
else
    echo "[init-kdc] realm already exists at /var/lib/krb5kdc; skipping create"
    # Re-export keytab in case the keytabs volume was wiped between runs but
    # the database was retained.
    if [[ ! -s "${KEYTAB_OUT}" ]]; then
        echo "[init-kdc] keytab missing; re-exporting"
        kadmin.local -q "ktadd -k ${KEYTAB_OUT} ${SERVICE_PRINCIPAL}@${REALM}"
        SERVICE_PRINCIPAL_HOSTBASED="${SERVICE_PRINCIPAL%:*}"
        if [[ "${SERVICE_PRINCIPAL_HOSTBASED}" != "${SERVICE_PRINCIPAL}" ]]; then
            kadmin.local -q "ktadd -k ${KEYTAB_OUT} ${SERVICE_PRINCIPAL_HOSTBASED}@${REALM}" || true
        fi
        chmod 644 "${KEYTAB_OUT}"
    fi
fi

echo "[init-kdc] starting krb5kdc and kadmind"
# krb5kdc -n keeps it in the foreground; kadmind backgrounds itself.
kadmind &
exec krb5kdc -n

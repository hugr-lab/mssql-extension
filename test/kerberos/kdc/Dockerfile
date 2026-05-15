# MIT Kerberos KDC for spec 042 integrated-auth tests.
#
# At first boot:
#   - Creates realm $KRB5_REALM with master password "$ADMIN_PASSWORD".
#   - Adds principal $USER_PRINCIPAL@$KRB5_REALM with password $USER_PASSWORD.
#   - Adds service principal $SERVICE_PRINCIPAL@$KRB5_REALM and exports its
#     long-term key into $KEYTAB_OUT (mounted into the SQL Server container
#     via the shared `keytabs` volume so SQL Server can decrypt service tickets).
#   - Starts krb5kdc + kadmind in the foreground.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        krb5-kdc krb5-admin-server krb5-user \
        ca-certificates dnsutils \
    && rm -rf /var/lib/apt/lists/*

COPY krb5.conf /etc/krb5.conf
COPY kdc.conf /etc/krb5kdc/kdc.conf
COPY init-kdc.sh /usr/local/bin/init-kdc.sh
RUN chmod +x /usr/local/bin/init-kdc.sh

EXPOSE 88/tcp 88/udp 749/tcp

ENTRYPOINT ["/usr/local/bin/init-kdc.sh"]

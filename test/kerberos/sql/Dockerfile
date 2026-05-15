# Spec 042 -- SQL Server with auto-applied init.sql + Kerberos keytab wiring.
#
# Extends the official mssql/server:2022-latest image with:
#   * Pre-written /var/opt/mssql/mssql.conf pointing at the shared keytab.
#   * Init script (entrypoint wrapper) that backgrounds sqlservr, waits for
#     it to accept connections, then runs init.sql exactly once.
#
# Without these, the compose stack starts but TestDB never exists and the
# Kerberos keytab is never wired -- the documented run-tests.sh smoke test
# fails at the first ATTACH. See spec 042 ultrareview finding bug_009.

FROM mcr.microsoft.com/mssql/server:2022-latest

# Bake in the Kerberos config so it's picked up the first time sqlservr starts.
# At first boot the keytab volume mount may not yet have the keytab in it (the
# kdc container's healthcheck just guarantees the KDC database exists, not
# that ktadd has run). The startup wrapper handles that race below.
USER root
RUN mkdir -p /var/opt/mssql && \
    printf '[network]\nkerberoskeytabfile = /var/opt/mssql/secrets/mssql.keytab\n' > /var/opt/mssql/mssql.conf && \
    chown -R mssql:root /var/opt/mssql /var/opt/mssql/mssql.conf

COPY init.sql /init/init.sql
COPY entrypoint.sh /init/entrypoint.sh
RUN chmod +x /init/entrypoint.sh

USER mssql
ENTRYPOINT ["/init/entrypoint.sh"]

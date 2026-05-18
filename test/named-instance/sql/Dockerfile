# Spec 045 SQL Server -- binds to TCP 11433 (non-default) to prove the
# resolver actually translates TESTINST -> 11433 instead of falling back
# to the default 1433.
#
# Extends mssql/server:2022-latest with:
#   * A baked-in /var/opt/mssql/mssql.conf setting [network] tcpport=11433
#   * An entrypoint that backgrounds sqlservr, waits for it to accept
#     connections, then applies init.sql exactly once (creates
#     NamedInstTest + dbo.Probe).
#
# Modelled on test/kerberos/sql/Dockerfile (spec 042).

FROM mcr.microsoft.com/mssql/server:2022-latest

USER root
RUN mkdir -p /var/opt/mssql && \
    printf '[network]\ntcpport = 11433\n' > /var/opt/mssql/mssql.conf && \
    chown -R mssql:root /var/opt/mssql /var/opt/mssql/mssql.conf

COPY init.sql /init/init.sql
COPY entrypoint.sh /init/entrypoint.sh
RUN chmod +x /init/entrypoint.sh

USER mssql
ENTRYPOINT ["/init/entrypoint.sh"]

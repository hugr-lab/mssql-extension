# Issue #261 / spec 068: a TDS server whose only job is to answer a login with
# a ROUTING redirect, so the Kerberos stack can exercise a hop.
#
# python3 stdlib only -- no pip install, no build step, nothing to break on a
# base-image bump. The gateway never validates the SPNEGO blob it receives; it
# only has to be something the client asks the KDC for a ticket to reach.
FROM python:3.12-slim

COPY gateway.py /gateway.py

ENV ROUTE_TO_HOST=sql.example.com \
    ROUTE_TO_PORT=1433 \
    LISTEN_PORT=1433

EXPOSE 1433/tcp

CMD ["python3", "-u", "/gateway.py"]

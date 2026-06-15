# Spec 045 mock SQL Server Browser.
#
# Minimal Python UDP responder. ~5MB image. The whole purpose of this
# container is to answer CLNT_UCAST_INST queries on UDP 1434 with a
# faithful SVR_RESP packet pointing at the sibling sql.example.com
# container's TCP port. See browser.py for the wire-format details.

FROM python:3.12-slim

WORKDIR /app
COPY browser.py /app/browser.py
RUN chmod +x /app/browser.py

# UDP 1434 is the well-known SQL Server Browser port (MC-SQLR).
EXPOSE 1434/udp

# Run as non-root to satisfy default container security policies.
RUN useradd -r -u 1001 mock && chown -R mock:mock /app
USER mock

# Unbuffered Python output so `docker compose logs -f mock-browser` is live.
ENV PYTHONUNBUFFERED=1

CMD ["python3", "/app/browser.py"]

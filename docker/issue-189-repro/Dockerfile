FROM python:3.12-slim

# The released community mssql binary (pre-spec-053) still hard-links
# libgssapi_krb5.so.2; slim images don't ship it (see issue #161 / spec 053).
RUN apt-get update \
    && apt-get install -y --no-install-recommends libgssapi-krb5-2 \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir duckdb

# Bake the community mssql extension into the image so repeated runs don't
# need network access. repro.py re-runs INSTALL (idempotent, served from the
# local cache) unless MSSQL_EXTENSION_PATH points at a local build.
RUN python -c "import duckdb; duckdb.connect().execute('INSTALL mssql FROM community')"

COPY repro.py /repro.py

ENTRYPOINT ["python", "/repro.py"]

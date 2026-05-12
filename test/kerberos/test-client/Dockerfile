# Spec 042 test-client.
#
# Built FROM the repo root (build context is two dirs up from this file).
# Copies the already-built mssql.duckdb_extension and a duckdb binary,
# plus krb5-user for kinit/klist/kdestroy.
#
# This Dockerfile assumes the user has already run `GEN=ninja make` from the
# repo root, producing build/release/extension/mssql/mssql.duckdb_extension.
# The compose entry rebuilds via depends_on so an extension rebuild before
# `docker compose up --build` is the standard workflow.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        krb5-user libgssapi-krb5-2 \
        ca-certificates curl unzip \
        sqlite3 \
    && rm -rf /var/lib/apt/lists/*

# Install the DuckDB CLI matching the extension's API. We pull the official
# DuckDB binary so the test client doesn't depend on the host having duckdb
# installed.
ARG DUCKDB_VERSION=v1.4.3
RUN ARCH="$(dpkg --print-architecture)" \
    && case "${ARCH}" in \
         amd64) URL_SUFFIX="linux-amd64" ;; \
         arm64) URL_SUFFIX="linux-arm64" ;; \
         *) echo "Unsupported arch: ${ARCH}" >&2; exit 1 ;; \
       esac \
    && curl -fSL -o /tmp/duckdb.zip \
        "https://github.com/duckdb/duckdb/releases/download/${DUCKDB_VERSION}/duckdb_cli-${URL_SUFFIX}.zip" \
    && unzip /tmp/duckdb.zip -d /usr/local/bin \
    && chmod +x /usr/local/bin/duckdb \
    && rm /tmp/duckdb.zip

# Copy the locally-built extension into the image. Path is relative to the
# build context (the repo root).
COPY build/release/extension/mssql/mssql.duckdb_extension /home/tester/mssql.duckdb_extension

RUN useradd -m -s /bin/bash tester
WORKDIR /home/tester

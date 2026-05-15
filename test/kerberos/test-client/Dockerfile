# Spec 042 test-client.
#
# Multi-stage: build the extension INSIDE a Linux container so the test stack
# works regardless of host OS. The earlier single-stage Dockerfile COPY'd the
# host-built .duckdb_extension verbatim; on macOS that produced a Mach-O dylib
# that Linux DuckDB cannot LOAD (the magic bytes don't match ELF).
#
# Build context = repo root (set via docker-compose.yml `build.context: ../..`).

# ----------------------------------------------------------------------------
# Stage 1: build the extension. Linux/glibc, matches the runtime stage's libc.
# ----------------------------------------------------------------------------
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkg-config \
        libssl-dev libkrb5-dev \
        ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

# Build simdutf from source. Spec 044 made find_package(simdutf CONFIG REQUIRED)
# unconditional in CMakeLists.txt; the main CI build gets it via vcpkg, but
# this Dockerfile uses plain apt + cmake (faster than spinning up vcpkg). Ubuntu
# 24.04 doesn't ship libsimdutf-dev, so we build a pinned tag from upstream and
# install into /usr/local where the next find_package call will pick it up.
# Static + tests/benchmarks off keeps the install tiny (just a .a + headers +
# the CMake config) and avoids a runtime libsimdutf.so dependency.
ARG SIMDUTF_VERSION=v8.2.0
RUN git clone --depth 1 --branch "${SIMDUTF_VERSION}" https://github.com/simdutf/simdutf.git /tmp/simdutf \
    && cmake -S /tmp/simdutf -B /tmp/simdutf/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DSIMDUTF_TESTS=OFF \
        -DSIMDUTF_BENCHMARKS=OFF \
        -DSIMDUTF_TOOLS=OFF \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    && cmake --build /tmp/simdutf/build --target install \
    && rm -rf /tmp/simdutf

WORKDIR /src

# Copy the whole repo (build context = repo root). The .git directory is
# included on purpose because CMakeLists.txt runs `git rev-parse` against the
# duckdb submodule for version tracking.
COPY . /src/

# Build the extension. ENABLE_KRB5=ON is the default; we set it explicitly so
# the test stack always exercises the Kerberos path.
#
# CMAKE_BUILD_PARALLEL_LEVEL=2 caps ninja parallelism so we don't OOM on
# hosts with low Docker memory limits (DuckDB's compile is heap-heavy --
# 4-way parallel needs ~8GB; 2-way fits in ~4GB).
ENV CMAKE_BUILD_PARALLEL_LEVEL=2
RUN GEN=ninja make CMAKE_ARGS="-DENABLE_KRB5=ON"

# ----------------------------------------------------------------------------
# Stage 2: runtime test-client. krb5-user for kinit, libgssapi for runtime,
# DuckDB CLI from the official release tarball.
# ----------------------------------------------------------------------------
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        krb5-user libgssapi-krb5-2 libssl3 \
        ca-certificates curl unzip \
        file \
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

# Copy the extension from the Linux build stage. Sanity-check it's an ELF so
# any future Dockerfile change that breaks the multi-stage chain fails loudly
# at image-build time instead of at LOAD time.
COPY --from=builder /src/build/release/extension/mssql/mssql.duckdb_extension /home/tester/mssql.duckdb_extension
RUN file /home/tester/mssql.duckdb_extension | grep -qE 'ELF.*(shared object|dynamically linked)' \
    || (echo "ERROR: extension is not a Linux ELF shared object; multi-stage build is broken" >&2; \
        file /home/tester/mssql.duckdb_extension >&2; exit 1)

RUN useradd -m -s /bin/bash tester
WORKDIR /home/tester

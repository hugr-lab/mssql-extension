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

# zip/unzip/tar are needed by vcpkg's bootstrap + package extraction.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkg-config \
        libssl-dev libkrb5-dev \
        ca-certificates curl zip unzip tar \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy the whole repo (build context = repo root). The .git directory is
# included on purpose because CMakeLists.txt runs `git rev-parse` against the
# duckdb submodule for version tracking.
COPY . /src/

# Bootstrap vcpkg INSIDE the container. The extension gained a vcpkg simdutf
# dependency (spec 043/044) after this test stack was first written. (Host
# vcpkg/ is .dockerignore'd, so this clone is always fresh.) vcpkg builds
# openssl + simdutf from the manifest during cmake configure.
#
# NOTE: a full clone is required -- manifest mode resolves vcpkg.json's
# builtin-baseline commit, which a `--depth 1` clone does not contain
# ("failed to unpack tree object ... versions/baseline.json").
RUN git clone https://github.com/microsoft/vcpkg.git /src/vcpkg \
    && /src/vcpkg/bootstrap-vcpkg.sh -disableMetrics

# Build the extension AND the matching DuckDB CLI directly with cmake, mirroring
# the project's CI (.github/workflows/ci.yml). We deliberately do NOT use the
# `make` wrapper here: its auto-vcpkg path breaks compiler/Ninja detection once
# a vcpkg toolchain is injected, and the duckdb submodule is a tagless shallow
# checkout so the make path can't derive a version. Building the CLI from the
# same submodule guarantees the runtime CLI and the extension share an exact
# DuckDB version (the prior approach downloaded a fixed v1.4.3 CLI that no
# longer matches the submodule). ENABLE_KRB5=ON exercises the Kerberos path.
#
# CMAKE_BUILD_PARALLEL_LEVEL=2 caps ninja parallelism so we don't OOM on hosts
# with low Docker memory limits (DuckDB's compile is heap-heavy -- 4-way needs
# ~8GB; 2-way fits in ~4GB).
ENV CMAKE_BUILD_PARALLEL_LEVEL=2
RUN cmake -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=/src/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_MANIFEST_DIR=/src \
        -DDUCKDB_EXTENSION_CONFIGS=/src/extension_config.cmake \
        -DENABLE_KRB5=ON \
        -S /src/duckdb -B /src/build/release \
    && cmake --build /src/build/release --config Release

# ----------------------------------------------------------------------------
# Stage 2: runtime test-client. krb5-user for kinit, libgssapi for runtime,
# DuckDB CLI copied from the build stage (version-matched to the extension).
# ----------------------------------------------------------------------------
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        krb5-user libgssapi-krb5-2 libssl3 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Use the DuckDB CLI built from the SAME submodule as the extension. This
# guarantees the CLI and extension share an exact DuckDB version, so the
# absolute-path LOAD does not fail on a version-stamp mismatch (the duckdb
# submodule tracks main/nightly, not a tagged release).
COPY --from=builder /src/build/release/duckdb /usr/local/bin/duckdb
RUN chmod +x /usr/local/bin/duckdb

# Copy the extension from the Linux build stage. Sanity-check it's an ELF so
# any future Dockerfile change that breaks the multi-stage chain fails loudly
# at image-build time instead of at LOAD time.
COPY --from=builder /src/build/release/extension/mssql/mssql.duckdb_extension /home/tester/mssql.duckdb_extension
RUN file /home/tester/mssql.duckdb_extension | grep -qE 'ELF.*(shared object|dynamically linked)' \
    || (echo "ERROR: extension is not a Linux ELF shared object; multi-stage build is broken" >&2; \
        file /home/tester/mssql.duckdb_extension >&2; exit 1)

RUN useradd -m -s /bin/bash tester
WORKDIR /home/tester

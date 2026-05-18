# Spec 045 test-client.
#
# Two-stage build that produces a runtime image containing both:
#   - the standalone test_instance_resolver binary (Phase 0-2 resolver smoke)
#   - the full duckdb_mssql extension + matching DuckDB CLI (Phase 3+ ATTACH
#     end-to-end tests)
#
# Modelled on test/kerberos/test-client/Dockerfile -- simdutf-from-source
# avoids the vcpkg dance; pinned tag keeps cache friendliness across PRs.

# ----------------------------------------------------------------------------
# Stage 1: build the extension + the standalone resolver test binary.
# ----------------------------------------------------------------------------
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkg-config \
        libssl-dev \
        ca-certificates curl ccache \
    && rm -rf /var/lib/apt/lists/*

# Build simdutf from source (spec 044 makes find_package(simdutf CONFIG REQUIRED)
# unconditional in CMakeLists.txt). Static + tests/benches off keeps the
# install tiny and avoids a runtime libsimdutf.so dependency.
ARG SIMDUTF_VERSION=v8.2.0
RUN git clone --depth 1 --branch "${SIMDUTF_VERSION}" https://github.com/simdutf/simdutf.git /tmp/simdutf \
    && cmake -S /tmp/simdutf -B /tmp/simdutf/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DSIMDUTF_TESTS=OFF -DSIMDUTF_BENCHMARKS=OFF -DSIMDUTF_TOOLS=OFF \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    && cmake --build /tmp/simdutf/build --target install \
    && rm -rf /tmp/simdutf

WORKDIR /src

# Build context = repo root (see docker-compose.yml). COPY the whole tree so a
# future header addition doesn't silently break the Linux build (spec 045 F8).
COPY . /src/

# Build the extension (and the DuckDB CLI alongside it). Parallel level capped
# at 2 to fit hosts with low Docker memory limits (DuckDB compile is heap-heavy).
ENV CMAKE_BUILD_PARALLEL_LEVEL=2 \
    CCACHE_DIR=/ccache \
    CCACHE_MAXSIZE=1G
RUN --mount=type=cache,target=/ccache \
    GEN=ninja make CMAKE_ARGS="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache" \
    && ccache --show-stats

# Standalone resolver test binary (Phase 0-2 smoke). Self-contained C++ unit
# tests; no DuckDB linkage required.
RUN mkdir -p /src/build/test && \
    c++ -std=c++17 -pthread -O2 -Wno-deprecated-declarations \
        -I /src/src/include \
        /src/test/cpp/test_instance_resolver.cpp \
        /src/src/connection/instance_resolver.cpp \
        -o /src/build/test/test_instance_resolver

# ----------------------------------------------------------------------------
# Stage 2: minimal runtime.
# ----------------------------------------------------------------------------
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates netcat-openbsd file libssl3 \
    && rm -rf /var/lib/apt/lists/*

# Resolver smoke binary.
COPY --from=builder /src/build/test/test_instance_resolver /usr/local/bin/test_instance_resolver
RUN chmod +x /usr/local/bin/test_instance_resolver

# DuckDB CLI -- built from the same source tree as the extension so the
# version stamp matches (extensions built from a shallow-cloned duckdb
# submodule stamp as v0.0.1 and won't LOAD into a versioned upstream CLI).
COPY --from=builder /src/build/release/duckdb /usr/local/bin/duckdb
RUN chmod +x /usr/local/bin/duckdb

# The mssql extension. Sanity-check it's a Linux ELF so a future
# Dockerfile mistake fails loudly at image-build time instead of at LOAD time.
COPY --from=builder /src/build/release/extension/mssql/mssql.duckdb_extension /home/tester/mssql.duckdb_extension
RUN file /home/tester/mssql.duckdb_extension | grep -qE 'ELF.*(shared object|dynamically linked)' \
    || (echo "ERROR: extension is not a Linux ELF shared object; multi-stage build is broken" >&2; \
        file /home/tester/mssql.duckdb_extension >&2; exit 1)

RUN useradd -m -s /bin/bash tester
WORKDIR /home/tester

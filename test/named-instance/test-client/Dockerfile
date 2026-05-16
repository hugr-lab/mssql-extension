# Spec 045 test-client.
#
# Phase 2's test-client only needs the resolver binary — not the full
# extension — because Phase 3 (the user-visible ATTACH plumbing) isn't
# in this PR yet. We compile the standalone test_instance_resolver
# binary inside the container so the resolver code paths are exercised
# against the containerised mock browser regardless of host OS.
#
# When Phase 3 lands, this Dockerfile should grow a stage 2 that copies
# the full extension + DuckDB CLI (mirror test/kerberos/test-client/
# Dockerfile) — until then, keep it tiny.
#
# Build context = repo root (set via docker-compose.yml `build.context: ../..`).

FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY src/include/connection/instance_resolver.hpp /src/src/include/connection/
COPY src/connection/instance_resolver.cpp /src/src/connection/
COPY test/cpp/test_instance_resolver.cpp /src/test/cpp/

RUN mkdir -p /src/build && \
    c++ -std=c++17 -pthread -O2 -Wno-deprecated-declarations \
        -I /src/src/include \
        /src/test/cpp/test_instance_resolver.cpp \
        /src/src/connection/instance_resolver.cpp \
        -o /src/build/test_instance_resolver

# ----------------------------------------------------------------------------
# Stage 2: minimal runtime — just enough to invoke the test binary.
# ----------------------------------------------------------------------------
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/test_instance_resolver /usr/local/bin/test_instance_resolver
RUN chmod +x /usr/local/bin/test_instance_resolver

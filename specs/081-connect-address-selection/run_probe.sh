#!/usr/bin/env bash
# Spec 081 — run probe_dial_outcomes.cpp natively and, on a Mac, on Linux too.
#
# The question it settles (RST vs dropped SYN for a bound-but-unlistening port)
# is answered differently by macOS and Linux, so one platform's run is half an
# answer. From a Mac this gives both.
set -uo pipefail
cd "$(dirname "$0")"

echo "=============== native ==============="
if c++ -std=c++11 -O0 -Wall -o /tmp/probe081 probe_dial_outcomes.cpp; then
	/tmp/probe081
else
	echo "  native build FAILED" >&2
fi

echo
echo "=============== linux (container) ==============="
if docker info >/dev/null 2>&1; then
	docker run --rm -v "$PWD":/src:ro gcc:13 \
		sh -c 'g++ -std=c++11 -O0 -Wall -o /tmp/p /src/probe_dial_outcomes.cpp && /tmp/p'
else
	echo "  docker unavailable — skipped. On Linux the native run above IS this one."
fi

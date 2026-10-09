#!/usr/bin/env bash
# Spec 081 — run probe_dial_outcomes.cpp natively and, on a Mac, on Linux too.
#
# The question it settles (RST vs dropped SYN for a bound-but-unlistening port)
# is answered differently by macOS and Linux, so one platform's run is half an
# answer. From a Mac this gives both.
#
# Exit status is the FIRST non-zero of the stages that actually ran: a build
# failure, or a probe exiting 1 (control violated) / 2 (bad input), must reach
# the caller. The script is proposed for a CI lane, and a lane that cannot see
# a violated control is a green light for an unmeasured premise — the false
# green the spec's own §4 calls worse than no test. A skipped stage (no docker)
# is not a failure and does not set the status.
set -uo pipefail
cd "$(dirname "$0")"

status=0
note_failure() { # $1 = stage status; remember the first non-zero
	if [ "$1" -ne 0 ] && [ "$status" -eq 0 ]; then
		status=$1
	fi
}

echo "=============== native ==============="
if c++ -std=c++11 -O0 -Wall -o /tmp/probe081 probe_dial_outcomes.cpp; then
	/tmp/probe081
	note_failure $?
else
	echo "  native build FAILED" >&2
	note_failure 1
fi

echo
echo "=============== linux (container) ==============="
if docker info >/dev/null 2>&1; then
	docker run --rm -v "$PWD":/src:ro gcc:13 \
		sh -c 'g++ -std=c++11 -O0 -Wall -o /tmp/p /src/probe_dial_outcomes.cpp && /tmp/p'
	note_failure $?
else
	echo "  docker unavailable — skipped. On Linux the native run above IS this one."
fi

exit "$status"

#!/usr/bin/env bash
#
# Fail when a macOS binary requires a newer macOS than the release supports.
#
# Usage: check_macos_minos.sh <expected minos, e.g. 15.0> <binary>...
#
# The runner's SDK decides a binary's minimum macOS (LC_BUILD_VERSION minos)
# unless MACOSX_DEPLOYMENT_TARGET pins it. The release supports macOS 15.0 and
# later; a build that quietly followed a newer runner SDK (15.5, say) would
# refuse to load on the earlier 15.x releases.

set -euo pipefail

expected="${1:?expected minos, e.g. 15.0}"
shift
[ "$#" -gt 0 ] || { echo "usage: $0 <expected minos> <binary>..." >&2; exit 2; }

status=0
for bin in "$@"; do
    # The first LC_BUILD_VERSION's minos. otool's output goes to a file, not a
    # pipe, so an early awk exit cannot SIGPIPE otool under pipefail.
    load_commands="$(mktemp)"
    otool -l "$bin" > "$load_commands"
    minos="$(awk '/cmd LC_BUILD_VERSION/ {found = 1} found && $1 == "minos" {print $2; exit}' "$load_commands")"
    rm -f "$load_commands"
    if [ -z "$minos" ]; then
        echo "::error::$bin has no LC_BUILD_VERSION load command"
        status=1
    elif [ "$minos" != "$expected" ]; then
        echo "::error::$bin requires macOS $minos, expected $expected -- is MACOSX_DEPLOYMENT_TARGET set for this build?"
        status=1
    else
        echo "OK: $bin requires macOS $minos"
    fi
done
exit "$status"

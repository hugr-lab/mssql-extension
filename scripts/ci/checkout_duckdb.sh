#!/usr/bin/env bash
#
# Check out the DuckDB submodule, resiliently.
#
# Three things this has to get right, each learned the hard way.
#
# 1. A STALL MUST BECOME AN ERROR. The failure that motivated this (ci.yml run
#    34050028399, the osx_arm64 leg of commit 1f1be81) did not fail -- it HUNG.
#    The step sat in `git submodule update` for 45 minutes and died with no log
#    and no failing step. A plain retry loop is useless against that: it only
#    runs when the command RETURNS non-zero, and a hung process never does.
#    GIT_HTTP_LOW_SPEED_LIMIT/TIME make git abort a transfer that drops below
#    1 KB/s for 60 s, which turns the stall into an exit status the loop can
#    act on. (Scope: this covers a stalled HTTPS transfer, the likely case for
#    a clone this size. A hang in checkout rather than transfer is still the
#    job-level `timeout-minutes` backstop's problem, not this script's.)
#
#    `timeout(1)` would be the obvious tool and is deliberately not used: it is
#    not in base macOS, and this script runs on the macOS leg too.
#
# 2. THE CLONE IS NOT SHALLOW, ON PURPOSE. `--depth 1` is the reflex here and
#    it breaks the build in a way that shows up much later. The Makefile
#    records why: "A shallow clone stamps v0.0.1 and hides this; a full one
#    does not" -- and an extension stamped v0.0.1 will not LOAD into a
#    versioned CLI. Slow-and-correct beats fast-and-unloadable.
#
# 3. A RETRY MUST NOT INHERIT THE WRECKAGE. A transfer killed mid-flight can
#    leave duckdb/ half-populated or an index.lock behind in the submodule's
#    git dir. Retrying into that state fails deterministically for a brand new
#    reason, so each attempt after the first starts from a clean slate.

set -uo pipefail

ATTEMPTS="${DUCKDB_CHECKOUT_ATTEMPTS:-3}"

# Abort a transfer sitting under 1 KB/s for 60 s instead of waiting forever.
export GIT_HTTP_LOW_SPEED_LIMIT="${GIT_HTTP_LOW_SPEED_LIMIT:-1000}"
export GIT_HTTP_LOW_SPEED_TIME="${GIT_HTTP_LOW_SPEED_TIME:-60}"

reset_state() {
    # `git rev-parse --git-dir` rather than a literal .git/, because in a git
    # WORKTREE .git is a file and .git/modules/duckdb does not exist.
    local git_dir
    git_dir="$(git rev-parse --git-dir 2>/dev/null || echo .git)"
    git submodule deinit -f duckdb >/dev/null 2>&1 || true
    rm -rf duckdb "${git_dir}/modules/duckdb"
}

for attempt in $(seq 1 "$ATTEMPTS"); do
    if [ "$attempt" -gt 1 ]; then
        echo "Resetting partial submodule state before attempt ${attempt}"
        reset_state
    fi

    if git submodule update --init --recursive duckdb; then
        echo "DuckDB submodule checked out on attempt ${attempt}"
        exit 0
    fi

    echo "::warning::DuckDB submodule checkout failed (attempt ${attempt}/${ATTEMPTS})"

    # No backoff after the last attempt -- it would delay the failure without
    # any remaining attempt to space out.
    if [ "$attempt" -lt "$ATTEMPTS" ]; then
        sleep $((attempt * 20))
    fi
done

echo "::error::DuckDB submodule checkout failed after ${ATTEMPTS} attempts"
exit 1

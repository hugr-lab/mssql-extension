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

# Anchor at the repo root before anything relative happens. `rm -rf duckdb`
# below is a relative destructive path, so running from elsewhere -- a future
# `working-directory:`, or a local shell -- would delete something else and
# then fail confusingly. No `|| .git` fallback: outside a repository this
# script cannot do its job at all, so say so.
repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "::error::checkout_duckdb.sh must run inside the repository"
    exit 1
}
cd "$repo_root" || exit 1

ATTEMPTS="${DUCKDB_CHECKOUT_ATTEMPTS:-3}"
# Wall clock per attempt. A healthy clone here is a couple of minutes, so this
# is ~5x headroom; three attempts plus backoff stay well inside the job's own
# bound.
ATTEMPT_TIMEOUT="${DUCKDB_CHECKOUT_TIMEOUT:-600}"

# A secondary, faster signal: abort a transfer sitting under 1 KB/s for 60 s.
# It is NOT the main guard -- see RunAttempt.
export GIT_HTTP_LOW_SPEED_LIMIT="${GIT_HTTP_LOW_SPEED_LIMIT:-1000}"
export GIT_HTTP_LOW_SPEED_TIME="${GIT_HTTP_LOW_SPEED_TIME:-60}"

# Run one attempt under a wall-clock bound, so that ANY hang becomes an exit
# status the retry loop can act on.
#
# GIT_HTTP_LOW_SPEED_* alone is not enough, and assuming it was is what the
# previous version got wrong: it bounds the HTTP TRANSFER only. It does not
# bound TCP/TLS connect, delta resolution, index and worktree writes, or the
# recursive pass over DuckDB's own third_party submodules. The failure this
# exists for (run 34050028399: 45 minutes, no log, no failing step) never
# identified which of those it was, so guarding one of them is a guess.
#
# timeout(1) is the obvious tool and is not in base macOS. perl's alarm is,
# and is present on the ubuntu and macOS runners and in Git Bash on Windows.
# If perl is somehow missing, run unbounded rather than not at all -- the
# job-level timeout-minutes is the last backstop either way.
run_attempt() {
    if command -v perl >/dev/null 2>&1; then
        # fork + setpgrp + kill the GROUP, not exec + alarm.
        #
        # `exec` replaces this process with git, so SIGALRM reaches only the
        # top-level `git submodule update`. Its children -- git clone /
        # git-remote-https, one per nested submodule -- are orphaned and keep
        # running: still writing into duckdb/ and holding locks while the next
        # attempt's reset_state does `rm -rf` on the tree they are populating.
        # A stub test of the exec form showed exactly this, an orphaned child
        # outliving the killed parent by its full sleep.
        perl -e '
            my $t = shift;
            my $p = fork;
            die "fork failed: $!" unless defined $p;
            # setpgrp is best-effort: it is NOT implemented on native Win32
            # perl (Strawberry, which is on the PATH of GitHub windows-*
            # runners). Unguarded it dies BEFORE exec, so git never runs and
            # every attempt fails in milliseconds -- a hard red on the MSVC and
            # MinGW release legs, which are not dispatch-gated. Git Bash
            # normally resolves /usr/bin/perl (MSYS2), but "perl found but the
            # wrapper cannot work" had no fallback (roborev 1460).
            if (!$p) { eval { setpgrp(0, 0) }; exec @ARGV; exit 127 }
            # And if the group was never created, killing -$p addresses a group
            # that does not exist: git would keep running past the timeout,
            # which is the orphan this wrapper exists to prevent. Fall back to
            # the pid.
            $SIG{ALRM} = sub { kill("KILL", -$p) or kill("KILL", $p); waitpid($p, 0); exit 142 };
            alarm $t;
            waitpid($p, 0);
            my $st = $?;
            exit($st & 127 ? 128 + ($st & 127) : $st >> 8);
        ' "$ATTEMPT_TIMEOUT" git submodule update --init --recursive duckdb
    else
        # Say so. A leg running unbounded must not look identical in the log to
        # one running bounded -- the only backstop left is the step timeout,
        # which kills without retrying.
        echo "::warning::perl not found; DuckDB checkout attempt runs UNBOUNDED (step timeout is the only backstop)"
        git submodule update --init --recursive duckdb
    fi
}

reset_state() {
    # Resolve the submodule's ACTUAL gitdir rather than guessing two locations.
    #
    # The previous form removed both `--git-dir/modules/duckdb` and
    # `--git-common-dir/modules/duckdb`. In a WORKTREE those are different
    # trees and the common dir is the MAIN clone's .git, so running this from a
    # worktree deleted the main checkout's submodule gitdir -- leaving its
    # populated duckdb/ an orphaned working tree whose `git -C duckdb status`
    # fails and whose next `submodule update` re-clones from scratch. The
    # comment here used to record "--git-common-dir .git modules/duckdb ABSENT"
    # as if that were general; it held only because that checkout happened not
    # to have initialised the submodule (roborev 1460). The script is anchored
    # at `git rev-parse --show-toplevel` and is runnable by hand, so this was
    # reachable, not theoretical.
    #
    # `--git-path` does worktree-correct resolution, and when duckdb/ exists
    # its `.git` file names the gitdir exactly. The common dir is used only
    # when it IS this checkout's git dir (a plain clone), never to reach across.
    local sub_gitdir git_dir common_dir
    sub_gitdir="$(git rev-parse --git-path modules/duckdb)"
    git_dir="$(git rev-parse --git-dir)"
    common_dir="$(git rev-parse --git-common-dir)"
    # Unchecked, an empty result would make the rm below take an absolute path
    # outside the repository.
    if [ -z "$sub_gitdir" ] || [ -z "$git_dir" ] || [ -z "$common_dir" ]; then
        echo "::error::could not resolve the git dir; refusing to reset"
        exit 1
    fi

    local -a targets=(duckdb "$sub_gitdir")
    # A plain clone: the common dir is this checkout's own, so the legacy
    # location is ours to clear too. In a worktree the two differ and we leave
    # the main clone alone.
    if [ "$git_dir" = "$common_dir" ] && [ "$sub_gitdir" != "${common_dir}/modules/duckdb" ]; then
        targets+=("${common_dir}/modules/duckdb")
    fi

    git submodule deinit -f duckdb >/dev/null 2>&1 || true
    rm -rf "${targets[@]}"

    local t
    for t in "${targets[@]}"; do
        if [ -e "$t" ]; then
            echo "::error::could not reset partial DuckDB submodule state (${t} remains)"
            exit 1
        fi
    done
}

for attempt in $(seq 1 "$ATTEMPTS"); do
    if [ "$attempt" -gt 1 ]; then
        echo "Resetting partial submodule state before attempt ${attempt}"
        reset_state
    fi

    # `status=$?` AFTER the if would capture the if-construct's own status --
    # 0 when the condition fails and no branch runs -- so the 142 test below
    # could never fire. It has to be read inside the else.
    if run_attempt; then
        echo "DuckDB submodule checked out on attempt ${attempt}"
        exit 0
    else
        status=$?
    fi

    if [ "$status" -eq 142 ]; then
        echo "::warning::DuckDB submodule checkout exceeded ${ATTEMPT_TIMEOUT}s (attempt ${attempt}/${ATTEMPTS})"
    else
        echo "::warning::DuckDB submodule checkout failed with status ${status} (attempt ${attempt}/${ATTEMPTS})"
    fi

    # No backoff after the last attempt -- it would delay the failure without
    # any remaining attempt to space out.
    if [ "$attempt" -lt "$ATTEMPTS" ]; then
        sleep $((attempt * 20))
    fi
done

echo "::error::DuckDB submodule checkout failed after ${ATTEMPTS} attempts"
exit 1

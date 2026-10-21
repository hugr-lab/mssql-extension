#!/usr/bin/env bash
#
# Print the DuckDB release tag (vX.Y.Z) the duckdb submodule is pinned to, or
# nothing when the pin is not a release.
#
# Why this exists: DuckDB 2.0's CMake no longer derives its version from
# `git describe` (duckdb/CMakeLists.txt). Unless DUCKDB_VERSION or
# OVERRIDE_GIT_DESCRIBE is given, a build is labelled
# v<scripts/ci/release_version.txt>.0-dev<commit count> -- even when the commit
# IS a release tag. A -dev label has two consequences:
#   - the build looks for extensions under the 10-character source id, and on a
#     release DuckDB publishes its own extensions only under the tag
#     (extensions.duckdb.org/v2.0.0/... answers, .../d54cbbe3df/... is a 404),
#     so INSTALL azure fails;
#   - our loadable carries the -dev label, so it loads in no released DuckDB.
# The caller exports the tag as OVERRIDE_GIT_DESCRIBE (checkout_duckdb.sh for
# CI, the Makefile locally), and the build then calls itself what it is.
#
# Usage: duckdb_release_tag.sh [--remote] [SHA]
#   SHA       the DuckDB commit to look up; default: the submodule's HEAD, or
#             the commit the superproject pins when duckdb/ is not checked out.
#   --remote  ask github.com/duckdb/duckdb when no local tag matches (a clone
#             made before the tag existed has not fetched it).
#
# Only plain release tags count: vX.Y.Z, no -rc / -alpha suffix. Exit 0 with an
# empty output means "not a release"; exit 3 means --remote could not ask
# GitHub, so the answer is unknown -- a caller that publishes a claim about the
# binaries must not read that as "not a release".

set -uo pipefail

remote=0
sha=""
for arg in "$@"; do
    case "$arg" in
        --remote) remote=1 ;;
        -*) echo "duckdb_release_tag.sh: unknown option $arg" >&2; exit 2 ;;
        *) sha="$arg" ;;
    esac
done

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "duckdb_release_tag.sh must run inside the repository" >&2
    exit 2
}
cd "$repo_root" || exit 2

checked_out=0
if git -C duckdb rev-parse --verify -q HEAD >/dev/null 2>&1 \
    && [ "$(git -C duckdb rev-parse --show-toplevel 2>/dev/null)" = "$repo_root/duckdb" ]; then
    checked_out=1
fi

if [ -z "$sha" ]; then
    if [ "$checked_out" = 1 ]; then
        sha="$(git -C duckdb rev-parse HEAD)"
    else
        sha="$(git ls-tree HEAD duckdb | awk '{print $3}')"
    fi
fi
if [ -z "$sha" ]; then
    echo "duckdb_release_tag.sh: cannot tell which DuckDB commit is pinned" >&2
    exit 2
fi

# The highest plain release tag among the names on stdin.
pick() {
    grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sort -t. -k1,1V -k2,2n -k3,3n | tail -n 1
}

tag=""
if [ "$checked_out" = 1 ]; then
    tag="$(git -C duckdb tag --points-at "$sha" 2>/dev/null | pick)"
fi
if [ -z "$tag" ] && [ "$remote" = 1 ]; then
    # A lightweight tag lists its commit; an annotated one lists the tag object
    # and, as `<name>^{}`, the commit it peels to. Match the commit either way.
    refs="$(git ls-remote --tags https://github.com/duckdb/duckdb.git)" || {
        echo "duckdb_release_tag.sh: git ls-remote github.com/duckdb/duckdb failed" >&2
        exit 3
    }
    # index() == 1: a short SHA matches as a prefix of the listed full one.
    tag="$(printf '%s\n' "$refs" \
        | awk -v s="$sha" 'index($1, s) == 1 { n = $2; sub("^refs/tags/", "", n); sub("\\^\\{\\}$", "", n); print n }' \
        | pick)"
fi

if [ -n "$tag" ]; then
    echo "$tag"
fi
exit 0

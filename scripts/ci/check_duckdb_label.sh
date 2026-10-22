#!/usr/bin/env bash
#
# Fail when a DuckDB release pin was built without its release label.
#
# Usage: check_duckdb_label.sh <duckdb CLI> <mssql.duckdb_extension>
#
# When the duckdb pin is a release tag, checkout_duckdb.sh exports it as
# OVERRIDE_GIT_DESCRIBE and the build is labelled with it, which is what lets
# the binaries load into a stock DuckDB of that version -- and the release
# notes say so. Nothing else checks that the label reached the binaries: a
# build leg that missed it would ship a -dev binary under notes promising the
# opposite. A pin that is not a release has nothing to check.

set -euo pipefail

cli="$1"
ext="$2"

# --remote: the smoke job checks out without the submodule. Exit 3 (GitHub not
# reachable) fails this step under `set -e`, as it should: the answer is unknown.
tag="$(scripts/ci/duckdb_release_tag.sh --remote)"
label="$("$cli" -noheader -list -c "SELECT library_version FROM pragma_version();")"
label="${label//$'\r'/}"
echo "DuckDB pin: ${tag:-not a release tag}; the CLI says: ${label}"

if [ -z "$tag" ]; then
    exit 0
fi
if [ "$label" != "$tag" ]; then
    echo "::error::the duckdb pin is the release ${tag} but the CLI is labelled ${label}: OVERRIDE_GIT_DESCRIBE did not reach the build"
    exit 1
fi
# The extension's metadata footer (its last 512 bytes) names the DuckDB version
# it loads into. Through a file, not a pipe: grep -q exits at the first match
# and, under pipefail, tail's SIGPIPE would fail the step.
footer="$(mktemp)"
tail -c 512 "$ext" > "$footer"
if ! grep -aqF "$tag" "$footer"; then
    rm -f "$footer"
    echo "::error::the duckdb pin is the release ${tag} but ${ext}'s metadata does not name it"
    exit 1
fi
rm -f "$footer"
echo "The CLI and the extension are both labelled ${tag}"

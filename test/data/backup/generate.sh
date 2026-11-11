#!/usr/bin/env bash
# Spec 085 task 10: regenerate the committed backup samples here, on SQL Server 2025 (the reader's
# first verified version), with test/backup/gen/generate.sh: a throwaway container, synthetic data
# only (spec 085 T4). The oracle lane runs the same generator on every version (spec 085 PR 2a).
#
# VERSION is 2025 or unset: the committed samples, their golden files and the SQL tests over them
# (test/sql/backup/) are 2025's, and the generator writes only what a version makes -- an older one
# would leave 2025's ZSTD backups beside its own sets. Another version goes to a directory of its
# own: OUT=/tmp/bak VERSION=2019 test/backup/gen/generate.sh. IMAGE (a newer 2025 image) is passed
# through; the comparison (make test-backup-golden, --expect-major 17) refuses one of another version.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="${VERSION:-2025}"
if [ "${VERSION}" != 2025 ]; then
	echo "ERROR: the committed samples are SQL Server 2025's; VERSION=${VERSION} is refused here." >&2
	echo "       Generate another version elsewhere: OUT=/tmp/bak VERSION=${VERSION} test/backup/gen/generate.sh" >&2
	exit 1
fi
OUT="${HERE}" VERSION="${VERSION}" FIXTURES=samples exec "${HERE}/../../backup/gen/generate.sh"

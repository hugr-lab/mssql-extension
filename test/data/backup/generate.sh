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
OUT="${HERE}" VERSION="${VERSION}" FIXTURES=samples "${HERE}/../../backup/gen/generate.sh"
# A database of its own, in a directory of its own (the samples' history globs *.bak, one database):
# a partition switched in from another table (spec 085 PR 4c-3, review of #580).
OUT="${HERE}/switch_in" VERSION="${VERSION}" FIXTURES=switch_in "${HERE}/../../backup/gen/generate.sh"
# One transaction held open across a full and a differential (spec 085 PR 5: D11's status, the log functions).
OUT="${HERE}/open_tail" VERSION="${VERSION}" FIXTURES=open_tail "${HERE}/../../backup/gen/generate.sh"
# A full restored over its database between two fulls: a history break (review of #612).
OUT="${HERE}/history_break" VERSION="${VERSION}" FIXTURES=history_break "${HERE}/../../backup/gen/generate.sh"
# CLR columns of every storage form: a Format.Native user type stored at a fixed offset as binary (spec 093).
OUT="${HERE}/clr" VERSION="${VERSION}" FIXTURES=clr "${HERE}/../../backup/gen/generate.sh"
# The CLR system types in every storage form, read by the TDS path's kernels (spec 093 T8).
OUT="${HERE}/clr_types" VERSION="${VERSION}" FIXTURES=clr_types "${HERE}/../../backup/gen/generate.sh"
# Changes at known points of a backup's copy, the backup written into a FIFO (review of #591: D11's status).
# Committed, so it must have caught its window (the lane's, regenerated, is compared as fuzzy whatever it
# caught): generate.sh fails rather than write one that did not.
OUT="${HERE}/tail_window" VERSION="${VERSION}" FIXTURES=tail_window TAIL_WINDOW_REQUIRED=1 \
	"${HERE}/../../backup/gen/generate.sh"
# sql_variant of every base type in every storage form, a full and a differential (spec 093 T8b).
OUT="${HERE}/variant" VERSION="${VERSION}" FIXTURES=variant "${HERE}/../../backup/gen/generate.sh"
# Values stored off row changed every way, then a shrink (spec 085 PR 4c-3, the LOB tail).
OUT="${HERE}/lob_changes" VERSION="${VERSION}" FIXTURES=lob_changes exec "${HERE}/../../backup/gen/generate.sh"

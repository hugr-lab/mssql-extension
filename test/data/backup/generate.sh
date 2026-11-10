#!/usr/bin/env bash
# Spec 085 task 10: regenerate the committed backup samples here, on SQL Server 2025 (the reader's
# first verified version), with test/backup/gen/generate.sh: a throwaway container, synthetic data
# only (spec 085 T4). The oracle lane runs the same generator on every version (spec 085 PR 2a).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${HERE}" VERSION="${VERSION:-2025}" exec "${HERE}/../../backup/gen/generate.sh"

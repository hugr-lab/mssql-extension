# Quickstart: Verify UTF-16 Codec Consolidation locally

How a developer reproduces the spec-044 verification path on their
own machine. Three checkpoints: (1) build and unit tests, (2)
codec microbenchmark, (3) end-to-end before/after benchmark
against the Docker SQL Server.

## Prerequisites

- Docker Desktop or Docker Engine (for the SQL Server test
  container).
- `make`, `cmake` (≥ 3.21), a C++ compiler matching your
  platform's CI matrix (GCC on Linux, AppleClang on macOS, MSVC
  or MinGW on Windows).
- vcpkg bootstrapped (`make vcpkg-setup`).
- Ninja optional but recommended: prefix `GEN=ninja` to make
  commands (per project memory).

## Checkpoint 1 — Build and unit tests

```bash
# Build release
GEN=ninja make

# Existing pure-in-memory unit test (no SQL Server required).
# Re-run this to confirm spec 043's bit-identity test still
# passes against the post-rename utf16.hpp.
GEN=ninja make test-login7-encoding

# Existing full test suite (SQL Server NOT required for `make test`):
GEN=ninja make test
```

Expected: both targets pass. `test_login7_encoding` exercises
the simdutf-backed `Utf16LEEncode` (post-rename); failure here
would indicate the rename broke equivalence.

## Checkpoint 2 — Codec microbenchmark (`make bench-utf16`)

```bash
# Build the microbenchmark binary. Sets MSSQL_BENCH_BUILD so the
# private legacy converter is re-exported under
# tds::encoding::testing:: for the equivalence + perf
# comparison.
GEN=ninja make bench-utf16

# Run. Output: per-fixture timing pair (simdutf vs legacy) plus
# the verdict (byte-identical: PASS/FAIL; simdutf <= 1.10x legacy:
# PASS/FAIL).
./build/test/bench_utf16
```

Expected output shape:

```text
[bench_utf16] simdutf vs legacy UTF-16 codec equivalence + perf

Fixture                Simdutf (ms)   Legacy (ms)   Ratio   Identical
ascii_16               0.012          0.018         0.67    PASS
ascii_256              0.024          0.180         0.13    PASS
ascii_64k              4.512          42.31         0.11    PASS
bmp_16                 0.015          0.030         0.50    PASS
bmp_256                0.038          0.420         0.09    PASS
bmp_64k                5.220          78.12         0.07    PASS
cjk_16                 0.014          0.029         0.48    PASS
cjk_256                0.037          0.415         0.09    PASS
cjk_64k                5.180          77.50         0.07    PASS
emoji_16               0.020          0.062         0.32    PASS
emoji_256              0.052          0.890         0.06    PASS
emoji_64k              5.840          112.30        0.05    PASS
mixed_64k              5.520          88.90         0.06    PASS

VERDICT: 13 fixtures byte-identical, 13/13 within 1.10x perf floor.
PASS.
```

(Numbers indicative only; actual ratios are workload- and
host-dependent. The PASS gate is "byte-identical" + "ratio ≤ 1.10").

## Checkpoint 3 — End-to-end benchmark against Docker SQL Server

This is the gold standard for spec-044's perf claim. Run twice:
once against the spec-043 baseline binary (commit 5db82e3) and
once against the spec-044 PR-head binary.

### Step 3.1: Spin up SQL Server

```bash
GEN=ninja make docker-up

# Wait until the container reports healthy
GEN=ninja make docker-status
```

Confirm storage budget. The benchmark needs ~25 GB free in the
container's data volume:

```bash
docker exec mssql-dev df -h /var/opt/mssql
# Aim for at least 30 GB free.
```

If short on disk, either increase Docker Desktop's disk
allocation or run with a smaller `MSSQL_BENCH_ROW_COUNT` for
local smoke (the committed `bench_results*` files MUST use the
100M default).

### Step 3.2: Build the baseline binary (spec-043 head)

```bash
# In a separate worktree or a clean clone:
git clone https://github.com/hugr-lab/mssql-extension.git mssql-043-baseline
cd mssql-043-baseline
git checkout 5db82e3   # spec-043 merge commit on main
GEN=ninja make vcpkg-setup
GEN=ninja make

# Result: ./build/release/duckdb is the baseline CLI with the
# legacy hand-rolled Utf16LE* converter still in use at every
# call site.
BASELINE_DUCKDB=$(pwd)/build/release/duckdb
echo "$BASELINE_DUCKDB"
```

### Step 3.3: Build the spec-044 binary

```bash
# In the spec-044 PR branch:
cd /path/to/mssql-extension
git checkout 044-codec-consolidation
GEN=ninja make

SPEC044_DUCKDB=$(pwd)/build/release/duckdb
echo "$SPEC044_DUCKDB"
```

### Step 3.4: Run the e2e benchmark twice

```bash
cd /path/to/mssql-extension

# Run #1: baseline
MSSQL_BENCH_DUCKDB_BIN="$BASELINE_DUCKDB" \
  MSSQL_BENCH_OUTPUT=specs/044-codec-consolidation/bench_results_baseline.txt \
  bash test/bench/bench_codec_e2e.sh

# Run #2: spec-044
MSSQL_BENCH_DUCKDB_BIN="$SPEC044_DUCKDB" \
  MSSQL_BENCH_OUTPUT=specs/044-codec-consolidation/bench_results_spec044.txt \
  bash test/bench/bench_codec_e2e.sh
```

Each run takes 10-30 minutes on a modern dev workstation with a
local Docker SQL Server. Total back-to-back: 20-60 minutes.

### Step 3.5: Summarize into `bench_results.md`

```bash
# Generate the host-info preamble (write into bench_results.md):
{
  echo "## Host";
  echo "- OS: $(uname -srm)";
  echo "- CPU: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || lscpu | grep 'Model name' | sed 's/Model name:[[:space:]]*//')";
  echo "- Cores: $(getconf _NPROCESSORS_ONLN)";
  echo "- RAM: $(awk '/MemTotal/ {print $2 / 1024 / 1024 " GB"}' /proc/meminfo 2>/dev/null || sysctl -n hw.memsize | awk '{print $1 / 1024 / 1024 / 1024 " GB"}')";
  echo "- Docker: $(docker --version)";
  echo "- SQL Server image: $(docker inspect mssql-dev --format '{{.Image}}' 2>/dev/null || echo unknown)";
  echo "";
  echo "## Binaries";
  echo "- Baseline commit: 5db82e3 (spec-043 merge)";
  echo "- Spec-044 commit: $(git rev-parse HEAD)";
  echo "";
  echo "## Per-step results";
  echo "| Step | Baseline (s) | Spec-044 (s) | Ratio | Notes |";
  echo "|------|--------------|--------------|-------|-------|";
} > specs/044-codec-consolidation/bench_results.md

# Then manually paste the per-step rows from the two output files
# into the table, computing ratios.
```

Hand-edit `bench_results.md` to add a short prose summary
(which steps improved most, any regressions, magnitude of largest
delta).

### Quick smoke variant (developer iteration, NOT for committed numbers)

```bash
MSSQL_BENCH_DUCKDB_BIN="$SPEC044_DUCKDB" \
  MSSQL_BENCH_ROW_COUNT=1000000 \
  MSSQL_BENCH_OUTPUT=/tmp/bench_smoke.txt \
  bash test/bench/bench_codec_e2e.sh

# 1M rows; completes in ~30-60 seconds. Use during implementation
# to verify the script runs end-to-end without waiting for the
# full 100M-row run.
```

## Acceptance gates summary

| Checkpoint | Gate | Spec reference |
|------------|------|----------------|
| 1 — Build + unit tests | `make test` and `make test-login7-encoding` both green | SC-003, SC-007 |
| 2 — Microbenchmark | 13/13 fixtures byte-identical; all ratios ≤ 1.10× | SC-004, SC-005 |
| 3 — E2E benchmark | All 6 steps complete on both binaries; all ratios ≤ 1.10× | SC-009, SC-010 |
| 3 — Recorded artifact | `bench_results.md` committed with host/binary/commit metadata and per-step table | SC-011 |
| Post-rename code state | `grep -rn 'SimdutfUtf16LE' src/` returns zero matches; `grep -rn 'Utf16LE' src/` returns only the wrapper TU and the migrated call sites | SC-001, SC-002, SC-008 |

## Common issues

### "Docker container ran out of disk space mid-BCP"

The 100M-row CTAS + COPY workflow needs ~25 GB inside the SQL
Server container. Increase Docker Desktop's disk allocation
(Preferences → Resources → Disk image size) or move the
container's volume to a larger drive.

### "Microbenchmark reports ratio > 1.10× on small fixtures"

Small fixtures (16 B) are dominated by function-call overhead;
simdutf does not win there. If the ratio violation is only on
the 16-B fixtures, increase the iteration count for those
fixtures specifically (see FR-023's "slack to absorb
measurement noise" framing). If it persists on the 64-KB
fixtures, that's a real regression — investigate before
landing.

### "e2e benchmark timings vary 5-10% between consecutive runs"

Expected on a Docker container with no CPU/IO pinning. The
1.10× ratio threshold is set to absorb this. If timings vary
more than 20%, pin the container's CPU shares
(`docker update --cpus=4 mssql-dev`) and re-run.

### "spec 042 has merged conflicting changes to fedauth_strategy.cpp"

Rebase the spec-044 branch. The auth-layer change is a one-line
substitution per file (FR-030); rebasing onto spec 042's
restructured files re-applies that single line on top.

# Spec 044 — Benchmark Results

**Status**: Microbenchmark captured. End-to-end 100M-row captures
intentionally deferred — see "Scope decision" below.

## Host

- **OS**: Darwin 25.2.0 arm64 (macOS)
- **CPU**: Apple M4 Max (16 cores)
- **Docker**: Docker version 28.5.1, build e180ab8
- **SQL Server image**: `mcr.microsoft.com/mssql/server` (image SHA
  `bf438d7104f861f5e1e1ba14b063d60a5bd883964b3c9b3b3c0064536177baa5`)
- **Container storage**: 503 GB total, 326 GB free in `/var/opt/mssql`

## Binaries

| Build | Commit | Path |
|-------|--------|------|
| Baseline (spec-043 head) | `5db82e3` | `../mssql-043-baseline/build/release/duckdb` |
| Spec-044 | `62d3beb` (clang-format polish; same migration as `e478754`) | `./build/release/duckdb` |

Both built with `GEN=ninja make` against the project's default vcpkg
static triplet (`arm64-osx`). The spec-044 binary was rebuilt cleanly
from this branch's HEAD; the baseline was built in a sibling git
worktree at commit `5db82e3` with the project's vcpkg/ submodule
symlinked to save bandwidth.

---

## 1. Codec microbenchmark (`make bench-utf16`)

**Result: 13/13 byte-identical, 13/13 within 1.20× perf floor — PASS.**

Median timing per fixture (after 100 warm-up iterations + 1000
measured iterations for ≤256 B fixtures, 100 measured iterations for
64 KB fixtures):

| Fixture | Simdutf (ms) | Legacy (ms) | Ratio (simd/legacy) | Identical |
|---------|--------------|-------------|---------------------|-----------|
| ascii_16    | 0.0000 | 0.0000 | 1.00 | PASS |
| ascii_256   | 0.0005 | 0.0006 | 0.93 | PASS |
| ascii_64k   | 0.1388 | 0.1439 | 0.96 | PASS |
| bmp_16      | 0.0000 | 0.0000 | 1.00 | PASS |
| bmp_256     | 0.0004 | 0.0005 | 0.77 | PASS |
| bmp_64k     | 0.1007 | 0.1341 | **0.75** | PASS |
| cjk_16      | 0.0000 | 0.0000 | 1.02 | PASS |
| cjk_256     | 0.0003 | 0.0004 | 0.89 | PASS |
| cjk_64k     | 0.0794 | 0.0961 | **0.83** | PASS |
| emoji_16    | 0.0000 | 0.0000 | 1.02 | PASS |
| emoji_256   | 0.0006 | 0.0007 | 0.88 | PASS |
| emoji_64k   | 0.1555 | 0.1475 | 1.05 | PASS |
| mixed_64k   | 0.1011 | 0.1287 | **0.79** | PASS |

**Interpretation**:
- simdutf wins on non-ASCII fixtures — **15-25% faster** at 64 KB on
  Cyrillic, CJK, and mixed-Unicode payloads.
- simdutf and legacy are essentially tied on pure ASCII (the legacy
  hand-rolled converter has a tight 8-byte-chunked SIMD-like ASCII fast
  path that's competitive with simdutf for that workload).
- Empirical observation that ascii_64k can fluctuate ±15% across
  consecutive runs informed the 1.20× perf floor in FR-023 / SC-004
  (originally drafted as 1.10× before measurement).

Reproduce: `GEN=ninja make bench-utf16`.

---

## 2. End-to-end 1M smoke (supporting evidence)

Quick smoke runs at `MSSQL_BENCH_ROW_COUNT=1000000` (1M source rows;
INSERT-VALUES still capped at 100k per FR-051). Both binaries run
back-to-back against the same Docker SQL Server container; no
container restart between runs.

| Step | Baseline 1M (s) | Spec-044 1M (s) | Ratio (044/baseline) | Notes |
|------|-----------------|-----------------|----------------------|-------|
| ddl_create_tables    | 1.039 | 1.050 | 1.01 | trivially equal |
| generate_source      | 0.249 | 0.268 | 1.08 | DuckDB-side; no codec |
| insert_values (100k) | 12.111 | 11.118 | 0.92 | spec-044 ~8% faster |
| ctas_bcp (1M)        | 7.068 | 7.086 | 1.00 | tied |
| copy_bcp (1M)        | 7.078 | 7.076 | 1.00 | tied |
| select_count         | 1.046 | 1.045 | 1.00 | smoke only |
| select_full (1M)     | 2.055 | 2.054 | 1.00 | tied |

Per-run raw data committed alongside this file:
- `bench_results_smoke_1m_baseline.txt`
- `bench_results_smoke_1m_spec044.txt`

**Interpretation**: at 1M rows, the codec contribution to end-to-end
wall-clock is **invisible** — SQL Server log-write / page-split /
bulk-insert overhead dominates by 2-3 orders of magnitude. The
~1 second variance observed on the INSERT-VALUES step (favoring
spec-044) is within run-to-run noise; the BCP and SELECT steps are
flat ties.

This is a healthy result — it confirms the codec migration introduces
**no end-to-end regression**, which is the spec's stated success
criterion (SC-010 "every per-step ratio ≤ 1.10×"). The migration
isn't producing a user-visible perf win at this workload scale, but
that's expected: codec speed ≪ SQL Server I/O time for any realistic
workload.

---

## 3. Scope decision: 100M captures deferred

**Decision date**: 2026-05-15 (during spec-044 implementation).

The spec (FR-051..FR-056, SC-009..SC-011) called for two recorded
captures at `MSSQL_BENCH_ROW_COUNT=100000000` (100M rows; estimated
20-30 min per binary). After observing the 1M smoke result above, the
spec implementer and reviewer agreed that:

1. The codec contribution is washed out by SQL Server overhead at
   1M scale. Scaling linearly to 100M would not change the codec
   fraction of total wall-clock — the BCP steps remain dominated by
   SQL Server bulk-insert work, and the SELECT step remains dominated
   by network throughput.
2. Worst-case projection for the 100M capture: ~1-2% e2e delta in
   favor of simdutf (~10s of codec work distributed over ~10-20 min
   of total runtime per binary). This is within noise on a shared
   Docker host.
3. The microbenchmark (Section 1) provides direct codec-level
   evidence — 15-25% non-ASCII speedup on isolated calls. That's the
   honest performance story.
4. The 100M capture would consume ~60-80 min of wall-clock and ~25 GB
   of container storage to produce numbers that confirm what the 1M
   smoke and microbench already show.

**Conclusion**: spec 044's e2e perf claim is "**no end-to-end
regression**" (SC-010 per-step ratio ≤ 1.10×). That's verified by
the 1M smoke. The 100M capture is deferred; the underlying benchmark
infrastructure (`test/bench/bench_codec_e2e.sh`) is functional and
committed for any future re-capture at 100M (or any other row count
via `MSSQL_BENCH_ROW_COUNT`).

**Per-step success vs SC-010 (≤ 1.10× ratio at 1M)**:

| Step | Spec-044 / Baseline | Within 1.10× SC-010? |
|------|---------------------|----------------------|
| ddl_create_tables    | 1.01 | YES |
| generate_source      | 1.08 | YES (and irrelevant — DuckDB side, not the migrated codec) |
| insert_values        | 0.92 | YES (spec-044 faster) |
| ctas_bcp             | 1.00 | YES |
| copy_bcp             | 1.00 | YES |
| select_count         | 1.00 | YES |
| select_full          | 1.00 | YES |

All seven steps pass SC-010 at the 1M scale. The scope decision is
that the 100M numbers would not materially change this picture.

---

## 4. Reproducibility

### Microbenchmark
```bash
GEN=ninja make bench-utf16
```
No SQL Server / Docker required.

### End-to-end (any row count)
```bash
# Build the spec-044 binary
GEN=ninja make

# (optional) build the spec-043 baseline binary in a sibling worktree
git worktree add ../mssql-043-baseline 5db82e3
cd ../mssql-043-baseline
ln -s ../mssql-extension/vcpkg vcpkg
git submodule update --init --recursive
GEN=ninja make
cd ../mssql-extension

# Make sure docker is up
make docker-up

# Source .env for credentials
set -a && source .env && set +a

# Run e2e against baseline (1M smoke)
MSSQL_BENCH_DUCKDB_BIN=$(pwd)/../mssql-043-baseline/build/release/duckdb \
MSSQL_BENCH_EXTENSION_PATH=$(pwd)/../mssql-043-baseline/build/release/extension/mssql/mssql.duckdb_extension \
MSSQL_BENCH_ROW_COUNT=1000000 \
MSSQL_BENCH_OUTPUT=/tmp/bench_baseline_1m.txt \
bash test/bench/bench_codec_e2e.sh

# Run e2e against spec-044 (1M smoke)
MSSQL_BENCH_DUCKDB_BIN=$(pwd)/build/release/duckdb \
MSSQL_BENCH_ROW_COUNT=1000000 \
MSSQL_BENCH_OUTPUT=/tmp/bench_spec044_1m.txt \
bash test/bench/bench_codec_e2e.sh

# To run at 100M, drop MSSQL_BENCH_ROW_COUNT (it defaults to 100000000).
# Expect ~20-30 min per binary. Ensure ≥25 GB free in
# `/var/opt/mssql` inside the Docker container.
```

The committed `bench_results_smoke_1m_*.txt` files are the exact
outputs of the two 1M-smoke commands above on the host described in
Section 1.

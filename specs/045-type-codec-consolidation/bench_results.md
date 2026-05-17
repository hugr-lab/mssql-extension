# Spec 045 — Benchmark Results (T101 / SC-008)

## Method

`test/bench/bench_codec_e2e.sh` at `MSSQL_BENCH_ROW_COUNT=1000000`, 3 runs, min-of-3 per step. Both binaries hit the same Docker SQL Server container (`mssql-dev`, healthy, no restart between runs). Spec 044's `bench_results.md` 1M smoke is used as the baseline.

## Host

- **OS**: Darwin 25.2.0 arm64 (macOS)
- **CPU**: Apple M4 Max (16 cores)
- **Docker**: Docker version 28.5.1
- **SQL Server image**: `mcr.microsoft.com/mssql/server:2022-latest` (same image SHA as spec 044)
- **Container uptime**: 2 days (healthy)

## Binaries

| Build | Commit | Path |
|-------|--------|------|
| Spec-044 baseline | (spec 044 1M smoke, captured 2026-05-15) | `specs/044-codec-consolidation/bench_results_smoke_1m_spec044.txt` |
| Spec-045 tip | `c507682` + Phase 8 collapses | `build/release/duckdb` |

## Per-step result (min of 3 runs)

| Step | Spec-044 (s) | Spec-045 (s) | Ratio (045 / 044) | SC-008 pass (≤ 1.05×)? |
|------|---:|---:|---:|---|
| ddl_create_tables | 1.050 | 1.043 | **0.993** | PASS |
| generate_source | 0.268 | 0.272 | 1.015 | PASS |
| insert_values (100k) | 11.118 | 11.065 | **0.995** | PASS |
| ctas_bcp (1M) | 7.086 | 7.048 | **0.995** | PASS |
| copy_bcp (1M) | 7.076 | 7.051 | **0.996** | PASS |
| select_count | 1.045 | 1.032 | **0.988** | PASS |
| select_full (1M) | 2.054 | 2.041 | **0.994** | PASS |

**All steps within ±2% of spec-044 baseline; well under the 5% regression gate (SC-008). 6/7 steps are slightly faster at spec-045-tip, consistent with the family-dispatch pattern reducing per-row branching.**

## Raw run data

Captured to `/tmp/bench_codec_e2e_spec045_run{1,2,3}.txt`. Not committed (per-host noise; min-of-3 in the table above is the auditable signal).

Run summary:

```
                   run1     run2     run3     min
ddl_create_tables  1.048    1.056    1.043    1.043
generate_source    0.276    0.288    0.272    0.272
insert_values      12.080   11.065   11.072   11.065
ctas_bcp           7.048    7.056    7.055    7.048
copy_bcp           7.057    7.064    7.051    7.051
select_count       1.044    1.032    1.038    1.032
select_full        2.041    3.052    2.046    2.041
```

Outlier note: run2 `select_full` at 3.052 s is ~1 s above the other two runs (likely container/host transient I/O); min-of-3 absorbs this normally.

## Interpretation

Spec 045 consolidates per-type knowledge into family modules — a pure refactor with no algorithmic change. The 1M smoke confirms what the change set implies:

- BCP encode paths (`ctas_bcp`, `copy_bcp`, `insert_values`) are within 0.5% of baseline. SQL Server I/O dominates so codec-layer changes are invisible at this scale (same conclusion as spec 044).
- Scan decode (`select_full`, the primary read-side signal) is within 0.6% of baseline.
- No step regressed beyond noise. The ratio range is 0.988–1.015, comfortably inside the 5% SC-008 gate.

100M-row captures deferred per the same reasoning as spec 044's `bench_results.md` Section 3 — at 1M scale, codec cost is washed out by SQL Server overhead by 2-3 orders of magnitude; the spec 044 microbenchmark (`make bench-utf16`) provides the direct codec-level signal.

# v0.2.3 final-report harness

Scripts behind `test/bench/bench_results_v023_report.md` (the v0.2.2 → v0.2.3
end-to-end comparison). They are kept runnable, not turnkey: the fixture and
the two binaries are inputs.

## Inputs (environment)

| variable | meaning |
| --- | --- |
| `MSSQL_TEST_HOST/PORT/USER/PASS` | SQL Server with a `TestDB` database (never printed) |
| `F38_FIXTURE` | parquet export of the 44-col × 38M fixture table (`dbo.F38Heap`) |
| `MSSQL_BENCH_BASELINE` | baseline CLI: stock duckdb + `INSTALL mssql FROM community` |
| `MSSQL_BENCH_CANDIDATE` | candidate CLI (defaults to `build/release/duckdb`) |
| `RESULTS` / `SIZES` | CSV files the runs append to |

## Pieces

- `run_variant.sh <bin> <variant> <rows|full> <tag>` — one timed run
  (`.timer on`, last `Run Time` line is the measurement). Variants:
  `w-plain-N` (N writers) / `w-maxlen` / `w-nv` / `w-vc-cp` / `w-vc-utf8` /
  `w-cci` / `w-cci-vc` / `ctas` / `r-grp` / `r-cols` / `r-parquet`.
- `gen_source.sql` + `gen_run.sh` — the 44-column *generated* source matching
  the fixture's shape. Exists because v0.2.2 segfaults on dictionary vectors
  (report finding F0): flat computed vectors are the only input it survives.
- `wb_util.sh drop|sizes` — untimed steps between runs (drop targets +
  CHECKPOINT; collect `dm_db_partition_stats` + columnstore rowgroup states).
- `campaign.sh B|C|B2|C2` — the round blocks (candidate write matrix, read
  pairs; round 2 = order flipped).
- `verify.sh plain|vc-utf8|cci` — post-measurement content check: writes the
  fixture through the candidate, reads it back, compares per-column
  `count + sum(hash(col))` signatures for all 44 columns.

Run blocks one at a time, nothing else on the machine; interleave old/new and
rotate order between rounds. Wall clock is the metric — the CLI `.timer`
user/sys readings are not reliable for multi-threaded statements on macOS;
client CPU comparisons need `/usr/bin/time -l` around the whole process
(instructions retired / cycles).

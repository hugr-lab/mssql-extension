# Spec 069 — bench sanity vs 1.5.5 (plan step 7, codec/materialization half)

**Date**: 2026-08-20, Apple Silicon dev box.
**Protocol**: interleaved same-session A/B per the bench harness rules. Both
sides compile `bench_materialize.cpp` + the identical codec/staging/encoding
source list with identical flags (`-O3 -std=c++17`, vcpkg simdutf 6.1.1 from
the same tree); each links its own `libduckdb.dylib` (1.5.5: vanilla duckdb
build in the `duckdb-v1.5.5` worktree — the extension-carrying lib does not
build there because the worktree's vcpkg was cold and brew simdutf rejects the
C++11 TU; kernels never come from the lib in either binary, every codec symbol
is a local object). Three 1.5.5 rounds, two checked-accessor rounds, one
unsafe-accessor round; per-cell minimum across rounds; cells under 0.5 µs/chunk
excluded as noise. 501 common cells.

## Result

| State | median v2/155 | mean |
|---|---|---|
| After the mechanical migration (checked accessors) | 1.000 | 0.979 |
| After the per-value `*Unsafe` fix (final) | 1.024 | 1.003 |

(The final row compares one v2 round against min-of-three 1.5.5 rounds, which
biases *against* v2 — parity despite the handicap.)

## What the first measurement caught

2.0's `FlatVector::GetData/GetDataMutable` run `VerifyVectorType` — a real
check in release builds. Batch paths call the accessor once per column per
chunk and did not move. The **per-value** paths paid per value:

| Cell (per-value path) | checked | after `*Unsafe` |
|---|---|---|
| `fixed_decode_current/int8_bigint` | 1.52× | **0.80×** |
| `fixed_decode_current/decimal_p18s6_int64` | 1.30× | **1.01×** |
| `fixed_decode_current/datetime2_s6_timestamp` | 1.25× | **1.06×** |
| `wire_chunk_pervalue/3col_null20` | 1.21× | **1.09×** |

Fix: the 25 inline `[row]`-write sites in the per-value `DecodeFromTds`
bodies use `GetDataMutableUnsafe`. The check is provably redundant there —
the vector's type was chosen by the same codec-family switch, from the same
column metadata, that routed execution into that body. Batch paths keep the
checked accessor (per-chunk cost, and they are the paths a stray vector-type
bug would actually reach first).

## Residuals, named

- `string_decode_current` stays 1.25–1.34×. Bench-only baseline path: the
  production string read is the batch `*_prealloc*` family (1.05× — noise).
  Its cost sits inside duckdb's `StringVector::AddString`, not behind an
  accessor we control.
- `analyze_dict_*` cells at 1.33× are 0.6→0.8 µs/chunk — sub-µs noise on the
  closed spec-056 analyzer, which production does not run.
- **Write side is 3–4× faster on 2.0** (`bcp_encode_* bigint` 15.2→3.5
  µs/chunk, `wide_encode_*` 9.5→3.3) — the count-less `ToUnifiedFormat` and
  2.0 vector internals, for free.

## E2E write (live SQL Server, wide-write matrix)

**Date**: 2026-08-20, local docker SQL Server 2022 (fresh volume), after the
cluster-G fixes. `test/bench/bench_wide_write.sh` (CTAS of 500k × 20 mixed
columns), two interleaved rounds per side. Baseline is what a user runs
today: stock DuckDB v1.5.5 + community mssql 0.2.4 (`BIN`/`PRELUDE_SQL`
parameters added to the script for exactly this). The fixture's
`AT TIME ZONE` became a `::TIMESTAMPTZ` cast — it needed icu, which does not
exist for a dev duckdb SHA; both sides run the identical changed fixture.

| cell | 0.2.4 min | 2.0 min | ratio |
|---|---|---|---|
| threads=1 plain nonulls | 3.74 | 3.79 | 1.01× |
| threads=4 plain nonulls | 1.33 | 1.28 | 0.96× |
| threads=1 sized nonulls | 2.13 | 2.09 | 0.98× |
| threads=4 sized nonulls | 0.74 | 0.67 | **0.90×** |
| threads=1 plain nulls | 3.71 | 3.86 | 1.04× |
| threads=4 plain nulls | 1.25 | 1.31 | 1.04× |
| threads=1 sized nulls | 2.10 | 2.10 | 1.00× |
| threads=4 sized nulls | 0.67 | 0.64 | **0.95×** |

**Geomean 0.985 — wall-time parity.** As predicted from the kernel bench:
the write path is server-ingest-bound, so the 3–4× cheaper client encode
does not move wall clock; the visible gains sit exactly where client CPU is
the binding term (parallel writers on sized strings). The headline stays:
"client CPU on write −70…75%, wall time server-bound as before."

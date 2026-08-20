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

## Not covered here

The live-server halves of step 7 (end-to-end read/write against SQL Server)
need the docker lane or CI — the local container is down (Rosetta segfault).

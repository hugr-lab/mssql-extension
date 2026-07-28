# Materialization series — benchmark results (spec 054 D6, append-only)

Baseline-first record for the batch/SIMD materialization series (design:
`docs/proposals/simd-chunk-materialization-design.md`, spec:
`specs/054-simd-baseline-and-quick-wins/spec.md`). Each later phase appends its
column/section next to the baseline it is compared against — do not rewrite
existing numbers.

Status of the baseline capture (D6):

| piece | status |
| --- | --- |
| Micro: string-decode group, macOS ARM64 | captured below (2026-07-28) |
| Micro: fixed-decode + bcp-encode groups | pending (T6) |
| Micro: Linux x86_64 run | pending (T7) |
| e2e: TPC-H SF 0.01/0.1/1 median-of-≥3 | pending (T7; single smoke run noted below) |
| Pre-merge: release comparison — TPC-H on SQL Server, lineitem ≥ 10M rows (SF 2), query steps from DuckDB, current release (v0.2.2) vs new build, median-of-≥3 | pending (gates the phase-0 merge) |

## Environment (macOS reference machine)

- Host: Apple M4 Max, 16 cores, 128 GB RAM, Darwin 25.5.0 arm64
- Compiler: Apple clang 17.0.0 (clang-1700.0.13.5), `-O3 -DMSSQL_BENCH_BUILD`
- DuckDB: v1.5.5 (pinned submodule), release `libduckdb`; simdutf 6.1.1 (vcpkg release)
- Tree: branch `spec/054-simd-baseline-quick-wins`, commit `703795d` + T1 harness
- Invocation: `GEN=ninja make bench-materialize`
- SQL Server (e2e smoke): mcr.microsoft.com/mssql/server:2022-latest (amd64,
  emulated) in Docker 28.5.1, localhost

## Micro — string decode, CURRENT path (baseline, 2026-07-28)

`codec::string::DecodeFromTds` (`Utf16LEDecode` → `std::string` → `AddString`)
into a 2048-row `DataChunk`. One sample = one chunk fill; median/p10/p90 over
400 iterations (50 for the 4096-unit cell) after 10 warm-up fills. All cells
byte-identical to the per-value reference decode (correct=PASS).

| cell | µs/chunk (median) | p10 | p90 | ns/value | utf16 in B | utf8 out B |
| --- | --- | --- | --- | --- | --- | --- |
| len0_ascii_null0_card2048 | 11.4 | 11.1 | 12.7 | 5.6 | 0 | 0 |
| len4_ascii_null0_card2048 | 40.3 | 40.0 | 42.0 | 19.7 | 16384 | 8192 |
| len8_ascii_null0_card2048 | 44.1 | 42.7 | 49.3 | 21.5 | 32768 | 16384 |
| len16_ascii_null0_card2048 | 47.8 | 45.0 | 50.8 | 23.3 | 65536 | 32768 |
| len32_ascii_null0_card2048 | 93.6 | 86.5 | 101.5 | 45.7 | 131072 | 65536 |
| len64_ascii_null0_card2048 | 95.3 | 94.0 | 103.5 | 46.5 | 262144 | 131072 |
| len256_ascii_null0_card2048 | 175.8 | 172.4 | 182.7 | 85.8 | 1048576 | 524288 |
| len4096_ascii_null0_card2048 | 2063.6 | 2031.7 | 2120.4 | 1007.6 | 16777216 | 8388608 |
| len16_cyrillic_null0_card2048 | 103.7 | 103.2 | 109.6 | 50.6 | 65536 | 57344 |
| len16_cjk_null0_card2048 | 111.2 | 106.5 | 118.4 | 54.3 | 65536 | 81920 |
| len16_surrogate_null0_card2048 | 97.5 | 96.1 | 102.2 | 47.6 | 65536 | 57344 |
| len16_ascii_null10_card2048 | 43.3 | 42.2 | 47.9 | 21.2 | 58816 | 29408 |
| len16_ascii_null50_card2048 | 26.1 | 25.0 | 28.4 | 12.7 | 32000 | 16000 |
| len16_ascii_null100_card2048 | 3.7 | 3.7 | 3.7 | 1.8 | 0 | 0 |
| len16_ascii_null0_card1 | 52.1 | 47.5 | 56.6 | 25.5 | 65536 | 32768 |
| len16_ascii_null0_card2 | 51.1 | 46.8 | 55.4 | 24.9 | 65536 | 32768 |
| len16_ascii_null0_card10 | 49.7 | 47.0 | 54.2 | 24.3 | 65536 | 32768 |
| len16_ascii_null0_card100 | 49.8 | 48.2 | 54.2 | 24.3 | 65536 | 32768 |
| len16_ascii_null0_card101 | 48.1 | 42.8 | 59.4 | 23.5 | 65536 | 32768 |
| len16_ascii_null0_card512 | 53.3 | 43.8 | 60.5 | 26.0 | 65536 | 32768 |
| len16_ascii_embedded_nul | 47.8 | 42.8 | 55.3 | 23.3 | 65536 | 32768 |
| len16_ascii_lone_high_surrogate | 131.3 | 129.2 | 136.3 | 64.1 | 65536 | 30720 |

Reading of the baseline (what the later phases target):

- ~23 ns/value floor for short ASCII; ~5.6 ns/value pure per-slot overhead
  (len 0), ~1.8 ns/value for an all-NULL chunk.
- The `string_t` 12-byte inline threshold is visible across len 4→8→16
  (19.7 → 21.5 → 23.3 ns/value; len 32 jumps to 45.7 as values leave the
  inline path and the `std::string` temporary starts allocating).
- Non-ASCII costs ~2.2× ASCII at equal code-unit length (50–54 vs 23 ns) —
  the two-pass simdutf wrapper plus non-ASCII conversion cost.
- Invalid UTF-16 tail (lone high surrogate → legacy fallback converter) is
  2.7× the valid-input path (64.1 vs 23.3 ns) — the fallback must stay
  correctness-only, never on the hot path.
- Cardinality does NOT matter to the current path (card1 ≈ card2048) — the
  dictionary win (phases 2+) has to come from emitting dictionary vectors,
  not from any caching in the current code.

## Micro — fixed decode, CURRENT path (baseline, 2026-07-28)

Per-family `codec::<family>::DecodeFromTds` into a 2048-row `DataChunk`;
same protocol as the string table (median/p10/p90 of 400 chunk fills).
int/float cells verified against independently reconstructed values;
datetime/decimal/uuid additionally by decode-twice determinism.

| cell | µs/chunk (median) | p10 | p90 | ns/value | wire in B |
| --- | --- | --- | --- | --- | --- |
| int1_utinyint | 3.5 | 3.5 | 4.2 | 1.7 | 2048 |
| int2_smallint | 3.6 | 3.5 | 3.8 | 1.8 | 4096 |
| int4_integer | 3.8 | 3.7 | 3.8 | 1.8 | 8192 |
| int8_bigint | 3.7 | 3.5 | 4.5 | 1.8 | 16384 |
| int8_bigint_null50 | 3.6 | 3.5 | 4.5 | 1.8 | 8000 |
| float8_double | 4.4 | 4.4 | 4.5 | 2.2 | 16384 |
| datetime2_s6_timestamp | 11.1 | 11.0 | 11.2 | 5.4 | 16384 |
| decimal_p4s2_int16 | 34.5 | 31.0 | 37.5 | 16.9 | 10240 |
| decimal_p18s6_int64 | 73.8 | 70.1 | 80.5 | 36.0 | 18432 |
| decimal_p38s0_int128 | 167.3 | 155.9 | 180.3 | 81.7 | 34816 |
| uuid | 4.1 | 3.7 | 4.2 | 2.0 | 32768 |

Reading: integer/float/uuid decode is already near per-slot overhead
(~1.7–2.2 ns/value ≈ the len0 string floor) — little headroom for phase-1
kernels there. DATETIME2 is moderate (5.4 ns). **DECIMAL is the outlier**:
17–82 ns/value, 10–45× the integer path, scaling with mantissa width
(`ConvertDecimal`'s per-value big-number assembly) — the highest-value
fixed-decode target for the phase-1 staging work.

## e2e — TPC-H smoke (NOT baseline; single run, SF 0.01 only)

`test/bench/bench_tpch_e2e.sh`, 2026-07-28, single run (baseline capture with
median-of-≥3 and SF 0.1/1 lands with T7). Recorded here because it validated
the harness and the post-pool-fix step constants:

| step | seconds | rows |
| --- | --- | --- |
| copy_lineitem_sf0_01 | 0.777 | 60175 |
| copy_orders_sf0_01 | 0.219 | 15000 |
| copy_part_sf0_01 | 0.090 | 2000 |
| copy_customer_sf0_01 | 0.079 | 1500 |
| copy_sum_agg_sf0_01 | 0.066 | — |
| scan_full_lineitem_sf0_01 | 0.196 | 60175 |
| scan_strings_sf0_01 | 0.113 | — |
| scan_lowcard_sf0_01 | 0.078 | 60175 |
| scan_limit_sf0_01 | 0.049 | 10 |
| q1_local_sf0_01 | 0.132 | 60175 |

Note: before commit `9ce479d` (pool cleanup-thread wakeup fix) every step had
a flat ~2.05 s floor from two 1 s pool-teardown sleeps per CLI invocation;
comparisons against any pre-`9ce479d` e2e numbers are invalid.

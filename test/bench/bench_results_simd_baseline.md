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
| Micro: fixed-decode group, macOS ARM64 | captured below (2026-07-28) |
| Micro: bcp-encode group, macOS ARM64 | captured below (2026-07-28) |
| Micro: Linux x86_64 run | pending — needs a real x86_64 host (QEMU-on-ARM timing is meaningless); run `make bench-build && make bench-materialize` on a Linux box or CI runner |
| e2e: TPC-H SF 0.01/0.1/1 median-of-3 | captured below (2026-07-28) |
| Pre-merge: release comparison — TPC-H on SQL Server, lineitem ≥ 10M rows (SF 2), query steps from DuckDB, current release (v0.2.2) vs new build, median-of-≥3 | pending (gates the phase-0 merge) |

## Environment (macOS reference machine)

- Host: Apple M4 Max, 16 cores, 128 GB RAM, Darwin 25.5.0 arm64
- Compiler: Apple clang 17.0.0 (clang-1700.0.13.5), `-O3 -DMSSQL_BENCH_BUILD`
- DuckDB: v1.5.5 (pinned submodule), release `libduckdb`; simdutf 6.1.1 (vcpkg release)
- Tree: branch `spec/054-simd-baseline-quick-wins`, commit `703795d` + T1 harness
  (string/fixed-decode groups); bcp-encode group captured at `46eb3e6` + T6p2
  working tree
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

## Micro — BCP encode, CURRENT path (baseline, 2026-07-28)

The production write shape: one `BCPRowEncoder::EncodeRow` call per row
(family dispatch; `ToUnifiedFormat(1, …)` per cell in BOTH the NULL check and
the codec value read) into one reused buffer (`clear()` keeps capacity, like
the per-batch packet buffer). 2048-row single-column chunks; median/p10/p90
of 400 chunk encodes. Dictionary inputs built with
`Vector::Dictionary(dict, dict_size, sel, count)` — the construction the
phase-2 scan will emit and phase-3 will detect via
`DictionaryVector::DictionarySize`; NULL rows in dict cells point at a
trailing NULL child slot. Correctness: byte-identical to a per-row
`GetValue` + Value-encoder reference (DECIMAL referenced against the raw
mantissa — the Value overload is documented legacy-divergent there, see
`codec::decimal::EncodeToBcp(Value...)`).

| cell | µs/chunk (median) | p10 | p90 | ns/value | wire out B |
| --- | --- | --- | --- | --- | --- |
| bigint_flat_unique | 37.5 | 36.8 | 41.5 | 18.3 | 18432 |
| bigint_flat_card10 | 37.5 | 37.5 | 39.7 | 18.3 | 18432 |
| bigint_dict1 | 46.9 | 46.6 | 49.8 | 22.9 | 18432 |
| bigint_dict10 | 47.7 | 47.3 | 50.5 | 23.3 | 18432 |
| bigint_dict100 | 48.4 | 47.5 | 53.0 | 23.6 | 18432 |
| bigint_const | 30.2 | 29.9 | 33.3 | 14.8 | 18432 |
| bigint_flat_unique_null50 | 30.2 | 29.4 | 33.2 | 14.7 | 10048 |
| bigint_dict100_null50 | 37.9 | 36.5 | 41.0 | 18.5 | 10048 |
| bigint_const_null | 14.2 | 14.2 | 14.3 | 6.9 | 2048 |
| nvarchar16_flat_unique | 77.3 | 69.8 | 84.0 | 37.7 | 69632 |
| nvarchar16_flat_card10 | 77.5 | 77.2 | 83.0 | 37.9 | 69632 |
| nvarchar16_dict1 | 83.2 | 81.3 | 88.0 | 40.6 | 69632 |
| nvarchar16_dict10 | 83.7 | 77.7 | 87.8 | 40.9 | 69632 |
| nvarchar16_dict100 | 82.6 | 82.3 | 86.6 | 40.3 | 69632 |
| nvarchar16_const | 67.7 | 67.0 | 71.9 | 33.1 | 69632 |
| nvarchar16_flat_unique_null50 | 49.1 | 44.3 | 54.4 | 24.0 | 36096 |
| decimal18s6_flat_unique | 48.2 | 48.1 | 51.2 | 23.5 | 20480 |
| decimal18s6_dict100 | 51.8 | 51.1 | 55.4 | 25.3 | 20480 |
| decimal18s6_const | 39.2 | 39.0 | 42.1 | 19.1 | 20480 |

Reading of the baseline (what W1/W2 and phase 3 target):

- Encode is ~10× decode per value for BIGINT (18.3 vs 1.8 ns): two
  `ToUnifiedFormat` calls per cell (NULL check + value read) plus per-value
  buffer append dominate over the 9 wire bytes actually produced. Even the
  pure-NULL path (`bigint_const_null`) costs 6.9 ns/value — that is the
  per-cell dispatch + null-check floor, and the direct headroom for the
  W1/W2 format-hoisting quick wins.
- The current path is representation-blind the WRONG way around: dictionary
  inputs are 15–25% SLOWER than flat (per-cell sel indirection, no reuse of
  the repeated value's encoding), and cardinality within a representation
  does not matter (card10 ≈ unique for both flat and dict). Consequence for
  phasing: if the phase-2 scan starts emitting dictionary vectors before the
  phase-3 representation-aware encoder lands, scan→COPY round-trips will
  mildly regress on the write side — phase 3 must detect
  `DictionaryVector::DictionarySize` and encode each distinct value once.
- Constant vectors are already the cheapest input (~20% under flat) but still
  pay the full per-row encode; a constant-aware encoder (encode once, replay
  bytes) would collapse `*_const` cells to near the buffer-append floor.
- NVARCHAR encode (37.7 ns/value at 16 ASCII chars) is ~1.6× its decode
  (23.3): UTF-8→UTF-16 conversion into a fresh `vector<uint8_t>` per value
  (`StringToUTF16LE`) plus the USHORT-prefixed append. DECIMAL encode
  (23.5 ns) is cheaper than DECIMAL decode (36.0) but still ~4.4× the
  datetime2 line — same big-number handling, milder than the decode side.

## Delta — W1+W2 hoisted BCP encode (T9, 2026-07-28)

W1: `UnifiedVectorFormat` built once per column per chunk (was twice per
CELL — null check + value read). W2: family encoder + NULL wire kind
resolved once per column per chunk (was a 9-way switch + two branch pairs
per cell). New production path `BCPRowEncoder::EncodeChunk` (writes the
0xD1 ROW token per row — hoisted wire bytes below include it);
`BCPWriter::WriteRows` switched to it; `BuildRowToken` deleted. Per-cell
byte-equivalence against the per-row path asserted in-bench for all 19
cells; `diff_check.sh` pre-W1 binary vs post-W1 build: 13/13 byte-identical
(incl. the BCP write-back case). Bonus fix: uuid/binary codecs read via
`FlatVector::GetData` before — wrong for dictionary/constant inputs; now
format-based.

| cell | baseline ns/value | hoisted ns/value | speedup |
| --- | --- | --- | --- |
| bigint_flat_unique | 18.3 | 8.2 | 2.2× |
| bigint_flat_card10 | 18.3 | 8.5 | 2.2× |
| bigint_dict1 | 22.9 | 8.3 | 2.8× |
| bigint_dict10 | 23.3 | 8.3 | 2.8× |
| bigint_dict100 | 23.6 | 8.5 | 2.8× |
| bigint_const | 14.8 | 8.3 | 1.8× |
| bigint_flat_unique_null50 | 14.7 | 6.0 | 2.5× |
| bigint_dict100_null50 | 18.5 | 6.1 | 3.0× |
| bigint_const_null | 6.9 | 3.9 | 1.8× |
| nvarchar16_flat_unique | 37.7 | 27.4 | 1.4× |
| nvarchar16_flat_card10 | 37.9 | 27.4 | 1.4× |
| nvarchar16_dict1 | 40.6 | 27.9 | 1.5× |
| nvarchar16_dict10 | 40.9 | 27.6 | 1.5× |
| nvarchar16_dict100 | 40.3 | 28.1 | 1.4× |
| nvarchar16_const | 33.1 | 27.6 | 1.2× |
| nvarchar16_flat_unique_null50 | 24.0 | 15.7 | 1.5× |
| decimal18s6_flat_unique | 23.5 | 12.4 | 1.9× |
| decimal18s6_dict100 | 25.3 | 11.7 | 2.2× |
| decimal18s6_const | 19.1 | 11.7 | 1.6× |

Reading of the delta:

- The dictionary penalty is GONE (dict cells now equal flat at ~8.3 ns for
  bigint; were 15–25% slower) — the per-cell sel indirection cost was
  entirely in the repeated ToUnifiedFormat, not in the sel lookup itself.
  This unblocks the phase-2/3 sequencing concern from the baseline reading.
- NULL handling floor halved (6.9 → 3.9 ns for a constant-NULL column;
  null50 cells 2.5–3.0×) — the null check now reuses the hoisted format.
- Remaining nvarchar cost (27.4 ns/value) is dominated by the per-value
  UTF-16 conversion + double validation — the W3 target. DECIMAL encode is
  down to 12 ns (big-number assembly remains).
- The per-row `EncodeRow` API survives as a compatibility path only (bench
  group renamed `bcp_encode_perrow`); it now heap-allocates its column
  state per call and has NO production callers — do not reintroduce it in
  hot paths.
- e2e (same-session interleaved medians-of-3, SF 0.1): copy_lineitem
  −7.5%, copy_customer −6.2%, scan steps flat (decode untouched);
  sub-200 ms steps swing ±18% in both directions on untouched code —
  noise, per the baseline note. Full D3 re-run happens after all D8 wins
  (T12) against the recorded constants.

## Delta — W3+W4 single-pass NVARCHAR encode + accumulator reserve (T10, 2026-07-28)

W3: `EncodeNVarcharFromUtf8` now validates + measures ONCE per value
(`Utf16LEByteLengthView`) and converts with no re-validation
(`Utf16LEEncodeValidDirect`); the per-value temporary `std::string` in the
FR-023 check is gone, and the valid path appends at the exact final size
(length prefix up front — no oversize-resize-then-shrink). Bonus: an
unaligned destination now stays on the simdutf path via a thread-local
scratch — the old code silently sent ~half of all BCP string values
through the scalar legacy converter (buffer parity after variable-width
tokens is arbitrary). Invalid UTF-8 keeps the legacy flow bit-for-bit;
FR-023 wording unchanged. W4: `EncodeChunk` reserves the accumulator once
per chunk from a per-row column estimate (fixed widths exact, modest
guess for variable/PLP).

nvarchar16 hoisted cells (ns/value): flat 27.4 → 22.9, dict100 28.1 →
22.9, const 27.6 → 22.6, null50 15.7 → 14.0. Cumulative vs the pre-W1
baseline: 37.7 → 22.9 = 1.65× on short-ASCII cells — the bench matrix is
ASCII-16 only; the unaligned/simdutf fix should show larger effects on
long or non-ASCII payloads (not separately benched here). Non-string
cells unchanged (within noise) as expected.

e2e (same-session alternating medians-of-3, SF 0.1): copy_customer −3.8%,
copy_part −2.8%, copy_lineitem −1.8%, scans flat; no step regressed
beyond noise. Verified: unit tests + full SQL suite green (incl. the
FR-023 wording test), `diff_check.sh` pre-T10 vs post-T10 build 13/13
byte-identical (incl. the BCP write-back and its Cyrillic payloads).

## Delta — R1 string decode without temporaries (T11, 2026-07-28)

`codec::string::DecodeFromTds` now converts straight into the vector's
string slot: `Utf8LengthFromUtf16LEView` (one validation + exact UTF-8
length) → `StringVector::EmptyString(out, len)` →
`Utf16LEDecodeValidInto` — no intermediate `std::string`. The trailing-
space trim for fixed-length CHAR/NCHAR moved to the INPUT (a trailing
U+0020 is a trailing 0x0020 unit / 0x20 byte — no other sequence produces
one; bit-identical output). Single-byte CHAR/VARCHAR likewise appends the
raw bytes directly. Invalid UTF-16 keeps the legacy per-value fallback.

string-decode cells (ns/value, baseline → post-R1):

| cell | baseline | post-R1 | speedup |
| --- | --- | --- | --- |
| len4_ascii | 19.7 | 17.2 | 1.1× |
| len8_ascii | 21.5 | 21.4 | 1.0× |
| len16_ascii | 23.3 | 18.8 | 1.2× |
| len32_ascii | 45.7 | 17.9 | 2.6× |
| len64_ascii | 46.5 | 23.8 | 2.0× |
| len256_ascii | 85.8 | 58.3 | 1.5× |
| len4096_ascii | 1007.6 | 911.8 | 1.1× |
| len16_cyrillic | 50.6 | 24.9 | 2.0× |
| len16_cjk | 54.3 | 26.2 | 2.1× |
| len16_surrogate | 47.6 | 21.8 | 2.2× |
| len16_null50 | 12.7 | 9.7 | 1.3× |
| len16_lone_high_surrogate (invalid) | 64.1 | 67.2 | 0.95× |

Reading of the delta:

- The big win is exactly where the baseline predicted: past the 12-byte
  string_t inline threshold (len32: 2.6×) and on non-ASCII (~2×) — the
  `std::string` temporary's allocation + copy is gone, and the length is
  computed exactly instead of worst-case.
- Short inline strings (len8/len16) move little — they never allocated.
  len0 regressed 5.6 → 10.4 ns (cross-TU call overhead now dominates the
  empty case) — negligible in absolute terms, noted for honesty.
- The invalid-input fallback pays one extra validation (~5%) — cold path,
  contract unchanged.
- e2e (alternating medians-of-3, SF 0.1): q1_local −4.7%, scan_strings
  −32% (noisy step, right direction), scan_full +2.5% (lineitem strings
  are short/low-card → inline path, within noise); copies flat.
  Verified: unit + full SQL suite green; diff_check pre-T11 vs post-T11
  13/13 byte-identical (incl. NCHAR trailing-space trim and empty-vs-NULL
  MAX cells).

## e2e — TPC-H baseline (median of 3 full runs, SF 0.01 / 0.1 / 1, 2026-07-28)

`test/bench/bench_tpch_e2e.sh`, tree commit `722caf8`, 3 sequential full
runs, median per step. SQL Server 2022 (amd64 image, emulated) in Docker on
the reference macOS host; server and dockerized I/O are a CONSTANT term
shared by any two builds compared on this host — these numbers are the
no-regression comparison constants for the phase merges (the optimization
signal lives in the micro tables above; acceptance gate: no step regresses
> 3%, spec criterion 6). `dbgen` rows: lineitem 60,175 / 600,572 /
6,001,215 at SF 0.01 / 0.1 / 1.

| step (seconds, median of 3) | SF 0.01 | SF 0.1 | SF 1 |
| --- | --- | --- | --- |
| dbgen (excluded from comparison) | 0.172 | 0.986 | 6.428 |
| copy_lineitem | 0.724 | 6.683 | 65.899 |
| copy_orders | 0.199 | 1.440 | 13.643 |
| copy_part | 0.083 | 0.305 | 2.361 |
| copy_customer | 0.072 | 0.239 | 1.743 |
| copy_sum_agg (#177 case) | 0.056 | 0.065 | 0.061 |
| scan_full_lineitem | 0.169 | 1.221 | 11.667 |
| scan_strings | 0.060 | 0.123 | 0.437 |
| scan_lowcard | 0.078 | 0.337 | 3.041 |
| scan_limit | 0.054 | 0.052 | 0.051 |
| q1_local | 0.132 | 0.516 | 4.418 |
| q6_local | 0.143 | 0.119 | 0.252 |

Reading:

- SF 1 throughput: BCP write ≈ 91k rows/s (copy_lineitem, 16 columns), full
  scan ≈ 514k rows/s, 2-column low-cardinality scan ≈ 2.0M rows/s. These are
  the end-to-end constants the micro-level decode/encode wins have to move.
- Run-to-run spread is ≤ ~5% on multi-second steps (run 1 is typically the
  slowest — cold caches); sub-100 ms steps are noise-dominated and serve
  only as smoke checks, not regression evidence.
- `scan_limit` is flat across SF (~0.05 s) — pure per-query/bind overhead,
  confirming the pool-fix (`9ce479d`) removed the old 2 s floor.
- `q6_local` stays sub-second even at SF 1: the selective filter is pushed
  down, so almost nothing crosses the wire — a control step that measures
  pushdown health rather than materialization.

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

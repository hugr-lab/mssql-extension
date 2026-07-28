# Spec 054 — Materialization baseline & quick wins (phases M + 0)

**Status:** Draft
**Date:** 2026-07-28
**Design:** `docs/proposals/simd-chunk-materialization-design.md` (§7 benchmark plan, §9 related
issues, §10 phasing). This spec covers phases **M (baseline)** and **0 (quick wins)**.
**Process note:** this spec is documentation, not a spec-kit pipeline. Implementation proceeds
natively; this file (plus the design doc) is the single source of truth for scope and
acceptance. Update it in the same PR when reality diverges.

---

## 1. Goal

Build the measurement infrastructure for the batch/SIMD materialization series, capture a
reproducible performance baseline of the **current** implementation before any conversion code
changes, fix issue #177 (which blocks the aggregate-then-COPY benchmarks), then land the
low-risk quick wins and record the first before/after delta.

**Hard ordering rule:** the baseline (D6) must be captured and committed before any change to
conversion code (D8) lands. The #177 fix (D7) is the one exception — it converts a hard error
into a working path, so no baseline exists for it to invalidate; it must land *before* the
TPC-H e2e capture because the benchmark itself hits that error.

## 2. Non-goals

- No representation changes (staging, CONSTANT/DICTIONARY emission, batch kernels) — phases 1+.
- No C++17 migration. Per maintainer decision: C++17 is adopted only if a concrete change is
  justified by readability or performance, not for its own sake. The verified mechanism and
  constraints live in design doc §2.7 for when that day comes. All code in this spec stays
  C++11-compatible.
- No new user-facing settings (counters are debug-only output).
- e2e numbers are *no-regression evidence*, not the optimization signal (spec-044 lesson:
  SQL-Server-in-Docker I/O dominates at scale; microbenchmarks carry the signal).

## 3. Deliverables

### D1. Micro-benchmark harness (`test/cpp/bench_materialize.cpp`, `make bench-materialize`)

Follows the `bench_utf16.cpp` pattern: `-DMSSQL_BENCH_BUILD -O3`, median-of-N timing
(`std::chrono::steady_clock`), correctness assertion per fixture (output must be byte/value
identical to the reference path), manual-run only (not part of `make test` / CI).

Benchmark groups:

1. **String decode** (`utf16 payload → string vector`): current path
   (`Utf16LEDecode`→`std::string`→`AddString`) vs strategy A-lite (`EmptyString` direct
   convert) vs strategy A (shared backing buffer + prefix sum) — B/C variants are added in
   later phases but the fixture matrix must already cover their gating cases.
   Fixture matrix (design §7.1): lengths {0,4,8,16,32,64,256,4096} code units ×
   scripts {ASCII, Cyrillic, CJK, surrogate pairs} × NULL {0,10,50,100}% ×
   cardinality {1,2,10,100,101,512,2048} × {embedded U+0000} × {value ending in a lone high
   surrogate}. The 12-byte `string_t` inline threshold makes {4,8,16} mandatory cells.
2. **Fixed decode** per family (int widths, float, DATETIME2 components, DECIMAL buckets,
   GUID shuffle): current per-value path vs staged scalar loop (reference implementation for
   phase 1 kernels — measured here to size the expected win, thrown away if phase 1 reshapes it).
3. **BCP encode**: current per-cell path vs hoisted-format path (D8/W1-W2) on
   FLAT-unique / FLAT-repetitive / DICTIONARY {1,10,100} / CONSTANT inputs × NULL ratios.
   Dictionary/constant inputs are built with `Vector::Dictionary` / constant vectors directly
   in the bench — this also pins down the representation-detection API use for phase 3.

Metrics per cell: ns/value (median, plus p10/p90 spread), bytes copied per output byte,
allocations per chunk (arena hook or malloc-count interposition where portable).

### D2. TPC-H enablement for bench builds only

`duckdb_extension_load(tpch)` must NOT go into `extension_config.cmake` (it ships to
community builds). Instead: `test/bench/bench_extension_config.cmake` containing the tpch
load, appended to `DUCKDB_EXTENSION_CONFIGS` by a dedicated make target
(e.g. `make bench-build` → configures with
`DUCKDB_EXTENSION_CONFIGS='<repo>/extension_config.cmake;<repo>/test/bench/bench_extension_config.cmake'`).
Verify `CALL dbgen(sf=0.01)` works in the resulting CLI.

### D3. End-to-end TPC-H benchmark (`test/bench/bench_tpch_e2e.sh`)

Same TSV protocol + host-metadata footer as `bench_codec_e2e.sh` (spec 044). Parameters:
`SF ∈ {0.01, 0.1, 1, 10}` (small AND large — both are required outputs), DSN via env.
Steps per SF:

| step | direction | what it exercises |
| --- | --- | --- |
| `dbgen` | — | data generation (excluded from comparison) |
| `copy_lineitem` / `copy_orders` / `copy_part` / `copy_customer` | DuckDB→BCP | numeric/date-heavy, string-heavy, low-cardinality columns (`l_shipmode`, `l_returnflag`) |
| `copy_sum_agg` | DuckDB→BCP | aggregate-then-COPY (HUGEINT → DECIMAL(38,0), regression for #177) |
| `scan_full_lineitem` | TDS→DuckDB | drain scan via `COPY (SELECT * FROM mss...) TO '/dev/null'` |
| `scan_strings` | TDS→DuckDB | part/customer string columns |
| `scan_lowcard` | TDS→DuckDB | `l_shipmode`/`l_returnflag` projection (dictionary win case, phases 2+) |
| `scan_limit` | TDS→DuckDB | `LIMIT 10` cold bind (per-chunk overhead / abandonment) |
| `q1_local` | mixed | TPC-H Q1 over the attached table (scan + aggregate) |

### D4. Counters (debug-only, zero-risk instrumentation of the current paths)

Per-stream / per-writer counters printed at `MSSQL_DEBUG>=2` on close: rows, chunks, values
converted per family, string bytes in/out, PLP values, fallback counts (invalid UTF), and
wall time inside `FillChunk` / `WriteRows` (steady_clock, accumulated). Plain integer
increments guarded by the existing debug-level check; no atomics (streams are
single-threaded), no output when disabled. The full counter list from design §7.4 lands with
the code it instruments in later phases; this spec only wires what current code can count.

### D5. Differential harness (`test/bench/diff_check.sh`)

Runs an identical query list through two extension builds (env: two `duckdb` binaries or two
`--unsigned` load paths) against the same SQL Server; compares full ordered result sets
(ORDER BY PK, `COPY TO` csv + `cmp`, or checksum aggregate). Query list covers: every type
family (reuse `docker/init/init.sql` AllDataTypes), NULL-heavy, empty-string vs NULL, embedded
NUL, PLP/MAX values, 0-row and 1-row results, >2048-row results. Used from phase 0 onward as
a merge gate for every conversion-touching PR in the series.

### D6. Baseline capture → `test/bench/bench_results_simd_baseline.md`

- Micro: full D1 matrix on the reference machine (macOS ARM64 dev box) + one Linux x86_64 run
  (CI runner or the lab container) — record both; medians with run count and spread.
- e2e: D3 at SF 0.01/0.1/1 locally (SF 10 optional if the Docker volume allows; record what
  ran). Environment metadata per the spec-044 footer (host, CPU, RAM, Docker, server image).
- Committed to the repo before any D8 change merges. This file is append-only for the series:
  each later phase adds its column next to baseline.

### D7. Issue #177 fix — HUGEINT/UHUGEINT BCP encode

In `src/codec/integer_codec.cpp` `EncodeToBcp` (both `Vector` and `Value` overloads): replace
the `NotImplementedException` arms with forwarding to
`BCPRowEncoder::EncodeDecimal(value, /*precision*/38, /*scale*/0)`, mirroring the adjacent
UBIGINT arm and matching the existing DDL (`FormatDdlTypeName` → `DECIMAL(38,0)`) and literal
(`SerializeDecimal(…, 38, 0)`) paths.

**Range guard:** `DECIMAL(38,0)` holds |v| ≤ 10^38−1; int128 reaches ≈1.70·10^38 and uint128
≈3.40·10^38. Values with 39 digits must raise a clean client-side conversion error naming the
column and value (InvalidInputException style), not a server-side error mid-BCP-batch.
Constant for the bound: `10^38 − 1` as hugeint (compare via `Hugeint::GreaterThan` on the
absolute value; UHUGEINT compared against the same bound).

Tests: `test/cpp/codec/test_integer_codec.cpp` — round-trip encode for 0, ±1, int64 edges,
10^38−1, −(10^38−1); error for 10^38 and above; UHUGEINT edges. SQL-level regression in
`test/sql/copy/` (SUM over BIGINT → COPY bcp; also CTAS default path). Closes #177.

**As-landed divergence (commit `e12f418`):** the integer-codec arms alone were not enough.
(a) `COPY … CREATE_TABLE true` reads the created table's metadata back, so a HUGEINT source
column arrives as `DECIMAL(38,0)` and dispatches through the **Decimal** family — the same
mantissa-fits-precision guard was added to `codec::decimal::EncodeToBcp` (both overloads),
which also protects any decimal-target BCP from mid-batch server rejections.
(b) `TargetResolver::GenerateColumnMetadata` emitted precision/scale **0/0** for HUGEINT in
COLMETADATA; now 38/0 with max_length 17.
(c) The CTAS BCP sink leaked its connection mid-bulk-load on a row-encode error, hanging the
next DDL on the pool ("Query timeout") — fixed with the issue-#191 COPY pattern
(`AddChunkBCP` catch + `FlushBCP` close-before-release + `~CTASExecutionState` last resort
via a `weak_ptr` pool handle).
(d) `SUM(UBIGINT)` returns HUGEINT (not UHUGEINT) in DuckDB v1.5.5, so UHUGEINT stays
unwired in `FamilyFromLogicalType`; its codec arms are defensive and unit-tested only.
(e) The guard forwards `col.precision/col.scale` (as the UBIGINT arm does) rather than
literal 38/0, keeping the wire byte size in lockstep with COLMETADATA.

### D8. Phase-0 quick wins (after D6 is committed)

Write path:

- **W1** — hoist `ToUnifiedFormat` from per-cell to once per column per chunk: build
  `UnifiedVectorFormat` array in `BCPRowEncoder::EncodeRow`'s caller (`BCPWriter::WriteRows`)
  and thread it through; delete the per-call `GetVectorValue`/`IsVectorNull` reconstruction
  (`integer_codec.cpp:48-55` pattern, all 9 families + `bcp_row_encoder.cpp:34-59`).
- **W2** — resolve the family encoder once per column per chunk (function-pointer or
  pre-switched loop) instead of the 9-way switch per row (`bcp_row_encoder.cpp:111-138`).
- **W3** — single-pass NVARCHAR encode: today `ValidateNVarcharLength` (validate + utf16
  length) precedes `Utf16LEEncodeDirect` (validate again + convert) — collapse to one
  validate + one length + one convert per value, or `convert_utf8_to_utf16le_with_errors`
  where the sized output is already reserved. Length-overflow error text must not change
  (FR-023 wording is tested).
- **W4** — reserve the accumulator per chunk from column estimates; kill the per-string-value
  `resize` in `AppendNVarcharNonPlp` (`string_codec.cpp:50`).

Read path:

- **R1** — string decode without temporaries: replace
  `Utf16LEDecode → std::string → StringVector::AddString` with `utf8_length_from_utf16le` →
  `StringVector::EmptyString(vector, len)` → `convert_valid_utf16le_to_utf8` directly into the
  slot (invalid input falls back to the legacy per-value path unchanged). Applies to
  `string_codec.cpp:154-179`; binary codec (`AddStringOrBlob`) is already single-copy — leave.

After D8: re-run D1 + D3, append the delta column to the baseline file.

## 4. Acceptance criteria

1. `make bench-materialize` and `bench_tpch_e2e.sh` run reproducibly (documented invocation,
   median-of-N, spread reported); D6 baseline file merged **before** the first D8 commit.
2. #177: repro from the issue passes; range-guard errors are clean; codec unit tests + SQL
   regression green; issue closed.
3. Quick wins: differential harness (D5) reports zero result differences on the full query
   list; all existing `make test` / integration suites green; micro delta appended showing
   the win; **no e2e step regresses >3%** (median over ≥3 runs).
4. No changes to shipped defaults, settings, or wire behavior; counters invisible unless
   `MSSQL_DEBUG>=2`.
5. Design doc updated if any finding here contradicts it (append to §Appendix A).

## 5. Task order

```text
T1  D1 harness skeleton + string-decode group (current path only)     [no product code]
T2  D2 bench build config + tpch smoke                                 [no product code]
T3  D3 e2e script (runs with #177 workaround ::DECIMAL(38,0) cast)     [no product code]
T4  D7 #177 fix + tests                                                [small product change]
T5  D4 counters on current paths                                       [debug-only]
T6  D1 remaining groups (fixed decode, bcp encode)                     [no product code]
T7  D6 baseline capture (micro macOS+Linux; e2e SF 0.01/0.1/1[/10])    → commit baseline
T8  D5 differential harness                                            [no product code]
T9  D8 W1+W2 (hoisting)          → diff-check + re-run affected benches
T10 D8 W3+W4 (NVARCHAR single-pass + reserve)                → same
T11 D8 R1 (decode zero-temp)                                  → same
T12 Append phase-0 delta to baseline file; update design doc if needed
```

T1–T3, T5, T6, T8 are pure additive (benches/scripts) and can proceed in parallel; T4 lands
independently; T9–T11 are sequential and each gated by the diff harness.

## 6. Risks

- **Bench noise** (Docker SQL Server, laptop thermals): mitigate with medians over ≥3 runs,
  reported spread, and pinning the comparison to the same machine per file section.
- **W1/W2 touch every family codec** — mechanical but wide; the diff harness plus existing
  codec unit tests are the guard. Do W1 as one mechanical PR, not interleaved with W3/R1.
- **R1 changes string memory ownership** (vector-owned slot instead of copied std::string):
  the invalid-UTF fallback must still produce identical bytes — covered by the D1 correctness
  assertion and D5.
- SF 10 dataset (~10 GB in SQL Server) may not fit the dev Docker volume — the spec accepts
  SF 1 as the largest mandatory point, SF 10 recorded when available.

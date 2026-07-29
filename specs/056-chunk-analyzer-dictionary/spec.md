# Spec 056 — Chunk analyzer: CONSTANT and DICTIONARY emission (phase 2)

**Status:** Draft — analyzer costs measured, string-dictionary case NOT yet justified (§5)
**Date:** 2026-07-29
**Design:** `docs/proposals/simd-chunk-materialization-design.md` §3.5 (chunk analyzer), §5 (memory
ownership), §8 (settings). Phase **2** of the materialization series.
**Depends on:** spec 055 (staging + batch decode). The analyzer consumes `ColumnStagingSet`; it
cannot be built before staging exists.

---

## 1. Goal

Emit non-FLAT vectors from the scan where the chunk's data allows it:

- every value in the chunk equal (or all NULL) → **CONSTANT** vector, one decode;
- at most `mssql_scan_dictionary_max` (default 100) distinct values → **DICTIONARY** vector: only
  the distinct values are decoded into a reusable child, and a selection vector maps the 2048 rows
  onto it.

Both are legal for a table function — the Parquet reader does exactly this
(`dictionary_decoder.cpp:117`), with two constraints we already satisfy: the chunk must be filled
from offset 0 (`FillChunk` always is), and NULL rows point at a trailing invalid child slot.

## 2. The economics changed — read this before designing

Phase 1 (spec 055) made decoding roughly ten times cheaper. The saving a dictionary can offer on
the **decode side** shrank with it, while the cost of *detecting* low cardinality did not move.
Measured (2048-row chunks, macOS ARM64, ns/value):

| detector | len4 string | len16 string | int64 | what it would save |
| --- | --- | --- | --- | --- |
| CONSTANT (compare to first) | 2.0 hit / **0.0 miss** | 1.5 hit / **0.0 miss** | **0.1** | the whole decode |
| DICTIONARY (hash + probe, full value) | 4.8–6.2 | **13.4–20.7** | **0.4** | the whole decode |
| DICTIONARY, prefix-only hash | 3.8–5.8 | **86.8** ⚠ | — | — |
| overflow (cardinality > cap) | 0.2 | 0.8–0.9 | 0.0 | nothing |
| — decode cost for comparison — | 2.5 | 2.1 | 1.8 | |

Three conclusions follow, and they differ per type:

**CONSTANT: always on, every family.** A miss costs **zero** — the detector exits at the second
distinct value. A hit costs 1.5–2 ns/value and saves 2047 decodes. There is no configuration in
which this loses.

**INT (and other fixed-width): dictionary detection is cheap enough to always attempt.** 0.4 ns
against a 1.8 ns decode, and overflow costs 0.0 because the bail-out fires early. Note this
**contradicts design §3.5**, which excludes 1:1 integer types from dictionary consideration; the
measurement supersedes it, and the design doc gets an Appendix-A correction.
Free bonus: a min/max pass costs 0.1–0.2 ns/value, and a narrow range enables direct-mapped
dictionary building with no hashing at all.

**STRINGS: dictionary detection costs 6–10× the decode it would save.** At len16 the detector is
13–21 ns while decoding every value costs 2.1. **A string dictionary can no longer be justified by
decode savings — that argument died with phase 1.** What remains is downstream value, and that is
not measurable in a materialization microbenchmark (§5).

**FLOAT: CONSTANT only.** Low cardinality in a float column is not a realistic expectation, and
dictionary detection would buy nothing. Maintainer decision, 2026-07-29.

## 3. Detection policy

```text
per column, at chunk finalize:
  1. all-NULL                        → CONSTANT + ConstantVector::SetNull
  2. all-equal (raw-byte compare)    → CONSTANT, one decode          [always, every family]
  3. dictionary attempt:
       fixed-width (int, date, …)    → always; min/max first, narrow range → direct-mapped
       strings                       → ONLY behind the metadata gate (§4)
       float                         → never
       any PLP value in the chunk    → never (design §3.2)
  4. otherwise                       → FLAT (spec 055 path)
```

Hash rule: **hash the whole value.** A prefix-only hash measured 86.8 ns/value at len16 — four
times *worse* than the full hash — because the fixture's distinguishing bytes live in the tail, so
every value collided and linear probing degenerated into a full-table walk with a `memcmp` per
step. That is not a fixture artifact: `CUST-000123`, file paths and URLs share prefixes by
construction. If hashing must be cheapened, sample head **and** tail, never the head alone.

Every hash hit is confirmed by full equality before being treated as a duplicate (design §6
invariant, unchanged).

## 4. The metadata gate for string columns

Narrow string columns are overwhelmingly lookup/reference columns — country codes, currency codes,
status flags — and they are exactly where a dictionary pays off downstream. The gate should read
**catalog metadata, not data**: `nvarchar(N)` / `char(N)` with small N is known before a single row
arrives, costs nothing to evaluate, and cannot be fooled by a chunk that happens to look narrow.

Threshold to confirm during implementation (start at N ≤ 8 code units, tune with the e2e gate in
§5). The measured detector cost supports this: at len4 detection is 4.8–6.2 ns versus 13–21 at
len16, so restricting the attempt to narrow columns also makes the attempt itself cheap.

Actual staged length can be used as a secondary refinement, but never as the primary gate — a wide
column that happens to hold short values in one chunk is not a reference column.

## 5. The open question this spec must answer before it ships

For strings, the phase must demonstrate **downstream** benefit, since the decode-side argument is
gone. That means an end-to-end measurement, not a microbenchmark:

- `GROUP BY` on a low-cardinality string column of an attached table,
- a join whose key is such a column,
- a filter (`WHERE status = 'ACTIVE'`) over a large scan,

each run with dictionary emission on and off (debug setting), same session, interleaved A/B per
the spec-054 protocol. If DuckDB's operators do not convert the win into query time, string
dictionaries do not ship and `mssql_scan_dictionary_max` governs fixed-width types only.

Memory is a second, independently valid justification: 2048 × 16-byte `string_t` plus payload
versus ≤100 child values plus a selection vector. Record it, but do not treat it as sufficient on
its own.

## 6. Deliverables

- **D1** Analyzer over `ColumnStagingSet`: all-NULL, all-equal, cardinality probe with cap and
  early bail-out; per-family policy from §3.
- **D2** CONSTANT emission (all families) — one decode into a constant vector.
- **D3** DICTIONARY emission: reusable child per column
  (`DictionaryVector::CreateReusableDictionary`, cap+1 slots so NULL gets the trailing invalid
  slot, Parquet convention), owned `SelectionVector` reused across chunks,
  `Vector::Dictionary(reusable_dict, sel)`.
- **D4** min/max fast path for fixed-width types; direct-mapped dictionary when the range is
  narrow.
- **D5** Metadata gate for string columns (§4).
- **D6** Settings: `mssql_scan_dictionary_max` (default 100, `0` disables all non-FLAT emission —
  the escape hatch), read once at stream init.
- **D7** Counters: `chunks_flat / dictionary / constant`, `dictionary_overflow`, detector time per
  family, cardinality histogram — these are the calibration inputs for the cost policy.
- **D8** e2e gate from §5, recorded in the baseline file.
- **D9** Design-doc corrections: §3.5's "never dictionary for 1:1 ints" is wrong (D4 measurement);
  Appendix A entry for the prefix-hash trap.

## 7. Acceptance criteria

1. `diff_check.sh` 13/13 byte-identical — representation must be invisible at the SQL level.
2. All existing sqllogictests and the integration suite pass unchanged, including
   `test/sql/rowid/` (composite-PK rowid copies PK columns; if a PK column becomes a dictionary
   the copy must still work, or those columns are forced FLAT).
3. No micro regression on high-cardinality columns beyond the measured bail-out cost
   (≤ 1 ns/value): a column that is not low-cardinality must pay almost nothing for being checked.
4. Fixed-width dictionary/constant emission shows an end-to-end win on the low-cardinality scan
   step (`scan_lowcard`) under interleaved A/B.
5. **String dictionaries ship only if §5 shows a downstream win**; otherwise they are documented as
   evaluated-and-rejected, with the numbers, and the gate stays off.
6. `mssql_scan_dictionary_max=0` restores exactly the phase-1 behaviour (FLAT everywhere).

## 8. Risks

- **The write path must not regress.** Spec 054 measured dictionary inputs on the BCP encode path
  at parity with FLAT after W1 (8.3–8.6 vs 8.2–9.1 ns/value) — before W1 they were 15–25% *slower*.
  Emitting dictionaries from the scan means the write path will now actually see them (e.g. a
  scan piped into `COPY TO mssql`). Parity is the floor; exploiting them is spec 057.
- **Downstream flattening.** Operators are known not to force-flatten in the common paths, but a
  plan that does turns the analyzer into pure overhead. The counters (D7) must make this visible,
  and `mssql_scan_dictionary_max=0` is the escape hatch.
- **Hash quality on adversarial data** — see the prefix-hash finding; full-value hashing with
  equality confirmation is the invariant.
- **rowid interaction** — criterion 7.2.

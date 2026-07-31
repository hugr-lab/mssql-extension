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

---

## 6. The downstream question, answered — 2026-07-30

§5 said string dictionaries could only be justified by DOWNSTREAM benefit, since
spec 055 killed the decode-side argument. That benefit has now been measured,
without implementing dictionary emission.

**Method.** The same 20M rows in two local DuckDB tables — one written under
`PRAGMA force_compression='dictionary'`, one `'uncompressed'` — so the operators
above the scan see exactly the vector types this spec would create. Confirmed via
`pragma_storage_info` that the two really differ. Interleaved, order rotated,
three passes, `.timer on`, `threads=4`. Columns: `s100` (100 distinct),
`s10` (10 distinct).

| query | dictionary | flat | delta |
| --- | ---: | ---: | ---: |
| `GROUP BY s100` | 0.016 | 0.057 | **−72%** |
| `GROUP BY s10` | 0.014 | 0.048 | −71% |
| `count(DISTINCT s100)` | 0.004 | 0.032 | −87% |
| `JOIN` on the string | 0.034 | 0.069 | −51% |
| `GROUP BY` on two string keys | 0.034 | 0.065 | −48% |
| equality filter | 0.014 | 0.016 | −12% |
| **`ORDER BY s100 LIMIT 5`** | 0.048 | 0.016 | **+200%** |

**The benefit is real and large — and still not enough.** The GROUP BY saving is
0.041 s over 20M rows = **2.05 ns/row** of wall time. At `threads=4` the CPU
saving is roughly 4× that, so call it ~8 ns/row. This spec's own §2 measured
string dictionary DETECTION at **13–21 ns/value**. Detection therefore costs
more than the downstream benefit, just as it costs more than the decode benefit.

Two corrections, both against the dictionary:

- The measured dictionary is built ONCE for the whole column at write time and
  reused by every scan. This spec would rebuild one per 2048-row chunk, which is
  a strictly worse ratio — the same detection cost amortized over far fewer rows.
- `ORDER BY` is 3× SLOWER on dictionary input. Any emission policy would have to
  predict the consumer, which the scan cannot do.

**Verdict: string dictionaries do not ship.** Not for lack of downstream value —
it is there — but because detecting the dictionary costs more than having it
saves, on both sides of the ledger. They would only pay if the dictionary
arrived for free, and TDS does not carry one.

What survives from this spec: CONSTANT detection (a miss costs 0.0 — the
detector exits at the second distinct value) and fixed-width dictionaries
(0.4 ns detection). Both are small wins; neither justifies a phase of its own.
**Recommendation: fold the CONSTANT/fixed-width part into whatever phase next
touches the scan, and close this spec.**

---

# Reopened for CONSTANT only (2026-07-30)

The verdict above stands for dictionaries but rested on one number that turns
out to be a property of the prototype rather than of the problem: **13-21 ns per
value to detect**. That prototype hashed values generically. When the value fits
in a 64-bit word — INT, BIGINT, and any string of at most 8 bytes — the value
*is* the key: no hashing of bytes, no `memcmp`, no pointer chasing, just open
addressing on a power-of-two table.

Re-measured on that shape, 2048-row chunks (ns per value):

| detector | 1 distinct | 16 | 128 | 1024 | 2048 |
| --- | --- | --- | --- | --- | --- |
| CONSTANT check, int32 | 0.49 | ~0 | ~0 | ~0 | ~0 |
| CONSTANT check, int64 | 0.28 | ~0 | ~0 | ~0 | ~0 |
| dictionary build, int32 | 0.46 | 0.47 | 0.74 | 0.88 | 0.88 |
| dictionary build, int64 | 0.46 | 0.47 | 0.78 | 0.93 | 0.89 |
| dictionary build, 3-char strings | 1.81 | 1.86 | 1.83 | 1.90 | 1.86 |
| dictionary build, 5-char strings | 2.55 | 2.24 | 2.40 | 2.40 | 2.44 |

Against a decode of ~1.8 ns/value for integers and ~2.1 for short ASCII strings.

**Dictionaries stay closed, for the corrected reason.** The scan-side economics
are now fine for integers (0.5-0.9 ns), but the deciding half is downstream and
unchanged: GROUP BY / DISTINCT / JOIN 2-3.5× faster, **`ORDER BY` 3× slower**,
and the scan cannot know which it is feeding. Emitting one is a bet on the
consumer. Revisit only with an end-to-end measurement on both shapes.

**CONSTANT ships.** It is the one case that is not a bet:

- a miss costs nothing — the detector exits at the second distinct value, and
  the mixed-NULL case exits on a counter comparison before looking at any value;
- a hit costs 0.3-0.5 ns per value;
- unlike a dictionary, a constant vector has a fast path in essentially every
  DuckDB operator, so there is no consumer for which it is a pessimisation;
- and on a hit the batch decode collapses from N values to **one** — the scan
  itself gets faster, so the win does not depend on downstream at all.

## Implementation

Hook: `RowStager::FinalizeChunk`, per column, ahead of the kernel switch. Three
cases, ordered so the common one exits first:

1. `st.null_count == row_count` → the column-chunk is entirely NULL.
   `ConstantVector::SetNull(out, true)` and no kernel runs at all. Detected by
   comparing two numbers already in hand.
2. `st.null_count != 0` → mixed NULLs, cannot be constant. One comparison, then
   the normal kernel.
3. `st.null_count == 0` → scan for equality; on success decode row 0 alone.

### The equality scan, per arm

- **Direct** (`ops.direct_write`): the values are already in the output vector,
  written there as they arrived. Compare `stride` bytes at row *i* against row 0
  and, on success, only `out.SetVectorType(VectorType::CONSTANT_VECTOR)` — row
  0's value is already at offset 0, so this copies **nothing**.
- **Fixed**: same comparison against `st.buffer` at `stride`.
- **Var**: `st.lengths[i] == st.lengths[0]` first, then `memcmp` — length alone
  rejects most columns before a byte is read.

### Decoding the single value

On a hit the family's existing per-value entry point — `codec::<family>::
DecodeFromTds(st.ValueAt(0), col, out, 0)` — writes slot 0, and the vector is
then marked constant. That is one call per column per chunk, not a per-value
path, and it avoids teaching every batch kernel a "just row 0" mode. The string
kernel in particular sizes its work from `st.PayloadSize()`, i.e. the whole
column, so calling it with `count = 1` would convert everything and save nothing.

### Invariants this relies on

`MSSQLResultStream::FillChunk` calls `chunk.Reset()` before filling
(`mssql_result_stream.cpp:277`), and `DataChunk::Reset` → `ResetFromCache` sets
`vector_type = FLAT_VECTOR` and resets validity. So a constant vector never
survives into the next chunk, where `BeginChunk` re-takes `direct_dst` from
`FlatVector::GetData` and would otherwise be handed a one-value buffer. A
`D_ASSERT` on the vector type in `BeginChunk` pins that for any future caller at
no release cost.

**Columns feeding `pk_direct_to_rowid` must be excluded.** In that mode the
stream writes PK values straight into the rowid vector rather than into a plain
output column, so the constant path does not apply. The composite-PK paths are
safe as they stand: they copy with `VectorOperations::Copy`, which is
vector-type aware. Resolve the exclusion once, in `Configure`, as a per-column
flag.

### Tests

- a constant chunk for each of Direct / Fixed / Var → CONSTANT emitted, value
  correct;
- an all-NULL chunk → CONSTANT and NULL, with no kernel run;
- a mixed-NULL chunk → stays FLAT;
- a chunk that differs **only in its last row** → stays FLAT (the detector must
  not stop early);
- two consecutive chunks, constant then not, then all-NULL — the reset hazard;
- a scan with a rowid column over a constant PK.

### Acceptance

The wins to show: a constant column-chunk costs less than a flat one (the decode
collapses to one value), and nothing else regresses — the miss path is measured
against the current build on the same interleaved A/B discipline as spec 055.

## As landed

Two things changed against the plan above, both because a measurement said so.

**Only columns that decode take the uniformity scan.** The plan scanned every
arm, including direct-write ones, on the reasoning that a uniform Direct column
costs a comparison and a single store. It does — and it saves nothing, because
there is no decode to collapse: measured **+0.26 ns/value** on a uniform BIGINT
column for a downstream constant alone. Restricted to the staged families, the
scan is a pure win and the miss stays free:

| column | uniform | distinct | delta |
| --- | --- | --- | --- |
| BIGINT (direct) | 2.26 | 2.27 | 0 |
| DECIMAL(18,2) | 4.81 | 5.59 | **-14%** |
| NVARCHAR(16) | 6.72 | 7.88 | **-15%** |

The **all-NULL** case still applies to every column including Direct: it is two
counters compared, never looks at a value, and skips publishing the validity
mask.

An earlier version of the detector used `memcmp` per value and measured +1.7
ns/value on BIGINT — a call into libc where a word compare belongs. Typed loads
brought that to +0.26 before the restriction removed it entirely.

**The `pk_direct_to_rowid` exclusion turned out to be unnecessary.** That mode
writes PK values into a plain output vector and `PopulateRowIdVector` returns
without touching it; the composite paths copy through `VectorOperations::Copy`,
which is vector-type aware. The real hazard was elsewhere: `CountChunkForDebug`
reads every row through `FlatVector::GetData`, which on a constant vector counts
whatever is left in the buffer. Debug-only, and fixed with the change.

## What the tests caught

- **NTEXT decoded as single-byte text.** The constant path publishes row 0
  through the family's per-value entry, and `string::DecodeFromTds` listed
  NCHAR / NVARCHAR / XML as UTF-16 but not NTEXT — which was invisible while
  nothing called it for NTEXT, since the legacy path could not read the type at
  all before #197. Fixed in `string_codec.cpp`.
- **A test fixture that reset nothing.** The unit tests reused loose vectors
  across chunks, a state production never sees because `FillChunk` calls
  `DataChunk::Reset`. The `D_ASSERT` in `BeginChunk` fired on the first run; the
  fixture now holds a real `DataChunk` and resets it, so the reuse path under
  test is the one that ships.

Gates: 17 unit blocks, the full local integration suite, and `diff_check.sh`
byte-identical across 13 queries against a build without the constant path —
including the NULL, empty-value, embedded-U+0000, PLP and BCP write-back cases.

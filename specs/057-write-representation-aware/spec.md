# Spec 057 — Representation-aware BCP encoding (phase 3)

**Status:** Draft — prototype measured (§3); numbers need one re-measurement before they are
binding (§3, note on prototype allocation)
**Date:** 2026-07-29
**Design:** `docs/proposals/simd-chunk-materialization-design.md` §4.2 (representation-aware
encoding), §9 (issue #153). Phase **3** of the materialization series.
**Depends on:** nothing structurally — the write path can exploit CONSTANT/DICTIONARY inputs the
moment they arrive, and they already do arrive today from DuckDB-side producers (Parquet scans,
`VALUES`, joins). Spec 056 makes the MSSQL scan produce them as well, which widens the input set
but is not a prerequisite.

---

## 1. Goal

The shipped encode path (spec 054 W1/W2) resolves DICTIONARY and CONSTANT inputs *transparently*
through `format.sel` — correct, but it re-encodes every row. A constant `nvarchar` column converts
the same string to UTF-16 two thousand times per chunk.

This phase inspects `vec.GetVectorType()` **before** `ToUnifiedFormat` (which erases it — design
fact §2.4) and encodes each distinct value exactly once:

- **CONSTANT** → one encode, then the same byte span appended per row;
- **DICTIONARY** → one encode per *used* child, then the cached span appended per row;
- **FLAT** → unchanged, the shipped hoisted path.

The TDS/BCP wire format still carries every row — the saving is conversion and allocation, not
bytes (design §4.2, non-goal unchanged).

## 2. Why this is the biggest remaining write-side win

Per-row conversion cost is what gets multiplied away. The more expensive the per-value encode, the
larger the factor: strings (UTF-16 conversion per value) gain most, fixed-width integers least.

## 3. Prototype measurements

macOS ARM64, 2048-row single-column chunks, median of 400, ns/value. Wire output verified
byte-identical to the shipped encoder.

| cell | pre-054 per-row | shipped (hoisted) | repr-aware | vs shipped |
| --- | --- | --- | --- | --- |
| nvarchar16_const | 48.8 | 22.7 | **4.5** | **5.0×** |
| nvarchar16_dict1 | 54.6 | 23.2 | **5.2** | **4.5×** |
| nvarchar16_dict10 | 53.7 | 23.2 | **5.7** | **4.1×** |
| nvarchar16_dict100 | 53.0 | 23.1 | **9.5** | **2.4×** |
| bigint_const | 35.3 | 8.9 | **4.4** | 2.0× |
| bigint_dict1 | 39.3 | 9.0 | **4.9** | 1.8× |
| bigint_dict10 | 38.9 | 8.7 | **5.0** | 1.7× |
| bigint_dict100 | 38.6 | 9.0 | **5.8** | 1.6× |
| bigint_dict100_null50 | 37.3 | 6.2 | **5.4** | 1.1× |
| decimal18s6_const | 37.9 | 11.7 | **4.5** | 2.6× |
| decimal18s6_dict100 | 42.1 | 11.7 | **15.2** | **0.77× (worse)** |
| bigint_const_null | 29.2 | 3.6 | **4.3** | **0.84× (worse)** |
| all FLAT cells | — | 8.8 / 23.8 / 12.2 | unchanged | falls through |

**Two regressions, and what they mean.**

`decimal18s6_dict100` (11.7 → 15.2): the prototype allocates the encode cache plus three side
arrays **per chunk**, and builds each child value through `Vector::GetValue` → `duckdb::Value` →
the Value-overload encoder, which for DECIMAL is the documented legacy-divergent (mantissa
rescaling) path. `bigint_dict100` carries the same allocations and still wins, so the allocation
is not the whole story — the per-child Value round-trip is. The production encoder must cache
spans in the writer's arena (reused across chunks, mirroring the read-side staging arena) and
encode children through the *Vector* overload, not through `Value`. **These numbers are re-measured
with that shape before this spec's acceptance criteria bind.**

`bigint_const_null` (3.6 → 4.3): an all-NULL constant already costs almost nothing on the shipped
path (a 1-byte NULL marker per row). Needs an early-out, exactly like the read side's all-NULL
column.

**Reading:** the win tracks per-value conversion cost, as expected — 4–5× for strings, 1.6–2× for
integers, and nothing at all for FLAT input (correctly falls through). Dictionaries with 100
children still win 2.4× on strings, so the cap does not need to be small to pay off.

**Caveat on the dictionary rows:** an arena-backed re-measurement (children encoded through the
Vector overload, caches reused across chunks) fixed `decimal18s6_dict100` — 16.0 → **7.0**, i.e.
1.7× better than the shipped 12.1 — but *regressed* several other dictionary cells versus the
first prototype (`nvarchar16_dict1` 5.1 → 12.6, `bigint_dict1` 4.9 → 6.2). The two prototypes
differ in three ways at once (Value vs Vector child encoding, per-chunk vs arena allocation, NULL
child handling), so the cause is not isolated. **No dictionary number here is binding until a
one-variable-at-a-time A/B is run.** The CONSTANT rows agree across both prototypes and are safe.

## 3a. FLAT strings: bulk conversion + a pre-sized accumulator (3.6×)

Independently of representation, the FLAT string path has a large win of its own — and finding it
took two failed attempts worth recording, because both failures were prototype artefacts that
looked like properties of the approach.

The scheme mirrors the read path (spec 055 D5): gather the vector's UTF-8 values into one
contiguous buffer with a `U+0000` delimiter after each, convert the whole column with **one**
`convert_valid_utf8_to_utf16le` into a buffer of 2× the UTF-8 size (an ASCII byte is the worst
case: 1 byte in → 2 bytes out), then emit the row-major BCP framing. `written == utf8_bytes` means
the column was all ASCII, so each value's UTF-16 length is exactly twice its UTF-8 length and no
scan is needed; otherwise one word-wise sweep over the converted buffer locates the `0x0000`
delimiter units (4 units per 64-bit word).

| cell | shipped (hoisted) | bulk + pre-sized | speedup |
| --- | --- | --- | --- |
| nvarchar16_flat_unique | 24.0 / 23.4 | **6.7 / 6.6** | **3.6×** |
| nvarchar16_flat_card10 | 24.0 / 23.3 | **6.6 / 6.6** | **3.6×** |
| nvarchar16_flat_unique_null50 | 14.0 / 13.9 | **5.0 / 5.1** | 2.8× |

**Two measurement failures and what they taught:**

1. The first prototype scanned every value **byte by byte** for an embedded NUL before gathering,
   and grew the gather buffer with `insert`. Result: 22.7 vs 23.3 — "no win", which was read as
   the approach not transferring from the read path. It was the scan. The embedded-NUL question
   only matters on the delimiter-scan path, where extra zero units are detected for free.
2. With that removed and the buffers pre-sized: 16.8 — a real but modest 1.4×. The remainder was
   `push_back` per framing byte and `insert` per value into the accumulator. Computing every
   output length first (they are all known before a byte is written), sizing the accumulator once
   and filling it through a raw pointer took it to **6.6**.

**The second point generalizes beyond strings and is the most reusable finding here:** per-byte
`push_back` capacity checks in the accumulator cost **~10 ns per value** — more than the entire
UTF-16 conversion. The shipped encoder appends through `push_back`/`resize` in every family, so a
"compute all row sizes → resize once → write through a pointer" restructuring is likely worth more
than any single codec optimization. BCP wire lengths are computable up front for every fixed-width
family by construction, and for strings after the bulk conversion.

## 4. Deliverables

- **D1** Representation dispatch in `BCPRowEncoder::EncodeChunk`: check `GetVectorType()` per
  column per chunk, before `ToUnifiedFormat`.
- **D2** CONSTANT path: encode once (via the Vector overload, row 0), append the span per row;
  all-NULL constant gets an early-out that keeps the shipped NULL-marker cost.
- **D3** DICTIONARY path: per-column span cache in the writer arena — one contiguous byte buffer
  plus `(offset, length)` per child, populated lazily for *used* children only; per row the work
  is a `memcpy` of the cached span.
- **D4** Arena ownership for the cache (reused across chunks; no per-chunk allocation), with the
  same watermark policy as the read-side staging arena.
- **D5** FLAT strings: bulk gather + one conversion + delimiter/arithmetic boundaries (§3a).
  Other FLAT families unchanged — no regression permitted (criterion 5.2).
- **D5b** Blocked encoding + wire-buffer sizing. Two related findings:

  *Blocking.* Processing the column in row blocks instead of whole-column passes keeps the
  gathered UTF-8, the converted UTF-16 and the accumulator slice resident together. Measured on
  top of D5/D5a: 6.6 → **5.9** ns/value (flat), 5.0 → **4.2** (50% NULLs). Block size is a plateau
  from 256 rows upward; 64 is worse (per-block overhead). Default 256.

  *NULL handling from the vector's own mask.* `UnifiedVectorFormat::validity.AllValid()` removes
  the per-row NULL branch entirely on the common path, and NULL rows are skipped before the gather
  — they contribute no payload and no delimiter, only a fixed-size wire marker whose length is
  known. Most of the NULL-heavy gain above comes from this. The same applies on the read path
  (spec 055 D7): an NBC bitmap says which values are absent before anything is decoded.

  *Wire-buffer sizing (design item, needs e2e measurement).* The current flush path copies the
  data three times after encoding: `accumulator_buffer_` → `BuildBulkLoadMultiPacket` (a
  `vector<TdsPacket>`, one 4 KB payload copy per packet) → `packet.Serialize()` (another copy, to
  prepend the 8-byte header) → one `send()` **per packet**. At the default
  `mssql_copy_flush_rows` (100 000) a batch is megabytes, so all three passes run over data long
  since evicted from cache, and a 10 MB batch becomes ~2 560 syscalls plus ~2 560 per-packet
  allocations.

  The shape that follows from the measurements above: encode **directly into wire frames** —
  a contiguous buffer of K packets with the 8-byte headers reserved in place — sized to stay
  L2-resident (order 64–256 KB, i.e. 16–64 packets at the negotiated 4 KB), and flush it with one
  write per buffer rather than per packet. Rows may cross packet boundaries (TDS transport framing
  is independent of token framing), so the only constraint is skipping the header slots. That
  turns three post-encode passes into zero and cuts the syscall count by 16–64×.

  This cannot be measured in the materialization microbenchmark (no socket); it needs an e2e COPY
  step and belongs to the same PR as D5a so the buffer shape is decided once.

- **D5a** Pre-sized accumulator writing, all families: compute every row's wire length first,
  size the accumulator once, fill through a raw pointer. Measured worth ~10 ns/value on the string
  path (§3a) and expected to help every family, since it removes per-byte capacity checks rather
  than per-type work. Sequence it **before** D1–D3 — the representation paths inherit it, and
  measuring them on top of the old append shape would attribute its win to them.
- **D6** Counters: encodes-per-chunk vs rows-per-chunk per column, representation taken
  (flat/const/dict), cache hit rate — the evidence that the path is doing what it claims.
- **D7** Issue [#153](https://github.com/hugr-lab/mssql-extension/issues/153) policy decision:
  should BCP COPY into an existing table allow compatible numeric coercion (BIGINT→INT)? Batch
  encoders make per-column coercion kernels cheap, so the question becomes a policy one — error
  vs range-checked coercion. Decide and document; implementation only if the decision is
  "coerce".

## 5. Acceptance criteria

1. Wire output byte-identical to the shipped encoder for every representation — verified in the
   codec unit tests and by `diff_check.sh` (which already includes a BCP write-back case).
2. **No FLAT regression** (≤ 3%): FLAT is the common case and must not pay for the dispatch.
3. CONSTANT and DICTIONARY cells beat the shipped path on the re-measured (arena-backed) prototype
   shape; the two regressions in §3 are resolved, not shipped.
4. `mssql_copy_*` behaviour, settings and defaults unchanged.
5. e2e: a COPY whose source is a low-cardinality DuckDB-side column (e.g. a Parquet scan piped to
   `COPY TO mssql`) improves under interleaved A/B; no other COPY step regresses > 3%.

## 6. Risks

- **Representation must be read before `ToUnifiedFormat`** — the format erases it. A refactor that
  moves the format construction earlier silently disables this whole phase; the counters (D6) are
  the tripwire.
- **`GetValue`-based child encoding is a trap** — it allocates a `duckdb::Value` per child and, for
  DECIMAL, routes through the legacy-divergent overload. Encode children through the Vector
  overload with a pre-built format over the child vector.
- **FSST vectors** exist in v1.5.5 and are out of scope; treat any unexpected representation via
  `Flatten()` fallback rather than a new arm (design §11.5).
- **Cache invalidation across chunks**: the span cache is keyed by child index, which is only
  meaningful for the current chunk's dictionary. Reset per chunk (keep capacity, drop contents) —
  a stale span from the previous chunk would silently corrupt the wire stream.

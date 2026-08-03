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

  **The shape, and why it is not simply "reserve more".** The write path must obey the same rule as
  the read path — one batch call per column, no per-value dispatch — but the BCP wire is
  **row-major**: a row carries all its columns consecutively. Column-wise encoding into a row-major
  buffer is therefore two passes over the chunk, not one:

  1. **Lengths.** Per column, produce each row's wire length. Fixed-width families know it without
     looking at the data (`1 + width`, or the NULL form), so this pass is arithmetic on the
     validity mask alone. Strings and binaries get theirs from the bulk conversion they already
     have to do — the UTF-16 length is a by-product, not an extra pass.
  2. **Row offsets.** Sum the columns' lengths per row, prefix-sum into `row_offset[]`, resize the
     accumulator **once**. This is where the ~10 ns/value goes: the current path discovers the size
     one `push_back` at a time.
  3. **Scatter.** Per column, one loop over rows writing through `dst + cursor[r]` and advancing
     `cursor[r]` by that value's length. `cursor[]` is a 2048-entry array touched sequentially;
     the write is strided, but the loop carries no branch on type, no capacity check and no
     indirect call — the family kernel is resolved once per column, exactly like `ColumnOps` on
     the read side (spec 055 D6a).

  Two consequences worth stating up front, because they constrain the kernels:

  - A column's kernel never sees the row token or its neighbours; it is handed `dst`, `cursor[]`,
    the vector and its format, and owns only its own bytes. That is what makes CONSTANT and
    DICTIONARY (D1–D3) a change inside one kernel rather than a change to the row loop.
  - `cursor[]` must be updated by every column including the ones that write nothing (NULL rows
    still occupy their fixed marker), so length and scatter must agree exactly. The framing test
    that pins the read walk against `RowReader` (#217) has no counterpart here — the equivalent
    gate is criterion 5.1, byte-identical wire output against the shipped encoder, which must run
    over every family and every representation before this lands.
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

---

# Reconnaissance, 2026-07-30 — the premise does not survive measurement

This spec was scoped around making the BCP encoder cheaper: representation-aware
encoding, dictionary and constant vectors handled without materialising, a
pre-sized accumulator. Before building any of it, the write path was decomposed
against a live server. **The encoder is 5% of the write.**

500k rows × 4 columns (BIGINT, BIGINT, DECIMAL(18,2), NVARCHAR(40)) = 2M values,
local SQL Server 2022 in Docker, heap target, default `mssql_copy_flush_rows`:

| phase | total | ns/value | share of wall |
| --- | --- | --- | --- |
| encode (`BCPRowEncoder::EncodeChunk`) | 43.1 ms | 21.6 | **5%** |
| packet build + send | 61.1 ms | 30.5 | 7% |
| **waiting for the server to confirm the batch** | **705.0 ms** | **352.5** | **86%** |
| whole statement | 823 ms | 411 | 100% |

The client and the server work strictly in turn: accumulate a batch (~98k rows),
send it, then block until the server has inserted it. During those 705 ms the
client is idle; during the encode and send the server is.

## What that means for this spec

**Representation-aware encoding targets ~5% of write wall time**, and only the
part of it that is not already hidden. Dictionary and constant inputs would make
that 5% smaller; nothing else changes. The D5b argument in this spec — that the
copy chain and one `send` per packet dominate — is refuted: build + send is 7%,
and the system time for the whole statement is under 1%.

**The leverage is overlap, not throughput.** Encoding batch N+1 while the server
digests batch N hides essentially all client work behind the 86%. Upper bound on
the win: the 12% the client currently spends serialised ahead of the wait, so
call it ~14% of wall including the flush handshake. That is 2-3× what perfecting
the encoder could yield, and it does not touch the encoder at all.

Batch size is the one knob that already moves this, and it is nearly exhausted:
25k rows → 0.93 s, 100k (default) → 0.78 s, 500k (single batch) → 0.75 s.

## Conditions on the above

The target is a heap with no indexes and TABLOCK available — the server's
best case, and it still takes 86%. A table with indexes or a busy server moves
further in the same direction. A slow network would raise the send share; on
loopback it is 7%.

## Recommendation

Re-scope 057 from "make the encoder faster" to "stop waiting", and prove the
bound before building: a prototype that double-buffers the accumulator and
sends batch N while encoding N+1, measured on the same fixture. If it does not
show ~10%+, the write path is finished as far as the client is concerned and
this spec closes.

## Measurement notes, so the numbers are reproducible

- **`MSSQL_DEBUG=1` inflates the client's own CPU roughly 4×** on this path
  (0.047 s → 0.194 s for the same statement). Phase shares are taken from the
  instrumented run; absolute client cost must come from an uninstrumented one.
- **`.timer on` reports `user` across all DuckDB threads**, and it is not stable
  for this statement — three identical runs gave 0.19, 0.04 and 0.85 s while
  wall stayed within 5%. Use it for wall, not for attribution.
- The read path's counters were moved to `MSSQL_DEBUG=1` in this branch for the
  same reason: at level 2 the parser logs every token from inside the timed
  parse, which inflated `parse` from 22 to 1133 ns/row and collapsed the socket
  wait from 128 to 12 — the numbers did not get noisy, they inverted.

---

# Re-scope, 2026-07-30 — the write is bound by what we ASK the server to do

Continued reconnaissance found three defects and one opportunity, all on the
server side of the wire, and together they are worth an order of magnitude —
against the 5% this spec was scoped around. Same fixture throughout: 500k rows
× 4 columns (BIGINT, BIGINT, DECIMAL(18,2), NVARCHAR(40)), local SQL Server
2022, `.timer on` wall time, medians of three.

| what is being done | wall | vs achievable |
| --- | --- | --- |
| **today's default, `COPY ... CREATE_TABLE true`** | **1.68 s** | **4.5×** |
| sized column, TABLOCK missing (existing table) | 0.78 s | 2.1× |
| NVARCHAR(MAX) target, TABLOCK on | 1.04 s | 2.8× |
| sized column + TABLOCK | **0.37 s** | 1× |
| four concurrent sessions, sized + TABLOCK | ~0.13 s | 0.35× |

## 1. TABLOCK is not applied on the CREATE_TABLE path — a defect

The documented rule is "auto-enabled for new tables when not explicitly set".
It does not reach the config: `BCPCopyBind: create_table=1 ... tablock=0`.
Setting it by hand takes the same load from 1.70 s to 1.06 s. Free to fix.

Worth extending to an EMPTY existing table — the situation is identical and one
`SELECT TOP 1` before the load settles it. Not to a non-empty one: TABLOCK
blocks concurrent access, which is why it is not simply the default.

**The win is not minimal logging.** Recovery model makes no difference here:
FULL 0.783 / SIMPLE 0.785 without TABLOCK, FULL 0.384 / SIMPLE 0.369 with it.
It is the bulk path — table-level locking removes the per-page latching and
allows bulk allocation.

## 2. Every string column is created NVARCHAR(MAX) — 2.7×

`target_resolver.cpp` maps `LogicalTypeId::VARCHAR` to `nvarchar(max)`, and so
does `codec::string::FormatDdlTypeName`. Measured against the same data in a
sized column: 1.04 s vs 0.37 s.

**The shortcut does not exist — measured.** The obvious cheap fix is to keep
creating MAX columns but declare a sized type in the BCP COLMETADATA, so values
travel with a 2-byte length instead of PLP. SQL Server rejects it outright:

    Invalid column type from bcp client for colid 4

The client's declared type has to be compatible with the target column, and
`NVARCHAR(4000)` into `NVARCHAR(MAX)` is not. (The reverse *is* allowed and we
already rely on it — XML is declared as `NVARCHAR(MAX)` and the server converts.)
So wire framing follows the column type, and the only lever is the column type.

**And the penalty is not the storage engine.** A server-side
`INSERT ... WITH (TABLOCK) SELECT` of the same 500k rows takes 0.217 s into
`NVARCHAR(40)` and 0.207 s into `NVARCHAR(MAX)` — identical. The 2.7× lives
entirely in the bulk-load path's handling of a PLP column.

**Design (maintainer's, 2026-07-30): custom types carrying the size.**

```
loader.RegisterType("MSSQL_VARCHAR",  LogicalType::VARCHAR, BindVarchar);
loader.RegisterType("MSSQL_NVARCHAR", LogicalType::VARCHAR, BindNVarchar);
```

The bind functions put the flavour (varchar / nvarchar) and the length into the
type's modifiers, so a user writes `col::MSSQL_VARCHAR(20)` in a CTAS, a COPY or
a `CREATE TABLE`, and the cast is physically free — the base type is VARCHAR and
the alias travels on the `LogicalType`. CTAS/COPY and DDL then emit
`VARCHAR(20)` / `NVARCHAR(20)` instead of MAX.

Decisions still open:

- **Reads**: should a scan surface `MSSQL_NVARCHAR(20)` instead of `VARCHAR`?
  It would preserve sizes across table-to-table copies, at the cost of a
  non-standard type in every result schema. Probably a setting, default off.
- **No modifier**: `MSSQL_VARCHAR` bare should stay MAX, as today.
- **Overflow: truncate, do not error** (maintainer's call). Two traps:
  Three regimes, and the limit means something different in each. Measured on
  SQL Server 2022, collation `SQL_Latin1_General_CP1_CI_AS`:

  | target | the limit counts | measured |
  | --- | --- | --- |
  | `NVARCHAR(20)` | 20 **two-byte units** | 20 BMP chars fit; **10 emoji fit exactly** (10 surrogate pairs = 20 units); 11 emoji fail with error 2628 |
  | `VARCHAR(20)`, legacy SBCS collation | 20 **bytes of the code page** | 20 × `é` fit (CP1252 has it); 10 × `字` stored as `??????????` — the server replaces what the code page cannot represent |
  | `VARCHAR(20)`, `_UTF8` collation (2019+) | 20 **bytes of UTF-8** | 1-4 bytes per character |

  So "NVARCHAR(20) holds 20 characters" is true for BMP text and wrong for
  anything supplementary — a character outside the BMP costs two units. The
  truncation must therefore:

  - **cut on character boundaries**, never mid-UTF-8-sequence, and never between
    the halves of a surrogate pair — that produces an unpaired surrogate, the
    same class of garbage spec 055 spent a release fixing on the read side. A
    pair goes in whole or not at all;
  - **be driven by the target column's collation, not only its declared length.**
    The collation id is already parsed into `ColumnMetadata::collation`.

  **A consequence that gates `MSSQL_VARCHAR`.** Today we never create a
  single-byte column — everything becomes NVARCHAR — so the read path's
  code-page handling is never exercised. It is broken: a `VARCHAR(20)` holding
  20 × `é` (CP1252 `0xE9`) reads back as 20 replacement characters, because the
  scan hands those bytes to DuckDB labelled UTF-8. Shipping `MSSQL_VARCHAR`
  without fixing that (spec 026 territory, `BuildColumnExpression` in
  `table_scan.cpp`) would turn a latent bug into a common one. Either fix it
  first, or restrict the type to ASCII and say so.

  Truncation is data loss either way. Count it and report the count when the
  COPY finishes; losing values silently is a bad trade even when it is chosen
  deliberately.

## 3. Parallel sessions scale — measured, not assumed

Four concurrent `COPY` sessions into the same heap, each with TABLOCK, loaded
1M rows in 0.381 s against 1.052 s for one session doing the same work — **2.8×**,
with four process startups included in the parallel figure. SQL Server grants
concurrent bulk-update locks, so the sessions do not serialise.

Proposed surface: `COPY ... TO ... WITH (FORMAT bcp, PARALLEL 4)`, N pooled
connections, one `BCPWriter` and accumulator each.

Constraints that decide the design:

- **Heap only.** With a rowstore clustered index SQL Server serialises the load;
  detect and fall back to one session.
- **TABLOCK on every session**, or they block each other instead of sharing.
- **Autocommit only.** Inside an explicit DuckDB transaction the writes must
  share one SQL Server transaction, and N connections cannot. Fall back to one.
- A failure in one session has to abort the rest.

## 4. Client-side work stays, and gets hidden rather than removed

Framing and encoding do not go away. With the server side fixed they become a
larger share of a much smaller total, which is the argument for overlapping
them: prepare batch N+1 while the server digests N. Bounded at ~14% of today's
wall (§ previous section), and worth re-measuring once 1-3 land, because the
share changes.

## Revised order of work

1. **The TABLOCK defect** — free, 1.6× on the default path.
2. **Custom sized types** — 2.7×, and it is the only item that also improves
   reads and storage.
3. **PARALLEL N** — 2.8× on top, with the fallbacks above.
4. **Overlap encode with send/wait** — the original spec's territory, re-measured
   after the above.

The representation-aware encoding this spec was named for (dictionary and
constant inputs encoded without materialising) targets the 5% — see
"Decision, 2026-07-30" at the end of this document, which overrides the ordering
above: it and streaming are both mandatory and land first, not last.

## The client-side options, ranked by what they actually return

| approach | effect | basis |
| --- | --- | --- |
| N connections in parallel | **2.8×** | measured: 1M rows, 1 session 1.052 s vs 4 sessions 0.381 s |
| sized columns instead of MAX | **2.7×** | measured: 1.04 s vs 0.37 s |
| fix the TABLOCK defect | 1.6-2× | measured: 1.70 s vs 1.06 s (new table), 0.78 vs 0.37 (existing) |
| stream packets as they fill, inside a batch | ~12% | derived: encode 43 ms + send 61 ms of an 823 ms wall, all of it currently ahead of the server's 705 ms |
| parallelise only the encoding | ≤4% | encode is 5% of wall |
| simply shrink the batch | **−20%** | measured: 25k rows/batch 0.93 s vs 100k 0.78 s |

**A smaller buffer on its own is a pessimisation** — more batches means more
round trips, and each one is a full stop. It only pays as part of the next item.

**Streaming inside a batch is the right form of that idea.** Today `WriteRows`
appends to a 4 MB accumulator and nothing goes out until the flush, so the
server first learns of the data when the client has finished encoding all of it:
the timeline is [encode 43][send 61][server 705] and the first two are pure
latency. BULK LOAD is a stream — the server inserts as packets arrive — so the
client should send each packet as it fills and keep encoding. The client's work
then hides under the server's, which is where the ~12% comes from. It needs no
second connection and no DDL change, which makes it the cheapest of the four.

**Parallel encoding alone is not worth doing.** Encoding is 5% of wall; four
threads on it buy 4% at best, and the same four threads with their own
connections and accumulators buy 2.8×. The parallelism belongs at the connection
level, not inside the encoder.

## Streaming inside a batch — prototyped and measured

Built behind an env var and measured in one binary. `WriteRows` sends whole
packets out of the accumulator as soon as eight are available, keeping one back
so the message never ends early, and the flush sends the remainder plus DONE
with EOM as before.

| rows | accumulate whole batch | stream as it fills | delta |
| --- | --- | --- | --- |
| 500k × 4 cols | 0.387 s | 0.348 s | **−10%** |
| 2M × 4 cols | 1.558 s | 1.396 s | **−10%** |

Correct, not just fast: `SUM(id)`, `SUM(v)`, `SUM(amt)` and `SUM(hash(s))` all
match the source exactly after a streamed load.

**The one protocol trap.** `BuildBulkLoadMultiPacket` marks the last packet of
whatever buffer it is given as end-of-message, so streaming a partial buffer
through it would terminate the batch early and the server would insert a
fraction of the rows. The prototype uses a separate sender that never sets EOM;
only the final flush does.

**What a production version needs beyond the prototype:**

- **No `erase` from the front of the accumulator.** The prototype memmoves the
  remainder of a 4 MB buffer on every send. The read path solved exactly this in
  spec 055 D0b with a cursor instead of erasing — do the same here.
- Interrupt checks between packets, so a cancelled COPY stops mid-batch.
- A decision on when to start streaming. Eight packets was picked to be
  obviously past the metadata; the real trigger should be "one packet is full".
- Interaction with `FlushBatch`/`Finalize` re-verified under the batch-size
  setting, and with the mutex when the sink runs on several threads.

## External validation: reproducing the "13× slower than FastTransfer" gap

<https://blog.arpe.io/import-parquet-into-sqlserver-duckdb-vs-fasttransfer> loads
38M rows × 44 columns of Parquet into SQL Server: our extension 553 s,
FastTransfer 42 s, and 16 GB vs 283 MB on disk. Same protocol on both sides —
FastTransfer's `msbulk` is .NET SqlBulkCopy, i.e. TDS BULK LOAD — and the same
reader, DuckDB. The article's own conclusion: *"The reading side is the same.
The gap is entirely in how SQL Server rows get written."*

**Reproduced locally**, on a table shaped like theirs (44 columns, ~10 of them
short string codes, 2M rows = 88M values; their screenshot of the generated
schema shows `nvarchar(max)` on every string column):

| step | time | ns/value | vs today |
| --- | --- | --- | --- |
| **today: `CREATE_TABLE true` → nvarchar(max), heap** | **59.29 s** | 674 | 1× |
| sized string columns | 14.42 s | 164 | 4.1× |
| + TABLOCK | 11.37 s | 129 | 5.2× |
| + 2 sessions | 6.85 s | 78 | 8.7× |
| **+ 4 sessions** | **4.64 s** | **53** | **12.8×** |
| + 8 sessions | 5.09 s | 58 | 11.7× |

**12.8× against their 13×.** The whole gap is our defaults, on the same wire.
Their run already passed `TABLOCK true` explicitly, so for them it decomposed
into the schema (~3-4×) and `--degree -2`, which their documentation defines as
*cores ÷ 2* — the default, roughly 8 threads on a normal machine — against our
single connection.

Two things that only measurement shows:

- **Parallelism saturates.** Eight sessions are slower than four here (5.09 vs
  4.64). `PARALLEL N` needs a sane default and a ceiling, not "half the cores".
- **A columnstore target is slower to LOAD, and that is fine.** On the narrow
  fixture: heap 3.19 s vs clustered columnstore 7.37 s, but 204 MB vs 30 MB on
  disk. Their 56× storage advantage comes from the columnstore, not their speed
  — and `nvarchar(max)` is what blocks it, which is why the two findings are the
  same finding.
- **`flush_rows` default 100000 sits 2400 rows below SQL Server's 102400-row
  threshold** for writing compressed rowgroups directly instead of through the
  delta store. Crossing it is worth 12% on a columnstore target (8.33 → 7.37 s).

## Streaming does not stack with parallelism

Measured on the same wide fixture:

| | 1 session | 4 sessions |
| --- | --- | --- |
| accumulate the whole batch | 11.9 s | 4.70 s |
| stream packets as they fill | **9.5 s (−20%)** | 4.56 s (noise) |

They are the same overlap. Four sessions already hide the client's work behind
each other's server waits, so there is nothing left for streaming to hide.

**This decides the plan's shape.** `PARALLEL N` is the lever; streaming is the
fallback for where parallelism cannot go — inside an explicit transaction (N
connections cannot share one SQL Server transaction), against a target that
forbids concurrent bulk load, or when only one connection is available. It is
also strictly cheaper to build: no connection management, no failure fan-out.

## Storage options on the target — measured, 2026-07-30

Asked because the article's 56× storage advantage came from the target's
definition, not from its client. Same 44-column fixture as above, 500k rows,
single session, TABLOCK on, two passes (they agreed to within 0.1 s):

| target | load | size | vs plain heap |
| --- | --- | --- | --- |
| heap, no compression | **2.4 s** | 172.1 MB | 1× |
| heap, `DATA_COMPRESSION = ROW` | 2.8 s (+17%) | 102.3 MB | −41% |
| heap, `DATA_COMPRESSION = PAGE` | 3.9 s (+63%) | **84.6 MB** | −51% |
| clustered index, no compression | 3.7 s | 176.6 MB | +3% |
| clustered index, PAGE + `OPTIMIZE_FOR_SEQUENTIAL_KEY` | 5.1 s | 85.6 MB | −50% |

**Compression is applied during the load, not after it — but only because we
send TABLOCK.** A bulk insert into a PAGE-compressed heap without a table lock
stores rows uncompressed until someone rebuilds. So the TABLOCK defect in § 1 is
not only a 1.6× speed bug: on a compressed target it silently costs the user the
compression they asked for. Both fixes are the same fix.

**`OPTIMIZE_FOR_SEQUENTIAL_KEY` does nothing for us.** Isolated from compression
and measured where it could only help — four concurrent sessions into a clustered
index on a sequential key, TABLOCK off so that page latches are actually
contended:

| | pass 1 | pass 2 |
| --- | --- | --- |
| OFF | 1.45 s | 2.02 s |
| ON | 1.46 s | 1.63 s |

Noise in both directions. The reason is structural, not a fixture artefact: the
four workers write disjoint ascending ranges, so they land on different B-tree
pages and there is no hot last page for the option to relieve. It is an OLTP knob
for many sessions inserting the newest key. Do not set it, and do not recommend
it in the COPY documentation.

The actionable part of this section is the choice we hand the user, not a
default we impose: PAGE halves storage for two thirds more load time, ROW gives
41% for 17%. The extension's job is (a) to keep TABLOCK on so that either
actually takes effect, and (b) not to create the target as `nvarchar(max)`, which
is what blocks columnstore — the far bigger storage lever (§ "External
validation").

## Drive-by: issue #85, partitioned tables were unreadable

Found while looking at `WITH (DATA_COMPRESSION = ...)`, because the reproduction
needs a partitioned clustered index. `SELECT * FROM d.dbo.log` on a partitioned
table failed with `Catalog Error: Column with name log_ts already exists!`.

Three metadata queries in `mssql_metadata_cache.cpp` joined `sys.partitions`
directly to pick up `p.rows`. That view holds **one row per partition**, so on a
four-partition table every object and every column came back four times. A plain
clustered index reads fine; only partitioning triggers it. Fixed by joining a
pre-aggregated subquery (`SUM([rows]) ... GROUP BY object_id`).

The same join also caused a silent second bug: `approx_rows` took whichever
partition the join happened to surface — typically an empty one — so the planner
saw a partitioned table as nearly empty and its cardinality estimates, join
orders and the statistics provider (which already did `SUM`) disagreed. Both are
one fix.

Regression test `test/sql/catalog/partitioned_table.test`: four partitions with
rows in two of them, asserting the column list, that the table appears once, the
rows themselves, and `estimated_size = 3` — a value no single partition holds, so
the test cannot pass by picking one. Verified to fail with the exact issue-#85
error when the fix is reverted.

## Decision, 2026-07-30 — representation-aware encoding and streaming are both mandatory

Ruled by the user, overriding the measurement-derived ordering in "Revised order
of work". Recording it with what the measurements do and do not support, so the
plan is honest about which parts are backed by numbers:

- **Representation-aware encoding is mandatory, not optional.** The measurement
  that ranked it last stands — the encoder is ~5% of wall on a correctly defined
  target, and 4.0-5.0× on the encoder is a few percent end-to-end. What that
  measurement does not capture is that it is the only item here that is *ours*:
  the TABLOCK fix, sized types and `PARALLEL N` all buy their multiples by
  changing what we ask the server to do, and every one of them can be taken away
  by a user's DDL or by a target that forbids concurrent load. The encoder's win
  is unconditional and compounds with all of them, and it is what keeps the write
  path on the same rule as the read path: no per-value conversion, one batch call
  per column.
- **Streaming lands immediately, not as a fallback.** It stays true that it does
  not stack with parallelism (−20% at one session, noise at four), so its value
  is in the configurations parallelism cannot reach — an explicit transaction, a
  target that refuses concurrent bulk load, a single available connection. Those
  are not edge cases for a database extension, and it is the cheapest item to
  build: no connection management, no failure fan-out.

Both therefore move ahead of `PARALLEL N` in build order, with the TABLOCK defect
and sized types still first because they are free and large. The production
requirements for streaming listed above stay binding — in particular the cursor
instead of `erase` from the front of the accumulator, and never letting a partial
buffer set EOM.

## Open design items raised 2026-07-30 (user), with what is measured and what is not

### 1. Where the column length can come from — Parquet is not a source

Checked, and the usual framing ("COPY loses the length from Parquet") is wrong in
a way that matters for the design:

```
CREATE TABLE t (a VARCHAR(20), b VARCHAR);  -->  duckdb_columns(): a VARCHAR, b VARCHAR
```

DuckDB discards the string length modifier at `CREATE TABLE`, before any file is
involved; `VARCHAR(20)` and `VARCHAR` are the same type. Parquet has nowhere to
carry it either — a string column is `BYTE_ARRAY` / `UTF8` with no length.
`DECIMAL(9,2)` by contrast survives both (DuckDB keeps precision/scale, Parquet
stores `DecimalType`), which is why numeric columns are created correctly today
and string columns are not.

So there is no plumbing fix. A sized target column can only come from:

1. **an existing target table** — already works, and is why "insert into a
   properly created table" measured 4.1× faster than `CREATE_TABLE true`;
2. **our own type with a modifier** — `MSSQL_NVARCHAR(50)`, applied by the user
   as a cast in the SELECT feeding COPY. `RegisterType` with type modifiers
   supports this;
3. **an explicit COPY option** — a per-column type map, or a single default
   maximum length for string columns.

Data-derived sizing (scan the first batch, take the longest value) is rejected:
a later batch that exceeds the guess fails the whole load, and the failure comes
from data the user never saw.

### 2. `WITH (...)` options on the created table

Follows directly from the storage measurements above: `DATA_COMPRESSION = PAGE |
ROW` is a real user-facing choice (−51% / −41% size), and it only takes effect
during the load because we send TABLOCK. Exposing it as a COPY option next to the
type map keeps the two things that must agree in one place.
`OPTIMIZE_FOR_SEQUENTIAL_KEY` is deliberately **not** exposed — measured neutral,
see above.

### 3. Truncating an existing target as a COPY option

Wanted, and cheap. Must be explicit and separately named (not folded into an
existing flag) because it destroys data the user did not name in the statement;
and it should run in the same connection as the load so an aborted COPY cannot
leave the table empty *and* unloaded.

### 4. Choosing the degree of parallelism

Proposed inputs: the COPY parameters at create time, and the target's metadata
when inserting into an existing table — in particular "clustered index → single
stream".

The clustered-index rule is **not supported by measurement**: four concurrent
sessions loading into a clustered index on a sequential key completed correctly
and at the same speed as the OFSK variant (400k rows, § above). The user's own
note ("хотя это ни на что не влияет") matches what the numbers show.

What does constrain the degree, and should drive it instead:

- **An explicit transaction** — N connections cannot share one SQL Server
  transaction, so the degree is forced to 1. This is the case streaming exists
  for.
- **TABLOCK against a clustered index** — concurrent bulk load into a heap is
  fine (BU locks are mutually compatible), but the clustered-index case under
  TABLOCK needs measuring before a default is chosen; the parallel run above
  deliberately set `mssql_copy_tablock = false`.
- **Saturation** — eight sessions were slower than four on the wide fixture
  (5.09 vs 4.64 s). The default needs a ceiling, not "half the cores".

## Sizing policy — where the buffer size comes from (user, 2026-07-31)

Rejected: a measurement pass over the chunk to discover how many bytes it will
occupy. The objection is right, and it splits into two questions that the
two-pass sketch in D5a had conflated.

### The buffer size needs no pass at all

An upper bound is enough to allocate, and the bound is already in hand:
`BCPColumnMetadata::max_length` per column gives a per-row ceiling
(`1|2|8 + max_length`), so the whole chunk is one allocation computed from
metadata. Nothing looks at the data.

Resolution order for a column's bound, once per COPY at bind time:

1. **The target table** — already known for every COPY into an existing table,
   which is also the fast path (§ "External validation": 4.1× over
   `CREATE_TABLE true`).
2. **The source**, when the source is an MSSQL table — `max_length` from that
   catalog, including a different attached database.
3. **An explicit cast by the user** to an MSSQL type carrying a modifier
   (`MSSQL_NVARCHAR(50)`) — the custom-types item, which this makes load-bearing
   rather than cosmetic.
4. **A setting / COPY option** — a default string length, and per-column
   overrides in the COPY statement.

**Parquet is not a source of string lengths, measured.** A Parquet string column
is `BYTE_ARRAY` with the `UTF8` logical type and carries no length, and DuckDB
drops the `VARCHAR(n)` modifier at `CREATE TABLE` before any file is involved —
`VARCHAR(20)` and `VARCHAR` are the same type in DuckDB. `DECIMAL(9,2)` survives
both (DuckDB keeps precision/scale, Parquet stores `DecimalType`), which is why
numeric columns are already created correctly and string columns are not. So
step 2 covers numerics from Parquet and nothing else; strings fall to 3 or 4.

`MAX`/PLP columns have no bound by definition. They keep a grow path, and that
is one more reason the extension must stop *creating* `nvarchar(max)` by default.

### Row offsets still depend on the data — so the shape changes instead

A packed wire means `nvarchar(50)` holding `'ab'` writes 2 bytes, not 50, so the
start of row r+1 is not derivable from bounds. That does not require a second
pass over the data; it requires not writing straight into the wire buffer:

- **conversion stays one vectorized call per column**, into staging — the mirror
  of the read path;
- **lengths fall out of that conversion** as a by-product (the UTF-16 converter
  already returns what it wrote), so they are never computed separately;
- **one sequential assembly pass** memcpys the staged spans into the wire in row
  order. No dispatch, no capacity check, no conversion — a gather.

Over the data that is: convert once, copy once. The shipped path also copies
once, but through per-byte `push_back` with a capacity check and an indirect
call per value, which is where the ~10 ns/value of §3a lives.

**All-fixed-width chunks skip staging entirely.** With no variable-length column
in the row, the row stride is a constant computed from metadata, so each column
scatters directly into the wire at `base + r*stride + col_offset` — no staging,
no assembly, zero copies. Common for numeric tables, and the case where the
current per-value path is already cheapest in absolute terms (bigint 8.2
ns/value) and therefore hardest to beat by any shape that adds a copy.

## Columnar scatter measured — and it subsumes representation-awareness for fixed-width families

Prototype `EncodeChunkColumnarFixed` in `test/cpp/bench_materialize.cpp`, integer
family, macOS ARM64, 2048-row chunks, median of 400. Wire output byte-identical
to the shipped encoder on every cell (PASS). Sizing reads **metadata and the
validity mask only** — never a value — exactly as the sizing policy above
requires.

| cell | shipped | repr-aware | columnar | vs shipped |
| --- | --- | --- | --- | --- |
| bigint_flat_unique | 8.2 | 8.0 | **2.1** | **3.9×** |
| bigint_flat_card10 | 9.2 | 8.7 | **1.8** | **5.1×** |
| bigint_const | 9.2 | 3.9 | **2.1** | **4.4×** |
| bigint_dict1 | 8.5 | 4.3 | **1.8** | **4.7×** |
| bigint_dict10 | 8.8 | 4.9 | **1.8** | **4.9×** |
| bigint_dict100 | 8.8 | 5.7 | **1.8** | **4.9×** |
| bigint_flat_unique_null50 | 6.6 | 6.2 | 5.5 | 1.2× |
| bigint_dict100_null50 | 6.1 | 5.6 | 5.1 | 1.2× |
| bigint_const_null | 3.6 | 3.8 | 3.9 | **0.92× (worse)** |

**The headline is the const/dict rows, not the flat one.** Columnar reaches 1.8-2.1
ns/value on CONSTANT and DICTIONARY inputs *without knowing they are constant or
dictionary* — better than representation-aware encoding achieves by knowing
(3.9-5.7). Once the per-value work is one length-byte store plus one width-sized
store, there is nothing for representation-awareness to save: it exists to avoid
repeating an expensive conversion, and there is no longer an expensive
conversion to repeat.

**This inverts the spec's own premise.** D1-D3 remain justified only where the
per-value conversion is genuinely costly — strings, where UTF-16 conversion is
the work (23.6 ns/value flat today). For every fixed-width family, D5a replaces
them. Build order follows: D5a first and broadly, D1-D3 narrowed to the string
family, and the dictionary A/B that §3's caveat demands is only needed for
strings.

**Where it does not win yet, stated plainly:**

- **NULL-heavy columns: 1.2×.** The all-valid path is a constant stride with no
  bookkeeping; the moment any column has a NULL, the prototype falls to a
  per-row cursor with a validity branch per value. That branch is the whole
  difference (5.2 vs 1.8).

  **Removing the branch was tried and is worse — do not retry it.** The
  branchless form writes the length byte as `w * valid`, advances the cursor by
  `1 + w * valid`, memcpys the payload unconditionally (on a NULL row those
  bytes land in the slot the next column overwrites) and writes the ROW tokens
  last from a saved `row_start`, so a trailing spill cannot clobber the next
  row's token. Measured: null50 **5.5 → 7.6**, all-NULL constant **3.9 → 7.3**.
  It trades one well-predicted branch for an unconditional 8-byte write per NULL
  row, a multiply that stops the loop vectorizing, and a third pass over the
  rows. The branch was never the problem.

  What is left to try is bulk sizing rather than branchless scatter: a popcount
  per 64-bit mask word gives a column's payload count directly, so the row-size
  loop can stop consulting the mask bit by bit. That attacks the sizing half,
  which the all-valid path skips entirely and the NULL path does not.
- **All-NULL constant regresses 3.6 → 3.9.** The shipped path already writes one
  byte per row there, so the sizing pass is pure overhead. Needs the same
  early-out D2 was going to give it — which is the one piece of the original
  representation plan that survives for fixed-width families.

## Wide rows: the per-column pass degrades, blocking fixes it (user, 2026-07-31)

The columnar numbers above were measured on a **single-column** chunk, which is
the shape's most favourable case: with one 8-byte column the row stride is 10
bytes, six rows share a cache line and a per-column pass is nearly sequential. A
44-column target has a stride of ~400 bytes, so every store in a per-column pass
lands on its own line and the next column walks the same 2048 lines again, by
which time they are gone.

Measured (`BuildWideFixture` / `WideColFull` / `WideColBlocked` / `WideRowMajor`
in `test/cpp/bench_materialize.cpp`; BIGINT columns, all valid, 2048 rows,
median of 200, ns/value; every variant byte-identical to the shipped encoder):

| ncols | shipped | col full pass | blk 32 | blk 64 | blk 128 | blk 256 | row-major |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 8.14 | 0.53 | 0.55 | 0.53 | 0.61 | 0.63 | 0.63 |
| 4 | 7.21 | 0.42 | 0.37 | 0.38 | 0.40 | 0.40 | 1.20 |
| 16 | 7.67 | **1.01** | 0.42 | 0.42 | 0.42 | 0.52 | 1.14 |
| 44 | 7.83 | **0.80** | 0.43 | 0.39 | **0.36** | 0.39 | 1.29 |

(These are lower than §"Columnar scatter measured" because this fixture writes
straight from `FlatVector` data with no `UnifiedVectorFormat`/`sel` indirection
and no NULL handling. They compare *shapes*, not absolute production cost.)

**Two findings, and they point the same way:**

1. **The full per-column pass costs width.** 0.42 at 4 columns, 1.01 at 16 — same
   work per value, 2.4× the time. Blocking removes it entirely: 0.36–0.43, flat
   across every width tested. Block size is a plateau from 32 to 128; 64–128 is
   the sweet spot at width, and 256 starts to slip at 16 columns.
2. **Row-major sequential writing is worse, not better** (1.14–1.29 at width).
   It makes the write pattern ideal and the *read* pattern terrible: every row
   touches all N source arrays. The cache problem does not disappear by choosing
   the other axis — it moves.

So the shape is neither "one pass per column" nor "one pass per row": **columnar
transformation, blocked assembly**. Rows in blocks of ~64–128; within a block,
walk the columns. The staged column data and the block's slice of the wire stay
resident together. This is also what the string prototype found independently
(§3a/D5b: 6.6 → 5.9 with 256-row blocking), so the same block loop serves both.

**Non-temporal stores — not tested, and here is the condition.** Writing the
wire past the cache is attractive because the CPU never reads those bytes back.
But the BCP wire is packed: row starts are not 8- or 16-byte aligned, and
streaming-store instructions generally require alignment. The idea only becomes
testable together with D5b (encode directly into wire frames), where the layout
is ours to choose — measure it there, not here.

## Helping the server: what we can tell it, measured (2026-07-31)

Today the only hint we send is `TABLOCK` (plus `ROWS_PER_BATCH` on the COPY
path — CTAS omits it, an asymmetry worth closing). Two further ideas were
measured against the live server, 1M rows, three interleaved A/B pairs.

### `ORDER(key ASC)` — real, −11.8%, and safe to expose

Declaring the stream pre-sorted on the clustered index key lets the server skip
its own sort. Target: 8-column table with a clustered index on `id`, source
sorted by `id`.

| pair | no hint | `ORDER(id ASC)` |
| --- | --- | --- |
| 1 | 2.403 s | 2.069 s |
| 2 | 2.347 s | 2.084 s |
| 3 | 2.313 s | 1.908 s |

Median 2.347 → 2.069, **−11.8%**, every pair in the same direction.

**A false promise fails loudly — this is what makes it safe.** Feeding
descending data while claiming `ORDER(id ASC)`:

```
Cannot bulk load. The bulk data stream was incorrectly specified as sorted...
Sort order incorrect for the following two rows:
primary key of first row: (199999), primary key of second row: (199998).
```

Nothing lands; the whole load is rejected. So the hint cannot silently corrupt a
table, which is what would otherwise make it too dangerous to offer. It still
must not be set automatically from a plan's claimed ordering unless the sink is
order-preserving — but as an explicit COPY option it is sound.

### Wire column order — no effect, and the reason it cannot have one

Sending fixed-width columns first (matching how SQL Server groups the physical
row: fixed data, then null bitmap, then variable offsets and data) against the
table's declared interleaved order. COPY maps by column NAME, so this needed no
code change — just a reordered `SELECT`.

| pair | declared order | fixed-width first |
| --- | --- | --- |
| 1 | 1.097 s | 1.385 s |
| 2 | 1.111 s | 1.137 s |
| 3 | 1.090 s | 1.090 s |

At best identical, on median slightly worse.

**Re-run on a wide, string-heavy row, because the first fixture was too narrow
to conclude from.** 44 columns — 20 × `NVARCHAR(100)` each carrying 180 wire
bytes, plus 16 BIGINT, 4 DATETIME2(7), 4 BIT — measured at **~4096 bytes per row
on disk**, half the 8060 limit and forty times the first fixture. 100k rows:

| pair | declared order | fixed-width first |
| --- | --- | --- |
| 1 | 2.216 s | 2.274 s |
| 2 | 2.359 s | 2.416 s |
| 3 | 2.533 s | 2.552 s |

Same answer, three pairs of three: reordering is consistently ~2-3% *slower*.

The server does assemble each row in its own cache before it reaches a page, so
the objection is sound in principle. It does not bite because even a 4 KB row —
and the 8060-byte maximum — is far inside L1 (128 KB per performance core on
this machine, 32-48 KB on typical server parts). Whatever order the fields
arrive in, the row under construction stays resident. Intra-row scatter has
nothing to cost. The server-side costs that do matter are at page and log
granularity, and those are reached with TABLOCK, compression and batch hints,
not with field order.

**Dropped.** Recorded so it is not re-proposed: the idea is plausible, the
measurement is flat, and the mechanism explains why.

## Sorting, ORDER and parallelism against a clustered index (2026-07-31)

The first `ORDER` measurement was flawed: the source was sorted in a separate
statement, so the sort cost sat outside the timed COPY. Redone with the sort
inside the measured statement and a genuinely unordered source
(`hash(i) % 1000000`), 1M rows into an 8-column table with a clustered index on
`id`, three interleaved passes, medians:

| | wall | vs today |
| --- | --- | --- |
| A — unordered source, no hint (**what users get today**) | 3.734 s | 1× |
| C — DuckDB sorts, no hint | 2.360 s | **−37%** |
| B — DuckDB sorts + `ORDER(id ASC)` | **2.015 s** | **−46%** |

**The sort is the win, not the hint.** Ordered rows enter a clustered index by
appending to the end of the leaf level; unordered rows land all over it and split
pages. That is −37% before we tell the server anything. The hint adds a further
−15% (2.360 → 2.015) by letting it skip its own sort, consistent with the −11.8%
measured earlier when both sides were already sorted.

DuckDB's sort costs 2.2-2.7 s of CPU, but it parallelises and barely shows in
wall time — the server-side saving dominates it. This is the answer to "who
should sort": DuckDB, on many cores, rather than SQL Server on the insert path.

### Parallel sessions: `ORDER` is fine, TABLOCK is not

Four sessions, disjoint sorted key ranges, 4M rows total (the 4-session rows are
directly comparable to each other — same volume, same hint, only TABLOCK
differs):

| sessions | hint | TABLOCK | wall | per 1M rows |
| --- | --- | --- | --- | --- |
| 1 | ORDER | on | 2.63 s (1M) | 2.63 |
| 4 | — | on | 8.97 s (4M) | 2.24 |
| 4 | ORDER | on | 8.42 s (4M) | 2.11 |
| 4 | ORDER | **off** | **4.81 s (4M)** | **1.20** |

- **`ORDER` survives parallelism.** No session failed. The server validates sort
  order *per bulk-load stream*, not globally, so N sessions each sending their
  own sorted range is legal. Range-partitioning the input is therefore the
  natural shape for `PARALLEL N` into a clustered index.
- **TABLOCK serialises parallel load into a clustered index.** With it, four
  sessions manage 2.11 s/M against one session's 2.63 — almost no scaling.
  Without it, 1.20 s/M: **1.75× faster on identical work.** Heaps are the
  opposite case (BU locks are mutually compatible, which is why TABLOCK measured
  1.6× there).

**This contradicts the current auto-TABLOCK policy** (issue #45: enable it for
newly created tables). Correct for a heap, wrong for a clustered-index target
under parallel load. The policy needs the target's index shape as an input, not
just its newness.

Caveat to re-check on a real server: without TABLOCK the inserts are fully
logged, so on a database in FULL recovery the trade-off may shift. The container
used here is in SIMPLE.

## Revised TABLOCK policy — the full decision table, measured

Every cell measured on the live container, 8-column table, medians of 2-3
interleaved passes. The heap rows load 4M rows across 4 sessions; the clustered
rows are normalised per 1M.

| target | sessions | TABLOCK on | TABLOCK off | verdict |
| --- | --- | --- | --- | --- |
| heap | 1 | 11.37 s | 14.42 s | **on** (1.27×) |
| heap | 4 | **1.70 s** | 11.97 s | **on** (7.0×) |
| clustered index | 1 | 2.185 s | 2.166 s | neutral |
| clustered index | 4 | 2.11 s/M | **1.20 s/M** | **off** (1.75×) |

Two locks, two opposite behaviours. On a heap, concurrent bulk loaders take
mutually compatible BU locks, so TABLOCK is not merely an optimisation — without
it four sessions are **seven times slower** than with it, far more than the
"15-30%" the current setting documents. Against a clustered index the same hint
serialises the loaders, and dropping it buys 1.75×.

**The rule that follows needs no conditionals:** heap → TABLOCK on; clustered
index → TABLOCK off. Single-session clustered load is neutral, so it costs
nothing to decide by target shape alone and ignore the degree.

This replaces the issue-#45 policy (enable for newly created tables). Newness is
the wrong input: a freshly created table is a heap *until an index is created on
it*, which is exactly the case CTAS-then-index hits. The input must be the
target's index shape.

**What we must learn to see first.** The clustered index is currently invisible
to the extension: primary-key discovery filters on `kc.type = 'PK'`, so an index
like the article's — clustered, not a primary key — is never noticed. Discovery
needs `sys.indexes.type = 1` plus its key columns and their direction from
`sys.index_columns` (`key_ordinal > 0`, `is_descending_key`). That same metadata
is what the sort injection below needs, so it is one addition serving both.

## Sorting on the DuckDB side: where it can be injected

Measured worth −37% into a clustered index before any hint (§ above), so the
question is only where the sort goes.

| path | hook | can we inject? |
| --- | --- | --- |
| `INSERT INTO d.t SELECT ...` | `MSSQLCatalog::PlanInsert` | yes — we build the physical plan |
| `CREATE TABLE d.t AS SELECT ...` | `MSSQLCatalog::PlanCreateTableAs` | yes, but a new table has no index yet |
| `COPY src TO 'd.t' (FORMAT bcp)` | `CopyFunction` | **no** — the plan is not ours |

The COPY gap closes through the **optimizer extension** we already register
(`MSSQLOptimizer::Optimize`, `src/table_scan/mssql_optimizer.cpp`), which
receives the whole `unique_ptr<LogicalOperator>` and can rewrite it — including
wrapping a copy-to-file's child in a `LogicalOrder`. That makes one mechanism
serve every path, rather than two hooks that behave differently.

Conditions the injection must respect, in order:

1. **Only for a clustered-index target**, on its key columns, in its declared
   direction — a heap gains nothing from sorted input and would pay for the sort.
2. **Never on top of a user's own `ORDER BY`** on the same key; and if the user
   ordered by something else, theirs wins and we do not add the hint.
3. **The `ORDER` hint follows the sort, not the other way round.** The hint is
   only sound when the sink actually receives rows in that order, which requires
   `preserve_insertion_order` and a serialising sink; with `PARALLEL N` each
   session must own a disjoint key range (measured legal — the server validates
   per stream, § above).
4. **It must be defeatable.** A user who knows their input is already ordered, or
   who does not want to pay for a sort, needs an off switch.

## Fabric Warehouse now accepts TDS bulk load (2026-07 — recheck, user)

The extension disables BCP for Fabric endpoints
(`copy_function.cpp`, `mssql_ctas_planner.cpp`, gated on
`is_fabric_endpoint`) because the protocol was not supported there. That is no
longer true.

Microsoft Learn, *Ingest Data into Your Warehouse Using BCP API (Preview)*
(doc dated 2026-07-01, updated 2026-07-03):
<https://learn.microsoft.com/en-us/fabric/data-warehouse/ingest-data-bulk-copy>

> The BCP tool (bcp utility), .NET `SqlBulkCopy` class, and Java
> `SQLServerBulkCopy` class ... use the BCP API and TDS bulk-load protocol

— i.e. exactly the protocol this extension implements. **Status is preview**, not
GA.

Three constraints that land directly on our code:

1. **Microsoft Entra ID authentication only.** "SQL authentication (username and
   password) isn't supported in Warehouse." So the gate cannot simply be removed
   — BCP against Fabric is only viable when the connection is FEDAUTH.
2. **`TableLock` is ignored**, along with `CheckConstraints`, `KeepNulls` and
   `FireTriggers`: "bulk copy in Fabric Data Warehouse ignores them and uses
   default service behavior." The whole TABLOCK decision table above is
   inapplicable there. `ORDER` and `ROWS_PER_BATCH` are *not* named in the
   ignored list, which is not the same as being honoured — test, do not assume.
3. **Batch size wants 150 MB–1 GB, starting at 250–500 MB.** `mssql_copy_flush_rows`
   counts ROWS only (`BCPConfig::flush_rows`), with no byte threshold anywhere on
   the BCP path. At the 100 000-row default that is ~10 MB for a narrow table and
   ~400 MB for the 4 KB-row fixture above — the same setting meaning two very
   different things. A byte-based flush threshold is needed regardless of Fabric;
   Fabric just makes it unavoidable.

Microsoft still recommends `COPY INTO` over BCP where files can be staged, so
BCP is the path for "data is already in the client", which is precisely the
DuckDB case.

**Actions, and the order is forced by what can be tested.**

1. **Byte-based flush threshold — do now.** It is a defect independent of
   Fabric: `flush_rows = 100000` means ~10 MB on a narrow table and ~400 MB on
   the 4 KB-row fixture measured above, so the one knob means two different
   things. Needs no Fabric endpoint.
2. **Lifting the Fabric gate — last, and gated on hardware we do not have.**
   The feature is preview and our TDS implementation has never been exercised
   against a Fabric endpoint, so the gate should become "off unless the user
   asks *and* the connection is Entra" rather than being removed. Shipping that
   without a live test would be replacing a clear error with an unclear one.

**Verification is deferred: no warehouse available.** The user removed the
Fabric warehouse from their subscription to stop it accruing cost (2026-07-31),
so the questions below stay open until one is stood up again — deliberately, not
by oversight:

- is our hand-built `INSERT BULK` statement accepted at all;
- what Fabric does with `ORDER` and `ROWS_PER_BATCH` (absent from the ignored
  list, which is not the same as honoured);
- whether error reporting matches SQL Server's well enough for our parser.

Synapse dedicated pools are unaffected — never gated, BCP works there today.

## Requirements that shape the write path (user, 2026-07-31)

Recorded as constraints on everything above, with the places they collide with
what has already been measured called out — those collisions are the design
work, not the requirements themselves.

### 1. Defaults for how we CREATE tables in SQL Server

Today `CREATE_TABLE true` produces `nvarchar(max)` for every string column,
which measured **4.1× slower** than the same load into a properly typed table
and is what blocks a columnstore target (§ "External validation"). The defaults
that need deciding, each already backed by a measurement in this document:

- **string width** — must come from the sizing policy (target metadata, MSSQL
  source catalog, an explicit cast, or a setting), never `MAX` by default;
- **heap vs clustered index** — decides the TABLOCK policy and whether sorting
  pays at all;
- **compression** — `PAGE` halves storage for +63% load time, `ROW` gives −41%
  for +17%; a user choice, but only effective *because* we send TABLOCK.

### 2. Plain INSERT uses BCP only when there is no RETURNING

Bulk load returns no rows, so `RETURNING` keeps the existing batched-`VALUES` +
`OUTPUT INSERTED` path. The dispatch is per statement, not per connection.

### 3. Inside a transaction there is no parallel write

N connections cannot share one SQL Server transaction (connection pinning maps a
DuckDB transaction to one server-side transaction), so the degree is forced to 1
whenever the statement runs inside an explicit transaction. This is exactly the
case streaming exists for: it buys −20% at one session and nothing at four
(§ "Streaming does not stack with parallelism"), so the two features cover
disjoint situations rather than competing.

### 4. Parallel by default, with a setting

Measured: four sessions beat one by 2.8×, and eight were slower than four
(5.09 s vs 4.64 s) — **but that saturation point is an artefact of the test host,
not a property of SQL Server** (see the measurement-validity note below). A
ceiling is still needed; where it sits is unknown until this is re-run against a
native server.

The degree cannot be a standalone setting either. It has to be derived from, at
least: DuckDB's own thread count (writing with more streams than DuckDB has
threads produces nothing), the connection-pool limit (`mssql_connection_limit`,
default 64) since each stream holds a connection for the duration and must not
starve readers, and § "Revised TABLOCK policy" — on a clustered-index target
parallelism only pays with TABLOCK *off*. Mechanism still to be designed.

### 5. UPDATE / DELETE move to temp tables filled by BCP

This makes the write path carry DML volume too, not just COPY — every
optimisation here compounds. Two constraints follow that are easy to miss:

- **A `#temp` table is session-scoped — and that is the intended design, not a
  problem to solve.** The fill and the UPDATE are part of ONE transaction, hence
  one pinned connection and a session temp table. By § 3 that already forces a
  single stream, so parallel fill never arises and neither `##global` temps nor
  a real staging table are needed. The constraint to keep in mind is only that
  the BCP fill and the DML must not be allowed to land on different pooled
  connections.
- **A temp table is a heap** unless we index it, so TABLOCK is the right default
  there per the decision table; and if the subsequent UPDATE joins on a key, an
  index on the temp table may be worth more than the fill saving. Measure before
  assuming.


## Measurement validity: the test server runs under emulation

Every server-side number in this document was measured against SQL Server in
Docker on Apple Silicon, i.e. an amd64 image under emulation. That inflates the
server's share of wall-clock time, which systematically **overstates server-side
wins and understates client-side ones**.

What survives and what does not:

- **Directions hold**, because each has a mechanism that does not depend on CPU
  speed: unordered rows split pages in a clustered index (sorting wins), BU locks
  are mutually compatible so TABLOCK lets heap loaders run concurrently, TABLOCK
  serialises clustered-index loaders, and a declared sort order lets the server
  skip its own.
- **Magnitudes do not.** −37% for sorting, 7× for TABLOCK on a heap, 1.75× for
  dropping it on a clustered index, −11.8% for `ORDER`: all are upper bounds
  biased toward the server. Re-measure on native hardware before quoting any of
  them outside this document.
- **The parallel saturation point is not measurable here at all.** "Eight
  sessions are slower than four" says something about an emulated server on a
  laptop, nothing about a real one.

The client-side microbenchmarks (§ "Columnar scatter measured", § "Wide rows")
are unaffected — they never touch the server.

---

# Settled — what measurement has knocked down (2026-08-02)

This document is long enough that its own refuted claims are easy to lose, and
half of them are the sort that come back in six months as a fresh idea. Each row
names what killed it, so the entry is checkable rather than remembered.

## Refuted and closed

### By this spec's own later measurements

| claim | what killed it |
| --- | --- |
| Representation-aware encoding is the biggest remaining write-side win (§2, the premise this spec is named for) | twice: the encoder is 5% of wall (§"Reconnaissance"); and columnar scatter reaches 1.8-2.1 ns/value **without knowing** the input is const/dict, against 3.9-5.7 for the path that knows (§"Columnar scatter measured") |
| The `nvarchar(max)` penalty is the storage engine | a server-side `INSERT ... WITH (TABLOCK) SELECT` of the same rows: 0.217 s into `NVARCHAR(40)` vs 0.207 s into `NVARCHAR(MAX)` — identical. The 2.7× lives entirely in the bulk-load path's PLP handling |
| Keep MAX columns, declare a sized type in the BCP COLMETADATA | the server refuses: `Invalid column type from bcp client for colid 4`. Wire framing follows the column type; the column type is the only lever |
| TABLOCK's win is minimal logging | recovery model makes no difference: FULL 0.783 / SIMPLE 0.785 without it, FULL 0.384 / SIMPLE 0.369 with |
| Issue #45's policy — enable TABLOCK for newly created tables | newness is the wrong input. Right for a heap, wrong for a clustered-index target under parallel load (4 sessions: 2.11 s/M with, 1.20 s/M without). A new table is a heap only until an index is created on it |
| Send fixed-width columns first, matching the server's physical row layout | two fixtures, including 44 columns at ~4 KB/row: consistently 2-3% *slower*. Even the 8060-byte maximum row is far inside L1, so intra-row scatter has nothing to cost |
| `OPTIMIZE_FOR_SEQUENTIAL_KEY` helps a parallel clustered load | noise in both directions. Structural: the workers write disjoint ascending ranges, so there is no hot last page to relieve |
| A branchless NULL scatter beats the branch | null50 5.5 -> 7.6, all-NULL constant 3.9 -> 7.3. **Do not retry.** It trades one well-predicted branch for an unconditional 8-byte write, a multiply that stops the loop vectorising, and a third pass |
| Row-major sequential assembly is the cache-friendly shape | 1.14-1.29 ns/value at width vs 0.36-0.43 blocked. It makes the write pattern ideal and the read pattern terrible; the problem moves rather than disappears |
| Streaming inside a batch stacks with parallel sessions | -20% at one session, noise at four. They are the same overlap |
| A clustered-index target must fall back to a single stream | not supported by measurement: four concurrent sessions into a clustered index on a sequential key completed correctly and at the same speed |
| "COPY loses the string length from Parquet", i.e. there is plumbing to fix | DuckDB drops the `VARCHAR(n)` modifier at `CREATE TABLE`, before any file is involved, and Parquet never carried one. There is no plumbing fix — the length can only come from the target, an MSSQL source catalog, an explicit cast, or a setting |

### On the UTF-8 path

| claim | what killed it |
| --- | --- |
| `COLLATIONPROPERTY` is the robust way to detect a UTF-8 / double-byte collation | Fabric does not support it and **closes the connection** rather than raising. In a shared metadata query that would have broken catalog loading on Fabric outright |
| `CAST(x AS VARCHAR(m)) COLLATE <utf8>` | converts through the database's code page first: with a CP1252 database, Cyrillic, CJK and emoji all come back as `0x3F`. The `COLLATE` must sit on the source expression. Verified byte-for-byte |
| The client's `UTF8_SUPPORT` request may carry a data byte | live Azure SQL rejects the login with 18456, indistinguishable from a wrong password. No local server reproduces it |
| "Always cast unicode columns to UTF-8" | CJK loses 35%; the cast costs the same regardless of content, only the baseline varies |
| `UTF8SUPPORT` is worth ~2× on the wire (how the PR #227 table reads) | @oluies, 2019/2022/2025/Edge, matching `DATALENGTH`: 0.51× ASCII, 1.00× Cyrillic, **1.49× worse** CJK, 1.00× astral. It is arithmetic — UTF-8 beats UTF-16 at one byte per character, ties at two, loses at three |

### On the read path and the instrument

| claim | what killed it |
| --- | --- |
| Exceptions are the cost on the parse path | table-driven EH is zero-cost on the non-throwing path. What cost was **out-of-line calls** — removing two per value measured -11.7% on bigint |
| The auto-TABLOCK defect does not reproduce; the code path looks correct | it reproduces. `INSERT BULK` carries no `TABLOCK` on either COPY or CTAS when the table is created. Root cause: `TryGetCurrentSetting` succeeds unconditionally for a setting with a registered default, so `tablock_explicit` is always true and **both** auto-TABLOCK branches have been dead since spec 030 |
| Issue #233 — "the counters report one 2048-row chunk of a multi-chunk scan" | the accumulation is correct (`rows=50000 chunks=25` on a varying fixture). The real defect is that `CountChunkForDebug` assumes a FLAT vector while spec 056 (#221) publishes a uniform chunk as CONSTANT, so **the query dies** with `INTERNAL Error: Operation requires a flat vector`. `rows=2048 chunks=0` is the destructor's partial dump during stack unwinding — a symptom, not a sampling bug |

## Refuted, but the refutation has expired — re-open

The distinction matters more than the list above, because these look settled and
are not.

- **"D5b is refuted: build + send is 7%, system time under 1%"** and **"the
  encoder is 5% of wall"**. Both were taken against a baseline where the server
  held 86% *because we created the target badly* — `nvarchar(max)`, no TABLOCK.
  Once those are fixed the denominator collapses (1.68 s -> 0.37 s in this
  document's own table) and the same milliseconds become ~12% and ~16%. The
  syscall arithmetic in both the claim and its refutation is also stale: the
  negotiated packet size has been 16384 since spec 055, not 4096, so a 10 MB
  batch is ~640 sends, not ~2560. **Re-price at step 4 of the build order, not
  before.**
- **"Eight sessions are slower than four."** Withdrawn by this document: an
  artefact of an emulated server on a laptop, not a property of SQL Server.
- **Every server-side magnitude here.** Measured on amd64 under emulation on
  Apple Silicon. Directions hold — each has a mechanism independent of CPU speed
  — magnitudes do not. 7×, 2.8×, -37%, -11.8% are upper bounds biased toward the
  server.

## Withdrawn because the instrument was lying

- **`parse=577 ns/row` vs `process=28 ns/row`.** Taken at `MSSQL_DEBUG=2`, where
  an `fprintf` per token sits inside the function the parse timer wraps. The
  numbers did not get noisy, they inverted.
- **Every absolute wire-byte figure published so far** (85.8 -> 43.9 MB,
  1 020 060 -> 540 030). Ratios survive — both sides sampled identically —
  absolutes do not.
- **Spec 058's T0d bound on the `Feed` copy** (~77 B/row -> ~8 ns/row) is derived
  from `wire_in`, i.e. from an instrument that crashes outright on constant
  columns. It must be re-derived before it can gate anything.

## Never established — reasoning, marked as such

Not refuted, but not evidence either, and not to be quoted as measured.

- UTF-8 inflating against UTF-16 for double-byte East Asian code pages
  (CP932/936/949/950 `varchar`). Flagged publicly by the author as reasoning.
- The server-side transcode cost for a **sized** `varchar` under a UTF-8
  collation. Only the pathological `varchar(max)` figure exists — ~10 s CPU and
  11.9M logical reads per million rows — and it does not transfer.

---

# Measurement matrix (user, 2026-08-02)

Two rules first, because both were learned by getting them wrong.

**Every client-side delta is taken single-session.** Parallel sessions hide the
client's work behind each other's server waits — that is exactly why streaming
measures -20% at one session and noise at four. Measuring steps 3-6 with
`PARALLEL N` on would make their wins vanish into the overlap and get them
dropped for the wrong reason. Parallel is measured *on top*, last, as its own
axis.

**Interleaved same-session A/B, medians over >=3 pairs, order rotated**, with an
untouched control family in every run. A control that moves invalidates the run.
This is the protocol that has caught real bugs; it is not optional.

## Base configuration

Everything below is a one-axis sweep from this point unless a cross is named:
4 columns (BIGINT, BIGINT, DECIMAL(18,2), NVARCHAR(40)), 500k rows, heap target
with TABLOCK, autocommit, one session, all values valid, FLAT vectors, sized
columns.

## Axis A — type family, for every family we encode

One column per cell, so ns/value is ns/row and the framing cost is not diluted.

`BIT` · `TINYINT` · `SMALLINT` · `INT` · `BIGINT` · `HUGEINT` (#177) ·
`FLOAT` · `DOUBLE` · `DECIMAL` at 9 / 18 / 38 digits (the three magnitude
buckets) · `MONEY` · `SMALLMONEY` · `DATE` · `TIME(7)` · `DATETIME` ·
`DATETIME2(0)` / `(3)` / `(7)` · `DATETIMEOFFSET` · `UNIQUEIDENTIFIER` ·
`VARBINARY(n)` · `VARBINARY(MAX)` · `XML` · the string cells of axis B.

The all-fixed-width fast path (step 3c) is only exercised when *no* column in the
row is variable-width, so each fixed family also gets a 4-column all-fixed cell.

## Axis B — string flavour on the target, crossed with input annotation

This is the axis the read path already has and the write path does not. The
cross is required, not decorative: the annotation decides the *framing* (2-byte
prefix vs PLP), the collation decides the *encoding* (UTF-16 vs UTF-8 vs code
page), and they are independent.

**Annotated types and the UTF-8 wire form are already implemented — spec 060,
end to end, and never measured.** Checked in the code before this matrix was
written, because the first draft of it assumed the opposite:
`TargetResolver::GenerateColumnMetadata` reads the annotation through
`codec::TryGetTargetStringType`, retargets the column to `TDS_TYPE_BIGVARCHAR`
(0xA7) with a UTF-8 collation, `GetTDSMaxLength` sizes the wire from the
annotation (`2n`, PLP past 8000), `codec::string::EncodeVarcharUtf8` copies the
UTF-8 bytes instead of converting, and COPY and CTAS reach it through the same
builder. So this axis measures a shipped path, and its job is to find where that
path does *not* fire — not to build it.

| target column | wire form | client conversion |
| --- | --- | --- |
| `nvarchar(n)` | UTF-16LE, 2-byte prefix | UTF-8 -> UTF-16 encode |
| `nvarchar(max)` | UTF-16LE, PLP | encode + PLP framing |
| `varchar(n)`, `_UTF8` collation | **UTF-8, 2-byte prefix** (0xA7) | **none — bytes copied** |
| `varchar(max)`, `_UTF8` collation | UTF-8, PLP | none |
| `varchar(n)`, code page | UTF-16LE — deliberately: UTF-8 bytes read in that code page would be mangled | encode |

Crossed with input annotation: plain `VARCHAR` (unannotated, sized by
`mssql_default_string_length`) · `MSSQL_NVARCHAR(n)` · `MSSQL_VARCHAR(n)` ·
`MSSQL_VARCHAR(n, 'collation')`.

And crossed with content, because the UTF-8 branch's value is content-dependent
and that is precisely what PR #227's review established: ASCII (1 B/char) ·
Cyrillic (2) · CJK (3) · astral/emoji (4). Report wire bytes **and** client CPU
separately — the wire-byte table alone does not settle the CJK case, since the
UTF-8 target removes a conversion at both ends while adding 49% bytes.

Content lengths: 4 / 16 / 40 / 256 / 4096 characters, plus the MAX/PLP cells.

## Axis C — NULL density, crossed with representation

These two interact: both decide the scatter loop's branch structure, which is the
whole difference between 1.8 and 5.2 ns/value.

NULL {0, 10, 50, 100}% × {FLAT unique, FLAT card-10, DICTIONARY 1/10/100,
CONSTANT}. The all-NULL CONSTANT cell is the one the shipped path already wins
(3.6 ns/value, one byte per row) and must not regress — it needs the early-out.

## Axis D — row width

1 / 4 / 16 / 44 columns. This is the blocking axis: a full per-column pass costs
0.42 at 4 columns and 1.01 at 16, while blocked stays 0.36-0.43 flat. 44 columns
matches the external-validation fixture.

## Axis E — target shape, crossed with sessions

The TABLOCK decision table, extended. {heap, clustered rowstore, clustered
columnstore} × {TABLOCK on, off} × {1, 2, 4, 8 sessions}. The columnstore row
also carries the `flush_rows` >= 102400 threshold (worth 12% on that target).

## Axis F — transaction context

Autocommit vs an explicit DuckDB transaction. The second forces degree 1, which
is the configuration streaming exists for; it is the only place the streaming
delta should still be visible after parallel lands.

## What is reported per cell

ns/value and ns/row · wire bytes · **allocations per chunk** (the arena is
hooked; this is a stated goal, not a by-product) · client CPU vs wall ·
encodes-per-chunk against rows-per-chunk (the D6 counter that proves the
representation path did what it claims).

## Cells that are deliberately NOT crossed

Stated so the omission reads as a decision. NULL density against target shape
(independent). Row width against string flavour (additive). Content script
against representation (a CONSTANT column converts once whatever the script).

---

# Release comparison: v0.2.2 vs post-057

Following the precedent set at the end of spec 054 (`bench(spec-054): pre-merge
release comparison — v0.2.2 vs new build`), the series closes with a comparison
against the last release rather than against the branch point, on both
directions and at two volumes. The branch point measures the phase; the release
measures what a user actually gets, which is the number that goes in the notes.

Run at SF 1 and SF 10 equivalents, plus the 44-column external-validation
fixture, plus the narrow 4-column base. Both a heap and a columnstore target, so
the storage claim is covered too.

---

# Parallel write is mandatory (user, 2026-08-02)

Promoted from "its own spec" into this one. Outside an explicit transaction the
write should use N connections by default.

What is already measured, and what it constrains:

- **Heap: TABLOCK on.** Four sessions 1.70 s against 11.97 s without it — 7×,
  because concurrent bulk loaders take mutually compatible BU locks.
- **Clustered rowstore: TABLOCK off.** 1.20 s/M against 2.11 s/M with it — the
  hint serialises the loaders. Two locks, two opposite behaviours; the rule needs
  no conditionals beyond the target's index kind, which `MSSQLIndexKind`
  (spec 049) now supplies.
- **`ORDER` survives parallelism.** The server validates sort order per
  bulk-load stream, not globally, so N sessions each sending a disjoint sorted
  key range is legal and no session failed. Range-partitioning the input is
  therefore the natural shape for a clustered-index target.
- **Inside an explicit transaction the degree is forced to 1.** N connections
  cannot share one SQL Server transaction. This is not an edge case for a
  database extension, and it is the configuration streaming exists for.
- **The degree is derived, not set.** Inputs: DuckDB's own thread count (more
  streams than threads produces nothing), `mssql_connection_limit` (each stream
  holds a connection for the duration and must not starve readers), and the
  target's index kind. The saturation ceiling is unknown — the "eight is slower
  than four" observation is a test-host artefact and must be re-taken on native
  hardware before any default is chosen.
- **A failure in one session must abort the rest.**

## Revised build order

One PR, sequential commits, each carrying its own delta against the previous.
Wire output byte-identical to the shipped encoder is checked **per commit**, not
once at the end — steps 1-2 change what the encoder is fed while step 3 changes
the encoder, and a divergence found only at the end would not be localisable.

| # | step | why here |
| --- | --- | --- |
| 0 | **Instrument**: `CountChunkForDebug` off the FLAT assumption; `MSSQL_COUNTERS` split from `MSSQL_DEBUG`; absolutes re-derived; a test pinning a uniform column with counters on | everything after is ranked by it, and today it crashes the query it measures |
| 1 | **TABLOCK by target shape** (heap on / clustered rowstore off / columnstore on), replacing the dead `tablock_explicit` gate | free, and it resets the denominator — client work measured before this is measured against an inflated server wait |
| 2 | **Target form**: `mssql_default_string_length` default; byte-based flush threshold; UTF-8 collation on created string targets | decides what the encoder is fed. Sized columns turn PLP framing into a 2-byte prefix and make `max_length` available as the step-3a buffer bound |
| 3 | **Columnar transformation** — 3a bound from metadata + raw-pointer writes; 3b per-column kernel resolved once; 3c all-fixed-width direct scatter; 3d staging for variable width; 3e blocked assembly 64-128 rows; 3f NULLs from the validity mask; **3g truncation instead of a client-side throw** | mandatory. The UTF-8 wire form is NOT here — spec 060 shipped it; step 3 must carry it forward as a per-column kernel choice rather than rediscover it |
| 4 | **Encode straight into wire frames**, headers reserved in place, one write per L2-sized buffer | kills three post-encode copies and the per-packet allocations. Same buffer decision as 3e, so it is decided once |
| 5 | **Streaming inside a batch** | nearly free once 4 exists. Measured single-session, before parallel can mask it |
| 6 | **Representation-aware, strings only** | what survives of D1-D3 after 3 |
| 7 | **Parallel write** | last: the most invasive, and it hides every client-side delta above it |

---

# 3g — Truncate an over-long value, do not throw (user, 2026-08-02)

## What ships today

Both string guards **throw**, and they are the only thing standing between an
over-long value and the server:

- `codec::string::EncodeNVarcharFromUtf8` — `InvalidInputException` when the
  UTF-16 byte length exceeds `col.max_length` (non-PLP only; FR-023 / issue #91);
- `codec::string::EncodeVarcharUtf8` — the same against a UTF-8 byte bound.

So the failure the user sees is ours, not error 2628, and it aborts the whole
COPY on one bad row. The decision recorded earlier in this document — *overflow:
truncate, do not error* — was never implemented.

## The unit differs per regime, and getting it wrong is silent

Measured earlier in this document; repeated here because the truncation kernel is
where it becomes code:

The declared unit is SQL Server's; the *kernel's* unit is bytes in every row of
this table, because `BCPColumnMetadata::max_length` already holds a byte count.

| target | declared unit | kernel bound | cut must not split |
| --- | --- | --- | --- |
| `NVARCHAR(n)` | n byte-pairs (UTF-16 code units) | `2n` bytes, always even | a surrogate pair — half a pair is an unpaired surrogate, the exact garbage class spec 055 spent a release removing from the read side |
| `VARCHAR(n)`, `_UTF8` collation | n bytes | `n` bytes | a multi-byte UTF-8 sequence |
| `VARCHAR(n)`, code page | n bytes of *that code page* | not computable client-side | see below |

**The code-page case cannot be truncated correctly on the client.** Those columns
travel as NVARCHAR (we send UTF-16, the server converts), so the client never
knows how many code-page bytes a value will occupy — one character is 1 byte in
CP1252 and 2 in CP932, and a character the page cannot represent becomes `?`,
which is 1.

**Settled (user, 2026-08-02): this regime keeps the server's error 2628.** The
alternative — bounding conservatively at n/2 characters — is safe only in the
sense that it never overflows; it truncates data that would have fitted, on a
target that is already lossy by construction (a character outside the code page
becomes `?` regardless). Silently cutting *more* than the column asked for
compounds the loss instead of avoiding it. So a code-page target is the one place
the load still aborts, loudly, with the server naming the row — which is strictly
more information than a silent short value.

## Requirements

- **Cut on character boundaries.** Never mid-sequence, never between the halves
  of a surrogate pair. A pair goes in whole or not at all.
- **Drive it from the column's collation, not only its declared length.** The
  collation id is already parsed into `ColumnMetadata::collation`.
- **Count it, and report the count when the COPY finishes.** Truncation is data
  loss whichever way it was asked for; losing values without saying so is a bad
  trade even when it was chosen deliberately.
- **Report where the bound came from**, because the three sources carry very
  different blame and the user cannot tell them apart from the row alone:
  1. an explicit cast (`col::MSSQL_NVARCHAR(20)`) — the user stated the bound,
     truncating to it executes their instruction;
  2. `mssql_default_string_length` — a global default they may never have
     thought about, and the most dangerous of the three;
  3. an existing target column — here SQL Server would have raised 2628, so
     truncating converts a loud server error into silent loss.
- **An escape hatch.** A setting to restore the throw, for the loads where
  silently short data is worse than a failed batch. Default follows the
  maintainer's call: truncate.

## Where it lands in the columnar shape

**Unconditional, branchless, fused into the assembly pass (user's call).**

The earlier sketch here — compute the column's lengths, take a maximum, act only
if it exceeds the bound — was wrong on its own terms: a maximum over per-value
lengths is a per-value pass, a reduction instead of a branch but a pass all the
same. And no cheaper detector exists: a column's *total* converted bytes cannot
distinguish one over-long value among short ones from a uniform column of the
same sum.

**Moving truncation into a plan-level vectorised projection was considered and
rejected, and the reason is not the one first offered.** It is not that it costs
less per value — it costs exactly the same per value, in a separate operator,
with an extra pass over the data to get there. One loop beats two, and the loop
that has to exist anyway is the assembly pass. (The secondary objections stand
and are worth recording: a plan-level cast only covers a bound that arrived *as*
a cast, leaving the existing-column and `mssql_default_string_length` sources
uncovered, and it would stop `MSSQL_NVARCHAR(n)` being the `ReinterpretCast`
no-op that makes it free today.)

So: **always truncate, never test whether truncation is needed.** The assembly
pass already loads `len[r]` to copy the value; it copies `min(len[r], n)`
instead. There is no cold path and no overflow branch.

**The bound is in BYTES in both regimes, so there is one `min`, not two.** This
mirrors how SQL Server declares the types — `varchar(n)` counts n bytes,
`nvarchar(n)` counts n byte-pairs — and it is already what the code holds:
`GetTDSMaxLength` stores `length * 2` for `MSSQL_NVARCHAR(n)`, and both shipped
guards compare a byte count against `col.max_length`. Only the boundary
correction differs, and both are branchless, which is what keeps "unconditional"
from turning back into a branch:

- **UTF-16 / `NVARCHAR(n)`** — the byte bound is `2n`, always even, so a byte
  `min` can never split a code unit. Only a surrogate pair can be split, and that
  is corrected by `len -= 2 * (last_unit >= 0xD800 && last_unit <= 0xDBFF)`.
  Applying it to a value that was *not* truncated is a no-op for well-formed
  UTF-16, since a valid string never ends in a lone high surrogate — so it needs
  no guard either.
- **UTF-8 / `VARCHAR(n)` `_UTF8`** — the byte bound is `n` and arbitrary, so the
  `min` can land mid-sequence. Corrected by backing off over continuation bytes
  (`0b10xxxxxx`), at most three, computed as arithmetic over the last four bytes
  rather than as a loop.

The one thing kept from the earlier requirement is the **count**, because it was
a deliberate decision that losing values silently is a bad trade: `truncated +=
(raw_len[r] > n)` is one branchless add in the same loop and needs no separate
pass. If even that is unwanted it is a single line to remove — but then the COPY
summary can no longer say anything happened.

## Consequence: the shipped throws go away

`EncodeNVarcharFromUtf8` and `EncodeVarcharUtf8` currently raise
`InvalidInputException`, and that wording is asserted by existing tests with a
"do not change" note. Unconditional truncation removes both raises from the
default path; the tests move behind the escape-hatch setting, which is the only
configuration that still throws.

## Acceptance

- Every regime in the table above round-trips: a value at exactly the bound is
  stored whole, a value one unit over is stored truncated at a character
  boundary, and re-reading it yields valid UTF-8 with no replacement characters
  and no unpaired surrogates.
- The emoji case is pinned explicitly: `NVARCHAR(20)` holds exactly 10 emoji
  (10 surrogate pairs = 20 units); 11 emoji truncate to 10, never to 10.5.
- The truncation count and the bound's source appear in the COPY summary.
- With the escape hatch set, the shipped `InvalidInputException` wording is
  unchanged — it is asserted in existing tests.
- **No regression on the in-bound path.** This is the criterion that pins the
  "unconditional" decision: a column whose values all fit must measure within 2%
  of the same column encoded without the truncation kernel. If it does not, the
  `min` and the boundary correction did not fuse the way this section claims and
  the shape is wrong, not the threshold.

---

# BCP wire type compatibility — the current fix is a stopgap (user, 2026-08-02)

Issue [#153](https://github.com/hugr-lab/mssql-extension/issues/153) asked whether
COPY into an existing table should allow compatible numeric coercion. The
question turned out to rest on a false premise, and the answer shipped here is
deliberately a stopgap. Both halves need recording, because step 3 has to redo it.

## What was actually wrong

`IsTypeCompatible` advertised a widening-only policy. The widenings did not work:

| conversion | validator | runtime, before |
| --- | --- | --- |
| `INTEGER -> bigint` | allowed | `INTERNAL Error: Expected unified vector format of type INT64, but found type INT32` |
| `TINYINT -> int` | allowed | `INTERNAL Error: ... INT32 ... INT8` |
| `SMALLINT -> bigint` | allowed | `INTERNAL Error: ... INT64 ... INT16` |
| `BIGINT -> int` | rejected | never reached |
| `DECIMAL(21,1) -> decimal(18,2)` | allowed | silently 10x wrong |

`integer::EncodeToBcp` switched on `col.duckdb_type` — the TARGET's type — to
choose the read width, then read the SOURCE vector's memory at that width. So
only an exact match ever worked, the compatibility table turned a clean "type
mismatch" into an `INTERNAL Error`, and the one narrowing it refused was the one
case that could have been done safely. The decimal path had no width question at
all and was simply wrong.

So the choice in the issue — "keep strict" vs "allow coercions" — was never the
real choice: the shipped behaviour was neither.

## What shipped now, and why it is a stopgap

The encoder reads the source at its own width and range-checks against the
target's, so every widening works and narrowing is refused **by value** rather
than by type. The validator was widened to match what the encoder can do.

The shape is wrong on this project's own terms: each target arm carries a branch
on the source's `PhysicalType`, i.e. a per-value test of something that is
constant for the whole chunk. It is a crutch that buys correctness now, at the
cost of a comparison per value on the exact-match path.

## What step 3 must do instead

**Resolve the conversion once per column, as a kernel.** The pair
(source `PhysicalType`, target wire type) is fixed at COLMETADATA time, so it
selects one function — exactly like `ResolveColumnOps` on the read side — and the
inner loop carries no dispatch, no branch and no per-value type test. The
exact-match case then costs literally nothing rather than one comparison.

And with that in place, **revisit the compatibility question properly**, which
this stopgap does not:

- The policy currently lives in two places that must agree by hand —
  `IsTypeCompatible` (a table of type NAMES) and the encoder (what it can
  actually execute). They disagreed for two releases and nothing detected it.
  The compatibility answer should be *derived from* the set of resolvable
  kernels, so an unimplementable conversion cannot be advertised.
- The same question is open for the families this stopgap did not touch: float
  to/from decimal, decimal to integer, temporal precision changes
  (`datetime2(7)` source into a `datetime2(3)` column), and string to non-string.
  Each is either a kernel or an honest refusal; today most are neither.
- `tinyint` deserves settling while the kernels are being written: SQL Server's
  is UNSIGNED 0..255 and the extension maps it to DuckDB's signed `TINYINT`, so
  an exact-match source of -1 currently goes out as 0xFF and the server stores
  255. The stopgap left that path byte-identical on purpose — changing it is a
  wire-compatibility decision, not a refactor.
- Whether a refused value should abort the statement or be reported per row.
  Integers refuse (a value that does not fit is a different number); strings
  truncate to the column's stated bound and count it. Those two are consistent
  with each other but the rule has never been written down as a rule.

---

# `flush_rows` is a server instruction, not a buffer size (user, 2026-08-03)

The step-2 item was written as "a byte-based flush threshold, because 100 000
rows means 10 MB on a narrow table and 400 MB on a wide one". That framing is
wrong, or rather it answers a question that belongs to someone else: how much the
client holds before sending is the **pipeline's** concern, and steps 4-5 remove
it by encoding into wire frames and streaming them as they fill, so the batch
size stops implying a buffer size at all.

What `flush_rows` actually controls is the **boundary the server sees between
DONE tokens**, and for a columnstore target that boundary has a physical
threshold: SQL Server writes a bulk-load batch straight into a compressed
rowgroup only when the batch carries **102 400 rows or more**. Below it, the rows
go to the delta store and stay there until something rebuilds them.

## Measured, and the mechanism is visible rather than inferred

1M rows x 4 columns into a `table_kind = 'columnstore'` target, local SQL Server
2022, everything else at defaults:

| `flush_rows` | rowgroups afterwards | size on disk | wall |
| --- | --- | --- | --- |
| **100 000** (the shipped default) | `OPEN: groups=1 rows=1000000` | **53 MB** | 4.09 s |
| **102 400** | `COMPRESSED: groups=9 rows=921600` + `OPEN: rows=78400` | **7 MB** | 3.22 s |

`sys.column_store_row_groups.state_description` says it outright: at the default,
**not one row is compressed**. All 1M sit in a single open delta-store rowgroup —
a delta rowgroup only closes on its own at 1 048 576 rows, so a load of any
smaller size simply never compresses.

**7.6x on disk, from a 2400-row difference.** The -21% wall is the least
interesting number here.

## So this is a defect in spec 060's headline feature

`mssql_default_table_kind = 'COLUMNSTORE'` exists to give the user the storage
win — it is the lever behind the 56x the external-validation article reported.
With the shipped `flush_rows` default it produces a columnstore table that is not
compressed at all, and says nothing. The user asks for columnstore, gets the
CREATE CLUSTERED COLUMNSTORE INDEX, and still stores 53 MB where 7 would do.

## What to do (settled 2026-08-03, user)

**One constant, one setting, one COPY parameter.** `MSSQL_DEFAULT_COPY_FLUSH_ROWS`
becomes 102 400 — the threshold itself — and there is no columnstore special
case, no second constant and no "was it set explicitly" flag.

The alternative that was built first raised the boundary only for a
CLUSTERED_COLUMNSTORE target, mirroring the TABLOCK policy. It worked, and it was
rejected as more machinery than the problem deserves: batch size measured nearly
flat on a heap (25k 0.93 s / 100k 0.78 s / 500k 0.75 s), so the extra 2400 rows
buy nothing there and cost nothing either. A target-dependent default would be a
rule to document, a branch to test and a surprise to debug, all to avoid a 2.4%
change on the paths that do not care.

Consequences worth stating:

- A user who sets `flush_rows` lower gets exactly that, columnstore or not. The
  regression test pins that case alongside the compressing one.
- `ROWS_PER_BATCH` keeps describing the real boundary. It is a hint to the
  optimizer rather than what decides rowgroup handling, but a hint that disagrees
  with what is actually sent is worse than none.
- **A byte-based cap is still wanted, for a different reason** — Fabric asks for
  150 MB-1 GB batches, and a 4 KB-row table at 102 400 rows is already ~400 MB.
  It is a cap on MEMORY, not the batch policy, and once steps 4-5 stream frames
  as they fill it stops being urgent. Keep them separate: the row count answers
  to the server, the byte count to the client.

---

# Sized string columns on a wide table — measured, and it corrects this document (2026-08-03)

`mssql_default_string_length` stays at 0 (= MAX) for now; this section is the
measurement behind that being a real choice rather than an oversight.

500 000 rows x 44 columns (20 short string codes, 16 BIGINT, 4 DATETIME, 4 BIT),
local SQL Server 2022, two interleaved passes, everything driven through the
extension (`COPY ... (FORMAT bcp)`); the server-side state read back through
`mssql_scan` over `sys.dm_db_partition_stats` and `sys.column_store_row_groups`.

| target | `MAX` (today's default) | `string_length 200` | load ratio |
| --- | --- | --- | --- |
| heap | 14.09 / 14.49 s | **3.93 / 3.70 s** | **3.7x** |
| clustered columnstore | 18.55 / 17.97 s | **8.91 / 9.39 s** | **2.0x** |

| target | `MAX` size | `200` size |
| --- | --- | --- |
| heap | 278 MB | 278 MB |
| clustered columnstore | **77 MB** | 85 MB |

## Three findings, and the third retracts a claim made twice above

1. **Load time is where sized columns pay, on both shapes.** 3.7x on a heap and
   2.0x on a columnstore, consistent with the 2.7x / 4.1x measured earlier on
   different fixtures. This is the argument for changing the default, and it is
   a strong one.

2. **Storage is NOT where they pay.** On a heap the two are identical to the
   megabyte — `nvarchar(max)` holding short values is stored in-row, so the
   pages are the same pages. Any expectation that sizing the column shrinks a
   heap is unfounded.

3. **`nvarchar(max)` does not block columnstore.** Both targets compressed the
   same 409 600 rows, and the MAX variant was *smaller* — 77 MB against 85 MB.
   Two sections of this document say otherwise ("`nvarchar(max)` is what blocks
   it", § External validation; "not to create the target as `nvarchar(max)`,
   which is what blocks columnstore", § Storage options). **Both are wrong** and
   should be read as retracted. Columnstore has supported LOB/MAX columns since
   SQL Server 2017; the restriction they describe belongs to 2016 and earlier.

   What that changes: the article's 56x storage advantage came from the
   columnstore alone, not from the column types, and the two findings are *not*
   the same finding the way § External validation claimed. Sizing the columns and
   choosing the target shape are independent levers — one buys load time, the
   other buys storage.

## Consequence for the default

The case for a non-zero `mssql_default_string_length` is now narrower and better
understood: it is worth 2-4x on load and nothing on storage. That is still a lot,
but it no longer carries the "otherwise columnstore cannot compress" argument,
which was the part that made it look forced.

Against it, unchanged: a default length silently truncates values the user never
declared a bound for, on data they may not have seen. The step-3g truncation
kernel makes that loss cheap to detect and count, so revisit the default once it
exists — the decision is easier to defend when the load can report how many
values it cut.

---

# Step 1 gate — re-measured, and it does not say what this document predicted (2026-08-03)

The gate on steps 1-2 was: re-take the write phase split once TABLOCK fires and
the batch threshold is right, because "client work measured before this is
measured against an inflated server wait". Taken now, on the repaired
instrument (`MSSQL_COUNTERS=1`, `MSSQL_DEBUG` unset).

500k rows x 4 columns (BIGINT, BIGINT, DECIMAL(18,2), VARCHAR), new heap target,
interleaved, 3 pairs, order fixed within a pair. "pre" reproduces the old
behaviour with explicit options (`tablock false, flush_rows 100000`) rather than
by comparing against a number taken earlier — same binary, same fixture, same
machine.

| | pre (old behaviour) | post (new defaults) | sized target |
| --- | --- | --- | --- |
| wall, median | 1.99 s | 1.33 s | **0.61 s** |
| `flush` (end to end) | 1408 ms | 897 ms | 334 ms |
| `encode` | ~50 ms | ~39 ms | ~32 ms |
| **encode as a share of wall** | **2.5%** | **3.0%** | **5.2%** |

Steps 1-2 are worth **-33%** on the default path, and **3.3x** together with a
sized target (1.99 -> 0.61 s). All three pairs moved in the same direction.

## The prediction was wrong

This document argued that after steps 1-2 the denominator would collapse and the
encoder's 5% would become "~12%, and build+send ~16%". It did not. The encoder is
**5.2% of wall on a fully well-formed target** — heap, TABLOCK on, sized columns
— which is exactly what the original reconnaissance measured before any of this
landed.

The error was arithmetic on the wrong fixture: the 4.5x that the prediction
leaned on (1.68 -> 0.37 s) was TABLOCK *and* sized types together on the
44-column table. On this 4-column one TABLOCK alone is 1.5x, and the client's
share is small enough that shrinking the server does not move it.

**So the "re-open D5b, the refutation has expired" note earlier in this document
is itself now retracted.** The refutation stands: build + send remains a small
share. What has genuinely expired is only the syscall arithmetic in it — the
negotiated packet size has been 16384 since spec 055, so a 10 MB batch is ~640
sends, not ~2560.

## What the gate does bound

- **encode: ~5% of wall**, stable across pairs. Steps 3 and 6 target that.
- **syscalls: under ~2% of wall.** `sys` measured 0.01 s of 0.51 s and was the
  one stable CPU figure. That bounds the "one `send()` per packet" half of D5b
  directly, and agrees with the reconnaissance's "system time under 1%".
- **The packet-build share — measured, once the timer went in (below).**

## Consequence for the plan

**Pull step 4's timer forward.** Until a timer inside `BCPWriter::FlushBatch`
separates build + send from the server's confirmation, steps 3 and 4 cannot be
ranked against each other — and step 4 might be worth more than step 3, or
nothing at all. It is a small change and it belongs with the instrument work,
not with the redesign it is supposed to justify.

**And be honest about what justifies steps 3-6 now.** Not wall-clock on this
fixture: at 5%, a perfect encoder buys 5%. What survives is what the maintainer
already gave as the reason (§ "Decision, 2026-07-30") — it is the only item here
that is *ours*, unconditional, and taken away by nobody's DDL, where TABLOCK,
sizing and PARALLEL are each conditional on the target. Plus two things this
measurement cannot see: the allocation count, which is a stated goal in its own
right, and the emulation bias, which inflates the server's share and therefore
**understates** the client's everywhere in this document.

## `flush` decomposed — build+send is the same size as the encoder

The timer was pulled forward rather than left to step 4, because without it
steps 3 and 4 could not be ranked. It is instrumented in the **primitives**
(`SendBulkLoadPacket`, `Finalize`) rather than in `FlushBatch`: the final batch
does not go through `FlushBatch` at all — `BCPCopyFinalize` calls
`WriteDone` + `Finalize` directly — and at the default batch size that is a whole
batch of server time. Counting where the work happens covers every caller by
construction. (The instrumentation self-checks: `server_wait` comes out *larger*
than `flush`, which is only possible because the final batch is now included.)

Same sized-target fixture, 3 passes, medians:

| phase | ns/row | ms | share of ~0.60 s wall |
| --- | ---: | ---: | ---: |
| `encode` | 75 | 37.7 | ~6% |
| **`build_send`** | **75** | **37.6** | **~6%** |
| `server_wait` | 720 | 360 | ~60% |

**Build+send is not small — it is the same size as the encoder.** The
reconnaissance's "build + send is 7%, so D5b is refuted" measured it as a share
of a badly-defined target's wall; against a correct target the two client phases
are equal, and together they are **~12.5% of wall**, not 5%.

At ~25 MB of wire per run, 37.6 ms is roughly 665 MB/s — far below memcpy, which
is what three passes over the data (accumulator -> `vector<TdsPacket>` ->
`Serialize()`) plus a per-packet allocation would predict. So the number agrees
with the mechanism D5b describes, even though the syscall half of that argument
is bounded under 2% (`sys` = 0.01 s).

### What this changes

- **Steps 3 and 4 are worth about the same**, ~6% of wall each. Step 4 may be the
  better trade: it removes three copies and a per-packet allocation outright,
  where the encoder is already hoisted and dispatch-resolved (spec 054 W1/W2).
- The two compose — encoding **into** the wire frames is one pass instead of
  encode-then-copy-thrice, so doing 3 and 4 together is worth more than the sum,
  which is what §D5b already argued from the buffer-shape side.
- `other` stays ~60 us across 500k rows, so nothing is hiding in the sink outside
  these two.

---

# Kernel architecture — one declaration per (source, target), everything derived (user, 2026-08-03)

Raised while adding the second family to the columnar scatter, and it stops that
work until it is settled, because the obvious next step is the wrong one.

## The trap being walked into

The write path already keeps two things that must agree and have no mechanism
forcing them to:

- `IsTypeCompatible` in `target_resolver.cpp` — a table of type NAMES saying
  which conversions are allowed;
- the family encoders — what can actually be executed.

They disagreed for two releases: every widening the table advertised
(`TINYINT -> int`, `SMALLINT -> bigint`, `INTEGER -> bigint`) died in the encoder
with `INTERNAL Error`, while `DECIMAL(21,1) -> decimal(18,2)` was waved through
and silently moved the decimal point (issue #153).

Adding a `ScatterFn` per family *alongside* the existing `EncodeFn` would make it
**three** lists to keep in step. The next divergence would be a family that
scatters correctly and appends wrongly, or vice versa, on a path no test
distinguishes because both produce a plausible number.

## The shape instead — mirror the read path (user, 2026-08-03)

The read side already solved this in spec 055 and there is nothing to invent.
`codec::staging::ResolveColumnOps(const tds::ColumnMetadata &, const LogicalType &)`
returns a `ColumnOps` resolved **once per column** holding:

- `AppendArm arm` — which per-value arm the row walk takes;
- `FinalizeKernel kernel` — which batch kernel publishes the column;
- `StagingKind kind` + `stride` — fixed width, or Var;
- `direct_write` — the bypass when the wire bytes ARE the DuckDB representation;
- `max_value_bytes` — the most bytes ONE value can occupy, so a chunk's worst
  case is preallocable from metadata alone;
- and `Unsupported` / `needs_value_fallback` as the escape hatches, so a
  divergence degrades to the old path instead of writing wrong bytes.

The write side takes the same shape, named to match so one reader can hold both:

```
struct WriteColumnOps {
    ScatterArm  arm;              // DirectCopy1/2/4/8, Decimal, Date, Time,
                                  // DateTime2, DateTimeOffset, Money, Guid, ...
    ConvertArm  convert;          // None, WidenInt, NarrowInt, RescaleDecimal, ...
    WireKind    kind;             // Fixed | VariableUShort | Plp
    uint32_t    wire_width;       // bytes per value for Fixed; 0 for variable
    uint32_t    max_value_bytes;  // provable worst case for a Var column
};
WriteColumnOps ResolveWriteColumnOps(const LogicalType &source, const mssql::BCPColumnMetadata &target);
```

**Enums, not function pointers** — this is the part worth copying deliberately.
An arm selected by a small enum inside one loop can be inlined and vectorised; a
`ScatterFn` called per value cannot be either, which would give back most of what
the columnar shape just bought. The first sketch of this section proposed
function pointers; the read path's choice is better and it is why.

Three properties follow, and each closes a hole this spec already paid for:

1. **The append form is DERIVED, not written.** For a fixed-width column,
   appending is `resize(n + 1 + w); buf[n] = w; <arm writes at &buf[n+1]>`. One
   implementation per family, so the row path and the columnar path cannot
   diverge — they are the same bytes by construction, not by a test.

2. **Compatibility IS resolvability.** `IsTypeCompatible` stops being a table of
   type names and becomes `ResolveWriteColumnOps(...).arm != Unsupported`. A
   conversion that cannot be executed cannot be advertised, which is exactly the
   failure mode of #153, and the error can name what is missing instead of
   saying "type mismatch" about a pair the encoder would have handled.

3. **Conversion is a per-column constant.** Source scale vs target scale, source
   width vs target width, source unit vs target unit — all fixed for the chunk.
   The stopgap shipped for #153 branches on the source's `PhysicalType` inside
   every target arm, i.e. per value; that becomes `convert` chosen once, and the
   inner loop carries no type test at all.

## What "automatically transformed on cast" has to mean

The user's framing, and it is the right one: the set of conversions the extension
performs implicitly should be exactly the set for which a kernel exists — no
wider (that is #153) and no narrower (that is the `BIGINT -> int` refusal, which
DuckDB's BIGINT-by-default arithmetic made a daily annoyance).

Which leaves one policy question per kernel, and it needs writing down as a rule
rather than decided family by family as it has been:

| when the value does not fit | behaviour | why |
| --- | --- | --- |
| integer narrowing | **refuse** | a value that does not fit is a different number, and nothing in `bigint -> int` says the user wanted the low 32 bits |
| decimal scale narrowing | **round**, half away from zero | matches what SQL Server's own CAST does, so the row lands as it would had the user written it |
| string longer than the column | **truncate**, on a character boundary, and count it | the column states a bound; truncating to a stated bound is executing the declaration |
| code-page target overflow | **refuse** (server's 2628) | the client cannot compute the code page's byte count |

The pattern under it: **truncate when the target states a bound the user chose,
refuse when it does not.** A `VARCHAR(20)` says twenty. An `int` does not say
"the low 32 bits of whatever arrives".

## Consequence for the build order

Step 3's remaining kernels (DECIMAL, temporal, GUID, then strings) land on top of
`ResolveKernel`, not beside the current `EncodeFn`. The direct-copy slice already
built fits without change — its kernel is `scatter = nullptr` (memcpy) with the
width from metadata, which is the degenerate case of the struct above.

Strings stay last, as they are the only family where `wire_width` is 0 and the
staging/blocked-assembly machinery is needed; everything above is a fixed stride
and needs none of it.

---

# Step 3 measured in production — and the width degradation does not reproduce (2026-08-03)

## What landed

Columnar scatter with the arm resolved once per column
(`ResolveWriteColumnOps`), and batch kernels for the families that need a
transformation rather than a copy.

| fixture | row path | columnar | |
| --- | --- | --- | --- |
| 8 x BIGINT/INT/…, all valid | 73.3 ms (9.2 ns/value) | **24.7 ms (3.1)** | **3.0x** |
| the same with NULLs | 64.2 ms | 58.2 ms | ~1.1x |
| 4 DECIMAL + GUID + 3 BIGINT | 135 ms (16.9 ns/value) | **68.8 ms (8.6)** | **1.96x** |

**A third of the decimal cost was an un-inlinable call.** The first version of
the kernel was a per-value `ScatterToBcp` called from `bcp_row_encoder.cpp`
across a translation-unit boundary: 102 ms. Turning it into one call per COLUMN,
with the loop inside `decimal_codec.cpp` where it specialises on the source width
and inlines the per-value step, took it to 68.8 ms — **1.48x for the same
arithmetic**. Exactly the cost spec 058 identified ("what DID cost is out-of-line
calls"), reintroduced by accident and then removed.

## The width degradation does not reproduce

The prototype measured a full per-column pass degrading with width — 0.42
ns/value at 4 columns, 1.01 at 16 — and that measurement is the entire case for
blocked assembly. In production it does not appear. BIGINT columns, 8M values in
every cell, best of 3:

| columns | rows | ns/value |
| --- | --- | --- |
| 1 | 8 000 000 | 3.00 |
| 4 | 2 000 000 | 2.43 |
| 16 | 500 000 | 2.24 |
| 44 | 181 818 | **2.34** |

Flat, and marginally *better* at width. The likely reason is cache size: a
2048-row chunk of 44 BIGINT columns is ~813 KB of wire, which fits this machine's
L2, so the per-column pass never leaves it. The prototype also measured a
different shape — no length byte, no validity — so it is not a like-for-like.

**This reading was wrong, and the prototype below overturns it.** The degradation
is real; production simply cannot see it yet, because ~1.5-2 ns/value of
per-value overhead sits on top and masks it. Left here with the correction rather
than deleted, because the reasoning error is the instructive part: a flat
production curve was read as "the effect does not exist" when it meant "something
larger is in front of it".

## What is still not the target shape

The maintainer's formulation, and it is the right one: *data arrives in columns,
so transform columns — with vectorisable instructions, not per value; every
branch resolved before the hot path; and only then assemble the transformed
columns into the wire in blocks.*

Against that, what shipped is two-thirds there:

- **one call per column** — yes;
- **branches before the hot path** — yes: the arm, the width, the scale shift and
  the source's storage type are all resolved before the loop;
- **vectorisable transform** — **no.** The transform is fused with a STRIDED
  store (`dst + r * stride`), so every value lands on its own cache line at
  width and the loop cannot vectorise however well it is specialised.

Separating them — transform the column into a contiguous staging array, then
gather into the row-major wire — is what makes the transform vectorisable, and it
is testable independently of blocking. For direct-copy families there is nothing
to gain (there is no transform, only the store). For DECIMAL, temporal and the
string families the transform is real work and this is where it would pay.

---

# Prototype: which shape actually wins (2026-08-03)

Built in `test/cpp/bench_materialize.cpp` next to the existing wide-encode
variants, so every cell is checked byte-identical against the shipped encoder by
the harness itself. BIGINT columns, 2048-row chunks, median of 200, ns/value.

| ncols | shipped | colfull | **colblk64** | tmplframe | typedw | tmpl+typedw |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 2.05 | 0.49 | 0.55 | **0.37** | 0.53 | 0.37 |
| 4 | 1.95 | 0.40 | 0.40 | **0.35** | 0.38 | 0.35 |
| 16 | 1.86 | 1.12 | **0.42** | 1.19 | 1.14 | 1.08 |
| 44 | 1.68 | 0.88 | **0.38** | 0.95 | 0.88 | 0.87 |

Two hypotheses died and one survived.

**Width specialisation buys nothing.** The production scatter holds the width in
a runtime `uint8_t` and calls `memcpy(dst, src, w)`; the theory was that this
cannot fold into a single store. Templating the loop on W measured 0.49 -> 0.53
at one column and 0.88 -> 0.88 at 44 — i.e. nothing. The compiler handles it. The
cost is not there.

**Template framing wins only when narrow.** Building one row's framing (the 0xD1
token and every column's length byte, all at fixed offsets) and replicating it by
doubling gives 0.49 -> 0.37 at one column, and 0.88 -> **0.95** at 44 — a loss.
Obvious in hindsight: at width the replication writes 813 KB that the payload
pass immediately overwrites, and that extra pass costs more than the length bytes
it saves. Recorded so it is not re-proposed for wide tables.

**Blocking is the win, and it is large.** 1.12 -> 0.42 at 16 columns, 0.88 ->
0.38 at 44, and flat across every width tested. Block size is a plateau from 32
to 256 with 64-128 the sweet spot, matching the earlier prototype.

## Why production could not see it, and what that orders

Production measures 2.24-3.00 ns/value where this microbenchmark's `colfull` is
0.88-1.12 for the same shape. So **~1.5-2 ns/value of per-value overhead sits on
top** — the microbenchmark walks `FlatVector::GetData<int64_t>(...)[r]` while the
production scatter goes through `fmt.data` plus `sel->get_index(r)` per value.
That indirection is what hides the difference between a good and a bad access
pattern: when every value already costs a branch and a possible load, the cache
behaviour of the store is not what dominates.

**So the order is the reverse of what this document proposed an hour earlier.**
Remove the per-value overhead first; blocking pays only once it is gone,
otherwise its win drowns exactly as the degradation did. Whether a column has a
selection vector at all is a COLUMN constant — the flat, no-selection case is the
overwhelming majority and deserves a unit-stride loop the compiler can see
through, which is the same rule as everything else here: every branch resolved
before the hot path.

---

# Temporal: the kernel, the width invariant, and what legacy `datetime` does (2026-08-03)

DATE and the DATETIME2 family take the columnar scatter: **96.3 -> 27.6 ms for 8M
values, 12.0 -> 3.45 ns/value (3.5x)**. The same constants-per-value disease as
decimal, with one extra: `EncodeTime` computed its scale divisor with a LOOP
(`for (i < 6 - scale) divisor *= 10`), up to six multiplies for a number fixed by
the column.

## The width invariant — new, and it applies to every future arm

**The scatter requires `wire_width` from metadata to equal what the kernel
actually writes.** Sizing reserves `1 + max_length` per value before any value is
touched, so a kernel that emits fewer bytes leaves every later column in the row
where the server is not looking.

The row path never had this constraint: it appends, so whatever it writes IS the
layout. This was found by breaking it — the first temporal kernel desynchronised
the stream on a legacy `datetime` target ("premature end-of-message"), because
`datetime`, `datetime2` and `smalldatetime` share both the wire token
(TDS_TYPE_DATETIME2) and `col.duckdb_type` (TIMESTAMP) and differ only in
`max_length`: 8, 6/7/8 and 4. Keying the arm on the DuckDB type routed all three
into a kernel that fits one.

The resolver now checks the equality explicitly instead of assuming it.

## Legacy `datetime` / `smalldatetime` stay on the row path — deliberately

They are correct there. The value still goes out in datetime2 form and the server
converts it; the rounding and day-carry rules below are shared by both paths, so
only the speed differs.

Giving them the kernel means making `SQLServerTypeMaxLength` report what is
actually SENT rather than the column's own storage size. That changes
COLMETADATA — what the server is told about the stream — so it is a separate
change needing its own verification, and the gain is narrow: only tables still on
the legacy type. Recorded here so the omission reads as a decision.

## Narrowing rounds, and it carries into the date

Found by asking what happens when the source precision differs from the target's,
and checking against the server rather than assuming:

| value -> target | SQL Server | before | after |
| --- | --- | --- | --- |
| `.1239999` -> datetime2(3) | `.124` | `.123` | `.124` |
| `.1235` -> datetime2(3) | `.124` | `.123` | `.124` |
| `.9999999` -> datetime2(0) | `12:00:01` | `12:00:00` | `12:00:01` |
| `23:59:59.9999999` -> datetime2(3) | `2024-01-16 00:00:00.000` | `...15 23:59:59.999` | `2024-01-16 00:00:00.000` |

**We truncated, the server rounds** — so the same data loaded by `COPY` and by
`INSERT ... CAST` disagreed, silently, on anything with sub-second precision finer
than the target. It also disagreed with this extension's own decimal path, which
narrows by rounding for exactly this reason. One rule now: *a narrowing conversion
lands where the server would have put it.*

And the carry is real, not theoretical. The first fix asserted in a comment that
rounding could not cross the day boundary; it can — `23:59:59.999999` into scale 3
rounds to 86 400 000 ms, which is 24:00:00 and not a legal time, and the server
rejects the row outright. SQL Server rolls the DATE instead, so the kernel does
too. Both the pre-rounding truncation and the missing carry were pre-existing;
only the kernel made them visible.

Rounding costs nothing per value: the divisor's half and the day's tick count are
column constants hoisted beside the scale factor, and the carry is one compare
that is false for every value but the last tick of a day.

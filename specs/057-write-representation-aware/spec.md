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

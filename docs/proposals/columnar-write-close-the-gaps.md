# Closing the gaps in the columnar write path

Follow-up to spec 057, scoped as its own PR. **Goal: everything that can be a
per-column kernel call becomes one, and the row-major path is reached only by
genuinely unsupported type pairs.**

## What exists today

Three paths, decided per chunk in `TryEncodeChunkColumnar`
(`src/tds/encoding/bcp_row_encoder.cpp`):

| path | taken when | kernel calls |
|---|---|---|
| **strided** (`ScatterBlock`) | every column fixed-width **and** no NULLs anywhere | one per column per block — fully columnar |
| **cursor** (`CursorBlock`) | any variable-length column **or** any NULL in a fixed-width column | one per column for `DirectCopy*` / `IntConvert` / `FloatConvert`; **one per VALUE** for `Decimal` / `Guid` / `Datetime` |
| **row-major** (`EncodeRow`) | any column resolves to `RowFallback` | none — whole chunk, every column |

The condition is literally `if (all_valid && !has_variable)`. **One NULL in one
fixed-width column** sends the chunk to the cursor path — no string column
required — and then *every* decimal/uuid/datetime column in that chunk pays a
per-value call across a translation-unit boundary, un-inlinable.

## What it costs (measured 2026-08-04, 2M rows x 8 columns)

Median of five runs, one column made nullable to force the cursor path:

| | strided | cursor | cost of the cursor path |
|---|---|---|---|
| 8 x `decimal(18,4)` | 0.163 s CPU | 0.396 s | **+0.23 s** (~14 ns/value) |
| 8 x `bigint` (control) | 0.117 s CPU | 0.102 s | **~0** |

`bigint` costs nothing extra because its cursor arm is inlined in the same TU.
The whole difference is the per-value calls. Encoding those columns gets ~2.4x
more expensive.

**Wall clock does not move** (0.7–0.85 s either way) because this server is
ingest-bound — the same story as everything else measured on spec 057. The win
is client CPU, which matters when the server is faster than the client or DuckDB
is doing other work alongside.

An earlier measurement of "~5%" used 8 decimals **plus a string column** and was
wrong: the string column's own cost dominated and masked the effect. Isolating
with NULLs instead is what shows it.

## Direction

Ordered by what the user asked for: move as much as possible to columnar
transformation — NULLs, decimal, bigint, everything currently on the cursor path
— and fall into the cursor only for the genuinely hard cases.

### 1. Kernels write unconditionally; NULL is a framing concern

The insight that makes this cheap: **the scatter kernels do not need to know
about NULLs.** A kernel can write the payload for every row, including rows whose
value is NULL, because

- a NULL row's length prefix is `0x00` and its cursor advances by **1**, not
  `1 + W`, so the next column writes over the garbage payload;
- so the garbage is never on the wire — it is overwritten before the frame is
  sent.

That removes the validity test from the inner loop *and* removes the reason the
per-value call existed (the caller was doing the NULL branch per row and calling
the kernel for the remainder).

Two things to get right:

- **the tail.** The last column of the last rows must not write past the end of
  the buffer. Over-allocate by the widest column, or handle the final block
  specially. Getting this wrong is an out-of-bounds write, not a wrong value, so
  it needs an explicit test with a NULL in the last column of the last row.
- **the framing pass** that writes the `0x00` markers and advances the cursor
  stays per column, over the validity mask — cheap and vectorisable, and it is
  the only place that reads validity.

### 2. Give decimal / uuid / datetime a cursor entry point

Rather than three new implementations, template the existing kernel on a
position policy (`StridePos{stride}` / `CursorPos{cursor}`) so both entry points
instantiate the same body in the same TU and keep inlining. One implementation
is also what `write_column_ops.hpp` already claims.

Requires byte-equality tests between the two paths per family — the existing
`time_rounding_both_paths.test` / `smalldatetime_write.test` /
`string_bound_truncation.test` are the pattern.

### 3. Shrink what reaches row-major at all

Two ordinary column types send the WHOLE table row-major today, and both already
travel as DECIMAL on the wire where a kernel exists:

- **HUGEINT** — this is the cheap one. Its source is physically INT128, which
  `ResolveWriteColumnOps` already accepts for the decimal arm, and
  `GenerateColumnMetadata` sets `max_length = 17`, which equals
  `GetDecimalByteSize(38)`. The only thing blocking it is that the dispatch gate
  is `target.duckdb_type.id() == LogicalTypeId::DECIMAL` and a HUGEINT column's
  `duckdb_type` is HUGEINT. Worth doing first: `SUM()` over integers returns
  HUGEINT, so aggregate-then-load is a common shape, and today one such column
  takes the whole table row-major. (It is also why HUGEINT is the standard trick
  for forcing the row path in tests — those will need another lever.)
- **UBIGINT** — needs an inconsistency resolved first. `GenerateColumnMetadata`
  sets `precision = 20, max_length = 9`, but `GetDecimalByteSize(20)` is **13**,
  so the resolver's `target.max_length == width` check cannot pass. Verified
  end-to-end on 2026-08-04 that UBIGINT loads correctly today (0, 1, 2^63-1 and
  2^64-1 all round-trip through `decimal(20,0)`), so this is not a bug to fix —
  it is a disagreement to settle before the column can be kernel-eligible. Also
  needs a UINT64 -> hugeint widening step, since the decimal arm accepts signed
  sources only.

Everything else on the `RowFallback` list is a deliberate refusal (unsigned into
a signed target, signed into `tinyint`) or a genuine conversion the row path
performs (a non-string source rendered as text, e.g. INTERVAL as NVARCHAR).
Those are the "hard cases" the cursor and row paths should be reserved for.

## Not in scope

Deduplicating the parallel-writer machinery between COPY and CTAS. It has
drifted, and it should be merged, but it buys no measured speed and reads as a
different kind of change — its own PR, reviewed as a refactor.

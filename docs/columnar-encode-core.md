# The columnar encode core — a reader's guide

Six files, ~1400 lines of code, and the place where every silent data corruption
this branch found actually lived. This is the map: what the pieces are, what
holds them together, and what a reviewer should be suspicious of.

Written for someone reading the code, not for someone using the extension — for
that, see the write-path section of [`../DATAMODEL.md`](../DATAMODEL.md).

## The problem it solves

Encoding a chunk for `INSERT BULK` used to be a per-value function call that
re-asked, for every single value, "what type is this, what does the target want,
how wide is it". A million rows × 44 columns asked that question 44 million
times to get 44 answers.

The core replaces it with: **resolve each column once, then run a loop that
carries no type test at all.**

```
ResolveWriteColumnOps(source_type, target_column)  ->  WriteColumnOps { arm, wire_width, ... }
                                                             |
                            +--------------------------------+--------------------------------+
                            |                                |                                |
                     ScatterBlock                       CursorBlock                       EncodeRow
                  (fixed stride)                    (per-row positions)               (row-major append)
```

Two properties are deliberate and worth knowing before reading anything:

- **The arm is an `enum`, not a function pointer.** An enum switch outside the
  row loop inlines and vectorises; an indirect call per value does neither, which
  would give back most of what the shape buys.
- **The row-major append form is derived from the arm.** For a fixed-width
  column, appending is `resize(n + 1 + w); buf[n] = w;` and then the *same* arm
  the scatter uses. The intent is that the two paths cannot produce different
  bytes because there is one implementation. **This intent is not fully realised
  — see "Weak points" below.**

## Which path a chunk takes

Decided per chunk in `TryEncodeChunkColumnar` (`bcp_row_encoder.cpp`):

| path | condition | cost |
|---|---|---|
| **strided** — `ScatterBlock` | every column fixed-width **and** no NULLs anywhere | one kernel call per column per block |
| **cursor** — `CursorBlock` | any variable-length column **or** any NULL in a fixed-width column | one call per column for direct-copy/convert arms; **one call per VALUE** for decimal/uuid/datetime |
| **row-major** — `EncodeRow` | any column resolves to `RowFallback` | no kernels at all, for any column |

The literal condition is `if (all_valid && !has_variable)`. Two consequences
that surprise people:

1. **One NULL sends the chunk to the cursor path.** No string column needed.
2. **One `RowFallback` column sends the whole chunk row-major** — every other
   column with it. A column type nobody thought about does not cost its own
   column, it costs the table. This is not hypothetical: `money` declaring a
   width nothing writes took a 44-column encode from 190 ms to 394 ms, and the
   cost was paid by the other 42 columns.

## The arms

`ScatterArm` (`write_column_ops.hpp`) is the whole vocabulary:

| arm | payload |
|---|---|
| `NullOnly` | source column absent — one NULL marker per row, no payload |
| `DirectCopy1/2/4/8` | the wire bytes ARE what the vector stores; a memcpy of N bytes |
| `IntConvert` | source width ≠ target width: read at source width, **range-check**, write at target width |
| `FloatConvert` | FLOAT↔DOUBLE width change. Its own arm because narrowing here is **not** an error — precision loss is what the declared column asked for |
| `Decimal` | sign byte + little-endian magnitude, width from the target's precision. MONEY/SMALLMONEY arrive here |
| `Datetime` | every temporal target: DATE, the DATETIME2 family, TIME, DATETIMEOFFSET, and DATE widened into DATETIME2 |
| `Guid` | the 16 bytes of a UNIQUEIDENTIFIER, mixed-endian per the spec |
| `VarString` | 2-byte-length string or binary. Payload width is per value, so a chunk with one can never be strided |
| `RowFallback` | encodable, but not by a scatter |
| `Unsupported` | no kernel for this pair — **this is the compatibility answer** |

That last row is the design's main structural idea. Compatibility used to be a
separate table of type *names* (`IsTypeCompatible`), and it disagreed with what
the encoders could actually execute: every widening the table advertised died in
the encoder, while a decimal scale mismatch was waved through and silently moved
the decimal point (issue #153). Making compatibility mean `arm != Unsupported`
is what stops a conversion being advertised that cannot be performed.

## The invariants to be suspicious of

A reviewer's checklist, in the order these have actually broken:

1. **The width the kernel WRITES must equal the width the chunk was sized
   from.** The scatter reserves the row layout first, so a kernel writing a
   different number of bytes puts every *later* column of that row where the
   server is not looking. The row path never had this constraint, because it
   appends — whatever it writes *is* the layout. `datetime`, `datetime2` and
   `smalldatetime` share one wire type and one `duckdb_type`, so the width is
   the only thing separating them; that is why every temporal case checks
   `max_length` instead of assuming it.
2. **A width alone does not determine what a value means.** Same-width pairs are
   where the sign bugs lived: `UBIGINT -> bigint` took the direct-copy arm and
   stored -1, and a round-trip range check cannot catch it because *every* value
   of a same-width unsigned/signed pair passes one. Likewise `decimal(19)`,
   whose wire magnitude is unsigned 8 bytes and reaches 10^19-1, while the kernel
   picked its arithmetic type from the target's byte size.
3. **The two paths must produce identical bytes.** They are separate
   implementations for the kernel arms, so this is asserted by tests rather than
   guaranteed by structure. `TIME` rounded on one and truncated on the other;
   `varbinary(n)` was bounded on one and not the other. Both were invisible until
   a test loaded the same values down both paths.
4. **Conservatism is not free.** Refusing a pair sends the whole chunk
   row-major. `UTINYINT` was swept into an unsigned-source refusal that was right
   for the wider types, and because the catalog maps SQL Server `tinyint` to
   UTINYINT, one such column sent entire tables row-major.

## What this branch changed, file by file

| file | code | what to read it for |
|---|---:|---|
| `codec/write_column_ops.{cpp,hpp}` | 447 | **Start here.** The resolver: one function mapping (source type, target column) to an arm. Everything downstream trusts its answer |
| `tds/encoding/bcp_row_encoder.cpp` | 658 | Row assembly and the three-way path choice. `ScatterBlock` / `CursorBlock` are the two layouts; `TryEncodeChunkColumnar` is where a chunk's fate is decided |
| `codec/decimal_codec.cpp` | 336 | Sign/magnitude kernel. The `decimal(19)` sign bug was here |
| `codec/datetime_codec.cpp` | 388 | Temporal kernels. Rounding, the end-of-day saturation for TIME, and the date carry |
| `codec/string_codec.cpp` | 372 | `PlanColumn` (measure the column once, keep the per-value wire length) and the bound-clamping that truncates on a character boundary |

## Weak points, stated rather than hidden

- **The "one implementation" property is partial.** It holds for the direct-copy
  and convert arms, which are templates instantiated in one place. It does *not*
  hold for decimal / uuid / datetime / string, where the row-major encoder has
  its own code. That gap is exactly where TIME and `varbinary(n)` diverged, and
  the byte-equality tests exist because the structure does not enforce it.
- **The cursor path still calls decimal/uuid/datetime kernels per value**,
  across a translation-unit boundary, so those calls cannot be inlined. Measured
  at +0.23 s CPU per 16M values against a `bigint` control that costs nothing.
  Design for closing it — the kernels need no NULL branch at all — is in
  [`proposals/columnar-write-close-the-gaps.md`](proposals/columnar-write-close-the-gaps.md).
- **`HUGEINT` and `UBIGINT` columns take the whole table row-major**, though both
  travel as DECIMAL on the wire where a kernel already exists. `SUM()` over
  integers returns HUGEINT, so this is an ordinary shape, not a corner.

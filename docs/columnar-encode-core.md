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
| **row-major** — `EncodeRow` | any column resolves to `RowFallback`, **or** `string::PlanColumn` fails on a variable column (`bcp_row_encoder.cpp:655`) | no kernels at all, for any column |

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
| `VarString` | string or binary of any framing — the 2-byte-length form **and** `nvarchar(max)`/`varchar(max)`/`varbinary(max)` under PLP's 8-byte framing, which resolve to the same arm because the plan owns the difference. Payload width is per value, so a chunk with one can never be strided |
| `RowFallback` | encodable, but not by a scatter: a source rendered as text, a HUGEINT/UBIGINT column with generated metadata, a family mismatch (non-int storage into DECIMAL, TIMESTAMP into DATE, non-UUID into `uniqueidentifier`), a width disagreeing with COLMETADATA, and the two deliberate refusals |
| `Unsupported` | no kernel for this pair — the compatibility answer this design is *aiming* at; see below |

That last row is the design's main structural idea, and it is **not implemented
yet** — the header is careful to say compatibility is "meant to become"
`arm != Unsupported`, and this guide should be equally careful.

Where it actually stands: `IsTypeCompatible` (a table of type *names*,
`target_resolver.cpp:648`) is still the gate, called at `:787`. And
`ResolveWriteColumnOps` can never return `Unsupported` today — the only producer
is `DirectCopyArm`'s `default:` (`write_column_ops.cpp:59`), reachable only for a
width outside {1,2,4,8}, which the caller has already excluded. Every other exit
assigns a concrete arm.

The motivation is real regardless: the names table disagreed with what the
encoders could execute for two releases — every widening it advertised died in
the encoder, while a decimal scale mismatch was waved through and silently moved
the decimal point (issue #153). Collapsing the two is what would stop an
unexecutable conversion being advertised.

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

   And the tests that do it need their **lever** checked, not just their
   assertions: four of them used a fixture believed to force the row path that
   in fact resolved columnar, so they compared the columnar path with itself and
   passed for the wrong reason. If you add one, instrument the path choice once
   and confirm the row-major branch is actually taken. An assertion that cannot
   fail is worse than no assertion, because it is counted.
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
- **`UBIGINT` columns take the whole table row-major**, though they travel as
  DECIMAL on the wire where a kernel already exists — its `UINT64` source is not
  in the decimal arm's accepted set. `HUGEINT` is row-major only when the
  metadata is GENERATED (CTAS, or COPY with `REPLACE`), where `duckdb_type` is
  the source type and `DirectCopyTargetWidth` returns 0. Loading into an
  **existing** `decimal(38,0)` it resolves to `ScatterArm::Decimal`: the catalog
  reports the column as DECIMAL, `INT128` is in the accepted set, and
  `max_length` already equals `GetDecimalByteSize(38)`.

  That distinction is not academic — it invalidated four tests. They used a
  `decimal(38,0)` column fed `1::HUGEINT` **into an existing table** to force the
  row path, so they resolved columnar and compared the columnar path with itself.
  Verified by instrumenting the path choice. The working lever is a signed
  `TINYINT` source into a SQL Server `tinyint`, which is unsigned and therefore
  cannot be a byte copy.

- **`INTERVAL` into an `nvarchar` target raises an INTERNAL Error.** The
  resolver's comment says such a "render-as-text" source keeps the row path,
  "which formats it first" — it does not: `string::EncodeToBcp` reads the
  INTERVAL vector as VARCHAR and throws `Expected unified vector format of type
  VARCHAR, but found type INTERVAL`. Reproducible on this branch; nothing tests
  it. Needs its own issue.

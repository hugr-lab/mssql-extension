# Spec 064 — Vectorise the write path, and map what can be converted at all

Follows spec 063 (PR #240), which made COPY and CTAS one writer. This is the
kernel work: **move everything that can be a per-column kernel call onto one, and
reach the row-major path only for pairs that genuinely cannot be encoded.**

Predecessor proposal:
[`columnar-write-close-the-gaps.md`](../../docs/proposals/columnar-write-close-the-gaps.md).
**Its premise needs correcting, and §1 is why.**

## 1. Reconnaissance — the fast path is unreachable in production

Measured 2026-08-05 on this branch, 500 000 rows × 20 columns covering every
family, median of two runs. The encode path is chosen **per chunk** and was not
reported by anything; this branch adds `encode path (chunks): strided / cursor /
row_major` to `MSSQL_COUNTERS=1` output, which is how the table below exists.

| NULLs | strings | threads | wall (s) | strided | cursor | row-major |
|---|---|---:|---:|---:|---:|---:|
| none | plain | 1 | 3.91 | **0** | 245 | 0 |
| none | plain | 4 | 1.38 | **0** | 245 | 0 |
| none | sized | 1 | 2.17 | **0** | 245 | 0 |
| none | sized | 4 | 0.93 | **0** | 245 | 0 |
| some | plain | 1 | 3.94 | **0** | 245 | 0 |
| some | plain | 4 | 1.72 | **0** | 245 | 0 |
| some | sized | 1 | 2.18 | **0** | 245 | 0 |
| some | sized | 4 | 0.70 | **0** | 245 | 0 |

**Not one chunk took the strided path, in any configuration.** The selector is
literally `if (all_valid && !has_variable)`, and `has_variable` is set by ANY
variable-length column. A control isolates it:

| table | strided | cursor |
|---|---:|---:|
| 5 fixed-width columns (bigint, int, decimal, date, uuid) | **98** | 0 |
| the same 5 **plus one nvarchar(50)** | **0** | 98 |

So the strided kernel — the thing spec 057 built and measured — is reachable only
by a table with **no string, binary or MAX column at all**. A realistic wide table
has one, and then every column in every chunk is encoded through the cursor,
including the ones that were fixed-width and NULL-free.

**This reorders the predecessor proposal.** It leads with "one NULL sends the
chunk to the cursor path" and treats the per-value decimal/uuid/datetime calls as
a secondary cost. The NULL axis is in fact *invisible* here — compare the `none`
and `some` rows above: the path was already cursor, so NULLs changed nothing. The
per-value calls are not a corner case; **they are what production pays on every
load.**

### What the other two axes did move

| lever | effect | note |
|---|---|---|
| threads 1 → 4 | ×2.3 – ×3.1 | server-side ingest; plateaus past 4 (spec 057) |
| `MSSQL_NVARCHAR(n)` vs unannotated | ×1.5 – ×2.5 | sized column instead of `nvarchar(max)` |
| NULLs present vs absent | none measurable | the path was already cursor |

Wall clock only. The phase totals `MSSQL_COUNTERS` prints are summed across
threads and rise with thread count while wall time falls. Absolute seconds are
not portable: this server runs under emulation.

## 1a. Strings decide more than the kernels do — and the fast path already exists

Measured 2026-08-05, 1M rows x 2 columns, threads=4, median of two. Same data,
same statement; only the TARGET column type differs.

| target | wall | encode (summed over threads) |
|---|---:|---:|
| `nvarchar(max)` — today's default | 0.73 s | 1.13 s |
| `nvarchar(50)` | 0.37 s | 0.71 s |
| `varchar(max)` **UTF-8 collation** | 0.71 s | 0.38 s |
| `varchar(50)` **UTF-8 collation** | **0.23 s** | **0.29 s** |

**Sizing and UTF-8 are independent levers acting on different bottlenecks.**
UTF-8 cuts client encoding by ~3x (1.13 → 0.38 s) because a UTF-8-collated
`varchar` takes the DuckDB bytes unchanged — no UTF-16 transcode — which is the
write-side mirror of what issue #225 did for reads. But `varchar(max)` is still
0.71 s of wall clock: MAX-ness is paid on the SERVER and does not care how cheap
the encode was. Together, `varchar(n)` + UTF-8 is **3.2x the wall clock** of the
default.

**And all of this works today.** Verified against the live server rather than
assumed:

| case | result |
|---|---|
| plain `VARCHAR`, `mssql_ctas_text_type = NVARCHAR` (default) | `nvarchar(max)`, CP1 collation |
| plain `VARCHAR`, `mssql_ctas_text_type = VARCHAR` | `varchar(max)` + `Latin1_General_100_CI_AS_SC_UTF8` |
| `x::MSSQL_VARCHAR(40)` | `varchar(40)` + UTF-8 collation, **under either setting** |
| `x::MSSQL_NVARCHAR(30)` | `nvarchar(30)`, CP1 — the annotation is honoured, including asking for UTF-16 |

So the annotation is respected, the collation is applied, and the byte
pass-through exists (`Utf8PrefixForByteLimit`, "the payload is the source
itself"). **The gap is not implementation — it is the default.** A user who
writes nothing special gets `nvarchar(max)`, the slowest cell in the table above,
and nothing tells them the other three exist.

That reframes a deliverable: the biggest available write-path win needs no
kernel work at all.

### D0 — the default text type, and the collation it names

`varchar(n)` + UTF-8 measured 3.2x today's `nvarchar(max)` default (§1a). Two
separate questions came out of it.

**Should NVARCHAR stop being the default? No.** A UTF-8-collated column cannot be
compared with a non-UTF-8 one — measured, `nvarchar` × `varchar` UTF-8 gives SQL
Server error 468 — so switching would break joins against existing tables in any
database that is not itself collated UTF-8. Inside a UTF-8 database there is no
conflict at all, including against `nvarchar`. So the 3.2x is available and
conflict-free exactly when the TARGET DATABASE is collated UTF-8; that belongs in
the documentation, not in a changed default.

**Which collation should we name when we do name one? BIN2** (decided by the
user, 2026-08-05). `MSSQL_DEFAULT_UTF8_COLLATION` becomes
`Latin1_General_100_BIN2_UTF8`, matching what Fabric Warehouse uses as its own
default. Binary comparison: no linguistic rules, no case- or accent-folding, no
locale table — the fastest answer for comparison and sort.

The cost is stated rather than hidden: BIN2 is case- and accent-SENSITIVE, so
`WHERE name = 'abc'` stops matching `'ABC'` on a column created this way. That is
why it is a default and not a rule — a column needing different semantics says so
per column.

**And it removes a reason the old code had to work around the old default.**
Fabric accepts exactly two collations (`LATIN1_GENERAL_100_BIN2_UTF8`,
`LATIN1_GENERAL_100_CI_AS_KS_WS_SC_UTF8`). The previous default was neither, so
naming it on Fabric would have been refused. BIN2 is one of them.

### The resolution order, which was already correct

A VARCHAR column's collation comes from, in order:

1. **the type annotation** — `MSSQL_VARCHAR(n, 'collation')`. Verified to
   outrank everything: `FormatTargetStringDdl` uses `spec.collation` when it is
   set and only then falls back;
2. **`mssql_utf8_collation`**, the setting;
3. **the database** — by emitting no `COLLATE` clause so the column inherits.

I briefly rewrote `ResolveVarcharCollation` to make the setting beat the database
unconditionally, on the grounds that "the setting should win". That was wrong and
the user caught it: on a UTF-8 database, imposing the setting's collation creates
a column in a DIFFERENT collation from everything around it, which is the error
468 measured above. Inheriting is the only conflict-free answer there, and the
existing code already did it. Reverted.

## 2. The question this spec has to answer twice

For every (DuckDB type, SQL Server type) pair there are **three** different
questions, and the code answers them in three unrelated places:

1. **Is it allowed?** `IsTypeCompatible` (`target_resolver.cpp:647`) — a table of
   type NAMES.
2. **Can it be vectorised?** `ResolveWriteColumnOps` (`codec/write_column_ops.cpp`)
   — returns a `ScatterArm`, or `RowFallback`.
3. **Can it be encoded at all?** the row-major encoder — and where the answer is
   no, the failure is an INTERNAL Error rather than a refusal (issue #238).

`write_column_ops.hpp` already says compatibility is *meant to become*
`arm != Unsupported`, and that it is not yet. Collapsing 1 into 2 is the
structural half of this spec; making 3 never surprise anyone is the correctness
half.

> **Closure note.** The correctness half shipped, in two steps. D5 first made
> the unknown-pair failure a bind-time refusal; review then moved it to FIRST
> DATA, because bind cannot tell a `NULL AS col` source from real data (DuckDB
> types a bare NULL as INTEGER before the extension sees it) and filling a
> column with NULLs is a pair-independent, legitimate thing to do. The rule
> now: an incompatible matched pair is admitted for the all-NULL case, the
> encoder verifies the mask per chunk, and a value raises the same
> type-mismatch error with the pair named — before any of that chunk is
> encoded. The structural half — collapsing 1 into 2 — did NOT ship in this
> branch and is recorded as deferred in
> `docs/proposals/columnar-write-close-the-gaps.md`: it needs the resolver to
> build `WriteColumnOps` at bind time, which it cannot until the source Vector
> types are known there.

## 3. Type compatibility map — as of RECONNAISSANCE

Extracted from `IsTypeCompatible`, not from documentation, **before any change
on this branch** — several ❌/⚠️ cells below are exactly what D1/D4/D5 then
fixed; the postscript after the map says where each landed. **"Vectorised?" is
the column this spec is about**, filled in from `ResolveWriteColumnOps` plus
the measurements in §1; every cell marked *(unverified)* still needs a probe.

| DuckDB source | SQL Server targets allowed | wire form | vectorised today |
|---|---|---|---|
| `BOOLEAN` | `bit` | 1 byte | ✅ DirectCopy1 |
| `TINYINT` (signed) | `tinyint`, `smallint`, `int`, `bigint` | int | ⚠️ into `tinyint`: **RowFallback by design** — SQL Server's `tinyint` is unsigned, so the row path range-checks per value. Widening targets vectorise |
| `UTINYINT` | same | int | ✅ DirectCopy1 into `tinyint`, IntConvert wider |
| `SMALLINT` / `INTEGER` / `BIGINT` | `tinyint` … `bigint` (both directions) | int | ✅ DirectCopy / IntConvert with a range check |
| `UBIGINT` | `bigint` only | **decimal(20,0)** | ❌ RowFallback — travels as DECIMAL but the decimal arm accepts signed sources only, and the generated metadata's `max_length` disagrees with `GetDecimalByteSize(20)` |
| `HUGEINT` | `decimal`, `numeric`, `money`, `smallmoney` | decimal | ⚠️ **depends on where the metadata came from**: into an EXISTING `decimal(38,0)` it resolves to the Decimal arm; with GENERATED metadata (CTAS / `REPLACE`) it is RowFallback |
| `FLOAT` / `DOUBLE` | `real`, `float` | 4/8 bytes | ✅ DirectCopy / FloatConvert (narrowing is not an error here) |
| `DECIMAL` | `decimal`, `numeric`, `money`, `smallmoney` | sign + magnitude | ✅ Decimal arm — **but per VALUE on the cursor path**, which §1 shows is every load |
| `VARCHAR` | `varchar`, `nvarchar`, `char`, `nchar`, `text`, `ntext`, `xml` | 2-byte len or PLP | ✅ VarString, planned once per column — and it is what makes every chunk cursor |
| `BLOB` | `varbinary`, `binary`, `image` | 2-byte len or PLP | ✅ VarString |
| `UUID` | `uniqueidentifier` | 16 bytes, mixed-endian | ✅ Guid arm — **per VALUE on the cursor path** |
| `DATE` | `date`, `datetime`, `datetime2`, `smalldatetime` | 3 bytes / widened | ✅ Datetime arm — **per VALUE on the cursor path** |
| `TIME` | `time` | 3/4/5 bytes | ✅ Datetime arm, same caveat |
| `TIMESTAMP` + `_MS` / `_NS` / `_SEC` | `datetime2`, `datetime`, `smalldatetime` | 6/7/8 bytes | ✅ Datetime arm, same caveat |
| `TIMESTAMP_TZ` | `datetimeoffset` | datetime2 + 2-byte offset | ✅ Datetime arm, same caveat |
| `INTERVAL` | *(falls to `default: return true`)* | rendered as text | ❌ **INTERNAL Error — issue #238.** The resolver says the row path "formats it first"; it does not |
| `LIST` / `STRUCT` / `MAP` | *(falls to `default: return true`)* | — | ❓ unverified — `IsTypeCompatible` says yes to everything it does not know |

**Where the branch left each cell** (the map above is the before-picture):

- `UBIGINT` → ✅ Decimal arm, both metadata sources — D4 fixed the generated
  `max_length` (9 → 13, and the duplicate in `GetTDSMaxLength`) and admitted
  `UINT64` sources. The review's added test then showed the existing-target
  half was DEAD: `IsTypeCompatible` still allowed `bigint` only, so the column
  CTAS itself creates from a UBIGINT source was unappendable. Allowed targets
  grew to the decimal family, and the row path's widening gained the UINT64
  arm the mixed-chunk fallback needs.
- `HUGEINT` → ✅ Decimal arm with generated metadata too (same D4 gate).
- `DECIMAL` / `UUID` / `DATE` / `TIME` / `TIMESTAMP*` — the "per VALUE on the
  cursor path" caveat is gone: D1 gave the kernels a position policy and the
  cursor path calls them once per column per block.
- `INTERVAL`, `LIST`/`STRUCT`/`MAP`, and everything else unknown → refusal
  naming the pair (D5, closes #238's error shape and #153) — raised at FIRST
  DATA rather than at bind, because any pair whose source column is entirely
  NULL is legitimate: `NULL AS col` reaches bind already typed, so the
  distinction only exists once the mask is visible. The all-NULL case routes
  through the missing-column NullOnly machinery (`null_only_columns.test`
  pins all three marker kinds on all three encode paths).
- Fixing that exposed a PRE-EXISTING columnar bug: the NullOnly arm wrote the
  one-byte 0x00 marker for every column kind, but a USHORT-length NULL is
  FF FF and a PLP NULL is eight 0xFF — an absent nvarchar/varbinary column
  corrupted the row framing whenever the chunk stayed columnar. Hidden until
  now because such chunks usually carried a real variable column too and went
  row-major, where the row path's marker writer always knew the kinds.
- `VARCHAR` → `xml` verified working end to end (the map row always allowed
  it; `xml_copy_bcp.test` pins it, Unicode included). GEOMETRY as a SQL Server
  `geometry` target — as opposed to WKB into varbinary, which works — is a
  separate PR.
- `GEOMETRY` → ✅ VarString into `varbinary`/`binary`/`image` — review-caught:
  the payload is standard WKB (verified byte-identical to `ST_AsWKB`), the pair
  worked for years through `default: return true`, and D5's refusal would have
  regressed it. Explicit case added.
- `UHUGEINT` → refused at bind, deliberately: the decimal widening has no
  UINT128 arm, so advertising the pair (as an early D5 draft did) reintroduces
  the INTERNAL error D5 exists to remove. Support is a close-the-gaps item.

**Two structural problems this map makes visible**, both worth more than any
individual cell:

- **`default: return true`.** An unknown source type is declared compatible with
  every target. That is how `INTERVAL` reaches an encoder that throws instead of
  being refused at bind time, and it means nested types are "allowed" with no
  evidence anyone tried.
- **The same pair answers differently depending on whether the target already
  existed**, because generated metadata and catalog metadata differ. HUGEINT is
  the known case; the map cannot claim it is the only one without a probe.

## 4. What "as much as possible vectorised" means concretely

Ordered by what §1 says it is worth, which is NOT the order the predecessor
proposal used:

### D1 — a cursor entry point for decimal / uuid / datetime  *(the main win)*

These three families call a kernel **per value** across a translation-unit
boundary on the cursor path, and §1 shows the cursor path is every real load.
Template the existing kernel on a position policy (`StridePos` / `CursorPos`) so
both entry points instantiate the same body in the same TU and keep inlining —
one implementation, which is what `write_column_ops.hpp` already claims.

Measured cost of the current shape (spec 057, 2M rows × 8 columns): decimal
0.163 s → 0.396 s CPU when the cursor path is forced, against a `bigint` control
that costs nothing because its arm is inlined in the same TU.

### D2 / D3 — CLOSED by measurement: after D1 the cursor path costs the same as strided

Both deliverables existed to move chunks from the cursor layout onto the strided
one. Measured after D1 (2026-08-05, same build, same 8 x decimal(18,4) x 2M
fixture, the ONLY difference being a NULL every 1000th row that flips the layout,
interleaved, 4 reps):

    strided medians 416 ms encode, cursor 413 ms — equal within noise,
    pairwise 3:1 for the CURSOR.

The whole strided-vs-cursor gap was the per-value call overhead, and D1 already
recovered it (decimal −42%, datetime −23%, 5/5 pairs each). What remains between
the layouts — the cursor array reads and the sizing pass — does not measure.

So:

- **D2 (NULLs onto strided)**: nothing left to recover. Its literal mechanism —
  kernels write payload unconditionally, the garbage is overwritten — is also
  UNSOUND on the cursor layout as proposed: passes are column-major, so the last
  column's garbage for a NULL row lands in the NEXT row's already-written bytes,
  not in a later pass's path. Recorded so nobody builds it.
- **D3 (stride for string-bearing chunks, incl. reserving a bounded string's
  declared width)**: the fixed-width columns such a chunk contains now encode at
  strided cost already. Reserving width would spend wire bytes to buy nothing
  that measures. The fallback the spec named — "accept the cursor and make D1
  complete" — is the outcome.

Re-measured across NULL DENSITY after D4/D5 (same fixture, three variants
interleaved, 4 reps): no NULLs (strided) 230 ms median, 0.1% NULLs (cursor) 245,
33% NULLs (cursor) 255 — every pairwise comparison a 2:2 tie against run spread
of up to 80%. Density does not measure: a NULL row writes no payload, only its
0x00 marker, which roughly refunds the cursor bookkeeping. A column that is
nullable but holds no NULLs is not a separate case — the path is chosen from the
ACTUAL mask (`AllValid()`), not from declared nullability, so it takes strided
by construction.

The stated goal — offsets known, NULLs written where the mask says, directly
into the wire buffer — is the shipped design after D1: the kernels write 0x00
straight into the frame, offsets come from the masks without reading a value,
and payloads are written for valid rows only.

Acceptance criterion 1 is satisfied by its second branch: the strided count for
string-bearing cells stays 0, with this measurement as the reason.

Host noise on this machine is brutal — identical work measured 266–745 ms across
reps — so only interleaved pairwise comparison decided anything here; medians of
interleaved runs are quoted, absolutes are not portable.

### D4 — shrink what reaches row-major

`UBIGINT` (settle the `max_length` vs `GetDecimalByteSize(20)` disagreement, add
a UINT64 → hugeint widening), and `HUGEINT` with generated metadata. Neither
appears in §1's matrix because the fixture avoided them — **that is itself worth
recording: the row path was never taken in 245 chunks × 8 cells.** For ordinary
types it is already rare.

### D5 — compatibility becomes one answer

Collapse `IsTypeCompatible` into `arm != Unsupported`, and delete
`default: return true` so an unknown source is refused rather than admitted.
Fix issue #238 as part of it: `INTERVAL` into `nvarchar` either formats or is
refused at bind time, but never throws an INTERNAL Error from the encoder.

## 5. Acceptance criteria

1. The wide-table matrix of §1 re-runs with the same fixture, and the strided
   count is no longer 0 for the string-bearing cells — or D3 is closed with a
   measurement showing why it should not be.
2. Decimal / uuid / datetime cost no more on the cursor path than on the strided
   one, measured the way spec 057 measured it (isolate with NULLs, `bigint`
   control).
3. Byte-equality between paths per family, as sections of
   `test/sql/copy/both_paths_agree.test` — **and its section 0 lever still
   forces the row path.** If D4 makes signed `TINYINT` → `tinyint` columnar, that
   file fails loudly and needs a NEW lever, not a deleted section 0.
4. `IsTypeCompatible` has no `default: return true`.
5. Issue #238 raises a bind-time error or succeeds; never an INTERNAL Error.
6. No throughput regression on the §1 matrix, measured interleaved.

## 6. Risks

- **D3 may not be worth it.** Interleaving fixed-width regions with variable ones
  could cost more in cache behaviour than the per-value dispatch it removes. The
  deliverable is a measurement, and "we measured and kept the cursor" is a valid
  outcome.
- **The wire is the bound, not the client.** Every spec-057 measurement said the
  server's ingest rate dominates; §1's wall-clock differences come mostly from
  the string sizing and the writer count, not from encode. Client CPU is worth
  reducing on its own terms — it matters when the server is faster than the
  client, or DuckDB is doing other work — but this spec should not promise wall
  clock.
- **`default: return true` may be load-bearing.** Removing it can refuse pairs
  that work today by accident. It needs the probe of §3 before deletion, not
  after.

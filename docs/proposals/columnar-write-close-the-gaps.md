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

Requires byte-equality tests between the two paths per family. **That merge has
happened**: `test/sql/copy/both_paths_agree.test` is now the single file, with a
section per family and one lever — add the new family as a section there.

Its **section 0 asserts the lever** rather than describing it: `-1::TINYINT` into
`tinyint` must raise an out-of-range error, which only the row encoder produces.
Verified to bite — disabling the INT8 -> UTINYINT fallback in the resolver fails
that assertion on assertion 4, before any section can pass for the wrong reason.

**So if this PR makes signed `TINYINT` -> `tinyint` columnar, that file fails
loudly instead of going vacuous.** When it does, the answer is a NEW lever, not a
deleted section 0. Nothing else in the suite currently forces the row path.

### 3. Shrink what reaches row-major at all

Two ordinary column types send the WHOLE table row-major today, and both already
travel as DECIMAL on the wire where a kernel exists:

- **HUGEINT — but only with GENERATED metadata**, which is the correction that
  came out of @oluies' review. Loading into an **existing** `decimal(38,0)` it
  ALREADY resolves to `ScatterArm::Decimal`: the catalog reports the column as
  DECIMAL, INT128 is in the accepted source set, and `max_length` equals
  `GetDecimalByteSize(38)`. What is still row-major is CTAS / `REPLACE`, where
  `GenerateColumnMetadata` keeps `duckdb_type = HUGEINT` so
  `DirectCopyTargetWidth` returns 0. Worth doing — `SUM()` over integers returns
  HUGEINT, so aggregate-then-create is a common shape — but the win is narrower
  than first written.

  Consequence already dealt with (`b6c21d3`): four tests used HUGEINT into an
  existing `decimal(38,0)` believing it forced the row path. It did not, and they
  compared the columnar path with itself. The lever is now a signed `TINYINT`
  into SQL Server's unsigned `tinyint`. **Verify any new both-paths test by
  instrumenting the path choice, not by assuming the fixture works.**
- **UBIGINT** — needs an inconsistency resolved first. `GenerateColumnMetadata`
  sets `precision = 20, max_length = 9`, but `GetDecimalByteSize(20)` is **13**,
  so the resolver's `target.max_length == width` check cannot pass. Verified
  end-to-end on 2026-08-04 that UBIGINT loads correctly today (0, 1, 2^63-1 and
  2^64-1 all round-trip through `decimal(20,0)`), so this is not a bug to fix —
  it is a disagreement to settle before the column can be kernel-eligible. Also
  needs a UINT64 -> hugeint widening step, since the decimal arm accepts signed
  sources only.

Everything else on the `RowFallback` list is a deliberate refusal (unsigned into
a signed target, signed into `tinyint`) or a conversion the row path is supposed
to perform. Those are the "hard cases" the cursor and row paths should be
reserved for.

**Except one, which does not work at all: `INTERVAL` into an `nvarchar` target
raises an INTERNAL Error.** `write_column_ops.cpp` says such a render-as-text
source "keeps the row path, which formats it first"; it does not —
`string::EncodeToBcp` reads the vector as VARCHAR and throws *"Expected unified
vector format of type VARCHAR, but found type INTERVAL"*. Pre-existing, nothing
covers it, and it is the one RowFallback case that is a bug rather than a
decision. Fix it here, since this is the PR that reasons about what reaches the
row path.

## Carried over from spec 063 (the writer merge), which is now done

Three things that spec could not close, each of which this one is better placed
to:

### A signal for "how many writers actually ran"

Spec 063 tried to assert concurrency from SQL and could not. The available
signal — `mssql_pool_stats(...).connections_created` — does not discriminate: the
count is 2 even single-threaded, because the operator opens the global writer's
connection in its global state and the one sink thread then claims a session of
its own on top of it. Measured threads=1 → 2, threads=4 → 3, so only a
scheduling-dependent number separates them and an assertion on it would flake.

The counters DO know (`writers=N/M`, added in D5) but they go to stderr. Two
tests therefore state in prose what they cannot check, and one acceptance
criterion went unmet:

- `parallel_writers.test` / `ctas_parallel_writers.test` assert that
  `mssql_copy_parallel_writers = 1` really disables the feature (exactly one
  connection created) and say plainly that the parallel section's concurrency is
  NOT verified;
- **unverified: a COPY into a `#temp` target with `threads > 1` opens exactly one
  bulk-load session.** That is the case `MSSQLResolveLoadPolicy` was written for,
  and with `mssql_reset_connection = false` it is the difference between wasted
  round trips and rows landing in a stale same-named table silently.

A table function exposing the last statement's writer count — or any
SQL-reachable form of the counters — would close all three at once.

### `INTERVAL` into `nvarchar` is issue #238

Filed while spec 063 was open. Still the one `RowFallback` entry that is a bug
rather than a decision, and still uncovered.

### The version-matrix script cannot run on macOS

`test/compat/mssql_version_matrix.sh` (merged as #232) fails to PARSE under
bash 3.2, which is stock macOS `/bin/bash`; bash 5 parses it cleanly. Documented
in `docs/TESTING.md` with two workarounds, but not fixed — the offending
construct was not located, and every bisection attempt cut into the SQL heredoc
and unbalanced its quotes, so each intermediate answer was an artifact of the
method. Whoever next touches that script should find it properly.

## Not in scope, and it comes first

Deduplicating the parallel-writer machinery between COPY and CTAS, plus the test
consolidation that goes with it:
[merge-writer-machinery-and-tests](merge-writer-machinery-and-tests.md).

**That work is done** — spec 063, branch `spec/063-writer-merge`. COPY and CTAS
now share `mssql::BulkLoadSession` and one `INSERT BULK` builder, so this PR
modifies ONE writer implementation, and the both-paths tests are one file rather
than three. Measured after it: CTAS and COPY differ by 0.0–2.6% in every cell of
a 2x2 matrix (1 vs 4 threads, annotated vs unannotated types) against a
run-to-run spread of up to 9%.

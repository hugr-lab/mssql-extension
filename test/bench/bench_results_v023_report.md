# v0.2.2 → v0.2.3: final performance report

Phase-R closing report for `docs/proposals/simd-chunk-materialization-design.md`:
what actually changes for a user upgrading community `mssql` 0.2.2 → 0.2.3,
measured end-to-end on one wide real-shape table. Two full interleaved rounds;
every number below is the midpoint of two runs unless marked otherwise
(round-to-round spread was ±1–8%).

## Methodology

- **Fixture**: `TestDB.dbo.F38Heap` — 44 columns × 38,000,000 rows, 23.1 GB heap
  (all type families: tinyint…bigint, real/float, bit, decimal(4,2)…(38,10),
  decimal(19,4)/(10,4), date, datetime2×5, time(3), datetimeoffset,
  uniqueidentifier, 8×nvarchar(200) — incl. 100%-Cyrillic and 100%-CJK columns —
  2×varbinary(max), NULL-pattern columns with 1%/50%/90%/100% NULLs). For write
  tests the fixture is a parquet export of that table (4.7 GB); both binaries
  read parquet through the identical DuckDB core.
- **Baseline**: stock DuckDB CLI v1.5.5 (`d8cdaa33fd`) + `INSTALL mssql FROM
  community` = **0.2.2**. **Candidate**: locally built CLI at the same DuckDB
  SHA with the 058-tip extension statically linked (= what ships as 0.2.3).
  Same core SHA ⇒ the comparison isolates the extension.
- **Timing**: `.timer on` wall clock, `MSSQL_COUNTERS` off, fresh process per
  run, interleaved old/new with order rotated between rounds. Client-side CPU
  cost measured separately with `/usr/bin/time -l` (instructions retired,
  cycles, RSS, syscall counts) because the CLI timer's user/sys readings
  proved unreliable for multi-threaded phases on macOS (see Honesty notes).
- **Environment**: single machine — macOS ARM64 client, SQL Server 2022 in
  Docker (linux/amd64 under Rosetta) on the same box, TestDB SIMPLE recovery.
  Absolute numbers are conservative (client and server share the box and the
  server runs emulated); ratios are the deliverable.

## Findings

### F0. v0.2.2 segfaults on every real columnar source at scale — fixed

`COPY (FROM read_parquet(...)) TO 'db.dbo.T' (FORMAT bcp, ...)` on 0.2.2 dies
with SIGSEGV in `BCPRowEncoder::EncodeBinaryPLP` → `memmove`, faulting address
`0x626262…5f` — string *content* bytes dereferenced as a `string_t` pointer.
The old row encoder indexed vectors as if flat; at full scale the parquet
reader emits **dictionary vectors** and the assumption breaks. A native DuckDB
table source crashes identically (DuckDB storage also emits dictionary
vectors); a 100k `LIMIT` hides it (small reads come out flat), and the crash
appears between 1M and 5M rows. So on 0.2.2, no real columnar source could
feed a full-scale bulk load at all — only generated/flat input survived.

The spec-057 columnar encode rewrite normalizes every input through
`ToUnifiedFormat` and completes all of these statements. Guarded in CI by
`test/sql/copy/vector_encodings_bcp.test` (dictionary vectors from parquet and
from native storage, constant vectors, content round-trip both directions) —
15 assertions, green. Baseline full-scale write numbers below use a
44-column *generated* source of the same shape (flat vectors — the one thing
0.2.2 survives), priced against parquet by a bridge run: the generated source
costs the candidate ×1.029 wall, so the substitution does not flatter either
side.

### F1. Write — the upgrade, like for like

Same statement, same generated 38M×44 source, single writer, strings as
NVARCHAR(MAX) on both sides:

| operation | 0.2.2 | candidate @1 writer | speedup |
| --- | --- | --- | --- |
| COPY | 933.2 s | 696.6 s | **1.34×** |
| CTAS | 942.7 s | 699.4 s | **1.35×** |

Both ends are bounded by the (Rosetta) server's single-session ingest — this
row is the *floor* of the upgrade, not the typical experience (next table).
Written bytes identical (25,058 vs 25,063 MB, 38M rows everywhere).

### F1b. Write — parallel writers (candidate only; no such knob in 0.2.2)

Plain COPY from parquet, heap, NVARCHAR(MAX):

| writers | wall (s) | scaling |
| --- | --- | --- |
| 1 | 676.9 | 1.00 |
| 2 | 396.1 | 1.71× |
| 4 | 230.0 | 2.94× |
| 8 | 149.9 | **4.52×** |

Unlike the 15-column spec-057 bench (plateau at 4), the 44-column table keeps
scaling to 8. `mssql_copy_parallel_writers = 0` (default) derives 8 on this
box, so **the out-of-the-box upgrade on this statement is ≈ 6.0×**
(933 s → ~154 s source-adjusted).

### F2. Write — string typing (writers = 4, heap) and the PLP tax

0.2.2 can express none of these (no `mssql_default_string_length`, no
`MSSQL_(N)VARCHAR(n)` annotations, no `mssql_utf8_collation` control):

| variant | target string type | wall (s) | table size (MB) |
| --- | --- | --- | --- |
| plain @4 | NVARCHAR(MAX), PLP wire | 230.0 | 23,103 |
| `mssql_default_string_length=200` | NVARCHAR(200) inline | **93.7** | 23,104 |
| annotated `MSSQL_NVARCHAR(200)` | NVARCHAR(200) inline | 93.5 | 23,103 |
| annotated `MSSQL_VARCHAR(200)`, DB code page¹ | VARCHAR(200) CP1252 | 92.5 | 18,092 |
| annotated `MSSQL_VARCHAR(200)`, UTF-8 collation | VARCHAR(200) UTF-8 | 92.2 | **17,812** |

¹ the two non-CP1252-representable columns (Cyrillic, CJK) stay NVARCHAR in
this variant; UTF-8 varchar takes all nine.

The headline is the **PLP tax**: giving the nine MAX-typed string columns a
length — same data, same bytes on disk — takes **2.45×** off the load
(230 → 94 s), one `SET` away. Each MAX value pays 8+4 bytes of PLP framing and
the server's LOB write path. VARCHAR then halves string bytes on the wire and
takes −23% off the table size; wall stays ~92-94 s because at 4 writers the
bound is no longer bytes. Against the 0.2.2 baseline the sized-string load is
**≈ 9.7×**.

### F3. Write — heap vs clustered columnstore (writers = 4)

| variant | wall (s) | table size (MB) | rowgroups |
| --- | --- | --- | --- |
| heap, plain | 230.0 | 23,103 | — |
| columnstore, plain | 301.8 | **5,166** | 369 COMPRESSED / 1 OPEN |
| columnstore, varchar-utf8 | 187.3 | 5,694 | 351 COMPRESSED / 2 OPEN |

4.5× smaller on disk; `mssql_copy_flush_rows = 102400` lands rows compressed
directly (369 COMPRESSED, 1 residual OPEN). Columnstore wall exceeds heap here
because compression burns the emulated server's CPU. Curiosity: UTF-16
nvarchar compresses *better* inside CCI than UTF-8 varchar (5.17 vs 5.69 GB)
while loading slower (302 vs 187 s) — the columnstore dictionary likes the
wider-but-more-uniform encoding.

### F4. CTAS

| binary | source | wall (s) |
| --- | --- | --- |
| 0.2.2 | generated | 942.7 |
| candidate, 1 writer | generated | 699.4 |
| candidate, defaults | parquet | **152.7** |

CTAS inherits the whole write path: like-for-like it mirrors F1 (1.35×), and
at defaults it matches plain@8 (149.9) — **≈ 6× out of the box**.

### F5–F7. Read

| shape | 0.2.2 (s) | candidate (s) | wall speedup |
| --- | --- | --- | --- |
| grouped aggregation, 5 cols (GROUP BY i8) | 36.6 | 29.9 | 1.22× |
| full drain, 8 cols (min over each) | 46.0 | 36.3 | 1.27× |
| SQL Server → parquet, all 44 cols | 175.8 | 125.3 | **1.40×** |

Aggregation and the parquet writer run in the shared DuckDB core, so the
deltas are the extension's scan path. Wall understates the read-path change —
on this box the stream is network/server-paced; the client-side cost is where
the rewrite shows (next table).

### F8. Client-side cost (`/usr/bin/time -l`, single matched runs)

| pair | metric | 0.2.2 | candidate | factor |
| --- | --- | --- | --- | --- |
| write 4M @1 (generated) | instructions retired | 204.9 G | 73.2 G | **2.8×** |
| | cycles | 40.8 G | 17.6 G | 2.3× |
| | peak RSS | 263 MB | **39 MB** | 6.7× |
| | packets sent | 793,914 | 200,613 | 4.0× |
| read: full drain, 8 cols, 38M | instructions retired | 1,420.8 G | 87.4 G | **16.3×** |
| | cycles | 247.8 G | 29.2 G | 8.5× |
| | user CPU | 60.4 s | 3.8 s | 16× |
| read: → parquet, all 44 | instructions retired | 1,398.1 G | 719.6 G | 1.9× |
| | cycles | 367.8 G | 210.8 G | 1.7× |

The 4× drop in packets sent is the TDS packet size going 4096 → 16384 (spec
055); the 6.7× RSS drop on write is the columnar encoder streaming instead of
buffering; the 16× on the 8-column drain is the staged batch-decode read path
(specs 054/055/058) — the old per-value path burned 1.42 T instructions to
read eight columns. On a same-box benchmark the freed CPU largely converts to
wall only where the client was the bottleneck; on a real network deployment
the client-CPU column is the one that scales.

### F9. Content verification (post-measurement)

- `test/sql/copy/vector_encodings_bcp.test`: 15/15 assertions green
  (dictionary + constant vectors, round-trip content equality).
- Full-scale 44-column signature check (`count` + `sum(hash(col))` per column,
  parquet vs read-back through the extension) for plain-heap, varchar-UTF-8
  and columnstore writes: **all three OK — 44/44 signatures match at 38M rows**
  (write and read paths content-exact end to end, including dictionary-vector
  input, UTF-8 varchar encode and CCI batch boundaries).
- Byte-level cross-check: 0.2.2 and candidate produced identical table sizes
  from the identical generated source (25,058 vs 25,063 MB; the 5 MB delta is
  heap page-fill noise) and identical row counts, both rounds.

## Per-phase attribution (from the specs' own measurements)

| Phase | What | Its own measured contribution |
| --- | --- | --- |
| spec 054 | SIMD baseline + quick wins (read) | PR #209 |
| spec 055 | read staging + batch decode; TDS packet 4K→16K | −28% client CPU / −43% wall read; −27% CPU write |
| spec 056 | CONSTANT-vector fast path (read) | PR #221 |
| spec 057 | write path rebuilt: columnar encode, parallel writers, TABLOCK/flush policy | 6.6× write (its bench) |
| spec 058 | read framing: skip-form fast walk | −1.16/−0.78 ns/value (mixed) |
| spec 064 | columnar-write gap closure | PR #241 |
| #225 | UTF8SUPPORT: UTF-8 varchar without UTF-16 transcode | 85.8→43.9 MB wire / 1M values |

## Honesty notes

- Single measurement box; the server runs under Rosetta emulation, so
  server-side ceilings (ingest rate, CCI compression) are lower than a native
  deployment. This *understates* wall-clock gains wherever the server is the
  bound (F1, F5–F7) and makes the parallel-writer scaling (F1b) conservative.
- The DuckDB CLI `.timer` user/sys readings were erratic for multi-threaded
  statements on macOS (identical runs reporting 45.7 s vs 7.5 s of user at
  identical wall) — all CPU claims in this report come from `/usr/bin/time`
  process totals instead; wall clock was stable ±1–8% across rounds and is
  the primary metric throughout.
- The read fixture's string columns are nvarchar ⇒ #225's UTF-8 read win is
  not visible in F5–F7 (it needs UTF-8-collated varchar columns server-side).
- F8 rows are single matched pairs (not medians); their cross-metric
  consistency (instructions, cycles, user, packet counts) is the evidence.
- `mssql_version()` prints 0.2.2 on both binaries (the bump lands in the
  release train); binaries were distinguished by path throughout the logs.

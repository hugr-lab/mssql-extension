# Spec 059 — Test coverage for the staged read path

**Status:** ready to implement. Every mechanic below was verified on 2026-07-30;
nothing here is a guess.

**Why it exists.** Spec 055 replaced the read path's per-value walk with one that
carries **no bounds test per value by design** — its safety rests on framing
invariants rather than on checks. Three out-of-bounds defects were found in it
by reading, none by a test:

| defect | found by | what tested it |
| --- | --- | --- |
| odd-length UTF-16 value → OOB in the delimiter walk | self-review | nothing |
| NBC row + `0xFFFF` prefix → 65535-byte over-read | @oluies review (PR #213) | nothing |
| diverging fixed-width column threw instead of falling back | a unit test **that was not wired into CI** | itself, silently, for weeks |

The third is the point. `make test-column-staging` existed from the day the
staging structures landed and nothing ran it; when PR #213 wired it in, it failed
immediately on a regression it had been documenting to nobody.

**Priority, set by the maintainer:** NBCROW coverage first. Nothing else moves
until the staged path's second walk has tests.

---

## 0. The blocker that turned out not to be one

Earlier work recorded that `RowStager` could not be linked into a standalone
test binary — "it pulls every family codec, which pull `target_resolver`, which
pulls the catalog". **That is wrong.** Verified by building and running a probe:

```
c++ -std=c++17 -I src/include -I duckdb/src/include -I "$PREFIX/include" \
    probe.cpp \
    src/codec/*.cpp src/codec/staging/*.cpp \
    src/tds/encoding/{utf16,datetime_encoding,decimal_encoding,guid_encoding,bcp_row_encoder,type_converter}.cpp \
    src/tds/{tds_row_reader,tds_column_metadata,tds_types}.cpp \
    -L "$PREFIX/lib" -lsimdutf -L build/release/src -lduckdb
```

links and runs. The codec headers reference `target_resolver` but nothing on
this path calls into it, so the symbol is never needed. **Every test in this
spec is therefore a cheap unit test, not an integration test.** That single fact
is why this spec is small.

---

## D1 — NBCROW (first, per the maintainer)

### The verified recipe

SQL Server emits NBCROW only when the null bitmap costs less than the per-value
NULL markers it replaces. **It is byte economics, not a column count** — the
"≥ 8 columns and ≥ 2 NULLs" rule this spec first recorded is an approximation
that misleads: it is right for 8 columns of integers and wrong for 11.

```
bitmap size   = ceil(columns / 8) bytes
markers saved = 1 byte per NULL in a fixed-width column (the zero length prefix)
                2 for NVARCHAR / VARBINARY (0xFFFF)
                8 for a MAX type (the PLP NULL marker)
```

NBCROW is sent when the second exceeds the first. Measured against a live
server, 2026-07-30, on an 11-column table:

| row | saved | bitmap | token |
| --- | --- | --- | --- |
| 2 NULL integers | 2 | 2 | `0xD1` ROW (a tie loses) |
| 2 NULL integers + 1 NULL `NVARCHAR(MAX)` | 10 | 2 | `0xD2` NBCROW |
| all NULL | 27 | 2 | `0xD2` NBCROW |

The first row of that table is how `nbc_row.test` was first written: it passed
while testing the other walk.

**Projection width is part of the recipe.** The server prices the bitmap against
the RESULT SET, not the table, so a query that projects four columns of an
eleven-column fixture gets plain ROWs. Every NBC assertion must therefore be
made through a wide projection — which is why the bulk case in `nbc_row.test`
aggregates all eleven columns in one query rather than in three readable ones.

Confirm the token with `MSSQL_DEBUG=2` and
`grep -o "token_type=0xD[12]" | sort | uniq -c`; the parser logs every token
type.

### D1a — a counter, so the test cannot lie — **DONE**

Add `nbc_rows` to the D10 staging counters (`StagingCounters`, incremented in
`StageNBCRow` — once per ROW, not per value). Without it, an NBCROW test that
stops producing NBCROWs — a server upgrade changes the heuristic, a column is
added, the fixture drifts — keeps passing while testing the other walk. With it,
the test asserts the path was entered.

**As landed it sits behind `counters_enabled_`, like every other counter here.**
The first version did not, on the argument that a counter gated at
`MSSQL_DEBUG>=2` cannot be asserted by a test — which is false for a unit test
that constructs its own `RowStager` and can simply call `EnableCounters()`. The
walks must not carry work that exists only for a test; and gated, the counter is
still real diagnostics, because which of the two walks a workload spends its
time in is not something the schema tells you.

### D1b — unit tests over synthetic NBC rows — **DONE**

`StageNBCRow` takes a raw byte pointer, so the tests construct rows directly and
need no server. Cases:

- every arm reached through the NBC walk, one column each, bitmap bit clear;
- the bitmap bit SET for each arm in turn — the column must consume zero bytes
  and stage a NULL;
- `0xFFFF` length prefix with the bitmap bit clear, on `P2StageBinary` **and**
  `P2StageString` — both must throw (PR #213's defect; the string arm currently
  survives only because `0xFFFF` is odd);
- a bitmap spanning more than one byte (≥ 9 columns) with NULLs on both sides of
  the byte boundary;
- all columns NULL — the row is bitmap-only, zero value bytes.

Landed as `test/cpp/codec/test_row_stager.cpp` (`make test-row-stager`, wired
into CI beside `test_column_staging`), with one case beyond the list above: the
same logical row staged through BOTH walks, asserted to produce identical
output. Every arm's wire bytes are hand-written, which is the third independent
statement of the framing that D2 needs anyway.

Two mutants were run to prove the tests can fail: inverting the bitmap
convention in the builder, and reverting the PR #213 guard. The first reports
failures AND trips `D_ASSERT(p == end)`; the second aborts on that assertion
inside the 0xFFFF case. So the debug-build lever below is already doing its job.

### D1c — one integration test that actually produces NBCROWs — **DONE**

`test/sql/query/nbc_row.test`: eleven columns of mixed families (Direct, Fixed,
Var, PLP), each row NULLing at least one wide-marker column so the bitmap
actually pays for itself, plus a 3000-row bulk case that crosses the 2048-row
chunk boundary. Asserts values and NULLs round-trip, including empty-vs-NULL for
a zero-length `NVARCHAR(MAX)` and `VARBINARY`.

Both scans verified 0xD2 with `MSSQL_DEBUG=2` — and the first draft of the
fixture was NOT, which is why the recipe above was rewritten.

---

## D2 — the differential framing test

**The highest-value test in this spec.** The staged walk's memory safety rests
entirely on `RowStager`'s framing consuming byte-for-byte what
`RowReader::SkipValue` / `SkipValueNBC` consumed — the parser hands up a row
bounded by the latter, and the former walks it with no bounds test. Those are
two independent switch statements over the same wire types **in different
files**, with nothing tying them together. That gap is precisely how the NBC
`0xFFFF` defect got in: `SkipValueNBC` returned 2, the stager consumed 65537.

Table-driven over **type × framing {bare, P1, P2, PLP, LOB} × row form {ROW,
NBCROW}**. For each case:

1. build a synthetic row: the column under test, then a **sentinel** column of a
   known type and value;
2. assert `SkipValue` and the stager agree on bytes consumed;
3. assert the sentinel decodes to its expected value — which proves the previous
   column consumed exactly the right number of bytes, not merely a number both
   sides agree on.

Built under ASan (the Linux lab — macOS sanitizers are broken, see the memory
note) this catches over-reads directly rather than by inference.

---

## D3 — the chunk boundary — **DONE**

`test/sql/query/staged_chunk_boundary.test`: 5000 rows over Direct/Fixed/Var
columns with NULLs, values checked either side of both 2048-row seams; the same
scan repeated at `mssql_tds_packet_size = 512`; and 60 values of 500k characters
for the payload budget.

Both mechanisms were verified to actually fire rather than assumed — the failure
mode this whole spec exists to avoid. `MSSQL_DEBUG=2` reports `chunks=2` for the
60-row budget table (2048-row chunks would give one), and the socket log shows
`packet_size=512` negotiated with the server.

One loose end noted, not chased: that same counter line reads `rows=59` for a
60-row scan while `COUNT(*)` returns 60. The data is right and the counter is
debug-only, but it is an off-by-one somewhere in the accounting.

The original notes follow.

## D3 — the chunk boundary

No existing SQL test crosses one: both spec-055 `.test` files use ≤ 5 rows, and
`STANDARD_VECTOR_SIZE` is 2048. Untested as a result: arena reuse across chunks,
`BeginChunk` re-taking `direct_dst` after `DataChunk::Reset` (which is free to
hand back different memory), the offset/count reset, and the whole watermark
policy.

- A > 2048-row scan over a mixed-type table — one Direct column, one Var column
  with NULLs, one Fixed. Cheap, and it covers all of the above at once.
- A chunk closed EARLY by `STAGING_CHUNK_PAYLOAD_BUDGET_BYTES`: MAX-typed values
  large enough to trip the budget before 2048 rows, so a short `DataChunk` is
  produced and consumed.
- `mssql_tds_packet_size = 512` on a mixed-width table, so rows straddle frame
  boundaries thickly and the partial-row path runs on nearly every row.

---

## D4 — fuzz the raw-row path

`fuzz/fuzz_tds_tokens.cc` never calls `SetRawRowMode(true)` and never constructs
a `RowStager`. So the parser that replaced the fuzzed one — the one with no
per-value bounds checks — is entirely unfuzzed, while the path it replaced is
fuzzed weekly.

`fuzz/fuzz_row_stager.cc`: drive COLMETADATA + rows from the fuzz input through
the parser in raw-row mode into a configured `RowStager` with real output
vectors. Unlike the existing `src/tds`-only harnesses this one links libduckdb —
see §0, it is a normal link.

---

## D5 — the string kernel's untested corners — **DONE**, and it found a shipped bug

The `SplitWithEmbeddedNuls` case below turned out to be more than a coverage gap: the
condition that triggers it was wrong, so values containing `U+0000` decoded silently
wrong on the shipped path. Live server, `N'ab' + NCHAR(0x00C4) + NCHAR(0)` followed by
`N'c'`: out came `abÄ` and `\0c`. SQL Server stores `U+0000` in NVARCHAR — verified,
not assumed — and spec 055 named it as the one hazard the delimiter scheme has.

Full account, including the three measured alternatives and why the delimiter cannot be
changed, is in `specs/055-read-staging-and-batch-decode/spec.md` beside the rule it
broke. The fix: the boundary walk no longer skips to each value's lower bound, which is
what made "did the walk end on the last byte" an exact test again.

Landed tests, all in `test/cpp/codec/test_row_stager.cpp`:

- every boundary strategy asserted through the D10 counters (ASCII offsets, word sweep,
  memchr) rather than assumed from the data;
- the mixed-script trap the kernel's own comment names — a value whose UTF-8 length falls
  strictly between `u` and `2u`, which every single-script bench fixture misses;
- `U+0000` both in the middle of a value and at its END (the second is the defect);
- NCHAR trailing-space trim over INVALID UTF-16, the stated reason the trim moved to the
  output side;
- the `needs_value_fallback` / `FinalizeKernel::Text` path (issue #89).

The original list follows.

## D5 — the string kernel's untested corners

From the PR #213 review, all in `src/codec/string_codec.cpp`:

- `SplitWithEmbeddedNuls` — a value containing its own U+0000. No coverage, and
  it is the subtlest function in the spec-055 diff.
- **The mixed-script trap the code comments name explicitly**: a value whose
  UTF-8 length falls strictly between `u` and `2u` code units. Every bench
  fixture is single-script, so the bug the comment warns about would pass the
  entire matrix.
- Both `FindDelimiter` instantiations — the choice is made once per column, so a
  given fixture only ever exercises one of them.
- `Utf8UpperBound` at its worst case: an all-CJK column at exactly 3 bytes per
  unit, filling the reservation to the byte.
- NCHAR trailing-space trim on **invalid** UTF-16 — the stated reason the trim
  moved to the output side, and untested.
- The `needs_value_fallback` / `FinalizeKernel::Text` path (issue #89) — the one
  PR #213 found broken for integer types.

---

## Acceptance

1. Every test here runs in CI. This spec exists because a test that does not run
   is worse than no test: it reads as coverage.
2. `nbc_rows > 0` asserted wherever NBCROW coverage is claimed.
3. D2 built under ASan in the Linux lab; a deliberately reintroduced framing
   mismatch must fail it.
4. The integration suite's own reporting is fixed or noted: it has reported
   PASSED while running zero `.test` files (#212), so a green check there does
   not by itself establish that any of this ran.

---

# Implementation directions

Written while PR #213 was in CI; these are the details that decide whether each
test above is cheap or a week's work.

## The single most useful lever: build the tests in DEBUG

`StageRow` and `StageNBCRow` end with `D_ASSERT(p == end)` — the walk's cursor
must land exactly on the row's end. **That assertion IS the framing check D2 is
about**, and it is compiled out in release. A debug build turns every existing
and future test into a framing test for free.

So: these tests build against `build/debug`, not `build/release`. Anything that
only reproduces under release (the `unsafe_vector` bounds checks come back in
debug, which changes what an over-read does) gets a release variant as well, and
the difference is stated in the test rather than discovered later.

## `StageRow` should return bytes consumed

Today it returns `void`, so D2 cannot compare consumption directly and has to
infer it from a sentinel column. Returning `p - row`:

- lets D2 assert `stager_consumed == skip_consumed` **per column**, not per row,
  which localizes a mismatch instead of just detecting one;
- costs nothing — the value is already computed for the `D_ASSERT`;
- is needed by **spec 058 anyway**, whose design has the stager tell the parser
  how many bytes it used. Doing it here means 058 inherits it.

Keep the sentinel column regardless (§D2 step 3): agreeing on a byte count and
consuming the *right* bytes are different claims, and only the sentinel tests
the second.

## Building synthetic rows

Both walks take a raw pointer, so no server is involved. A small builder keeps
the tests readable and is the reference the two switches lack:

```
struct RowBuilder {
    void Bare(const uint8_t *v, size_t n);        // no prefix (INT, DATETIME, MONEY)
    void P1(const uint8_t *v, size_t n);          // 1-byte length, 0 = NULL
    void P1Null();
    void P2(const uint8_t *v, size_t n);          // 2-byte length, 0xFFFF = NULL
    void P2Null();
    void Plp(std::initializer_list<span> chunks); // 8-byte total, chunk list, 0 terminator
    void PlpNull();
    void Lob(const uint8_t *ptr, size_t ptr_len, const uint8_t *v, size_t n);
    void LobNull();                               // 1 zero byte
    std::vector<uint8_t> FinishRow();
    std::vector<uint8_t> FinishNbcRow(const std::vector<bool> &nulls);  // prepends the bitmap
};
```

**Bitmap convention, from the implementation** (`row_stager.cpp:473`):
`ceil(n/8)` bytes, LSB-first within each byte, **bit SET means NULL** —
`bitmap[c >> 3] & (1u << (c & 7))`. Getting this backwards produces tests that
pass while exercising the opposite case, so assert it once directly.

The per-type wire bytes are hand-written, one table entry per type. That is
verbose on purpose: it is the third, independent statement of the wire format,
and the whole point of D2 is that the existing two can drift.

## Column metadata and output vectors

`test_column_staging.cpp` already has the pattern — a `meta(type_id, max_length)`
helper plus `TypeConverter::GetDuckDBType(col)` to pick the target type. Extend
it with `precision`/`scale` for DECIMAL and `scale` for the temporal types
rather than inventing a second helper.

Output vectors: one `Vector(GetDuckDBType(col))` per column, and the same
pointers passed to `Configure` and `BeginChunk`. `BeginChunk` must be called
again for each chunk — Direct columns re-take `direct_dst` there, and D3's
whole point is that `DataChunk::Reset` may hand back different memory.

## The `nbc_rows` counter (D1a)

`StagingCounters` field, incremented in `StageNBCRow`. **Unconditionally, not
under `counters_enabled_`**: it is one add per ROW, the D10 rule that nothing
may touch the per-VALUE path is not engaged, and a counter that only exists at
`MSSQL_DEBUG>=2` cannot be asserted by a test that runs without it.

Expose it the same way `ChunkNulls` is exposed, so a unit test reads it directly
and the integration test reads it through the counter block.

## Generating > 2048 rows server-side (D3)

No client-side loop and no large `.test` file:

```sql
INSERT INTO dbo.T (…)
SELECT TOP 3000 …
FROM sys.all_objects a CROSS JOIN sys.all_objects b;
```

For the early-close case, `STAGING_CHUNK_PAYLOAD_BUDGET_BYTES` is 32 MB, so ~40
rows of a 1 MB `NVARCHAR(MAX)` (`REPLICATE(CAST(N'x' AS NVARCHAR(MAX)), 500000)`)
trip it well before 2048 rows and produce a short `DataChunk`.

For the straddling-frame case, `SET mssql_tds_packet_size = 512` before the
scan — the smallest the clamp allows, so rows cross frame boundaries constantly.

## The fuzz harness (D4)

Shape it like `fuzz_tds_tokens.cc`, with three differences:

1. `parser.SetRawRowMode(true)` — the whole point;
2. after COLMETADATA, build output `Vector`s from the parsed metadata and
   `Configure` a `RowStager` on them; feed each `Row` token through
   `StageRow`/`StageNBCRow` and call `FinalizeChunk` at 2048 rows;
3. it links libduckdb, unlike the `src/tds`-only harnesses — see §0, this is now
   a normal link.

The harness must **catch exceptions**: this code throws on malformed input by
design (`ThrowBadPrefix`, `ThrowOddUtf16Length`, `ThrowNbcNullPrefix`,
`ThrowUnsupportedType`), and a throw is a PASS, not a finding. Only a crash,
an ASan report, or a hang is a finding.

Seed the corpus from real captures — the `.test` fixtures' wire bytes are the
obvious source, and a seeded corpus reaches interesting states in minutes rather
than hours.

## Order of work

D1a (the counter) first — it is ten lines and every NBCROW test depends on it to
prove it ran. Then D1b, which is where the known defect lives. Then D2, which
subsumes much of D1b's per-arm coverage and is the one that keeps paying. D3 and
D5 are independent and can land in any order. D4 last: it finds what the others
did not think of, which is only worth doing once the others exist.

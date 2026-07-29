# Spec 055 — Read staging & batch decode (phase 1)

**Status:** Draft — deliverables sized by prototype measurements (§8)
**Date:** 2026-07-29
**Design:** `docs/proposals/simd-chunk-materialization-design.md` (§3 read architecture, §5 memory
ownership, §7 benchmarks, §10 phasing). This spec covers phase **1**.
**Predecessor:** spec 054 (baseline + phase-0 quick wins, PR #209). Its baseline file
`test/bench/bench_results_simd_baseline.md` is the measurement reference and is append-only —
this phase adds its delta columns there.
**Process note:** documentation, not a spec-kit pipeline. Implementation proceeds natively; this
file plus the design doc are the source of truth for scope and acceptance. Update it in the same
PR when reality diverges.

---

## 1. Goal and invariant

Restructure the TDS→DuckDB read path from per-value conversion into **per-column staging with one
batch conversion per column**, keeping the output representation FLAT (CONSTANT/DICTIONARY is
phase 2 / spec 056).

**Phase invariant:** after this phase the hot path contains **no per-value conversion call and no
per-value type dispatch, for any family**. The only work that legitimately remains per value is:

1. **one** walk over the row that reads the length prefix of each variable-length value (the
   protocol gives no other way to find the next value; fixed-width columns need no walk at all,
   and under NBCROW their offsets are arithmetic from the null bitmap),
2. the raw scatter row-major → column-major (a store per value), fused into that same walk,
3. constructing the output `string_t` / writing the decoded slot.

The measured cost of that irreducible residue is the integer/float decode floor from the 054
baseline: **1.7–2.2 ns/value**. Everything above it is what this phase removes.

**Note what this phase is actually replacing — it is not "add staging to a path that had none".**
The current read path already stages, into the wrong layout, and then re-reads it:

- `RowReader::ReadRow` (`src/tds/tds_row_reader.cpp:11-33`) walks the row **and copies every
  value's payload** into `RowData::values[col]`, a `std::vector<std::vector<uint8_t>>`
  (`src/include/tds/tds_token_parser.hpp:81`) — one heap vector per column, `clear()`ed per value
  per row;
- `MSSQLResultStream::ProcessRow` then walks that row-major structure **again** and dispatches per
  value into the codec.

So the wire bytes are copied once and read twice, with a type dispatch in each pass. Staging does
not add a copy — it redirects the copy that already happens into column-major buffers, after which
conversion is one batch call per column and the second pass disappears entirely. This is why the
§8 end-to-end prototype measured 21.4 → 5.8 ns/value **including** its staging cost.

Prototypes for every strategy below were built in `test/cpp/bench_materialize.cpp` and measured
before this spec was written; §8 records the numbers, and the deliverables are sized by them
rather than by expectation.

### 1.1 What the live-server baseline says about the prize

`test/bench/bench_results_live_server.md` (2026-07-29) measures client CPU on the shipped path
against a real server, one step per family. It reframes this phase:

- there is an **~80 ns/value floor on reads that does not depend on the type** — `BIT` costs
  80.4 ns/value net of the local-sink control, and the wide-row step shows the floor is per
  *value*, not per row (15 columns cost 92.8 ns each);
- the codec delta sits *on top* of that floor: +56 for DECIMAL(38,10), +64 for NVARCHAR(16),
  +697 for NVARCHAR(200), ~0 for int/date/bool.

So taking string decode from 18.1 to 2.1 ns (the §8 prototype win) moves live NVARCHAR(16) read
cost from 143.6 to roughly 128 — about 11%. **The kernels are the minority term; per-value
dispatch and the row loop are the majority.** This phase's architecture already targets exactly
that (one pass per column, no per-value dispatch), so the direction is unchanged — but the
success criterion must be stated against the floor, not against the kernel:

> the phase succeeds if live read cost per value drops toward the wide-row-amortized floor for
> *every* family, including the ones whose decode is already free (int, date, bool). A win visible
> only on strings and DECIMAL means the floor survived and the phase missed its target.

**Prerequisite (D0, below):** the composition of that 80 ns is not yet known — token parsing,
per-value dispatch, per-column plumbing and chunk fill have never been separated. Measure it
before building staging, so the target is known rather than assumed.

## 2. Non-goals

- **No CONSTANT/DICTIONARY emission**, no chunk analyzer — spec 056. Staging is shaped so the
  analyzer can be added without a second rewrite.
- **No write-path (BCP) changes** — phase 3 / spec 057.
- **No hand-written SIMD intrinsics.** Portability across MSVC/MinGW/GCC/Clang × ARM/x86 (design
  §11.3). We rely on simdutf, libc `memchr`, and auto-vectorizable loops. §8 bounds what
  intrinsics could still buy, for a later phase.
- **No C++17** (spec 054 §2 stance unchanged; all code stays C++11-compatible).
- **No new user-facing settings.** Any knob here is debug-only.

## 3. Evidence discipline

Carried over from spec 054, unchanged:

- every conversion-touching commit is gated by `test/bench/diff_check.sh` (13 queries,
  byte-identical) against the pre-change build, plus the full unit + integration suites;
- e2e comparisons are **same-session interleaved A/B only** — cross-session numbers drift ±20% on
  sub-second steps and are recorded as smoke, never as a gate;
- micro deltas are appended to the 054 baseline file in the same table shape.

One behaviour change ships in this phase and must be called out in the release notes: **invalid
UTF-16 now decodes with standard U+FFFD substitution** (§D2).

## 4. Deliverables

### D0. Read-path phase timers — decompose the 80 ns floor before building on it

§1.1 shows an ~80 ns/value cost that exists for every type including ones with no decode worth
the name, and nothing currently attributes it. Extend the spec-054 D4 counters (same
`MSSQL_DEBUG>=2` latch, same `[MSSQL COUNTERS]` stream-close line, no cost when off) with
per-phase nanoseconds on the read path, taken **per chunk, not per value**:

- socket wait (time inside `recv`, i.e. the server/network term),
- token/row framing parse,
- per-value decode dispatch + codec,
- chunk/vector fill and validity.

Deliverable is the numbers, not the instrumentation: a table splitting the 80 ns for at least
`bool`, `bigint`, `str16` and `str200`, appended to
`test/bench/bench_results_live_server.md`. If the floor turns out to be dominated by something
staging does not remove, the rest of this spec gets re-sized before it is built.

Lands first, ahead of D1 — it is measurement, touches no conversion, and is gated by nothing more
than the existing suites.

**Status: done, and it moved the baseline.** Three phase timers already existed in `FillChunk`
(`parse` / `read` / `process`) but were broken in two ways, both shipped since spec 004 and
present in every release from v0.1.0:

- **never gated** — four `steady_clock::now()` calls per row in release builds, measured at
  ~59 ns/row on this machine;
- **truncated** — per-row intervals accumulated via `duration_cast<microseconds>`, so a ~100 ns
  row contributed 0 and `fill_process_us` reported nothing.

Gating them behind `MSSQL_DEBUG>=1` and accumulating in nanoseconds cut live read cost by
**23–64%** (bool 78.8 → 28.0, bigint 99.3 → 45.4, str16 153.7 → 118.7 ns/value), interleaved
same-session A/B over two passes agreeing within 1%.

Consequences for the rest of this phase, which supersede §1.1's arithmetic:

- the floor was roughly two thirds measurement apparatus, not architecture. The *real* remaining
  per-row client work is `parse` 21–27 ns and `process` 18–30 ns;
- both `parse` and `process` are in scope for staging, and an earlier draft of this note was wrong
  to call `parse` irreducible. `parse` is not a bare framing walk: it copies every value into
  row-major `RowData` scratch, which `process` then re-reads. Staging replaces that copy with a
  column-major one and deletes the second pass — see §1;
- with the apparatus gone, the codec share is larger than §1.1 estimated: str16 sits at 118.7
  ns/value against a `process` phase of 30 ns/row, so the string decode win is now a materially
  bigger fraction of what remains;
- `read` phase is wall, not CPU — mostly waiting on the server. Its CPU part is `recv` syscall
  overhead, which belongs to the TDS frame-size question, not to this spec.

Re-run the live baseline after D5/D6 land and compare against the post-D0 column, not against the
original one.

### D0b. TDS frame size + multi-frame receive staging — **done**, read −28% CPU / −43% wall

Added after D0 showed `read` to be the largest wall term, with `recv` syscall overhead as its CPU
component. Three causes, fixed together:

- LOGIN7 always requested `TDS_DEFAULT_PACKET_SIZE`. The comment at
  `tds_connection.cpp` claimed the server would "negotiate up"; TDS does the opposite — the server
  answers `min(requested, its maximum)` and never raises it. New setting `mssql_tds_packet_size`
  (clamped [512, 32767]), plumbed through `MSSQLPoolConfig` → `MSSQLConnectionInfo` → all pool
  factories and the ATTACH-validation connections, via `TdsConnection::SetRequestedPacketSize`.
- `TdsSocket::ReceivePacket` read through a fixed `uint8_t[4096]`, so a bigger frame would have
  been reassembled by the same number of `recv` calls. Receive staging is now sized to a **byte
  budget** (`TDS_RECV_BUFFER_TARGET_BYTES`, 64 KB) of whole frames — 16 frames at 4096, 4 at
  16384 — so raising the frame size never raises per-connection memory.
- Every completed packet did `receive_buffer_.erase(begin, begin + consumed)`, an O(n) memmove of
  the remaining bytes **per packet**. Replaced by a read cursor; the tail is compacted once per
  `recv`, not once per packet.

Measured (500k rows × 15 mixed columns, medians of 3): read 106.4 → 76.8 ns/value CPU and
2.47 → 1.40 s wall, write 52.7 → 38.7 ns/value. Default raised 4096 → 16384
(`TDS_PREFERRED_PACKET_SIZE`); this server caps the grant at 16384, so 32767 measured identically
and nothing is known about servers that would grant more. `diff_check.sh` 13/13 byte-identical.

Note this changes what the rest of the phase is measured against **again**: the post-D0b read
baseline for the wide step is 76.8 ns/value, not 106.4.

### D1. DECIMAL decode kernel — **done**, live dec38 −45%

Landed. Live A/B on top of D0b, interleaved, two passes: DECIMAL(38,10) 75.8 → 41.8 ns/value
(−45%), DECIMAL(18,4) 45.8 → 33.3 (−27%), with BIGINT as an untouched control at 34.9 → 35.1
(±0) — so the delta is the kernel, not drift. DECIMAL(38,10) now decodes at roughly BIGINT cost.
The direct little-endian load is guarded on `__BYTE_ORDER__`/MSVC; big-endian hosts and malformed
lengths beyond 16 magnitude bytes fall back to the original portable loop, so the overflow
behaviour on garbage input is unchanged. `diff_check.sh` 13/13 byte-identical; byte-contract
fixtures added to `test/cpp/codec/test_decimal_codec.cpp`.

`DecimalEncoding::ConvertDecimal` (`src/tds/encoding/decimal_encoding.cpp:8-24`) assembles the
magnitude with a **full 128-bit multiply-add per byte**:

```cpp
for (size_t i = length - 1; i >= 1; i--) {
    magnitude = magnitude * hugeint_t(256) + hugeint_t(data[i]);
}
```

The TDS magnitude is little-endian (`data[0]` = sign, `data[1..]` = magnitude), so the words load
directly: ≤8 magnitude bytes → one `uint64_t`; ≤16 → two; sign byte 0 → negate. Precision ≤ 18
decodes into `int64_t` without materializing a `hugeint_t` at all.

**Endianness:** direct word loads assume a little-endian host. Guard with a compile-time check and
keep the current loop as the big-endian fallback — the extension targets LE platforms only today,
but the assumption must be explicit rather than silent.

Measured (prototype, 2048-row chunks, macOS ARM64):

| cell | current | batch loop, current kernel | batch loop, direct assembly |
| --- | --- | --- | --- |
| decimal_p4s2 (int16) | 16.8 | 16.1 | **2.5** (6.7×) |
| decimal_p18s6 (int64) | 34.2 | 31.2 | **2.1** (16×) |
| decimal_p38s0 (int128) | 73.3 | 69.8 | **3.2** (23×) |

The middle column is the finding that shapes the phase: **the batch loop alone buys ~4%** — the
entire win is the kernel. DECIMAL lands in the same class as int/float/uuid (1.7–2.1 ns), so the
"DECIMAL is the outlier" headline from the 054 baseline disappears.

Tests: extend `test/cpp/codec/test_decimal_codec.cpp` with a table-driven wire→value fixture over
every magnitude length (1..16 bytes), both signs, zero (sign byte 0 must give `0`, not `-0`), and
the DECIMAL(38,x) extremes, asserted against the current implementation kept as reference.

### D2. UTF-16 fallback: standard U+FFFD substitution, legacy converter deleted — **done**

Landed. The data loss was confirmed end to end against a live server before the change, not
argued from the code: `NCHAR(0xD800) + N'A'` decoded to `EFBFBD` — the `A` swallowed — and
`N'A' + NCHAR(0xD800)` decoded to `41`, the surrogate gone with no replacement at all. Both now
decode to the standard two-character result.

The probe is `simdutf::validate_utf16le_with_errors`, not a conversion entry point: every
`convert_*` writes through its output pointer and cannot be used to locate an error. Each valid
run between errors is converted in bulk, so the loop is SIMD everywhere except the splices.

All expectations were validated against an independent reference (Python `utf-16-le`,
`errors='replace'`) rather than against our own reasoning, byte for byte. Fixtures: Test 9 in
`test/cpp/test_login7_encoding.cpp` (unit) and `test/sql/query/unpaired_surrogates.test`
(integration, real NVARCHAR values from SQL Server). `diff_check.sh` 13/13 byte-identical — none
of its 13 queries contains an unpaired surrogate, so nothing there moved.

The ENCODE direction (UTF-8 → UTF-16LE on invalid UTF-8) keeps its legacy fallback; only the
decode converter was deleted.

SQL Server `NVARCHAR` is UCS-2, so unpaired surrogates are legal on the wire and must not turn a
query into an error. simdutf is strictly standards-conformant and rejects them, which is why a
lenient fallback exists at all.

Today's lenient path is the hand-rolled `LegacyUtf16LEDecode`, and it does **not** implement
standard replacement: on an unpaired high surrogate it emits U+FFFD **and consumes the following
code unit** (`utf16.cpp`, the `i += 2` before the low-surrogate test), and an unpaired high
surrogate in the final position is dropped with no replacement at all. That is silent data loss
beyond replacement.

Replace it with a simdutf resume loop: `convert_utf16le_to_utf8_with_errors` → on error, convert
the valid prefix with `convert_valid_utf16le_to_utf8`, append U+FFFD, resume **after the offending
unit** (WHATWG maximal-subpart semantics). The hand-rolled converter is then deleted.

Measured: 50.7 vs 67.6 ns/value on the invalid-input cell — the standard path is also **1.3×
faster** than the legacy one. Decision confirmed by the maintainer 2026-07-29.

Release note required: values containing unpaired surrogates now decode with a U+FFFD in place of
the dropped/swallowed characters. `diff_check` will flag the
`len16_ascii_lone_high_surrogate` case; that diff is expected and approved.

### D3. `ColumnStagingSet` — staging structures and ownership

New `src/codec/staging/`. Owned by `MSSQLResultStream`, reused across chunks **and across result
sets** within one stream (multi-statement `mssql_scan`), capacity retained at a high-water mark
with a watermark release policy.

```cpp
template <class RAW>
struct FixedStaging {                    // capacity = STANDARD_VECTOR_SIZE
    RAW values[STANDARD_VECTOR_SIZE];    // alignas(64)
    uint64_t validity_words[STANDARD_VECTOR_SIZE / 64];
};

struct VarStaging {
    std::vector<uint8_t> payload;             // contiguous UTF-16LE, DELIMITED (see D5)
    uint32_t offsets[STANDARD_VECTOR_SIZE];   // byte offset of each value in payload
    uint32_t lengths[STANDARD_VECTOR_SIZE];   // byte length (unit: BYTES, fixed contract)
    uint64_t validity_words[STANDARD_VECTOR_SIZE / 64];
    bool saw_embedded_nul;                    // gates the delimiter scan (D5)
    bool boundary_risky;                      // value ends in an unpaired high surrogate
};
```

Rules: checked arithmetic on every offset/capacity computation; `uint32_t` offsets bound a
column's per-chunk payload, and the `max_payload` policy (D6) must stay well below that bound;
**no `string_t` handed to DuckDB may point into staging** — staging is overwritten next chunk.

`boundary_risky` and `saw_embedded_nul` are computed during append (one compare per value on bytes
already in cache) even though only D5's gating consumes them.

### D4. Per-column dispatch resolution

After COLMETADATA, resolve each column once into a small ops struct (`append_raw` /
`finalize_flat`; the `build_dictionary_child` slot is reserved for spec 056 but unused here),
removing the per-cell dispatch in `TypeConverter::ConvertValue` (`type_converter.cpp:263-345`).
Same shape as the write path's shipped W2 (`BCPRowEncoder::ResolveEncoder`).

The issue-#89 guard (`:275` — catalog says VARCHAR, TDS sends a non-string type, e.g. a view with
an inline CAST) is a **column-level** property: it resolves once into the ops struct as "this
column uses `WriteAsStringFallback`", removing a branch from every cell with identical behaviour.

`ConvertValue` survives as the per-value path used by the error route (D8) and by callers off the
chunk loop.

### D5. String materialization — one conversion per column, no length pass

This is the deliverable the prototypes reshaped most. The scheme:

1. **Stage delimited.** Every row's UTF-16 payload is followed by one `U+0000` unit (NULL rows
   contribute just the delimiter). One layout serves every path below.
2. **Allocate the worst case: 3 bytes per code unit** — i.e. **1.5× the wire bytes**, since the
   input is 2 bytes per unit. (1 unit → at most 3 UTF-8 bytes; a surrogate pair is 2 units → 4
   bytes, so 3/unit bounds it.)
3. **One `convert_valid_utf16le_to_utf8` for the whole column.** No length pass at all — the
   conversion returns the real size.
4. **Boundaries**, chosen from the conversion result and the column's average length:
   - `written == units` → the column was **all ASCII** (every unit produced exactly one byte), so
     output offsets are input offsets / 2. **No scan at all.** The ASCII verdict is free — it is
     a comparison of two numbers we already have.
   - otherwise, find the delimiters:
     - average length **≤ ~24 units** → one continuous word-wise sweep of the output;
     - **~24–64** → skip to the lower bound (value *i* occupies at least `u_i` bytes, so its
       delimiter cannot precede `seg + u_i`) then sweep;
     - **> ~64** → same skip, then `memchr` (already SIMD in libc; its call overhead only pays
       off once the scanned run is long).
5. **Invalid UTF-16** → isolate that value via the D2 resume loop; the rest of the column stays on
   the bulk path.

Correctness rules that must be in the code as comments, not just here:

- **A single `U+0000` delimiter is structurally unambiguous**: no UTF-8 encoding of a non-zero
  code point contains a `0x00` byte, so any zero byte found in the output *is* a delimiter. A
  longer delimiter adds nothing — the only hazard is `U+0000` in the data itself, and a two-unit
  delimiter is equally defeated by two consecutive NULs in the data. Handle it with the
  `saw_embedded_nul` gate (exact, detected during staging), never with a rarer delimiter.
- **Never probe a guessed boundary position.** Jumping to a guess such as `seg + 2*u_i` (the
  2-bytes-per-unit position) is unsafe: for a value whose real length falls between `u_i` and
  `2*u_i` — any mixed-script value, e.g. `"Привет 123"` — the guess can land on a *later* value's
  delimiter and merge two strings. Only the *lower* bound `u_i` may be used to skip; the first
  zero byte at or after it is provably this value's delimiter. (This bug was written, passed the
  whole fixture matrix because every fixture is single-script, and was caught by reasoning — see
  §8.)
- The upper bound is `3*u_i`, not `2*u_i`: U+0800–U+FFFF (all CJK) is three bytes per unit.

Measured (ns/value; `current` = the shipped post-R1 path):

| cell | current | this scheme | speedup |
| --- | --- | --- | --- |
| len4 ascii | 14.4 | **2.5** | 5.8× |
| len16 ascii | 18.1 | **2.1** | 8.6× |
| len64 ascii | 24.0 | **4.3** | 5.6× |
| len256 ascii | 61.2 | **12.4** | 4.9× |
| len4096 ascii | 879.1 | **201.8** | 4.4× |
| len16 cyrillic | 27.5 | **5.2** | 5.3× |
| len16 cjk | 27.6 | **7.5** | 3.7× |
| len64 cyrillic | 38.8 | **12.3** | 3.2× |
| len256 cyrillic | 83.2 | **36.4** | 2.3× |
| len256 cjk | 110.5 | **65.9** | 1.7× |
| null50% | 10.3 | **2.0** | 5.2× |

Why the length pass had to go: measured separately, `utf8_length_from_utf16le` costs about **twice
the conversion itself** (69% of a length+convert pair, consistently across sizes). The classic
"measure exactly, allocate exactly, convert" shape spends two thirds of its time learning a number
the converter returns anyway.

**Memory policy.** 1.5× the wire bytes per string column per chunk. A cap is required
(`max_payload`, D6): above it, fall back to the exact-length path (per-value
`utf8_length_from_utf16le` + exact allocation), which is slower but bounded. The waste is
transient — it lives in the chunk's string heap and is released with the chunk.

**All-NULL / empty columns** need an early-out: measured 1.8 → 1.9–2.5 ns, the one cell where the
batch scheme is not faster. Trivial to detect (validity words all zero).

### D6. Writing into DuckDB vectors in batch

How each family gets its values into the outgoing vector, and what is genuinely batchable:

| target | mechanism | per-value residue |
| --- | --- | --- |
| fixed-width (int, float, uuid, datetime, decimal) | `FlatVector::GetData<T>(vec)` once per column → write the contiguous array in the finalizer loop | one store |
| **1:1 fixed types** (INT1/2/4/8, FLOAT4/8) | **no staging at all** — the row reader scatters straight into `FlatVector::GetData<T>(vec)[row]`; the wire layout already equals the DuckDB physical layout | one store (unavoidable: the source is row-major) |
| validity | `FlatVector::Validity(vec)` → `EnsureWritable()` → `memcpy` the staged validity words into `GetData()` (32 bytes per 2048 rows) | none |
| strings | one `StringVector::EmptyString(vec, total)` for the whole column → convert into it → build each `string_t` | one 16-byte construction (~1.3 ns measured) |

Measured end to end (3-column chunk — bigint + nvarchar16 + decimal18s6 — from the row-major wire
image to a filled `DataChunk`, **including** the framing walk and the raw scatter; both paths
produce value-identical chunks):

| path | µs/chunk | ns/value |
| --- | --- | --- |
| per-value (production today) | 131.5 | 21.4 |
| staged + batch finalize | 35.4 | **5.8** (3.7×) |
| staged + bulk `memcpy` for the 1:1 column | 35.1 | 5.7 |
| …with 20% NULLs: per-value | 107.8 | 17.5 |
| …with 20% NULLs: staged | 31.4 | **5.1** (3.4×) |

This is the number that validates the architecture: every other measurement in §8 starts from
"value bytes already in hand", so it excludes staging; this one includes it.

**Bulk `memcpy` of values vs direct write: no measurable difference** (5.7 vs 5.8). A bulk column
memcpy is only legal when the wire layout already equals the DuckDB layout — and in exactly that
case the append can write into the vector directly, which needs no staging buffer at all. So 1:1
types use direct write for the memory and simplicity, not for speed.

**Cache locality rules** (the layout choices that make the above hold):

- Staging arenas are **reused across chunks**, never freed per chunk: the buffers stay warm, and a
  fresh allocation would start cold every time.
- Each column's staging is written strictly sequentially (row 0, 1, 2 …), so a column has **one
  active cache line** during the scatter. A 100-column scan therefore keeps ~6 KB of write
  frontier — comfortably L1. Wide tables degrade gracefully rather than thrashing.
- `alignas(64)` on fixed staging arrays so columns never share a cache line.
- The wire walk is sequential — the hardware prefetcher covers it; no software prefetch needed.
- **Open candidate, not adopted here:** for string columns whose output exceeds L2, converting in
  value-aligned blocks (~L2/2) and scanning each block immediately — while it is still hot —
  would replace one RAM write plus one RAM read with cache-resident reuse. It only helps the
  non-ASCII path (the ASCII path never re-reads the output) and only above L2, so it needs a
  large non-ASCII fixture to justify. Blocks stay thousands of values wide, so the per-value call
  overhead this phase eliminated does not come back.

Notes:

- There is **no bulk `string_t` API** in DuckDB, and there cannot be a zero-work one: values
  ≤ 12 bytes live *inline inside the `string_t`* (12-byte threshold), so short strings must be
  copied into the slot rather than pointed at. The measured 1.3 ns/value residue is that
  construction; it is the output-side analogue of the input-side pivot.
- Direct-write bypass for 1:1 types is **mandatory, not an optimization**: those families already
  sit at the 1.7–2.2 ns floor, so routing them through staging could only add a copy. Any
  regression there fails the phase (§7.2).
- `ValidityMask::Initialize(validity_t*, idx_t)` can point a mask at external memory — do **not**
  use it for staging buffers (they are reused next chunk). Copy the 32 bytes.

### D6a. Code architecture — abstraction without per-value cost

Maintainability constraint: the whole phase budget is ~2 ns/value, and a virtual call or a
function-pointer call costs 1–2 ns. **Therefore no abstraction may sit on the per-value path.**
All polymorphism resolves at the *column* level, which is entered ~4 times per chunk rather than
8192 times.

Three layers, each with one job:

```cpp
// src/include/codec/staging/column_staging.hpp — DATA, no behaviour
enum class StagingKind : uint8_t { Direct, Fixed, Var };

struct ColumnStaging {
    StagingKind kind;
    uint32_t stride;              // Direct / Fixed
    uint8_t *dst;                 // Direct: into the DuckDB vector; Fixed: into the arena
    duckdb::vector<uint8_t> payload;            // Var: delimited UTF-16 / raw bytes
    duckdb::vector<uint32_t> offsets, lengths;  // Var
    duckdb::vector<uint64_t> validity_words;
    bool saw_embedded_nul, boundary_risky;      // computed during append
};

// Resolved ONCE per column after COLMETADATA — the only indirect call in the design
struct ColumnOps {
    StagingKind kind;
    uint32_t stride;
    void (*finalize)(ColumnStaging &, Vector &out, idx_t count);
};
```

- **Append** (per value, hot): a `switch (kind)` inlined in the row reader. Three arms, and the
  arm is invariant per column, so the branch predictor is right every time. Never a virtual call,
  never a function pointer.
- **Finalize** (per column per chunk, cold enough): one indirect call into the family kernel.
- **Family kernels** live in the existing `src/codec/<family>_codec.cpp` (spec 045 structure
  preserved), written as templates on `<RAW, DST>` so each type variant is a statically
  specialized, auto-vectorizable loop rather than a runtime switch. One readable loop per family,
  not one per type.
- **`StagingArena`** owns every buffer, implements the high-water/watermark policy, and is the
  single place lifetime rules are stated — so "may this pointer outlive the chunk?" has exactly
  one answer to look up.

What this buys in review terms: the type-dependent logic stays inside the family module (as
today), the hot loop contains no dispatch to read past, and the dangerous invariants (no
`string_t` into staging, staging reused next chunk, arena capacity) are concentrated in the arena
rather than scattered across nine codecs.

### D7. NULL handling and PLP payload policy

- **NBC bitmaps** (`ReadNBCRow`, `tds_row_reader.cpp:51-95`): translate the wire bitmap into
  staging `validity_words`, then one memcpy per column per chunk (D6) instead of per-row
  `SetNull`.
- **ROW tokens**: per-value bit into the same words at append time.
- **PLP/MAX values**: accumulate into `VarStaging.payload` instead of a per-value
  `vector<uint8_t>`. A per-column `max_payload` bound with checked arithmetic replaces today's
  unbounded per-value growth; a single value larger than the bound still works (own oversized
  path); capacity above K× the recent peak is released on a watermark cadence (K and cadence are
  bench outputs, defaults recorded in the PR).

### D8. Error reporting contract

Batch finalizers lose per-value context. On a finalizer error, re-run the failing column per value
through the surviving `ConvertValue` path to locate the offending row, then raise with: column
index, column name, in-chunk row index, source TDS type, target DuckDB type, reason. Cold path —
diagnosability only.

### D9. Issue #197 — NTEXT / IMAGE

Both reach the codec with no decoder (`FamilyFromTdsType` has no arm for 0x63 / 0x22), so the
query dies with "Connection closed while waiting for COLMETADATA".

**Route: server-side CAST in `BuildColumnExpression`** (`table_scan.cpp:110-139`), as a new arm
after `is_cast_required` and before `NeedsNVarcharConversion`:

- `NTEXT` → `CAST([c] AS NVARCHAR(MAX))` — skipped today at the `col.is_unicode` early return
  (`table_scan.cpp:60`), since `ntext` is in the unicode list (`mssql_column_info.cpp:172`).
- `IMAGE` → `CAST([c] AS VARBINARY(MAX))` — skipped today because `IsTextType` is false
  (`table_scan.cpp:64`); it needs the binary target, so it cannot reuse the NVARCHAR path.

**Correction to the issue text:** #197 says `MSSQLColumnInfo::IsLegacyLobType` "already identifies
all three" — that helper does not exist. What #190 left is the file-static `IsLegacyTextLob`
(`table_scan.cpp:49-54`), matching `"text"` only. Generalize it (or add a sibling) — budget for
writing it, not finding it.

Catalog mapping already correct: `ntext` → `VARCHAR` (`mssql_column_info.cpp:172`), `image` →
`BLOB` (`:214`); both are in `IsKnownSQLServerType` (`:248-250`), which is why `is_cast_required`
is false.

Tests: SQL regression in `test/sql/catalog/` reproducing the issue (both columns, NULL rows, empty
values, >8000-byte values) + assertions that catalog types stay VARCHAR/BLOB. `mssql_scan` (raw
T-SQL, no catalog metadata → no CAST rewrite) still cannot read these columns; that is unchanged
and out of scope, and the test documents it. Closes #197.

### D10. Counters

Extend the 054 D4 counter block with what this phase makes countable: staged bytes per family,
per-kernel finalize time, direct-write-bypass value count, string boundary strategy taken
(ascii-arithmetic / sweep / skip+sweep / skip+memchr), prealloc cap fallbacks, PLP payload
high-water and release events, per-value error-path re-runs.

### D11. Benchmarks and delta capture

The prototypes already live in `test/cpp/bench_materialize.cpp` (research variants; they get
pruned to the shipped strategy plus the reference paths when the implementation lands). The
fixture matrix gained long non-ASCII cells (len 24/32/48/64/256 × cyrillic/cjk) — the crossover
points in D5 come from them and they stay.

Re-run the full D1 matrix + the D3 TPC-H e2e (SF 0.01/0.1/1) and append delta columns to
`test/bench/bench_results_simd_baseline.md`; e2e via same-session interleaved A/B.

## 5. Acceptance criteria

1. **Correctness:** `diff_check.sh` 13/13 byte-identical against the phase-0 build at every
   conversion-touching commit — with the single documented exception of the invalid-UTF-16
   replacement change (D2), which must be reviewed as an intended diff; full `make test` + the
   124-case integration suite green.
2. **No regression on the cheap path:** int/float/uuid/bit decode cells stay within 3% of the
   baseline. A regression here fails the phase regardless of gains elsewhere.
3. **DECIMAL:** `decimal_p38s0` ≤ 8 ns/value (prototype: 3.2, baseline 73.3);
   `decimal_p18s6` ≤ 6 (prototype 2.1, baseline 34.2).
4. **Strings:** short-ASCII cells ≤ 4 ns/value (prototype 2.1–2.5, baseline 14–19); non-ASCII
   len16 ≤ 9 (prototype 5.2–7.5, baseline 27); no cell slower than the shipped post-R1 path.
5. **e2e:** no step regresses > 3% under the interleaved protocol; `scan_full_lineitem` and
   `scan_strings` are the load-bearing steps and should improve.
6. **#197:** the issue's repro returns data for both columns; issue closed.
7. **Memory:** a large-PLP regression test (multi-MB values across many chunks) shows staging
   capacity returning to the watermark, not growing monotonically; the 1.5× string over-allocation
   respects `max_payload` and falls back cleanly; a debug-build check asserts no `string_t` points
   into staging.
8. **Representation invisible at SQL level:** all existing sqllogictests pass unchanged; output
   vectors remain FLAT this phase.

## 6. Task order

```text
T0   D0 read-path phase timers + live-server decomposition  -> numbers, re-size if surprising
T0b  D0b TDS frame size setting + multi-frame receive staging -> diff_check, sweep
T1   D1 decimal kernel + fixture tests + endianness guard   → diff_check, micro delta
T2   D11 bench pruning (keep shipped strategies + reference)
T3   D3 ColumnStagingSet + ownership/watermark tests
T4   D4 per-column ops resolution (dispatch hoist, per-value bodies unchanged)
T5   D3 RowReader pivot + direct-write bypass    → diff_check + int/float no-regression gate
T6   D7 NBC → validity words; D6 batch validity write
T7   D5 batch finalizers: datetime, money, uuid, bit
T8   D5 decimal finalizer (hoists T1's kernel out of the per-value call)
T9   D2 standard-replacement fallback; delete the legacy converter
T10  D5 string batch path: delimited staging, prealloc convert, boundary strategies
T11  D7 PLP payload policy + oversized/watermark tests
T12  D9 #197 NTEXT/IMAGE + SQL regression      → closes #197
T13  D8 error-path contract; D10 counters
T14  Full re-run: micro + e2e interleaved; append delta columns; update design doc
```

T0 gates the sizing of everything after it: if the 80 ns floor is not where this spec assumes,
re-scope before T3. T1–T4 are otherwise independent. T5 is the highest-risk commit and lands alone. T9 must precede T10 (the
bulk path needs the isolated-value fallback). T12 is independent of the staging chain.

## 7. Risks

- **Blast radius.** This rewrites the hottest, most-tested path. Mitigation: strict task order,
  diff_check at every step, T5 isolated.
- **Regressing already-optimal types** — see D6; criterion 5.2 makes it a blocker.
- **rowid STRUCT target vectors.** `SetTargetVectors` (`mssql_result_stream.hpp:130`, consumed by
  `table_scan.cpp` `PopulateRowIdVector`) writes into STRUCT children instead of `chunk.data`.
  Finalizers must honour that indirection — `test/sql/rowid/` must stay green unmodified.
- **Multi-statement `mssql_scan`**: staging must reset on COLMETADATA change, not just on chunk
  boundaries.
- **Memory profile change**: fixed arrays + payload arena + 1.5× string over-allocation vs today's
  32-byte-reserve per-column vectors. Bounded by `max_payload` and criterion 5.7; numbers go in
  the PR.
- **The invalid-UTF-16 behaviour change** is user-visible. Release note + an intended diff_check
  delta; not silently shipped.

## 8. Prototype measurements (macOS ARM64, 2048-row chunks, median of 400)

All numbers ns/value. Produced by `test/cpp/bench_materialize.cpp` before this spec was written;
two runs agreed within noise unless stated.

**Strings — strategy comparison at len16 ASCII** (why the scheme is what it is):

| strategy | ns/value |
| --- | --- |
| current (shipped post-R1) | 18.1 |
| A: per-value convert, shared output buffer | 14.4 |
| B: per-value length + one bulk convert | 8.4 |
| C: delimiters + `memchr` per value | 8.0 |
| C2: delimiters + one word-wise sweep | 5.1 |
| exact-length + arithmetic boundaries (ASCII) | 3.7 |
| **prealloc (no length pass) + arithmetic** | **2.1** |
| floor: bulk length + bulk convert, no boundaries | 2.3 |
| floor: bulk convert only, no length pass | 0.8 |

Reading: strategy A — keeping conversion per value and only removing temporaries — buys 1.3×.
Everything else comes from one conversion per column, and the last step comes from deleting the
length pass. The floor rows show the conversion itself is 0.8 ns/value: **the shipped path spends
~95% of its time not converting**.

**Boundary strategy crossovers** (non-ASCII, ns/value):

| cell | sweep | skip + sweep | skip + memchr |
| --- | --- | --- | --- |
| len16 cyrillic | **5.2** | 5.8 | 7.9 |
| len24 cyrillic | 7.2 | **7.0** | 8.9 |
| len48 cyrillic | 13.3 | **10.2** | 11.6 |
| len64 cjk | 27.2 | 20.9 | **20.8** |
| len256 cyrillic | 64.6 | 38.4 | **36.4** |
| len256 cjk | 110.1 | 70.6 | **65.9** |

Skip-ahead starts paying at ~24 units; `memchr` overtakes the hand-rolled sweep at ~64. Note
`len256 cjk` without skip (110.1) is *no better than the current code* (110.5) — for
heavily-expanding scripts the skip is not an optimization but a requirement.

Also settled here: scanning the output beats recomputing lengths from the input (len256 cyrillic
36.4 vs 55.3), so there is no input-side variant worth keeping.

**Intrinsics headroom:** at len256 cyrillic the best strategy sits 10 ns above the conversion
floor, of which ~1.3 is `string_t` construction. A hand-written NEON/AVX delimiter scan could
chase the remaining ~8 ns — bounded, and not worth the portability cost in this phase.

**A bug worth recording.** The first skip-ahead prototype probed the guessed position `seg + 2*u`
before scanning. Every fixture passed, because every fixture is single-script (Cyrillic is exactly
2 bytes/unit, CJK exactly 3), so the probe either always hit or always missed. For a mixed-script
value whose length falls strictly between `u` and `2u` the probe can land on a later value's
delimiter and silently merge two strings. The fixture matrix cannot catch this class of bug —
hence the explicit rule in D5 and a mixed-script fixture added to the matrix.

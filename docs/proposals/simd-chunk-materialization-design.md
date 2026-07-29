# Design: Batch / SIMD chunk materialization between TDS and DuckDB vectors

**Status:** Design (pre-spec). Investigation complete; benchmarks pending.
**Scope:** TDS → DuckDB DataChunk materialization (scan) and DuckDB DataChunk → BCP/TDS encoding (COPY/CTAS).
**Goals:** throughput and maintainability. **Verified against:** DuckDB v1.5.5 submodule (`d8cdaa3`), simdutf 6.1.1, extension @ v0.2.2.

This document grounds the original research draft (columnar staging + SIMD batch conversion +
CONSTANT/DICTIONARY vector reuse) in the actual code: what exists today with file:line evidence,
which DuckDB/simdutf APIs are confirmed available, which draft ideas survive contact with the
code, what changes shape, and how we benchmark before committing to an implementation.

---

## 1. Current implementation inventory (verified)

### 1.1 Read path: TDS bytes → DataChunk

```text
socket → TdsTokenParser::Feed (buffer_ append, sliding window)
       → ParseRow / ParseNBCRow                     tds_token_parser.cpp:259/301
       → RowReader::ReadRow / ReadNBCRow            tds_row_reader.cpp:11/51
         → per column: ReadValue → row.values[col]  (std::vector<uint8_t> per column)
       → MSSQLResultStream::FillChunk loop          mssql_result_stream.cpp:198-351
         → ProcessRow → per column:
           TypeConverter::ConvertValue              type_converter.cpp:263-341 (switch per VALUE)
           → codec DecodeFromTds → Vector write
```

Key measured facts:

| Fact | Evidence |
|---|---|
| Chunk loop fills up to `STANDARD_VECTOR_SIZE` (2048) rows, then `SetCardinality` | mssql_result_stream.cpp:218,341 |
| **Raw bytes are already copied once** into `RowData::values` (`vector<vector<uint8_t>>`, one per column, capacity reused across rows, 32 B pre-reserve) | tds_token_parser.hpp:81-107 |
| Codec dispatch is a 20-way `switch` on `type_id` executed **per value** (per cell) | type_converter.cpp:280-340 |
| Fixed-width types already write directly into `FlatVector::GetData<T>()[row]` — no `duckdb::Value`, no generic casts | integer_codec.cpp:89-117, float_codec.cpp:107-118 |
| Strings: `Utf16LEDecode(bytes)` → temporary `std::string` → `StringVector::AddString` → **second copy** into the vector heap; one simdutf call per value | string_codec.cpp:154-179 |
| NULLs: ROW tokens set `null_mask` per value during read; NBCROW parses a bitmap up-front; either way materialization does per-row `FlatVector::SetNull` | tds_row_reader.cpp:63-68, type_converter.cpp:266 |
| PLP (MAX types): chunked, accumulated into one `vector<uint8_t>` per value; may span packets | tds_row_reader.cpp:506-592 |
| Scan output vectors are always FLAT today; `SetTargetVectors()` exists for the composite-PK rowid STRUCT case | mssql_result_stream.hpp:129-131 |
| rowid is populated post-`FillChunk` by copying PK columns | table_scan.cpp:548-624 |

Interpretation: **a columnar staging layer replaces an existing per-row copy, it does not add a
new one.** The row→column pivot point already exists inside `RowReader`; today it pivots into a
row-oriented structure (`RowData`), the design pivots it into per-column staging instead.
The per-cell costs to eliminate on this path are (a) the per-value dispatch switch,
(b) the string double-copy through `std::string`, (c) per-value `FlatVector::SetNull` calls
where NBC bitmaps could bulk-write validity words.

### 1.2 Write path: DataChunk → BCP

```text
BCPCopySink(DataChunk)                              copy_function.cpp:542-620
  → BCPWriter::WriteRows (per-row loop, one mutex acquire per chunk)   bcp_writer.cpp:96-133
    → BCPRowEncoder::EncodeRow (per-column loop)    bcp_row_encoder.cpp:65-140
      → per value: ToUnifiedFormat + family switch + EncodeToBcp
  → accumulator_buffer_ (~1 MB reserve, reused across batches)
  → flush at mssql_copy_flush_rows → BuildBulkLoadMultiPacket (4 KB packets) → socket
```

| Fact | Evidence |
|---|---|
| `vec.ToUnifiedFormat(1, format)` is reconstructed **per value** (inside `GetVectorValue` / `IsVectorNull`) — per cell, per row | integer_codec.cpp:48-55, bcp_row_encoder.cpp:34-59 |
| Family dispatch: 9-way switch per row per column | bcp_row_encoder.cpp:111-138 |
| DICTIONARY/CONSTANT inputs are handled *transparently* (via `format.sel`) but **never exploited** — a constant string column is re-converted to UTF-16 for every row | design gap |
| NVARCHAR encode does up to 3 passes per value: `ValidateNVarcharLength` (validate + `utf16_length_from_utf8`), then `Utf16LEEncodeDirect` (validate **again** + convert) | string_codec.cpp:106-122, 48-55; utf16.cpp:344-355 |
| COLMETADATA precomputed once per batch; packet fragmentation post-batch; DONE per flush | bcp_writer.cpp:347-445, 461-479 |
| Fixed types (int/decimal/datetime/GUID) append to the accumulator with no per-value allocation | bcp_row_encoder.cpp:260-286, 339-377, 442-447 |
| INSERT literal path is per-value string formatting via `FormatSqlLiteral` — orders of magnitude more expensive than BCP by construction; out of scope for batch optimization | mssql_value_serializer.cpp:86-103 |

### 1.3 UTF-16 layer (simdutf 6.1.1)

Wrapper (`src/tds/encoding/utf16.cpp`) uses the two-pass scheme: `validate_*` gate →
`convert_valid_*` fast path → hand-rolled legacy fallback on invalid input (bit-identical to
pre-spec-043 semantics). All hot call sites are **per string value**:
decode `string_codec.cpp:159`, encode `string_codec.cpp:51,85`, length check `string_codec.cpp:115`.

Available-but-unused simdutf APIs relevant here: `convert_utf16le_to_utf8_with_errors`,
`convert_utf8_to_utf16le_with_errors` (single-pass with error position — candidate to replace
the double-validate on the BCP path). simdutf has **no multi-string batch API**: batching is
achieved only by concatenation or by amortizing call overhead into fewer, longer calls.

---

## 2. Verified platform facts (what the design may rely on)

All checked directly in the pinned v1.5.5 submodule — not from memory.

1. **Table functions may legally emit DICTIONARY and CONSTANT vectors.** The Parquet reader
   does exactly this: `result.Dictionary(dictionary, dictionary_selection_vector)` —
   `duckdb/extension/parquet/decoder/dictionary_decoder.cpp:117`. Two constraints observed there:
   - dictionary emission only when the chunk is filled **from offset 0** (`result_offset == 0`;
     otherwise it flattens via `VectorOperations::Copy`). Our `FillChunk` always fills from 0. ✅
   - NULL rows are represented by an **extra child slot appended after the dictionary values**
     (`sel.Verify(count, dictionary_size + can_have_nulls)`) — precisely the mixed-null model
     from the draft (§5.2). We adopt the same convention.
2. **APIs** (duckdb/src/include/duckdb/common/types/vector.hpp):
   - `Vector::Dictionary(Vector &dict, idx_t dictionary_size, const SelectionVector &sel, idx_t count)` (:199)
   - `Vector::Dictionary(buffer_ptr<VectorChildBuffer> reusable_dict, const SelectionVector &sel)` (:201) +
     `DictionaryVector::CreateReusableDictionary(type, size)` (:440) — lets us allocate the
     ≤100-entry child **once per column per stream** and reuse it across chunks.
   - `ConstantVector::SetNull/GetData/Validity` (:333-389).
   - `StringVector::AddBuffer(Vector&, buffer_ptr<VectorBuffer>)` (:571) — attach a caller-owned
     backing buffer so `string_t` may point into it; `StringVector::EmptyString(vector, len)`
     (:565) — allocate a writable, vector-owned string slot to convert **directly into**.
   - `SelectionVector` owns its data via `buffer_ptr<SelectionData>` (shared) — safe to build
     once and hand to the vector (selection_vector.hpp:31-136).
3. **`string_t` inline threshold is 12 bytes** (string_type.hpp:29). Strings ≤12 B are stored
   inline in the `string_t` itself — a shared backing buffer only pays off for longer strings;
   short strings must be written into the `string_t` regardless. This matters for the
   short-string benchmark fixtures.
4. **`UnifiedVectorFormat` erases the original `VectorType`.** A representation-aware BCP writer
   must check `vec.GetVectorType()` **before** calling `ToUnifiedFormat` (vector.hpp:31-76).
5. **ValidityMask is 64-bit-word addressable** (`GetData()`, `SetAllValid/SetAllInvalid`,
   validity_mask.hpp:60-381) — NBC bitmaps can be translated bitmap→validity words rather than
   per-row `SetNull`.
6. **Downstream operators do not force-flatten dictionary vectors**; expression execution works
   on unified format. (Flatten happens only where an operator's contract requires it.) So
   emitted dictionaries survive at least through common projections/filters — enough for the
   LIMIT/aggregate/filter workloads a remote scan feeds.
7. **C++ standard: mixed-mode is confirmed dead; whole-tree C++17 is achievable.**
   Empirically verified (2026-07-28, GCC 11.4 on Linux, pattern lifted from v1.5.5
   `types.hpp:392` / `types.cpp:179`):
   - Compiling extension TUs as C++17 against C++11-built DuckDB reproduces the historical
     `multiple definition of 'LogicalType::BIGINT'` link error. Root cause: GCC emits C++17
     inline variables as **GNU-unique (`u`) symbols**, which conflict with the strong
     out-of-line definitions still present in `types.cpp:179+`. So
     `target_compile_features(... cxx_std_17)` per-target remains forbidden — CLAUDE.md's
     warning is current, not stale. (duckdb-spatial is *not* a precedent: its root
     CMakeLists sets standard **11**; no `cxx_std_17` in spatial/azure/delta/iceberg.)
   - The viable lever: DuckDB loads `DUCKDB_EXTENSION_CONFIGS` at **top level, before
     `add_subdirectory(src)`** (duckdb/CMakeLists.txt:834 include → extension_build_tools.cmake:557
     foreach-include vs `add_subdirectory(src)` at :854). A plain
     `set(CMAKE_CXX_STANDARD 17)` in our `extension_config.cmake` therefore flips the
     **entire tree** — DuckDB, third_party, and the extension — to C++17 consistently, in
     local builds, our CI, and community-extensions builds alike (the config ships with the
     extension; no pipeline knob needed).
   - Gate before adoption: full-matrix build + tests (Linux GCC, macOS Clang, **MSVC and
     MinGW** — bundled fmt is a known MSVC sore spot, cf. issue #165; watch for C++17
     removals like `std::random_shuffle` in third_party).
   - **Adoption stance (maintainer decision):** C++17 is *not scheduled*. It is taken up only
     when a concrete kernel or refactor is demonstrably better in C++17 (readability or
     performance) — not for its own sake. Until then all code stays C++11-compatible; this
     subsection documents the verified mechanism for when that justification appears.
   No intrinsics in v1 either way — rely on simdutf + auto-vectorizable loops
   (GCC/Clang/MSVC all handle contiguous fixed-width loops; MinGW is the one to watch in CI).

---

## 3. Target architecture — read (TDS → DataChunk)

### 3.1 Pipeline

```text
TDS packets → token/row framing (unchanged)
  → per-column staging writes (pivot INSIDE RowReader, replacing RowData)
  → at chunk boundary (2048 rows or DONE):
      chunk analyzer per column:
        all-equal / all-null           → CONSTANT
        unique ≤ cap (default 100)     → DICTIONARY (reusable child + selection)
        otherwise                      → FLAT batch materialization
  → DataChunk handed to DuckDB
```

Latency note: the 2048-row accumulation boundary is identical to today's `FillChunk` loop —
staging adds no extra buffering stage, it changes the layout of the existing one.

### 3.2 Column staging classes

Replace `RowData` with a `ColumnStagingSet` owned by `MSSQLResultStream` (arena-style, reused
across chunks, capacity high-water-mark retained):

```cpp
// fixed-width families (integer/float/money/datetime raw/uuid/decimal buckets)
template <class RAW>
struct FixedStaging {                     // capacity = STANDARD_VECTOR_SIZE, alignas(64)
    RAW values[STANDARD_VECTOR_SIZE];
    uint64_t validity_words[STANDARD_VECTOR_SIZE / 64];
};

// var-width families (UTF-16 strings, binary)
struct VarStaging {
    std::vector<uint8_t> payload;         // contiguous UTF-16LE / raw bytes
    uint32_t offsets[STANDARD_VECTOR_SIZE];
    uint32_t lengths[STANDARD_VECTOR_SIZE];   // unit: BYTES (fixed contract)
    uint64_t validity_words[...];
    bool saw_embedded_nul;                // updated on append (for strategy C gating)
    bool boundary_risky;                  // last code unit is a high surrogate anywhere
};
```

**Direct-write bypass (important refinement of the draft §4.2):** for 1:1 families where the
TDS payload already equals DuckDB physical layout (INT1/2/4/8, FLOAT4/8 — both little-endian),
staging is *skipped entirely*: the row reader writes straight into
`FlatVector::GetData<T>()` at `row_idx`. Today these values take a detour through
`RowData::values` byte vectors; the redesign removes a copy for the cheapest types rather than
adding staging overhead to them. These columns are simultaneously fed to a tiny
constant-detector (compare against row 0) so the CONSTANT case is still caught with one
branch per value, but they are **excluded from dictionary building** (draft §5.4 policy).

Staged families and their batch finalizers:

| Family | Staged form | Finalize (FLAT case) |
|---|---|---|
| DATETIME2/DATETIMEOFFSET/DATE/TIME | packed raw (days/ticks or per-scale ints) | scale/epoch arithmetic loop → `timestamp_t[]` (auto-vectorizable multiply/add) |
| DECIMAL/NUMERIC | sign byte + fixed magnitude bucket (8/16 B) | per-precision-width specialized loop → int64/hugeint |
| MONEY/SMALLMONEY | raw int64/int32 | scale loop |
| UNIQUEIDENTIFIER | 16-byte records | byte-shuffle loop → DuckDB UUID layout |
| N(VAR)CHAR/XML | `VarStaging` UTF-16LE | §3.4 |
| (VAR)BINARY | `VarStaging` raw | memcpy into vector heap / backing buffer |
| BIT/BITN | byte per row | widen loop |

PLP/MAX values: staged into the same `VarStaging.payload` (chunk accumulation loop appends
there instead of a per-value vector). A per-column `max_payload` policy (checked arithmetic,
oversized-single-value support, watermark-based capacity release) replaces the current
unbounded per-value vectors. Columns containing any PLP value in the chunk are excluded from
dictionary consideration (draft §5.4: long payload dedup only when clearly profitable —
deferred to the adaptive phase).

### 3.3 NULL handling

- NBCROW: translate the wire bitmap directly into staging `validity_words` (word-wise, inverted;
  bitmap is column-major per row so this is a per-row scatter — but into words, not into
  per-row `SetNull` calls), then one `FlatVector::SetValidity`/mask copy per column per chunk.
- ROW tokens: per-value flag into validity words at append time (same cost as today's bool).
- Fixed staging writes a neutral placeholder into invalid slots so finalize loops stay
  branch-free (draft §6.2 confirmed viable — DuckDB does not require deterministic bytes under
  invalid slots).

### 3.4 String materialization strategies (to benchmark, not to hardcode)

Baseline **A** (per-value, zero-temporary): compute `utf8_length_from_utf16le` per value,
prefix-sum into output offsets, allocate ONE backing buffer for the whole column
(`make_buffer<VectorStringBuffer>`-owned, attached via `StringVector::AddBuffer`), then
`convert_valid_utf16le_to_utf8` per value directly into its slot; build `string_t` manually
(inline for ≤12 B, pointer into backing otherwise). Eliminates both the `std::string`
temporary and the `AddString` copy while keeping per-value simdutf calls.

**B** (bulk convert of concatenated payload): one `convert_valid_utf16le_to_utf8` over the whole
concatenated staged payload + per-value `utf8_length` prefix sum for slot boundaries.
Correctness hazard confirmed in the draft: a value ending in an unpaired high surrogate merging
with the next value's leading low surrogate changes semantics at the boundary. Gate: bulk path
requires whole-payload validation **plus** `boundary_risky == false` (no value ends in
0xD800–0xDBFF); otherwise fall back to A. The flag is computed during staging append (one
compare of the last code unit per value).

**C** (NUL-delimited single conversion + `memchr` splitting): only legal when
`saw_embedded_nul == false` (U+0000 is a valid NVARCHAR character; detection happens at append
via a 16-bit scan of the value). Benchmark hypothesis: wins only on long-string columns; the
delimiter overhead dominates for short strings.

Decision procedure: implement A as the default in the first phase (it already removes the two
copies and all temporaries); implement B and C behind the benchmark harness; promote per the
§7 acceptance criteria, keyed on average staged length (e.g. B/C for avg ≥ N bytes). Dictionary
and constant columns bypass all three: ≤100 unique values are converted individually into the
reusable child vector — at most 100 conversions instead of 2048 (draft §6.5 stands as-is).

### 3.5 Chunk analyzer & representation choice

Per column, at chunk finalize, in this order (draft §5.1 confirmed):

1. all-NULL → `CONSTANT` + `ConstantVector::SetNull`.
2. all-valid-and-equal (raw-representation compare) → `CONSTANT` (one decode).
3. dictionary attempt — only for families whose policy allows it (strings/decimal/datetime/UUID;
   never 1:1 ints/floats/bools): chunk-local hash over **raw staged bytes** (hash confirmed by
   full equality; keys are offsets into staging payload, which never reallocates mid-chunk —
   reserve enforced), cap `MAX = mssql_scan_dictionary_max` (default 100, `0` disables). On
   overflow → FLAT, dedup state discarded.
4. FLAT batch materialization otherwise.

Dictionary emission: reusable child per column (`CreateReusableDictionary`, size cap+1 slot for
NULL, parquet-style), unique values decoded into child, `SelectionVector` (owned, reused)
mapping 2048 rows → child indexes, NULL rows → the trailing invalid slot, then
`Vector::Dictionary(reusable_dict, sel)`.

Interaction checks (verified):
- `FillChunk` fills from offset 0 → dictionary emission legal (parquet precedent constraint).
- rowid post-processing (`PopulateRowIdVector`, table_scan.cpp:548) copies PK columns via
  vector ops that accept any representation; must be re-verified in tests when PK columns
  become dictionaries (worst case: `Flatten()` those specific columns — cheap, they are ints
  almost always, hence FLAT by policy anyway).
- Debug builds run `Vector::Verify`; the parquet conventions we mirror already satisfy it.

Cost policy: start with the draft §5.5 shape (thresholds + overflow fallback) but treat the
weights as benchmark outputs, not design inputs. The §8 counters exist precisely to calibrate
this.

### 3.6 Codec architecture changes

- Dispatch: resolved **once per column** after COLMETADATA into a small ops struct
  (`append_raw` / `finalize_flat` / `build_dictionary_child` function pointers or a
  per-family virtual resolved outside the hot loop) — removes the per-value 20-way switch
  (type_converter.cpp:280). Batch finalizers are statically specialized loops (templates on
  RAW/DST), which is what makes auto-vectorization possible.
- The chunk analyzer is family-agnostic; hashing/equality/cost callbacks come from the codec
  (draft §8.3 as written).
- File layout: staging + analyzer live in `src/codec/staging/` (new), family batch kernels join
  the existing `src/codec/<family>_codec.cpp` files so per-type logic stays consolidated
  (spec 045 structure preserved — maintainability goal).
- Error reporting contract (draft §10.2): slow-path decode errors carry column index, in-chunk
  row index, source TDS type, target type, reason. The staging layer must keep enough info to
  reconstruct the row index after batch failure (re-run the failing column per-value on error —
  rare path, correctness only).

---

## 4. Target architecture — write (DataChunk → BCP)

### 4.1 Stage 0 quick wins (no representation change, measurable immediately)

1. **Hoist `ToUnifiedFormat` to once per column per chunk.** Today it runs per *cell*
   (integer_codec.cpp:50). Mechanical change inside `BCPRowEncoder::EncodeRow` callers: build
   `UnifiedVectorFormat formats[ncols]` before the row loop, pass down. No behavior change.
2. **Hoist family dispatch out of the row loop**: per-column encoder function resolved once
   per chunk (mirror of read-side §3.6).
3. **Single-pass NVARCHAR encode**: replace validate+length / validate+convert
   (3 scans per value today, string_codec.cpp:115+51) with one
   `convert_utf8_to_utf16le_with_errors` into a sized scratch, or validate-once + length+convert.
4. Reserve the accumulator per chunk from column-width estimates instead of growth-by-resize
   inside string appends (string_codec.cpp:50 resize per value).

These are low-risk, pure-win changes and double as the control group for benchmarking the
bigger redesign.

### 4.2 Representation-aware encoding

Check `vec.GetVectorType()` **before** `ToUnifiedFormat` (fact §2.4):

- **CONSTANT**: encode payload once (`null` → per-row null markers only); row serializer
  memcpy's the same encoded span per row. TDS/BCP wire format still carries every row —
  the savings are conversion + allocation, not bytes (draft §1.2/§3.2 non-goals stand).
- **DICTIONARY**: chunk-local cache `encoded[child_index]`; encode each **used** child value
  once (selection scan → used set), then per-row memcpy of cached spans. This is where a
  DuckDB-side dictionary (e.g. reading from Parquet and writing to SQL Server) turns 2048
  UTF-16 conversions into ≤ dictionary-size conversions with zero analyzer cost — the
  representation arrives for free.
- **FLAT fixed-width**: columnar batch encode into per-column staged spans (datetime
  decomposition, decimal bucket write, GUID shuffle as loops), then the row-oriented serializer
  stitches spans. BCP framing itself stays row-oriented (protocol requirement).
- **FLAT strings**: baseline = per-value single-pass encode into the accumulator (after §4.1
  this is already 3× fewer scans). Adaptive local dedup (≤100, stop-on-overflow) is
  benchmark-gated and off by default (draft §7.4).

NULL serialization stays in the existing per-wire-type helpers
(`EncodeNullFixed/Variable/PLP/GUID/DateTime`, bcp_row_encoder.cpp:478-500) — unchanged
contract, validity checked before selection deref (draft §7.6).

---

## 5. Memory ownership rules

- Staging arenas: owned by `MSSQLResultStream` (read) / `BCPWriter` (write); fixed arrays
  are POD members; var payload keeps high-water capacity with a watermark release policy
  (release when capacity > K× recent peak; K and cadence TBD by bench).
- All `string_t` handed to DuckDB point into (a) the vector's own `VectorStringBuffer`
  (strategy A slots / `EmptyString`), (b) an `AddBuffer`-attached caller buffer, or
  (c) inline storage. Never into staging (staging is reused next chunk).
- Reusable dictionary children and `SelectionVector` data are `buffer_ptr`-owned and attached
  to the outgoing vector per the parquet pattern — lifetime extends with the chunk.
- Checked arithmetic on every capacity/offset computation (`uint32_t` offsets cap a column's
  chunk payload at 4 GiB; enforce `max_payload` well below).

---

## 6. Correctness & differential testing

Invariants (draft §10.1 adopted verbatim): row order, NULL vs empty distinction, embedded NUL
preservation, no cross-value UTF merging, decimal/temporal semantics identical to current
codecs, dictionary hash always confirmed by equality.

Test strategy:
1. **Differential harness**: run identical queries through a baseline build and the staged
   build against the same SQL Server; compare full result sets (ORDER BY PK + hash). Datasets:
   the §7 fixture matrix + existing `test/sql/**` suites (which already cover type edges:
   datetimeoffset NBC, wide varchar, XML, PLP).
2. Unit tests per batch kernel (`test/cpp/`): fixture arrays → expected vectors, including
   invalid-UTF fallback rows, boundary-surrogate cases, embedded NUL, all-NULL, dictionary
   overflow at exactly cap and cap+1.
3. Existing sqllogictests must pass unchanged — representation is invisible at the SQL level.
4. Fuzzing: the TDS-parser fuzz target (#162) keeps covering framing; add a staging-level
   fuzz entry (random column mixes → analyzer → materialize) if the parser refactor moves
   parsing boundaries.

---

## 7. Benchmark plan (before and during implementation)

### 7.1 Harness

- Extend the existing pattern: `make bench-utf16` (median-of-N, byte-identical assertion,
  `-DMSSQL_BENCH_BUILD`, test/cpp/bench_utf16.cpp) with new micro benches:
  - `bench_string_materialize`: current path vs A vs B vs C vs dictionary vs constant.
    Fixture matrix from the draft §12.1: lengths {0,4,8,16,32,64,256,4096} code units ×
    scripts {ASCII, Cyrillic, CJK, surrogate pairs} × NULL {0,10,50,100}% ×
    cardinality {1,2,10,100,101,512,2048} × embedded-NUL × malformed-boundary.
    The 12-byte `string_t` inline threshold makes {4,8,16} the critical short-string cells.
  - `bench_fixed_materialize`: per family — row-by-row (current) vs staged scalar loop vs
    staged vectorized loop (compiler reports: `-Rpass=loop-vectorize` / `-fopt-info-vec`
    asserted in CI for the kernels) vs direct-write bypass; dictionary at {1,10,100} uniques.
  - `bench_bcp_encode`: FLAT unique / FLAT repetitive / DICTIONARY {1,10,100} / CONSTANT ×
    null ratios; measures conversion throughput separately from packet serialization.
- Metrics per bench: ns/value, bytes copied/output byte, allocations/chunk (hook the arena).

### 7.2 End-to-end

- `test/bench/bench_codec_e2e.sh` stays the template (TSV protocol, host metadata,
  bench_results.md recording). Extended with representation-sensitive steps: low-cardinality
  scan (dictionary win case), constant-column scan, LIMIT-heavy scan, and the reverse COPY
  cases.
- **TPC-H**: add `duckdb_extension_load(tpch)` to `extension_config.cmake` (bench/local builds;
  it is not shipped — the loadable mssql extension is unaffected). Then:
  `CALL dbgen(sf=…)` → COPY lineitem/orders/customer/part → SQL Server → scan back.
  - small: SF 0.01 and SF 0.1 (latency-dominated; catches per-chunk overhead regressions),
  - large: SF 1 and SF 10 (throughput; lineitem = numeric/date-heavy, part/customer =
    string-heavy, l_shipmode/l_returnflag = natural low-cardinality dictionary columns).
  - measure both directions + `select_full`-style drain; record small AND large per the
    requirement that neither regresses.
- Environment reality check (from spec 044 experience, bench_results.md): at 100M rows the
  codec is invisible behind SQL Server I/O in Docker — e2e validates *no regression* and
  measures the drain step; **microbenches are the optimization signal**. Set expectations
  accordingly.

### 7.3 Acceptance criteria (draft §12.4 adopted)

An optimization ships enabled-by-default only if it: (1) shows a reproducible micro win and
non-regressing e2e on the target workloads; (2) does not degrade the common case beyond an
agreed threshold (proposal: 3% on the FLAT/unique path); (3) has a bounded-memory fallback;
(4) passes differential tests.

### 7.4 Observability

Per-stream counters (dumped at `MSSQL_DEBUG>=2` on stream close; zero-cost when disabled —
plain increments behind the existing debug-level check): draft §13 list verbatim
(`chunks_flat/dictionary/constant`, `dictionary_overflow`, `string_bulk_runs`,
`string_row_fallbacks`, `embedded_nul_fallbacks`, `invalid_utf_fallbacks`, raw/materialized
bytes, conversion ns). These are the calibration inputs for the §3.5 cost policy.

**"Zero-cost when disabled" is a requirement, not a description — and it has been violated
before.** Counting is cheap; *timing* is not. `FillChunk` carried four ungated
`steady_clock::now()` calls per row from spec 004 through v0.2.2 — ~59 ns/row on ARM64, on a path
whose entire per-row client budget is of that order; gating them cut live read cost by 23–64%
(spec 055 D0, `test/bench/bench_results_live_server.md`). Any timer added here must be latched at
construction and must accumulate in **nanoseconds**: the same code accumulated per-row intervals
via `duration_cast<microseconds>`, which truncated every ~100 ns row to zero and made the phase
it measured report approximately nothing.

---

## 8. Settings

| Setting | Default | Purpose |
|---|---|---|
| `mssql_scan_dictionary_max` | 100 | per-chunk unique cap; 0 disables dictionary/constant analysis (FLAT-only escape hatch) |
| `mssql_scan_batch_strings` | auto | `auto`/`per_value`/`bulk` — pins the §3.4 strategy for debugging/benchmarks |

(Existing settings table style; both read at stream init, atomics not needed.)

---

## 9. Related open issues to fix alongside

| Issue | Relation | Where it lands |
|---|---|---|
| [#177](https://github.com/hugr-lab/mssql-extension/issues/177) — `COPY TO (bcp)` throws NotImplemented on HUGEINT (e.g. `SUM()` output) | Left-behind TODO from spec 045 in `integer_codec.cpp` `EncodeToBcp` (~L151/189); DDL already maps HUGEINT→`DECIMAL(38,0)` and the literal path already serializes it — only the BCP arm throws | **Phase M/0** (small standalone fix): forward HUGEINT/UHUGEINT to `BCPRowEncoder::EncodeDecimal(v, 38, 0)` mirroring the UBIGINT arm, **plus a client-side range guard**: int128 extremes (2^127≈1.7e38, 39 digits) exceed `DECIMAL(38,0)` — out-of-range must raise a clean conversion error, not a server-side surprise. Regression test in `test/cpp/codec/test_integer_codec.cpp`; also unblocks the TPC-H benchmark itself (aggregate-then-COPY is exactly the failing pattern) |
| [#197](https://github.com/hugr-lab/mssql-extension/issues/197) — NTEXT/IMAGE unreadable (no decoder for wire types 0x63/0x22) | Decode-path gap in the same row-reader/codec layer the staging refactor rewrites | **Phase 1** ride-along: add legacy LONGLEN readers while the value-read dispatch is being restructured (cheaper to do during the pivot than before/after) |
| [#153](https://github.com/hugr-lab/mssql-extension/issues/153) — should BCP COPY allow compatible numeric coercion (BIGINT→INT) into existing tables? | Write-path type-mapping policy, adjacent to the representation-aware encoder | Open question for **Phase 3**: batch encoders make per-column coercion kernels cheap; policy decision (error vs range-checked coercion) stays a spec-level question |

Not related (tracked separately): #199 (test infra), #204/#122 (connectivity), #140 (DML), #85/#86 (catalog), #189, #129, #165 (MSVC fmt — relevant only as a C++17-gate canary, §2.7), #119.

## 10. Phasing → specs

**Measurement comes first.** No conversion code changes until the baseline is captured; every
phase re-runs the same suite; the final deliverable is a before/after report.

| Phase | Content | Risk | Candidate spec |
|---|---|---|---|
| **M (baseline)** | Bench harness + fixtures (§7.1) + TPC-H enablement (§7.2) + counters (§7.4) + differential harness (§6); **capture the full baseline on current main** into `test/bench/bench_results_simd_baseline.md` (micro: ns/value per family/strategy; e2e: TPC-H SF 0.01 / 0.1 / 1 / 10 both directions + drain; environment metadata per the spec-044 protocol). Includes the #177 HUGEINT fix (needed for aggregate-then-COPY benches). C++17 is **not** part of this phase (§2.7 adoption stance) | low | 054 |
| **0** | Write-side quick wins (§4.1: hoist UnifiedFormat + dispatch, single-pass NVARCHAR encode); read-side string copy elimination (A-lite: `EmptyString`/direct convert, kill the `std::string` temp). Re-run suite → first delta vs baseline | low | 054 |
| **1** | Read staging: `ColumnStagingSet`, RowReader pivot, direct-write bypass for 1:1 types, batch finalizers for datetime/decimal/GUID, NBC bulk validity, PLP payload policy; FLAT-only output. + #197 NTEXT/IMAGE decoders | medium | 055 |
| **2** | Chunk analyzer: CONSTANT + DICTIONARY emission, reusable children, cost policy v1, settings | medium | 056 |
| **3** | Write-side representation awareness: CONSTANT/DICTIONARY encode-once, FLAT columnar batch encode; #153 policy decision | medium | 057 |
| **4** | Bulk string strategies B/C promotion, adaptive dedup on write, cost calibration from counters, optional reader/materializer pipelining | opt-in by data | 058 |
| **R (report)** | Final before/after report: baseline vs post-implementation across the whole matrix (small AND large volumes), per-phase attribution of the wins, counter dumps for representative workloads, regressions (if any) with disposition. Lives next to the bench results; summarized in the release notes | — | closes the series |

Each phase is independently shippable and benchmark-gated; the baseline phase alone produces
the measurement infrastructure everything else is judged against.

---

## 11. Risks & open questions

1. **Dictionary payoff depends on downstream consumers.** Verified they don't force-flatten,
   but the actual win vs `selection_bytes` overhead is workload-dependent → cost policy is
   deliberately conservative + counter-calibrated. Worst case: `mssql_scan_dictionary_max=0`.
2. **VARCHAR under non-UTF-8 collations** is handled by scan-SQL rewrite (spec 026
   `BuildColumnExpression` / `mssql_convert_varchar_max`), so the decode hot path is
   overwhelmingly NVARCHAR/UTF-16 — the design targets the right bottleneck. Single-byte
   VARCHAR pass-through stays byte-for-byte unchanged.
3. **MSVC/MinGW auto-vectorization variance**: kernels must stay trivially vectorizable
   (contiguous, branch-free, no aliasing — `__restrict` where portable); vectorization-report
   assertions run on GCC/Clang only, MinGW gets perf smoke only (Rtools 4.2 constraint).
4. **rowid STRUCT target-vector path** (`SetTargetVectors`) must keep working with staging;
   composite-PK scan writes into STRUCT children — those columns get the FLAT path forcibly.
5. **FSST_VECTOR** exists in v1.5.5 but is out of scope (we never emit it; write-side treats
   it via Flatten fallback if ever encountered).
6. **Multi-statement `mssql_scan`** shares `FillChunk`; staging must reset cleanly across
   result sets within one stream (same reuse rules as chunks).
7. Parallelism (draft §11): explicitly deferred; the staged design is pipelining-ready
   (raw chunk = natural work unit), decision only after §7.4 counters show where time goes.

---

## Appendix A — draft deltas

Differences from the original research draft, post-verification:

- **Direct-write bypass for 1:1 fixed types replaces their staging** (draft §4.2 allowed this;
  code inspection shows it removes an existing copy — promoted from option to default).
- **Strategy B refined**: per-string convert into prefix-summed slots is the safe default
  formulation; the single-call concatenated variant needs the `boundary_risky` gate
  (unpaired-high-surrogate at value end), computed during staging.
- **Mixed-null dictionary model confirmed** against parquet: trailing NULL child slot, not a
  `[NULL, value]` child pair (draft §5.2 sketch adjusted to match engine convention).
- **`ToUnifiedFormat` per-cell on the write path** was worse than the draft assumed (per value,
  not per row) — hence Stage 0 exists.
- **TPC-H requires a one-line build config change** (not shipped in the default build).
- Dictionary cap is a setting (`mssql_scan_dictionary_max`), not a compile constant, from day 1.

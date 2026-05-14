# Feature Specification: UTF-16 Codec Consolidation

**Feature Branch**: `044-codec-consolidation`

**Created**: 2026-05-14

**Status**: Draft

**Input**: User description: "Take spec 044 from `feature-spec/refactoring-codec-044.md`. The source document is right about the *direction* (one source of truth for UTF-16 conversion) but heavily over-scoped: it bundles a `MssqlTypeCodec` virtual-base-class rewrite, a `VariantCodec` for XML/UDT/SQL_VARIANT (XML already shipped in spec 041!), DDL/INSERT-VALUES/filter-pushdown literal refactors, and a half-dozen unrelated type-mapping holes. Drop all of that. Scope 044 to exactly what spec 043 left unfinished: every remaining call site of the legacy `Utf16LE*` converter migrates to the `SimdutfUtf16LE*` wrapper introduced in 043, the legacy converter is removed from the public encoding surface, and NVARCHAR scan throughput is measured to validate the SIMD payoff. No new abstractions, no Vector/string_t batch APIs unless migration reveals them as natural. Run side-by-side with spec 042 (Integrated Authentication); coordinate on `src/tds/auth/` files. All comments and documentation in English only."

## Overview

Spec 043 ([[043-refactoring-foundation]]) added `simdutf` as a static
vcpkg dependency and exposed the `SimdutfUtf16LE*` free-function
primitives in `src/tds/encoding/simdutf_wrappers.{hpp,cpp}`. Only the
LOGIN7 builder and `TdsPacket::AppendUTF16LE` were migrated in 043; the
rest of the codebase still calls the legacy hand-rolled
`Utf16LEEncode` / `Utf16LEDecode` / `Utf16LEEncodeDirect` /
`Utf16LEByteLength` functions in `src/tds/encoding/utf16.cpp`.

This spec finishes the migration. It does three things:

1. **Migrate every production call site** of the legacy `Utf16LE*`
   functions to the `SimdutfUtf16LE*` wrapper. Thirteen call sites
   across ten source files (catalogued in §Call Sites below). The
   wrapper's invalid-input fallback contract (locked in spec 043
   Clarification Q1) preserves bit-for-bit semantics at every migrated
   site.
2. **Retire the public legacy converter** from
   `src/include/tds/encoding/utf16.hpp` and
   `src/tds/encoding/utf16.cpp`. The legacy hand-rolled implementation
   stays alive — but only as private fallback inside
   `src/tds/encoding/simdutf_wrappers.cpp` for the invalid-input path.
   No production source file outside the wrapper continues to depend
   on `utf16.hpp`. The `SimdutfUtf16LE*` symbols are renamed to the
   legacy `Utf16LE*` names so call sites read naturally — the
   `Simdutf` prefix existed only to allow coexistence during the
   migration window.
3. **Add a codec microbenchmark** (`make bench-utf16`) that measures
   `Utf16LEEncode` / `Utf16LEDecode` / `Utf16LEEncodeDirect`
   throughput in isolation on a representative payload mix (ASCII,
   BMP non-ASCII, non-BMP surrogate pairs, mixed). The microbenchmark
   asserts byte-identical output between the simdutf and legacy
   implementations and asserts the simdutf path is not measurably
   slower than the legacy path. Codec-level correctness + perf floor.
4. **Add an end-to-end before/after benchmark** that runs against the
   Docker SQL Server used by the integration test suite and exercises
   all four UTF-16 hot paths in one workflow: (a) DDL via SQL_BATCH
   (`CREATE TABLE`), (b) INSERT via SQL_BATCH (batched VALUES, 100k
   rows — bounded because the per-row codec cost is amortized once
   per batch, see User Story 5 step 3 rationale), (c) CTAS+BCP
   (100M rows; per-cell BCP encode hot path), (d) COPY TO MSSQL via
   BCP (100M rows; same hot path), (e) NVARCHAR scan decode via
   `COPY (SELECT * FROM ...) TO '/dev/null'` (100M rows; per-cell
   scan decode hot path). The benchmark is run twice — once against
   the pre-migration baseline (spec-043 main, commit 5db82e3) and
   once against the spec-044 PR head — and the per-step wall-clock
   times are recorded in `specs/044-codec-consolidation/bench_results.md`.
   System-level perf floor: the spec-044 build's wall-clock on every
   measured step is at most 1.10× the baseline's (i.e., no
   regression); we do not pre-commit to a "must be N% faster" target.

Explicitly **not in scope** (despite the source doc proposing them):

- **No `MssqlTypeCodec` abstract base class.** No `TdsReader` /
  `TdsWriter` abstractions. No `type_codec_registry`. Free-function
  primitives from spec 043 already match the legacy converter's
  signatures one-for-one; a class hierarchy adds boilerplate without
  buying anything the call sites need.
- **No `VariantCodec`.** XML support already shipped in spec 041
  ([[041-xml-type-support]]). SQL_VARIANT and UDT fallback handling
  is a separate feature for a future spec.
- **No type-mapping changes.** UUID, HUGEINT, TIMESTAMP_TZ, nested
  types as JSON — none of those touch UTF-16 encoding. They belong
  to separate DDL / CTAS specs.
- **No filter-pushdown or INSERT-VALUES literal refactor.** Those
  paths do not call the legacy `Utf16LE*` converter; their string
  formatting is already T-SQL-literal-shaped.
- **No new Vector / string_t / TdsWriter-coupled batch APIs.** The
  scan path is per-row by construction (one assembled byte buffer per
  cell, PLP chunks reassembled at the layer above the codec).
  Batch-style APIs would not change the call shape inside
  `TypeConverter::ConvertString`. If a future spec finds a hot spot
  that demands them, it can add them then.

## Clarifications

### Session 2026-05-14

- Q: After every consumer migrates to `SimdutfUtf16LE*`, what happens
  to the legacy `Utf16LE*` symbols? → A: Migration runs in two passes
  inside this one spec. Pass 1 swaps every call site from
  `Utf16LE*` to `SimdutfUtf16LE*`. Pass 2 deletes
  `src/include/tds/encoding/utf16.hpp`, renames the inner private
  helpers in `src/tds/encoding/utf16.cpp` to `LegacyUtf16LE*` in an
  anonymous namespace inside `src/tds/encoding/simdutf_wrappers.cpp`,
  and renames the public `SimdutfUtf16LE*` symbols back to
  `Utf16LE*` so consumers continue to call `encoding::Utf16LEEncode`
  etc. End state: one set of symbols, simdutf-backed, with the legacy
  hand-rolled implementation surviving only as a private
  invalid-input fallback compiled into the wrapper TU.
- Q: PLP NVARCHAR(MAX) chunk boundaries — do they need special
  handling in the migration? → A: No. The PLP reader (in
  `src/tds/tds_row_reader.cpp`) reassembles all PLP chunks into a
  single `std::vector<uint8_t>` before passing the buffer to
  `TypeConverter::ConvertString` (which calls `Utf16LEDecode`). The
  codec sees a fully-assembled UTF-16LE byte buffer; surrogate pairs
  spanning chunks are already glued together by the PLP layer.
  Migration is a one-line `Utf16LEDecode` → `SimdutfUtf16LEDecode`
  substitution and inherits the wrapper's bit-identical contract.
- Q: How do we measure the NVARCHAR-throughput claim from the source
  doc (15–25% improvement) without committing to that number as a
  hard SC? → A: We ship a microbenchmark that compares
  `SimdutfUtf16LEDecode` vs `LegacyUtf16LEDecode` on the same
  fixtures (ASCII, BMP non-ASCII, non-BMP surrogate-pair mix, mixed
  payload). The benchmark asserts (a) bit-identical output across
  the two implementations and (b) the simdutf path is **at least as
  fast** as the legacy path on representative input. We do not
  encode a hard "≥ N% faster" target — published simdutf benchmarks
  are workload-sensitive, and the project's CI runners are not
  performance-stable enough for tight numeric thresholds. The
  benchmark is an in-tree fixture, run on demand (`make
  bench-utf16`), not gated in CI.

## User Scenarios & Testing *(mandatory)*

### User Story 1 — NVARCHAR scan decode is unified on the SIMD path (Priority: P1)

A user runs `SELECT * FROM dbo.LargeNvarcharTable` against a table
whose rows contain non-ASCII NVARCHAR data (Cyrillic product names,
CJK city codes, emoji-laden user comments). Today, every cell's
UTF-16LE → UTF-8 conversion goes through the hand-rolled converter
in `src/tds/encoding/utf16.cpp` via `TypeConverter::ConvertString`.
After this spec lands, the conversion goes through `simdutf` and the
hand-rolled implementation is no longer reachable from the scan
path. Query results are byte-identical to the previous behavior;
throughput is at least as good on every payload class and measurably
better on non-trivial inputs.

**Why this priority**: NVARCHAR scan decode is the single hottest
UTF-16 consumer in the extension by call count. Every column of every
row of every Unicode result set passes through here. Migrating this
one call site delivers most of the runtime benefit of the entire
spec. P1 because the result-correctness contract is the user's
ground truth — silent corruption here would surface in production
data; an unmigrated call site here would leave the public claim of
"unified codec path" half-true.

**Independent Test**: Existing scan SQL test suite (everything in
`test/sql/query/`, `test/sql/catalog/`, `test/sql/integration/`)
runs unchanged against both the spec-043 baseline and the spec-044
binary; all green on both. New microbenchmark
`test/cpp/bench_utf16.cpp` (manual target `make bench-utf16`) feeds
a fixed payload mix through both `Utf16LEDecode` paths (the new
simdutf-backed one and the legacy fallback, accessed via test-only
visibility) and asserts byte-identical output plus
simdutf-not-slower-than-legacy on the host machine.

**Acceptance Scenarios**:

1. **Given** a table with an `NVARCHAR(200)` column containing 1,000
   rows of mixed ASCII / Cyrillic / CJK / emoji values, **When** the
   user runs `SELECT col FROM table`, **Then** every returned UTF-8
   string equals the original input bit-for-bit and the query
   completes successfully.
2. **Given** the same workload, **When** running against the
   spec-043 binary (pre-migration) and the spec-044 binary
   (post-migration), **Then** the result sets are byte-identical.
3. **Given** the microbenchmark fixture set (ASCII-heavy,
   non-ASCII-heavy, surrogate-pair-heavy, mixed), **When**
   `make bench-utf16` is run, **Then** the simdutf path produces
   the same bytes as the legacy path on every fixture and reports
   a wall-clock at most equal to the legacy path on every fixture.

---

### User Story 2 — NVARCHAR BCP write encode is unified on the SIMD path (Priority: P1)

A user runs `COPY tab FROM '...' (FORMAT csv)` or `CREATE TABLE t AS
SELECT ... FROM duckdb_source` against SQL Server with NVARCHAR
columns. Today, every column value's UTF-8 → UTF-16LE conversion in
the BCP row encoder goes through the hand-rolled
`Utf16LEEncodeDirect` (hot per-cell path) and the BCP writer header
helpers go through `Utf16LEEncode`. After this spec lands, both go
through `simdutf`. Write throughput matches or beats the previous
behavior on every payload class; the bytes on the wire are
byte-identical.

**Why this priority**: BCP encode is the write-side counterpart to
Story 1. Together they cover the full read/write hot path for
NVARCHAR. P1 because correctness here means "data the user inserts
arrives in SQL Server identical to what they sent"; a silent
encoding bug would corrupt user data. Same priority as P1 read path.

**Independent Test**: Existing BCP/COPY SQL test suite
(`test/sql/copy/`, `test/sql/ctas/`) runs unchanged. New regression
test `test/sql/copy/copy_to_nvarchar_unicode.test` round-trips a
fixture of Cyrillic / CJK / emoji values through `COPY TO MSSQL`
and asserts byte-identical retrieval.

**Acceptance Scenarios**:

1. **Given** a DuckDB table with 10,000 rows of mixed-Unicode
   NVARCHAR data, **When** the user runs `COPY tab FROM duckdb_tab`
   against SQL Server, **Then** every inserted row's NVARCHAR value
   round-trips byte-identical back to DuckDB via `SELECT`.
2. **Given** the same `INSERT ... VALUES` workload through the
   batched VALUES path, **When** the user runs an INSERT with
   non-ASCII string values, **Then** every value survives the
   round trip unchanged.
3. **Given** the BCP write microbenchmark
   `test/cpp/bench_utf16.cpp` (same fixture set as Story 1), **When**
   `make bench-utf16` is run for the encode direction, **Then** the
   simdutf path produces the same bytes as the legacy path on every
   fixture and reports a wall-clock at most equal to the legacy path.

---

### User Story 3 — All remaining UTF-16 call sites unified; legacy converter removed from public surface (Priority: P2)

A developer inspecting the codebase wants to know "where is UTF-16LE
conversion done in this extension?" and gets one answer:
`src/tds/encoding/simdutf_wrappers.{hpp,cpp}`. The other nine
historical call sites — Azure FedAuth token encoding, manual token
strategy, FedAuth strategy, BCP error message decoding, simple-query
result decoding, ENVCHANGE/INFO/ERROR token field decoding,
COLMETADATA column-name decoding, the standalone `EncodePassword`
helper, SQL_BATCH text encoding — all funnel through the same
wrapper. The public header `src/include/tds/encoding/utf16.hpp`
no longer exists; `src/tds/encoding/utf16.cpp` is folded into the
wrapper as a private invalid-input fallback. `encoding::Utf16LEEncode`
and friends still exist as call-site-facing names — but they resolve
to the simdutf-backed implementation, not to the hand-rolled one.

**Why this priority**: P2 because each individual call site here has
small runtime impact (auth runs once per connection, token-parser
fields are short, COLMETADATA happens once per query plan) — but
collectively they are what makes the "single source of truth" claim
real. Leaving any of them unmigrated would falsify the spec's core
promise; merging the cleanup into one PR keeps the migration
auditable.

**Independent Test**: After all call sites are migrated, `grep -rn
'\\bUtf16LE\\(Encode\\|Decode\\|EncodeDirect\\|ByteLength\\)' src/`
returns zero matches in production source files outside
`src/tds/encoding/simdutf_wrappers.cpp` (where the legacy converter
lives as a private fallback). The build still succeeds with the same
warnings posture as `main`. All existing integration tests pass.

**Acceptance Scenarios**:

1. **Given** the spec-044 commit on `main`, **When** running
   `grep -rn 'encoding::Utf16LE' src/` (or equivalent symbol search),
   **Then** every result resolves to a definition in the wrapper
   TU; no production call site links against the hand-rolled
   `src/tds/encoding/utf16.cpp` directly.
2. **Given** the spec-044 commit, **When** running `ls
   src/include/tds/encoding/`, **Then** `utf16.hpp` is not present
   (deleted) and `simdutf_wrappers.hpp` is present.
3. **Given** the existing test matrix
   (`make test`, `make integration-test`, `make test-all`), **When**
   run against the spec-044 commit, **Then** every test that was
   green on the spec-043 baseline is green on spec-044.

---

### User Story 4 — Azure / FedAuth token encoding goes through the SIMD wrapper (Priority: P2)

A user authenticates with an Azure AD token (interactive flow,
service principal, manual token, or `mssql_azure_auth_test`). The
access token (UTF-8 JSON-web-token text) is encoded to UTF-16LE for
the LOGIN7 FEDAUTH option. Today, this encoding goes through the
hand-rolled `Utf16LEEncode` in `src/azure/azure_fedauth.cpp`,
`src/tds/auth/fedauth_strategy.cpp`, and
`src/tds/auth/manual_token_strategy.cpp`. After this spec lands, all
three go through the simdutf wrapper. Token byte sequences are
byte-identical on the wire.

**Why this priority**: P2 because Azure tokens are guaranteed to be
ASCII (base64url + dots, per JWT spec) — there is no behavior change
expected from the migration. But these are auth-layer files that
spec 042 (collaborator) is restructuring; coordinating the
migration here lets us produce a clean diff against 042's branch
and gives 042 a single resolved auth-codec story to rebase against.

**Independent Test**: Existing Azure auth test suite
(`test/sql/azure/`) passes unchanged. The
`mssql_azure_auth_test()` function continues to acquire and apply
tokens against the configured Azure tenant. No new tests required
beyond confirming the migration compiles and the existing tests
pass.

**Acceptance Scenarios**:

1. **Given** an Azure AD access token obtained from the Azure CLI
   path, **When** the extension issues LOGIN7 with FEDAUTH, **Then**
   authentication succeeds against the configured tenant.
2. **Given** the migration touching `azure_fedauth.cpp`,
   `fedauth_strategy.cpp`, `manual_token_strategy.cpp`, **When**
   spec 042 (Integrated Authentication) is rebased on top of spec
   044, **Then** the auth-layer files have a single resolved
   encoding-call story (one `encoding::Utf16LEEncode` symbol, the
   simdutf-backed one) and no double-migration churn.

---

### User Story 5 — End-to-end before/after measurement on the integration SQL Server (Priority: P2)

A reviewer of the spec-044 PR wants concrete numbers: did this
migration produce real throughput change at the user level, or only
in synthetic microbenchmarks? The end-to-end benchmark answers that
question by running a fixed workload against the Docker SQL Server
used by the integration test suite, capturing per-step wall-clock
on the pre-migration baseline (spec-043 main, commit 5db82e3) and
on the spec-044 PR head, and recording both alongside the diff into
the spec directory. The reviewer can then read the numbers
directly out of `bench_results.md` instead of taking a "trust me"
on the performance story.

**Why this priority**: P2 because the codec-level microbenchmark
already covers correctness and the perf floor at the function-call
level. The integration benchmark adds a system-level perf floor and
turns the spec's perf claim into a falsifiable artifact rather than
a hand-waved expectation. Reviewers and future maintainers (and the
v0.2.0 release notes) cite real numbers from a reproducible
workflow.

**Independent Test**: A single shell script in `test/bench/`
(e.g., `bench_codec_e2e.sh`) that, when run against a binary built
from either the spec-043 baseline or the spec-044 PR head, executes
the six-step workflow below and emits per-step wall-clock times.
Run twice, capture two output files, diff them, paste the diff into
`bench_results.md`. The script has no behavioral dependency on
which binary is used — it just measures.

**Workflow** (executed end-to-end in one DuckDB session, against an
`ATTACH`'d MSSQL connection backed by the same Docker container the
integration test suite uses):

1. **Create target tables in SQL Server** via the extension:
   `CREATE TABLE bench_target_insert (id INT, name NVARCHAR(100),
   payload NVARCHAR(500), created_at DATETIME2)` and a parallel
   `bench_target_copy` for the COPY step. DDL goes through SQL_BATCH
   UTF-16 encoding (FR-001 migration). Small per-row footprint
   keeps the 100M-row data volume manageable on a developer
   workstation Docker SQL Server (~5–10 GB target-table footprint
   per BCP-loaded table; ~10–20 GB total Docker volume budget).
2. **Generate records in a DuckDB temp table**:
   `CREATE TEMP TABLE bench_source AS SELECT i::INT AS id, CASE i % 4
   WHEN 0 THEN 'ascii row ' || i WHEN 1 THEN 'Кириллица ' || i
   WHEN 2 THEN '中文行 ' || i ELSE '🔒 emoji ' || i END AS name,
   repeat('mixed-Unicode payload Αβγ ', 4) AS payload, timestamp
   '2026-01-01' + INTERVAL (i) SECOND AS created_at FROM
   range(0, 100000000) t(i)`. **100M rows**, deterministic mod-4
   pattern (25% ASCII, 25% Cyrillic, 25% CJK, 25% emoji). Per-row
   UTF-8 footprint ~150 bytes; total source data ~15 GB UTF-8 /
   ~30 GB on the UTF-16LE wire. DuckDB materializes this to
   temp-spill disk as needed.
3. **INSERT generated rows via batched VALUES**, on a bounded
   subset: `INSERT INTO bench_target_insert SELECT * FROM
   bench_source LIMIT 100000`. **Asymmetric row count: 100k, not
   100M.** Rationale: INSERT-via-VALUES does not exercise the
   per-cell UTF-16 codec — `mssql_value_serializer` formats each
   row as a T-SQL literal (string concatenation), and the whole
   SQL_BATCH text is encoded to UTF-16LE once per batch via
   `tds_protocol.cpp:751,762`. So the UTF-16 codec contribution
   is amortized across the batch and is a tiny fraction of the
   per-row cost; the dominant cost is SQL Server's log writes,
   page splits, and row-by-row insert work, which scales linearly
   with row count and would push a 100M-row INSERT into the
   multi-hour range while producing essentially no additional
   codec signal. 100k rows are enough to amortize the per-batch
   SQL_BATCH encode cost and produce a measurable wall-clock for
   that path; bumping further only buys SQL-Server-side overhead.
4. **CTAS** a second SQL Server table from the **full** source:
   `CREATE TABLE bench_target_ctas AS SELECT * FROM bench_source`.
   100M rows. Default path uses BCP for the data phase
   (`mssql_ctas_use_bcp=true`), exercising the per-cell BCP encode
   hot path (FR-003 on `bcp_row_encoder.cpp:407,477`) at full
   scale — this is the primary write-side signal.
5. **COPY (BCP) into the prepared copy target** at the **full**
   source size: `COPY bench_target_copy FROM (SELECT * FROM
   bench_source)`. 100M rows. Re-exercises per-cell BCP encode
   plus BCP-writer header encoding (FR-001 on
   `bcp_writer.cpp:615`).
6. **Read back the data**. First `SELECT COUNT(*) FROM
   bench_target_ctas` (smoke check; small COLMETADATA decode,
   no NVARCHAR cells pulled). Then `COPY (SELECT * FROM
   bench_target_ctas) TO '/dev/null' (FORMAT csv)` (Linux/macOS)
   or `... TO 'NUL'` (Windows) — materializes 100M rows through
   the scan decode path (FR-002 on `type_converter.cpp:424`) and
   discards the output. The `SELECT *` step is the primary
   read-side signal and is the workflow's largest codec-time
   contributor; `COUNT(*)` alone does not pull NVARCHAR bytes
   over the wire and would not measure scan decode.

Each of steps 1–6 is timed individually; the script reports six
wall-clock numbers per run.

**Acceptance Scenarios**:

1. **Given** the Docker SQL Server container is up (`make
   docker-up`) and a binary built from spec-043 main (commit
   5db82e3) is available at a known path, **When** the script is
   run against that binary, **Then** all six steps complete
   successfully and the per-step wall-clock times are written to
   `bench_results_baseline.txt`.
2. **Given** the same container and a binary built from the
   spec-044 PR head, **When** the script is run against that
   binary, **Then** all six steps complete successfully and the
   per-step wall-clock times are written to
   `bench_results_spec044.txt`.
3. **Given** both result files, **When** the per-step ratios
   (spec-044 / baseline) are computed, **Then** every ratio is at
   most 1.10× — no individual step regresses by more than 10% —
   and the read-back step (step 6's `SELECT *`) shows a ratio
   ≤ 1.00 within measurement noise on a developer laptop (the
   NVARCHAR scan decode path is the primary expected beneficiary).
4. **Given** the diff is computed, **When** the spec-044 PR is
   submitted, **Then** `bench_results.md` exists in the spec
   directory with both runs' numbers, the host's reported CPU /
   kernel / SQL Server image tag, the commit SHAs used for each
   binary, and a short prose summary of observed deltas.

---

### Edge Cases

- **Invalid UTF-8 input to encode path**: simdutf's pre-validate +
  legacy-fallback contract (spec 043 Clarification Q1) preserves
  bit-for-bit semantics. Migration changes the implementation but
  not the observable behavior on bad input.
- **Invalid UTF-16LE input to decode path**: same contract, opposite
  direction. The wrapper's `validate_utf16le` + legacy-fallback
  preserves the "replace with U+FFFD on bad surrogates, continue"
  behavior from the legacy decoder.
- **Empty input**: every wrapper function short-circuits on empty
  input and returns an empty result. Already tested by spec 043.
- **PLP NVARCHAR(MAX) cells spanning multiple TDS chunks**: PLP
  reassembly happens above the codec layer in
  `src/tds/tds_row_reader.cpp`. The codec receives a fully-assembled
  UTF-16LE byte buffer per cell; surrogate pairs split across PLP
  chunks are glued back together by the row reader before the codec
  sees them. Migration does not touch this boundary; pre-existing
  behavior is preserved.
- **Unaligned input buffer to decode**: the spec-043 wrapper already
  handles unaligned byte pointers by copying into a 2-byte-aligned
  scratch vector before calling simdutf. No additional handling
  required.
- **CHAR/VARCHAR (single-byte) path**: not affected. The
  single-byte path in `TypeConverter::ConvertString` does not call
  any UTF-16 converter and is left unchanged.
- **Locale-dependent narrowing**: not introduced. The simdutf
  wrapper does not depend on `LC_*` settings (verified in spec 043).
- **Spec 042 collision on `src/tds/auth/`**: spec 042 restructures
  the auth strategies. Spec 044 changes a single line in each of
  `fedauth_strategy.cpp` and `manual_token_strategy.cpp` (the
  `Utf16LEEncode` call). Whichever spec lands second performs a
  trivial rebase; the change is mechanical.
- **Windows / MinGW / MSVC builds**: simdutf already builds on all
  four CI platforms (spec 043 SC-008/SC-009). No new platform risk.
- **`SimdutfUtf16LE*` symbol rename to `Utf16LE*` after migration**:
  the rename is a pure search-and-replace inside the wrapper TU and
  the few inclusion sites. No semantic change; documented in the
  rename commit. Done after the call-site migration is complete and
  green, so that the rename diff is self-contained.

## Requirements *(mandatory)*

### Functional Requirements

**Call-site migration**

- **FR-001**: Every production call site of `encoding::Utf16LEEncode`
  in `src/` MUST be replaced with a call to the simdutf-backed wrapper
  introduced in spec 043. Affected files (eight call sites total):
  - `src/azure/azure_fedauth.cpp:28`
  - `src/copy/bcp_writer.cpp:615`
  - `src/tds/auth/fedauth_strategy.cpp:54`
  - `src/tds/auth/manual_token_strategy.cpp:34`
  - `src/tds/encoding/bcp_row_encoder.cpp:696`
  - `src/tds/tds_protocol.cpp:197` (the standalone
    `EncodePassword` helper)
  - `src/tds/tds_protocol.cpp:751` and `src/tds/tds_protocol.cpp:762`
    (SQL_BATCH text encoding)
  Line numbers are indicative as of `main` at the start of spec 044;
  the migration tracks function calls, not lines.
- **FR-002**: Every production call site of `encoding::Utf16LEDecode`
  in `src/` MUST be replaced with a call to the simdutf-backed
  wrapper. Affected files (six call sites total):
  - `src/copy/bcp_writer.cpp:208` (BCP error message)
  - `src/query/mssql_simple_query.cpp:43` (simple-query
    result decoder)
  - `src/tds/encoding/type_converter.cpp:424` (NVARCHAR / NCHAR /
    XML scan decode)
  - `src/tds/tds_column_metadata.cpp:406` (COLMETADATA column name)
  - `src/tds/tds_token_parser.cpp:337` and
    `src/tds/tds_token_parser.cpp:350` (ENVCHANGE / INFO / ERROR
    UTF-16LE field decode)
- **FR-003**: Every production call site of
  `encoding::Utf16LEEncodeDirect` in `src/` MUST be replaced with a
  call to the simdutf-backed wrapper. Affected file (two call sites
  total):
  - `src/tds/encoding/bcp_row_encoder.cpp:407` and
    `src/tds/encoding/bcp_row_encoder.cpp:477`
- **FR-004**: No production call site of
  `encoding::Utf16LEByteLength` exists today; the symbol is exported
  but currently unused. The post-rename wrapper (see
  `contracts/utf16_post_rename.hpp`) MAY retain the symbol because
  the simdutf wrapper from spec 043 already exposes it at zero cost
  and removing it would add a manual step to the otherwise-mechanical
  rename pass. Production consumers MAY adopt it in the future
  without requiring further spec work. The migration MUST NOT
  re-export it under the `Simdutf` prefix once Phase B renames symbols
  back to `Utf16LE*` — the same one-name-only rule that applies to the
  other four wrapper functions (FR-012).
- **FR-005**: After migration, every consumer source file MUST
  `#include "tds/encoding/simdutf_wrappers.hpp"` (or its eventual
  renamed equivalent) instead of `"tds/encoding/utf16.hpp"`.
  `grep -rn '"tds/encoding/utf16.hpp"' src/` MUST return zero
  results in production source (test source unaffected; test sources
  may directly link the legacy implementation for benchmark
  comparison, see FR-040).
- **FR-006**: Migration MUST preserve byte-for-byte output on every
  valid input. The simdutf wrapper's bit-identical contract on valid
  input (spec 043 FR-025 / SC-007) plus its
  validate-then-fallback contract on invalid input (spec 043 FR-034)
  jointly guarantee this; the migration relies on these contracts
  rather than re-testing every call site individually.

**Legacy converter retirement**

- **FR-010**: `src/include/tds/encoding/utf16.hpp` MUST be deleted
  in the same PR as the call-site migration. Build sanity is
  guaranteed by the fact that no consumer includes it after FR-005.
- **FR-011**: `src/tds/encoding/utf16.cpp` MUST be deleted from the
  source list. The legacy implementation MUST move into
  `src/tds/encoding/simdutf_wrappers.cpp` as private static helpers
  in an anonymous namespace, renamed to e.g.
  `LegacyUtf16LE{Encode,Decode,EncodeDirect,ByteLength}`. The
  invalid-input fallback path in `simdutf_wrappers.cpp` MUST call
  these private helpers; no external linkage to the legacy symbols
  remains.
- **FR-012**: After FR-010 and FR-011, the `Simdutf` prefix on the
  five wrapper symbols MUST be dropped — the public wrapper API
  becomes `encoding::Utf16LE{Encode,Decode,EncodeDirect,ByteLength}`
  with simdutf-backed implementations. Call sites read naturally:
  `encoding::Utf16LEDecode(...)`. The wrapper header
  `src/include/tds/encoding/simdutf_wrappers.hpp` MUST be renamed to
  `src/include/tds/encoding/utf16.hpp` (the deleted-then-resurrected
  name; the namespace is unchanged so callers do not move). The
  wrapper .cpp file MUST be renamed similarly to `utf16.cpp`. End
  state: filenames and symbol names match the original public API
  one-to-one; the implementation behind them is simdutf-backed.
- **FR-013**: The rename in FR-012 MAY be done in a single commit
  after the call-site migration is green, OR may be done as the
  final commit of the migration PR. It MUST NOT be split into a
  separate follow-up PR — leaving the codebase in a state where
  consumers call `SimdutfUtf16LE*` is a deliberate transition state
  for spec 044's PR review, not a shipping state.

**NVARCHAR microbenchmark**

- **FR-020**: A new manual-target benchmark
  `test/cpp/bench_utf16.cpp` MUST be added. The benchmark MUST run a
  fixture set through both the simdutf-backed
  `Utf16LE{Encode,Decode,EncodeDirect}` and the private legacy
  helpers (accessed via test-only visibility in the wrapper TU, e.g.
  a `tds::encoding::testing::` namespace exposed only when a
  CMake-controlled `MSSQL_BUILD_BENCHMARKS` flag is on).
- **FR-021**: The benchmark fixture set MUST include at least:
  (a) ASCII-only strings of three sizes (16 B, 256 B, 64 KB);
  (b) BMP non-ASCII strings (Cyrillic, accented Latin, CJK) of the
  same three sizes;
  (c) non-BMP (surrogate-pair, emoji) strings of the same three
  sizes;
  (d) mixed payloads (50% ASCII, 30% BMP, 20% non-BMP) of 64 KB.
- **FR-022**: The benchmark MUST assert byte-identical output across
  the simdutf and legacy paths for every fixture. Failure aborts
  the benchmark with a clear diagnostic.
- **FR-023**: The benchmark MUST measure per-fixture wall-clock
  time (rdtsc / `std::chrono::steady_clock` averaged over ≥ 1000
  iterations, warm-up excluded) and report both absolute timings
  and a simdutf/legacy ratio per fixture. The benchmark MUST assert
  the simdutf path's reported time is **at most 1.10×** the legacy
  path's time on every fixture (slack to absorb measurement noise on
  shared CI runners; the floor is "not measurably slower"). The
  benchmark MUST NOT assert a hard "≥ N% faster" threshold.
- **FR-024**: A `make bench-utf16` target MUST exist in the
  top-level `Makefile`. The benchmark MUST NOT run as part of
  `make test` or any CI job by default. It is a manual fixture for
  local validation and for spec-044 success-criterion reporting.

**End-to-end before/after benchmark**

- **FR-050**: A shell script MUST exist at
  `test/bench/bench_codec_e2e.sh` that, when invoked with one or
  more environment-variable-controlled inputs (the path to a built
  DuckDB CLI binary with the extension preloaded, plus the standard
  `MSSQL_TEST_HOST` / `MSSQL_TEST_PORT` / DSN env vars already used
  by `make integration-test`), executes the six-step workflow
  defined in User Story 5 and emits per-step wall-clock times in
  a fixed machine-readable form (e.g., one `step_name<TAB>seconds`
  line per step, plus a trailing summary block with host metadata).
- **FR-051**: The benchmark script MUST be deterministic in its
  source data shape: **100,000,000 rows** generated by `range()`
  with the fixed mod-4 ASCII / Cyrillic / CJK / emoji name pattern
  described in User Story 5 step 2, plus the fixed-text payload
  column. The INSERT-via-VALUES step (User Story 5 step 3) uses a
  bounded `LIMIT 100000` subset of the source (rationale in User
  Story 5 step 3); the CTAS, COPY, and SELECT steps use the full
  100M source. The same source-data SQL and the same per-step row
  counts are used on both the baseline run and the post-migration
  run, so observed timing deltas reflect only the migration, not
  workload variance.
- **FR-051a**: The benchmark script MAY accept a row-count
  override via an environment variable (e.g.,
  `MSSQL_BENCH_ROW_COUNT=10000000` to run a 10× smaller workload
  for a quick smoke). When overridden, the script MUST scale all
  per-step row counts proportionally (so the INSERT-VALUES step
  stays at `min(100k, override)` and the BCP/CTAS/SELECT steps
  use the override directly). The default and the recorded
  baseline + spec-044 runs MUST use the FR-051 sizes (100M for
  BCP/CTAS/SELECT, 100k for INSERT-VALUES); the override is for
  developer iteration only.
- **FR-052**: The benchmark script MUST be runnable against any
  build that loads the `mssql` extension — including the
  spec-043-baseline binary built from commit `5db82e3` and the
  spec-044 PR-head binary. It MUST NOT depend on any spec-044-only
  symbol; the workload SQL is portable across both binaries.
- **FR-053**: The benchmark script MUST clean up its target tables
  on each invocation (drop-if-exists before create) so repeated
  runs are idempotent and do not accumulate state in the test
  database.
- **FR-054**: Two recorded runs MUST be captured during spec 044's
  implementation phase: one against the spec-043-baseline binary
  and one against the spec-044 PR-head binary, both against the
  same Docker SQL Server image, on the same host, in the same
  session ordering, and within a single contiguous capture window
  to minimize host-state drift. The per-run output files MUST be
  committed to `specs/044-codec-consolidation/` (e.g.,
  `bench_results_baseline.txt`, `bench_results_spec044.txt`).
- **FR-055**: A summary file
  `specs/044-codec-consolidation/bench_results.md` MUST record:
  (a) the host's CPU model and core count, OS / kernel version, and
  cgroup / Docker resource limits in effect;
  (b) the SQL Server Docker image tag and configured RAM;
  (c) the two binary commit SHAs;
  (d) the per-step wall-clock table (baseline, spec-044, ratio);
  (e) a short prose summary calling out which steps improved,
  which regressed (if any), and the magnitude of the largest
  observed delta.
- **FR-056**: The end-to-end benchmark MUST NOT run as part of
  `make test`, `make integration-test`, or any CI workflow. It is
  a manual capture run by the spec-044 implementer once, recorded
  into the spec directory, and used as PR-review evidence.
  Spec 044's CI gate is the existing test suite (FR-040); the
  benchmark is reporting, not gating.

**Spec 042 coordination**

- **FR-030**: The two auth-layer changes (FR-001 for
  `fedauth_strategy.cpp` and `manual_token_strategy.cpp`) MUST be
  one-line `encoding::Utf16LEEncode` → `encoding::SimdutfUtf16LEEncode`
  substitutions with no surrounding refactor. This keeps the
  rebase against spec 042 (Integrated Authentication, parallel
  collaborator work) mechanical: spec 042 may restructure these
  files, but the single-line encoding-call change rebases trivially
  in either direction.
- **FR-031**: Spec 044 MUST NOT touch
  `src/tds/auth/sql_auth_strategy.cpp` (the standard SQL-auth
  path), which is part of spec 042's restructuring surface and
  uses no UTF-16 encoder directly. If spec 042 adds a new
  UTF-16-using auth strategy after 044 lands, that strategy
  inherits the migrated wrapper by default — no further action
  needed from 044.

**Testing requirements**

- **FR-040**: The full existing test matrix
  (`make test`, `make integration-test`, `make test-all`,
  `test/cpp/test_login7_encoding`) MUST remain green on every CI
  platform after migration. Spec 044 introduces no new test files
  in the SQLLogicTest tree; the existing scan / BCP / catalog /
  query / Azure-auth tests already exercise every migrated call
  site through normal operation.
- **FR-041**: A new regression test
  `test/sql/copy/copy_to_nvarchar_unicode.test` MUST round-trip a
  fixture of mixed-Unicode NVARCHAR values through `COPY TO MSSQL`
  and assert byte-identical retrieval via `SELECT`. The fixture
  MUST cover Cyrillic, accented Latin, CJK ideograph, and emoji
  (surrogate-pair) characters in at least one row each.
- **FR-042**: The microbenchmark `test/cpp/bench_utf16.cpp` MUST be
  buildable with `make bench-utf16` from a clean tree on Linux,
  macOS, and Windows. It MUST NOT run in CI by default.
- **FR-043**: No existing test file may be edited in a way that
  weakens or removes a pre-existing assertion. Spec 044 is a
  migration; the test surface only grows.

### Key Entities

- **Legacy UTF-16 converter**: The hand-rolled implementation
  currently in `src/tds/encoding/utf16.cpp` (functions
  `Utf16LEEncode`, `Utf16LEDecode`, `Utf16LEEncodeDirect`,
  `Utf16LEByteLength`). After this spec the public surface
  disappears; the implementation survives only as a private
  invalid-input fallback inside `src/tds/encoding/utf16.cpp` (the
  renamed wrapper TU), under different symbol names in an
  anonymous namespace.
- **simdutf wrapper**: Currently
  `src/tds/encoding/simdutf_wrappers.{hpp,cpp}` exposing
  `SimdutfUtf16LE*` symbols. After this spec, renamed to
  `src/tds/encoding/utf16.{hpp,cpp}` exposing `Utf16LE*` symbols
  (the originals' name; one resolved implementation behind them).
- **Migrated call sites**: Thirteen function-call sites across ten
  source files (catalogued in FR-001..FR-003 above). Each migration
  is a one-line edit at the call site plus a single-line
  `#include` rotation; no surrounding refactor.
- **NVARCHAR scan decode hot path**:
  `TypeConverter::ConvertString` in
  `src/tds/encoding/type_converter.cpp`. Single call to
  `Utf16LEDecode` per NVARCHAR / NCHAR / XML cell. Migrated to
  simdutf in FR-002.
- **BCP encode hot path**: `bcp_row_encoder.cpp`
  (`Utf16LEEncodeDirect` per cell) and `bcp_writer.cpp`
  (`Utf16LEEncode` for header / control fields). Migrated in
  FR-001 and FR-003.
- **NVARCHAR microbenchmark**: `test/cpp/bench_utf16.cpp`,
  manual target `make bench-utf16`, fixture spec in FR-021.
- **End-to-end benchmark script**:
  `test/bench/bench_codec_e2e.sh`, runs the six-step DDL → INSERT
  → CTAS+BCP → COPY → COUNT → SELECT workflow against the Docker
  SQL Server image used by `make integration-test`. Output goes
  to per-run files committed in the spec directory; the summary
  artifact `bench_results.md` records baseline + spec-044 numbers
  side by side with host metadata and commit SHAs.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 100% of production call sites of `encoding::Utf16LE*`
  in `src/` resolve through the simdutf-backed wrapper. After the
  migration commits land, `grep -rn '\\bUtf16LE\\(Encode\\|Decode
  \\|EncodeDirect\\|ByteLength\\)' src/` returns zero matches
  outside `src/tds/encoding/utf16.cpp` (the wrapper TU, where the
  legacy fallback lives privately).
- **SC-002**: The public header
  `src/include/tds/encoding/utf16.hpp` is the simdutf-backed wrapper
  header (renamed in FR-012). The legacy hand-rolled
  implementation is no longer reachable through any public symbol.
- **SC-003**: For all valid UTF-8 / UTF-16LE inputs, the migrated
  call sites produce byte-identical output to the v0.1.x baseline.
  Verified by (a) spec 043's pre-existing bit-identity unit test
  (`test/cpp/test_login7_encoding` and the wrapper-equivalence
  fixture from spec 043 FR-025) continuing to pass, and (b) the
  full existing scan / BCP / catalog / DML test suite passing on
  every CI platform.
- **SC-004**: The microbenchmark `make bench-utf16` reports the
  simdutf path's wall-clock time at most 1.10× the legacy path's
  time on every fixture (ASCII, BMP non-ASCII, non-BMP, mixed).
  Local-run figures are recorded in the spec's research artifact
  produced by `/speckit-plan`.
- **SC-005**: The microbenchmark asserts byte-identical output
  across the simdutf and legacy paths for 100% of fixtures (at
  least 13 fixtures per FR-021).
- **SC-006**: The new regression test
  `test/sql/copy/copy_to_nvarchar_unicode.test` round-trips
  Cyrillic, accented Latin, CJK, and emoji values through
  `COPY TO MSSQL` with byte-identical retrieval.
- **SC-007**: All existing test suites remain green on every CI
  platform (Linux GCC, macOS Clang, Windows MSVC, Windows MinGW).
  No new platform risk introduced beyond spec 043's CI baseline.
- **SC-008**: After migration the legacy converter `.cpp` and
  `.hpp` files at their original paths are gone (`utf16.{hpp,cpp}`
  in the encoding directory and include directory respectively now
  hold the renamed simdutf wrapper). `find src -name 'utf16.cpp' -o
  -name 'utf16.hpp'` returns the new files at the old paths; the
  legacy hand-rolled symbols are reachable only through the
  anonymous-namespace private helpers inside the wrapper TU.
- **SC-009**: The end-to-end benchmark
  (`test/bench/bench_codec_e2e.sh`) runs to completion against the
  Docker SQL Server image used by `make integration-test`, on both
  the spec-043-baseline binary (commit 5db82e3) and the spec-044
  PR-head binary. Both runs are committed as
  `bench_results_baseline.txt` and `bench_results_spec044.txt` in
  the spec directory.
- **SC-010**: For each of the six end-to-end workflow steps
  (DDL, INSERT, CTAS+BCP, COPY, COUNT, SELECT *), the ratio
  `spec044_seconds / baseline_seconds` is ≤ 1.10 — no individual
  step regresses by more than 10%. Read-path measurement (step 6
  `SELECT *`) is the primary expected beneficiary; the spec does
  not pre-commit to a "must be ≤ N%" target, only the no-regression
  floor.
- **SC-011**: The summary artifact
  `specs/044-codec-consolidation/bench_results.md` is present in
  the PR head, contains host / image / commit SHA metadata, the
  per-step wall-clock table with ratios, and a short prose
  summary. The artifact is the canonical performance citation for
  spec 044's PR description and v0.2.0 release notes.

## Assumptions

- The simdutf wrapper's contracts from spec 043 (FR-032..FR-036
  and Clarification Q1) are stable as of the start of spec 044:
  bit-identical on valid input, legacy-fallback on invalid input,
  no exceptions thrown, no locale dependency. This spec does not
  change the contract; it relies on it.
- PLP NVARCHAR(MAX) chunk reassembly happens in
  `src/tds/tds_row_reader.cpp` before the byte buffer reaches
  `TypeConverter::ConvertString`. The codec sees a fully-assembled
  buffer; spec 044 does not touch chunk-boundary handling.
  Confirmed by inspection of the row reader as of `main` (see
  `/speckit-plan` research artifact for cite).
- The scan path's per-row decode shape is sufficient. A
  Vector-coupled batch decode API would not change the call
  pattern: `ConvertString` is invoked once per cell with one
  pre-assembled byte buffer, and `simdutf::convert_valid_utf16le_to_utf8`
  already operates on the full buffer per call. Function-call
  overhead on the simdutf entry is negligible compared to the
  per-cell SIMD body.
- Azure AD access tokens are ASCII (JWT format: base64url + dots).
  The migration of FedAuth token encoders (FR-001 in
  `azure_fedauth.cpp`, `fedauth_strategy.cpp`,
  `manual_token_strategy.cpp`) therefore exercises only the
  wrapper's ASCII fast path. No new auth-behavior surface.
- Spec 042 (Integrated Authentication, parallel collaborator
  work) will rebase trivially on spec 044's auth-layer one-line
  changes, or spec 044 rebases on 042's auth-layer restructure.
  Either direction works because the conflict surface is one
  line per file (see FR-030).
- The `make bench-utf16` target is for local validation only.
  CI runners are shared resources without stable thermal /
  scheduler conditions; encoding tight performance thresholds in
  CI would produce false-positive flakes. The benchmark's
  assertion (`simdutf ≤ 1.10× legacy`) is generous enough to be
  reliable on a developer laptop but is NOT a CI gate.
- All source comments, documentation, commit messages, test
  fixture names, and PR descriptions for spec 044 are written
  in English.
- The end-to-end benchmark (FR-050..FR-056) is run on a developer
  workstation against a local Docker SQL Server (the same image
  `make integration-test` uses). The host is dedicated to the
  benchmark capture for the duration of the back-to-back baseline
  + spec-044 runs (no concurrent CPU-bound workloads) so that
  host-state drift between the two runs is minimized. Absolute
  wall-clock numbers are host-specific and not comparable across
  machines; only the *ratio* between the two runs on the same
  host is portable evidence.
- The 100M-row benchmark needs a Docker SQL Server with at least
  ~20–25 GB of usable container storage to hold both target
  tables after BCP load (~5–10 GB per table at NVARCHAR(100) +
  NVARCHAR(500) per row × 100M). The default `make docker-up`
  configuration may need a storage bump; if so, that bump is the
  benchmark operator's responsibility and is documented in
  `bench_results.md`. Expected total wall-clock per binary run
  on a modern developer workstation (~8-core CPU, NVMe SSD, local
  Docker, no TLS): 10–30 minutes (dominated by the two 100M-row
  BCP steps and the 100M-row scan). Total back-to-back baseline
  + spec-044 capture: 20–60 minutes.
- The spec-043-baseline binary is built from `main` at commit
  `5db82e3` (the spec-043 merge commit). It includes the
  simdutf wrapper but routes every non-LOGIN7 call site through
  the legacy hand-rolled converter — so it is a faithful "before"
  for the migration the spec-044 PR introduces. The spec
  implementer is responsible for keeping the baseline binary
  available for the duration of capture (e.g., `cmake-build/release-043/`
  alongside `cmake-build/release-044/`).
- Spec 044's PR can be rebased on top of spec 042's PR without
  conflict if 042 lands first. If 044 lands first, 042 inherits
  the migrated wrapper transparently — the new auth strategies
  introduced by 042 will see `encoding::Utf16LE*` already routing
  through simdutf.

## Out of Scope

- **`MssqlTypeCodec` abstract base class** and its registry
  (`type_codec_registry`), reader/writer abstractions
  (`TdsReader` / `TdsWriter`), and the 9-codec class hierarchy
  proposed by `feature-spec/refactoring-codec-044.md`. Deferred
  to a future spec if a concrete need surfaces; the current
  migration does not require any of it.
- **`VariantCodec`** for XML / SQL_VARIANT / UDT. XML support
  shipped in spec 041 ([[041-xml-type-support]]). SQL_VARIANT
  and UDT fallback handling is a separate feature.
- **Type-mapping holes** (UUID → UNIQUEIDENTIFIER round-trip,
  HUGEINT → DECIMAL(38,0), TIMESTAMP_TZ → DATETIMEOFFSET, nested
  LIST/STRUCT/MAP → NVARCHAR(MAX) JSON, INTERVAL → BIGINT,
  ENUM → NVARCHAR(N)). None of these touch UTF-16 encoding;
  they belong to a DDL / CTAS spec.
- **Filter-pushdown literal generator** in
  `src/table_scan/filter_encoder.cpp`. Does not call any
  `Utf16LE*` symbol; T-SQL literal formatting is already
  string-shaped.
- **INSERT-VALUES serializer** in
  `src/dml/insert/mssql_value_serializer.cpp`. Same — no
  `Utf16LE*` calls.
- **CTAS DDL type-mapping refactor** in
  `src/dml/ctas/mssql_ctas_executor.cpp`. Same.
- **Vector / string_t / TdsWriter-coupled batch APIs** on top of
  the wrapper. The per-row call shape is fine; no consumer asks
  for batch.
- **Performance gates in CI**. Both the microbenchmark from FR-020
  and the end-to-end benchmark from FR-050 are local-only fixtures.
  Neither runs in CI; neither blocks the PR merge on a perf number.
- **A formal "≥ N% faster than legacy" performance target**. The
  source doc claimed 15–25%; we do not encode that. SC-004
  (microbenchmark) and SC-010 (end-to-end) only require "not
  measurably slower" (≤ 1.10×) — no improvement floor.
- **Cross-machine perf comparison**. The end-to-end benchmark's
  absolute numbers are host-specific. Only the within-host ratio
  between the spec-043-baseline and spec-044 binaries is portable
  evidence. The committed `bench_results.md` records absolute
  numbers for one capture host; future re-captures on different
  hardware would produce different absolute numbers but should
  produce similar ratios.

## Dependencies and Coordination

- **Depends on spec 043** ([[043-refactoring-foundation]]). Spec
  043 must be merged to `main` before spec 044 can start in
  earnest. Confirmed: 043 merged in commit 5db82e3 prior to
  spec-044 kickoff.
- **Parallel with spec 042** (Integrated Authentication,
  collaborator). Spec 042 restructures `src/tds/auth/`. Spec 044
  touches three files in that area
  (`fedauth_strategy.cpp`, `manual_token_strategy.cpp`, and the
  shared `azure_fedauth.cpp`) with one-line changes per file.
  Coordinate via PR description; whichever spec lands second
  performs a mechanical rebase. See FR-030.
- **Unblocks future codec work**. With the legacy converter
  retired, any future spec that wants to introduce richer codec
  abstractions (per-type batch APIs, dictionary-vector encode
  fast paths, etc.) starts from a clean single-implementation
  baseline.
- **No vcpkg / CMake changes**. simdutf is already a project
  dependency (added in spec 043 FR-030). Spec 044 only edits
  source files and the source list.

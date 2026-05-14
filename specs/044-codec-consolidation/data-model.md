# Data Model: UTF-16 Codec Consolidation (spec 044)

Spec 044 is a refactor — no business data model. The "entities"
here are the source-level artifacts that change: source files,
test fixtures, benchmark artifacts, and the public symbol set.

## Source-level entities

### E1. Legacy hand-rolled UTF-16 converter (pre-044)

- **Source files**:
  `src/include/tds/encoding/utf16.hpp`,
  `src/tds/encoding/utf16.cpp`
- **Public symbols** (all in `duckdb::tds::encoding` namespace):
  - `std::vector<uint8_t> Utf16LEEncode(const std::string&)`
  - `std::string Utf16LEDecode(const uint8_t*, size_t)`
  - `std::string Utf16LEDecode(const std::vector<uint8_t>&)`
  - `size_t Utf16LEByteLength(const std::string&)`
  - `size_t Utf16LEEncodeDirect(const char*, size_t, uint8_t*)`
- **Production consumers**: 16 call sites in 10 files (catalogued
  in spec FR-001..FR-003).
- **End state (after spec 044)**: deleted at the public path. The
  same hand-rolled implementation survives, renamed to
  `LegacyUtf16LE*`, in an anonymous namespace inside the new
  `src/tds/encoding/utf16.cpp` (which now holds the simdutf-backed
  wrapper). Only the wrapper TU's invalid-input fallback path
  references the legacy code.

### E2. simdutf wrapper (introduced by spec 043)

- **Source files**:
  `src/include/tds/encoding/simdutf_wrappers.hpp`,
  `src/tds/encoding/simdutf_wrappers.cpp`
- **Public symbols** (all in `duckdb::tds::encoding`):
  - `std::vector<uint8_t> SimdutfUtf16LEEncode(const std::string&)`
  - `std::string SimdutfUtf16LEDecode(const uint8_t*, size_t)`
  - `std::string SimdutfUtf16LEDecode(const std::vector<uint8_t>&)`
  - `size_t SimdutfUtf16LEByteLength(const std::string&)`
  - `size_t SimdutfUtf16LEEncodeDirect(const char*, size_t, uint8_t*)`
- **Production consumers (pre-044)**: 2 call sites (LOGIN7
  helper in `tds_protocol.cpp:68`, `TdsPacket::AppendUTF16LE` in
  `tds_packet.cpp:72`).
- **End state (after spec 044)**: deleted at this path. Renamed to
  `utf16.{hpp,cpp}` and symbols renamed to `Utf16LE*` (no
  `Simdutf` prefix). All 13 Phase-A migrated call sites + the 2
  pre-existing LOGIN7 consumers + any new spec-042 consumers
  share one set of symbols.

### E3. Post-rename public UTF-16 wrapper (end state)

- **Source files**:
  `src/include/tds/encoding/utf16.hpp` (new content),
  `src/tds/encoding/utf16.cpp` (new content + private legacy
  fallback in anonymous namespace)
- **Public symbols**: same names as E1 (`Utf16LE*`), same
  signatures, simdutf-backed implementation.
- **Production consumers**: all 15 historical UTF-16 call sites
  (16 newly migrated + 2 pre-existing spec-043 consumers) plus
  any future consumer (including spec-042 auth strategies).
- **Private symbols**: `LegacyUtf16LE*` in an anonymous namespace
  inside `utf16.cpp`, invoked by the wrapper functions on
  invalid-input fallback (spec 043 Clarification Q1 contract).
- **Test-only re-export**:
  `tds::encoding::testing::LegacyUtf16LE*` exposed only when
  `MSSQL_BENCH_BUILD` is defined (microbenchmark
  comparison; see research R3).

## Call-site catalogue (the 13 production call sites migrated)

Listed by file with current call-site line ranges (indicative
as of `main` at spec-044 kickoff; the migration tracks function
calls, not lines).

| # | File | Line | Direction | Function |
|---|------|------|-----------|----------|
| 1 | `src/tds/encoding/type_converter.cpp` | 424 | decode | `Utf16LEDecode(value.data(), value.size())` |
| 2 | `src/tds/encoding/bcp_row_encoder.cpp` | 407 | encode-direct | `Utf16LEEncodeDirect(input, input_len, buffer.data() + start_pos + 2)` |
| 3 | `src/tds/encoding/bcp_row_encoder.cpp` | 477 | encode-direct | `Utf16LEEncodeDirect(input, input_len, out)` |
| 4 | `src/tds/encoding/bcp_row_encoder.cpp` | 696 | encode | `Utf16LEEncode(str.GetString())` |
| 5 | `src/copy/bcp_writer.cpp` | 208 | decode | `Utf16LEDecode(&response[pos], msg_len * 2)` |
| 6 | `src/copy/bcp_writer.cpp` | 615 | encode | `Utf16LEEncode(str)` |
| 7 | `src/tds/tds_protocol.cpp` | 197 | encode | `Utf16LEEncode(password)` (standalone EncodePassword helper) |
| 8 | `src/tds/tds_protocol.cpp` | 751 | encode | `Utf16LEEncode(sql)` (SQL_BATCH text) |
| 9 | `src/tds/tds_protocol.cpp` | 762 | encode | `Utf16LEEncode(sql)` (SQL_BATCH text) |
| 10 | `src/tds/tds_column_metadata.cpp` | 406 | decode | `Utf16LEDecode(data + offset, byte_length)` |
| 11 | `src/tds/tds_token_parser.cpp` | 337 | decode | `Utf16LEDecode(data + offset, byte_length)` |
| 12 | `src/tds/tds_token_parser.cpp` | 350 | decode | `Utf16LEDecode(data + offset, byte_length)` |
| 13 | `src/query/mssql_simple_query.cpp` | 43 | decode | `Utf16LEDecode(value.data(), value.size())` |
| 14 | `src/azure/azure_fedauth.cpp` | 28 | encode | `Utf16LEEncode(token_utf8)` |
| 15 | `src/tds/auth/fedauth_strategy.cpp` | 54 | encode | `Utf16LEEncode(access_token)` |
| 16 | `src/tds/auth/manual_token_strategy.cpp` | 34 | encode | `Utf16LEEncode(access_token)` |

(Counts: 16 call-site rows = 16 distinct call sites across
10 source files. Row clusters in the same file are kept as
separate rows because each is a logically distinct call site at a
distinct line. FR-001/FR-002/FR-003 in spec.md enumerate the same
16 sites grouped by direction: 8 encode + 6 decode + 2 encode-direct.)

Phase A swaps every `Utf16LE*` → `SimdutfUtf16LE*`. Phase B
renames `SimdutfUtf16LE*` → `Utf16LE*` (back to the original
names, simdutf-backed implementation).

## Test fixture entities

### F1. Microbenchmark fixture set
(`test/cpp/bench_utf16.cpp`, FR-021)

A static array of `FixtureSpec` records:

```cpp
struct FixtureSpec {
    const char* name;
    std::string utf8;
    std::vector<uint8_t> utf16le;
};
```

Each fixture has a known UTF-8 payload and the expected UTF-16LE
bytes for equivalence checking. Fixture set:

| Name | UTF-8 size | Contents |
|------|-----------|----------|
| `ascii_16` | 16 B | ASCII letters |
| `ascii_256` | 256 B | ASCII Lorem-style |
| `ascii_64k` | 64 KB | repeated ASCII run |
| `bmp_16` | ~24 B | "Привет мир" (Cyrillic) |
| `bmp_256` | ~384 B | Cyrillic + accented Latin Lorem |
| `bmp_64k` | 64 KB | repeated BMP non-ASCII |
| `cjk_16` | ~16 B | "中文测试" |
| `cjk_256` | ~256 B | CJK Lorem |
| `cjk_64k` | 64 KB | repeated CJK |
| `emoji_16` | ~16 B | "🔒🔑💾" (surrogate pairs) |
| `emoji_256` | ~256 B | emoji Lorem |
| `emoji_64k` | 64 KB | repeated emoji |
| `mixed_64k` | 64 KB | 50% ASCII / 30% BMP / 20% non-BMP |

13 fixtures total (FR-021 floor; exceeds minimum).

### F2. SQLLogicTest regression fixture
(`test/sql/copy/copy_to_nvarchar_unicode.test`, FR-041)

A multi-row insert of mixed-Unicode NVARCHAR values into a SQL
Server target via `COPY TO MSSQL`, followed by `SELECT` and
assertion that bytes round-trip identically:

```sql
# group: [copy] [integration]
# This test requires SQL Server (see make integration-test).

require mssql

statement ok
CREATE SECRET ...

statement ok
ATTACH '...' AS mssql_test (TYPE mssql);

statement ok
CREATE TABLE mssql_test.dbo.codec_unicode_rt (id INTEGER, name NVARCHAR(200));

# (omitted setup boilerplate)

statement ok
INSERT INTO mssql_test.dbo.codec_unicode_rt VALUES
    (1, 'ascii row'),
    (2, 'Привет мир'),
    (3, 'Ünlaut Áéí ñ'),
    (4, '中文测试'),
    (5, '🔒 emoji 🔑');

query II
SELECT id, name FROM mssql_test.dbo.codec_unicode_rt ORDER BY id;
----
1   ascii row
2   Привет мир
3   Ünlaut Áéí ñ
4   中文测试
5   🔒 emoji 🔑
```

(Actual `.test` file uses the project's standard ENV-driven
secret pattern, see test/sql/integration/ examples.)

### F3. End-to-end benchmark workflow
(`test/bench/bench_codec_e2e.sh`, FR-050..FR-056)

Six-step workflow defined in spec User Story 5; see the SQL
shape in research.md R5. The script's only state is two
target tables in the SQL Server `master` database (or
`MSSQL_TEST_DB`):

- `bench_target_insert (id INT, name NVARCHAR(100),
   payload NVARCHAR(500), created_at DATETIME2)`
- `bench_target_copy (id INT, name NVARCHAR(100),
   payload NVARCHAR(500), created_at DATETIME2)`
- `bench_target_ctas` is created and populated by step 4
  itself (CTAS).

Plus a DuckDB temp table `bench_source` in the running CLI
session (or a fresh source CTE per step — implementation
detail; see research R5).

## Recorded artifact entities

### A1. `bench_results_baseline.txt` (FR-054)

Plain-text per-step timing output captured from running
`test/bench/bench_codec_e2e.sh` against the spec-043-baseline
binary (commit 5db82e3). Format: see research R5.

### A2. `bench_results_spec044.txt` (FR-054)

Same format as A1, captured from running the script against the
spec-044 PR-head binary.

### A3. `bench_results.md` (FR-055)

Human-readable summary derived from A1 + A2. Sections:

1. **Host info**: CPU model + core count, OS + kernel version,
   total RAM, Docker version, SQL Server Docker image tag.
2. **Container budget**: CPU/RAM/storage limits in effect for
   the SQL Server container during the capture.
3. **Binaries**: full commit SHAs for both binaries; build
   flags (`make release` defaults assumed); vcpkg installed
   triplet.
4. **Per-step table**: step name, baseline seconds, spec-044
   seconds, ratio (spec-044 / baseline), row count, notes
   column.
5. **Prose summary**: which steps improved, which regressed
   (if any), magnitude of the largest delta, brief
   interpretation.
6. **Reproducibility**: exact env vars and CLI invocation used
   to reproduce both runs.

This file is committed to the spec directory and is the
canonical performance citation for the spec 044 PR description
and the v0.2.0 release notes.

## Public-symbol equivalence claim

The post-rename public API of `src/include/tds/encoding/utf16.hpp`
MUST be **signature-identical** to the legacy public API:

| Pre-044 public symbol | Post-044 public symbol | Implementation |
|-----------------------|------------------------|----------------|
| `Utf16LEEncode(const std::string&)` | same name & sig | simdutf-backed; invalid-UTF-8 falls back to private `LegacyUtf16LEEncode` |
| `Utf16LEDecode(const uint8_t*, size_t)` | same name & sig | simdutf-backed; invalid-UTF-16LE falls back to private `LegacyUtf16LEDecode` |
| `Utf16LEDecode(const std::vector<uint8_t>&)` | same name & sig | thin overload calling the pointer form |
| `Utf16LEByteLength(const std::string&)` | same name & sig | simdutf-backed; invalid input falls back to private legacy |
| `Utf16LEEncodeDirect(const char*, size_t, uint8_t*)` | same name & sig | simdutf-backed; falls back to private legacy on unaligned output buffer or invalid input |

Pre-spec-044 consumers of the legacy `utf16.hpp` (the 16 call
sites being migrated) see no compilation change after the
rename — same include, same symbols. The migration is invisible
to any future external consumer (since this is an extension's
internal header, "external" is theoretical, but the contract
holds).

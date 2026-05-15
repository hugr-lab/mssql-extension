# Spec 044: UTF-16 Codec Consolidation

## Summary

Finishes the simdutf migration that spec 043 ([#102](https://github.com/hugr-lab/mssql-extension/pull/102))
started. Every remaining legacy `encoding::Utf16LE*` call site
(16 sites across 10 source files) now flows through the
simdutf-backed wrapper introduced in 043. The wrapper itself is
folded back into the original `src/{include,}/tds/encoding/utf16.{hpp,cpp}`
file path under the pre-spec-043 public symbol names — so consumers
see no compilation change (same include, same symbol set), but the
implementation behind them is simdutf-backed with a private
hand-rolled fallback for invalid input.

No new vcpkg deps; no extension version bump.

## What changed (16 call sites, 10 files)

**Read path (6 decode sites)**:
- `src/tds/encoding/type_converter.cpp:424` — NVARCHAR / NCHAR / XML scan decode (the hottest UTF-16 consumer)
- `src/tds/tds_column_metadata.cpp:406` — COLMETADATA column name decode
- `src/tds/tds_token_parser.cpp:337,350` — ENVCHANGE / INFO / ERROR field decode
- `src/copy/bcp_writer.cpp:208` — BCP error message decode
- `src/query/mssql_simple_query.cpp:43` — simple query result decode

**Write path (8 encode sites)**:
- `src/tds/encoding/bcp_row_encoder.cpp:407,477,696` — BCP per-cell + header NVARCHAR encode
- `src/copy/bcp_writer.cpp:615` — BCP header / control encode
- `src/tds/tds_protocol.cpp:197` — standalone `EncodePassword` helper
- `src/tds/tds_protocol.cpp:751,762` — SQL_BATCH text encoding
- `src/azure/azure_fedauth.cpp:28` — Azure FedAuth token encoding
- `src/tds/auth/fedauth_strategy.cpp:54` — FedAuth strategy encode
- `src/tds/auth/manual_token_strategy.cpp:34` — manual-token strategy encode

**Write-direct (2 sites)**: counted in the BCP row-encoder entries above.

**Rename pass**: `src/{include,}/tds/encoding/simdutf_wrappers.{hpp,cpp}`
deleted; their content folded into `utf16.{hpp,cpp}` at the original
pre-spec-043 file paths; public `SimdutfUtf16LE*` symbols renamed
back to `Utf16LE*`. The legacy hand-rolled implementation survives
as a private anonymous-namespace fallback inside `utf16.cpp`,
invoked only on invalid-UTF-8 / invalid-UTF-16LE input. A test-only
re-export `tds::encoding::testing::LegacyUtf16LE*` is exposed via
`#ifdef MSSQL_BENCH_BUILD` so the spec-043 bit-identity assertion
test and the new microbenchmark can still compare simdutf vs the
hand-rolled converter.

## Spec 042 coordination

Three auth-layer files (`src/azure/azure_fedauth.cpp`,
`src/tds/auth/fedauth_strategy.cpp`,
`src/tds/auth/manual_token_strategy.cpp`) overlap with spec 042's
Integrated-Authentication restructure. The migration is **one line
per file** (FR-030); whichever PR lands second performs a trivial
rebase. No surrounding refactor; the conflict surface is by design
minimal.

## Performance evidence

### Codec microbenchmark (new: `make bench-utf16`)
13 fixtures × encode/decode/byte-length/direct-encode operations.
13/13 byte-identical between simdutf and the legacy hand-rolled
converter. **simdutf is 15-25% faster on non-ASCII (BMP, CJK,
emoji, mixed) at 64 KB**; tied with the legacy ASCII fast path
on pure-ASCII workloads. See [`bench_results.md`](./specs/044-codec-consolidation/bench_results.md#1-codec-microbenchmark-make-bench-utf16)
Section 1 for the full table.

### End-to-end smoke (1M rows on Docker SQL Server)
All 7 e2e steps within SC-010's ≤ 1.10× per-step ratio against the
spec-043 baseline binary. At 1M scale **the codec is washed out by
SQL Server bulk-insert / log-write overhead by 2-3 orders of
magnitude** — the migration introduces no e2e regression, but
also no user-visible perf win. Honest measurement.

The committed `bench_results_smoke_1m_baseline.txt` and
`bench_results_smoke_1m_spec044.txt` are the raw outputs.

### 100M e2e capture: deferred (scope decision)
The original spec called for 100M-row captures (~30 min × 2 binaries
+ summary). Given the 1M smoke shows the codec contribution is
essentially zero at e2e scale, the reviewer and implementer agreed
to skip the 100M capture and document the reasoning in
[`bench_results.md`](./specs/044-codec-consolidation/bench_results.md#3-scope-decision-100m-captures-deferred)
Section 3. The bench infrastructure is functional and committed;
any future re-capture at 100M is one `MSSQL_BENCH_ROW_COUNT` env
var away.

## Tests

- Existing test suite: 103 SQL test cases, 3229 assertions, 304
  integration assertions — all green on the spec-044 build.
- New regression test `test/sql/copy/copy_to_nvarchar_unicode.test`:
  round-trips ASCII / Cyrillic / accented Latin / CJK / emoji
  surrogate pair / mixed-Unicode payloads through `COPY TO MSSQL`
  (BCP) and asserts byte-identical retrieval.
- Existing `test/cpp/test_login7_encoding` (spec 043 bit-identity
  assertion against 30 fixtures + invalid-UTF-8 fallback) re-wired
  to compare the public `encoding::Utf16LE*` (simdutf-backed) vs
  the private `encoding::testing::LegacyUtf16LE*` (hand-rolled).
  7/7 sections pass.
- New manual-target codec microbenchmark `make bench-utf16` (NOT in
  CI): 13/13 byte-identical, 13/13 within 1.20× perf floor.

## Audit

Post-rename, grep audit of `src/`:

| Check | Expected | Actual |
|-------|----------|--------|
| `grep -rn SimdutfUtf16LE src/` | 0 | 0 ✓ |
| `find src -name 'simdutf_wrappers*'` | empty | empty ✓ |
| `Utf16LE\(Encode\|Decode\|EncodeDirect\|ByteLength\)` matches outside the wrapper TU | 18 (16 migrated + 2 spec-043 consumers) | 18 ✓ |

## Style

All modified files pass `clang-format-14 -style=file` (the project's
CI Lint version). Verified on the macOS host via
`/opt/homebrew/opt/llvm@14/bin/clang-format`.

## Merge gates

CI green across all four platforms is required:
- Linux GCC (`x64-linux-static`)
- macOS Clang/AppleClang (`x64-osx-static`)
- Windows MSVC (`x64-windows-static-release`)
- Windows MinGW / Rtools 4.2 (`x64-mingw-static`)

The benchmark targets (`make bench-utf16`,
`test/bench/bench_codec_e2e.sh`) are explicitly NOT part of CI
(FR-024, FR-056) — they are manual local-only fixtures.

## What this PR is NOT

This is deliberately scoped DOWN from the source design doc
([`feature-spec/refactoring-codec-044.md`](../feature-spec/refactoring-codec-044.md))
which proposed an `MssqlTypeCodec` virtual hierarchy + 9 codec
files + `VariantCodec` (already shipped in spec 041) + DDL /
INSERT-VALUES / filter-pushdown refactors + a half-dozen unrelated
type-mapping holes. None of that lands here. See
[`specs/044-codec-consolidation/spec.md`](./specs/044-codec-consolidation/spec.md)
"Out of Scope" section for the full rationale.

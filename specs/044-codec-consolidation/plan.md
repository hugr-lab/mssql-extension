# Implementation Plan: UTF-16 Codec Consolidation

**Branch**: `044-codec-consolidation` | **Date**: 2026-05-14 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/044-codec-consolidation/spec.md`

## Summary

Finish the simdutf migration that spec 043 ([[043-refactoring-foundation]])
started. Every remaining production call site of the legacy
`encoding::Utf16LE{Encode,Decode,EncodeDirect,ByteLength}` symbols
(sixteen call sites across ten source files) moves to the
`encoding::SimdutfUtf16LE*` wrapper. The legacy hand-rolled
implementation in `src/tds/encoding/utf16.{hpp,cpp}` is retired from
the public encoding surface and folded into the wrapper TU as an
anonymous-namespace private fallback for invalid input. The wrapper
files are then renamed back to `utf16.{hpp,cpp}` and the `Simdutf`
prefix dropped, so call sites read naturally
(`encoding::Utf16LEEncode(...)`) and the public-symbol set matches
pre-043 names — one resolved implementation behind them.

A codec microbenchmark (`make bench-utf16`) and an end-to-end
before/after integration benchmark (`test/bench/bench_codec_e2e.sh`,
100M-row workload on the integration Docker SQL Server) provide
falsifiable performance evidence. The microbenchmark is the
codec-level perf floor; the e2e benchmark is the system-level
perf floor and the recorded artifact for spec 044's PR.

Explicitly out of scope (despite the source doc proposing them):
no `MssqlTypeCodec` class hierarchy, no `VariantCodec` (XML
already shipped in [[041-xml-type-support]]), no type-mapping
holes, no DDL / INSERT-VALUES / filter-pushdown literal refactor,
no Vector/string_t/TdsWriter batch APIs.

## Technical Context

**Language/Version**: C++ — C++11-compatible at the ABI/header
surface (no `target_compile_features(cxx_std_17)`, no C++14+
features in headers shared with DuckDB), same constraint as spec
043 per `CLAUDE.md` "Build Troubleshooting → ODR Errors on Linux".
Internal `.cpp` TUs may use later features as long as nothing
leaks through headers.

**Primary Dependencies**:
- DuckDB (main branch, extension API) — unchanged
- `simdutf` (vcpkg static link) — **already present** from spec 043;
  no `vcpkg.json` or `CMakeLists.txt` changes for the dependency
  itself, only source-list adjustments around the legacy-converter
  rename
- OpenSSL (vcpkg) — unchanged
- Existing TDS protocol layer (`src/tds/`) — call-site edits only

**Storage**: N/A for the migration itself. The end-to-end benchmark
materializes ~5-10 GB per BCP-loaded target table in the Docker SQL
Server volume; benchmark-only, not a production storage concern.

**Testing**:
- DuckDB SQLLogicTest framework (`test/sql/`) — existing suite
  re-run; one new regression test
  `test/sql/copy/copy_to_nvarchar_unicode.test`
- C++ unit tests (`test/cpp/`) — existing `test_login7_encoding`
  re-run; one new microbenchmark `test/cpp/bench_utf16.cpp` (manual
  target, not in `make test`)
- Shell-driven integration benchmark
  (`test/bench/bench_codec_e2e.sh`) — new, runs against
  `make docker-up`'s SQL Server image; manual capture artifact

**Target Platform**: Linux (GCC, `x64-linux-static`), macOS
(Clang/AppleClang, `x64-osx-static`), Windows (MSVC,
`x64-windows-static-release`), Windows (MinGW / Rtools 4.2,
`x64-mingw-static`) — same matrix as spec 043; no new platform
risk.

**Project Type**: DuckDB community extension (C++ shared library
loadable into DuckDB).

**Performance Goals**:
- **Codec microbenchmark** (FR-020..FR-024 / SC-004..SC-005): per
  fixture, `Utf16LE{Encode,Decode,EncodeDirect}` simdutf path is
  ≤ 1.10× the legacy path on the same input. No "must be ≥ N%
  faster" target.
- **End-to-end benchmark** (FR-050..FR-056 / SC-009..SC-011): per
  workflow step, spec-044 wall-clock is ≤ 1.10× baseline. The
  read-path step (100M-row `SELECT *`) is the primary expected
  beneficiary; we record but do not gate on the size of the win.

**Constraints**:
- Byte-for-byte output preservation at every migrated call site,
  inherited from spec 043's invariants (FR-006).
- One PR, one mechanical migration. No surrounding refactors.
- Spec 042 (collaborator, Integrated Authentication) is parallel:
  three auth-layer call-site edits stay strictly one-line
  substitutions to keep rebase mechanical (FR-030..FR-031).
- The `Simdutf*` → `Utf16LE*` symbol rename is the final commit
  of the PR (FR-013) — leaves the codebase in a clean post-rename
  state on `main`.
- No CI perf gating; both benchmarks are local-only.

**Scale/Scope**:
- One PR.
- ~16 call-site edits + ~6 file-rename / source-list edits + 1
  new SQLLogicTest + 1 new C++ microbenchmark + 1 new shell script
  + 1 recorded `bench_results.md`.
- ~400-800 LOC including tests and benchmark.
- One development sprint.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

Constitution v2.0.0 principles applied:

| # | Principle | Status | Notes |
|---|-----------|--------|-------|
| I | Native and Open | **PASS** | simdutf is MIT-licensed (attribution already in `LICENSES/` + `README.md` from spec 043). No new dependency added. |
| II | Streaming First | **PASS** | The NVARCHAR scan decode path (`type_converter.cpp:424`) is per-row, called once per cell with one pre-assembled byte buffer. The migration preserves this shape exactly — no buffering introduced, no chunk boundary changes. PLP reassembly stays in `tds_row_reader.cpp`. |
| III | Correctness over Convenience | **PASS** | Migration relies on spec 043 FR-025/SC-007's bit-identity contract; FR-006 of this spec inherits it. No silent behavior change. Invalid-input semantics preserved via legacy fallback (spec 043 Clarification Q1). |
| IV | Explicit State Machines | **N/A** | Codec layer is stateless. No connection-state or protocol-state changes. |
| V | DuckDB-Native UX | **N/A** | No catalog, DDL, or user-facing surface change. |
| VI | Incremental Delivery | **PASS** | Single landable slice that completes the simdutf migration started in 043. Read-only path (Story 1: scan decode) and write path (Story 2: BCP encode) are independently testable on day one of the migration; subsequent stories are mechanical. |

**Row Identity Model**: N/A — no PK/rowid concern.

**Version Baseline**: SQL Server 2019+, "SQL text and parameters
MUST be transmitted as UTF-16LE per TDS protocol" — directly
applies; this spec is exactly that transmission's encoder/decoder
implementation consolidating on one path.

**Result**: Constitution Check PASSES. No violations. No
complexity-tracking entries required.

## Project Structure

### Documentation (this feature)

```text
specs/044-codec-consolidation/
├── plan.md                   # This file
├── research.md               # Phase 0 output
├── data-model.md             # Phase 1 output (key entities)
├── quickstart.md             # Phase 1 output (dev verification)
├── contracts/                # Phase 1 output
│   └── utf16_post_rename.hpp # Proposed post-rename public header surface
├── checklists/
│   └── requirements.md       # Created by /speckit-specify
├── bench_results.md          # Created during implementation (FR-055)
├── bench_results_baseline.txt# Created during implementation (FR-054)
├── bench_results_spec044.txt # Created during implementation (FR-054)
└── tasks.md                  # Phase 2 output (/speckit-tasks)
```

### Source Code (repository root)

```text
src/
├── include/tds/encoding/
│   ├── utf16.hpp                  # DELETED in migration pass, RECREATED in rename pass (simdutf-backed)
│   └── simdutf_wrappers.hpp       # DELETED in rename pass (folded into utf16.hpp)
├── tds/encoding/
│   ├── utf16.cpp                  # DELETED in migration pass, RECREATED in rename pass (simdutf-backed + private legacy fallback)
│   └── simdutf_wrappers.cpp       # DELETED in rename pass (folded into utf16.cpp)
├── azure/
│   └── azure_fedauth.cpp          # MODIFIED — one line (Utf16LEEncode → SimdutfUtf16LEEncode, then rename pass reverts the call back to Utf16LEEncode against the new implementation)
├── copy/
│   └── bcp_writer.cpp             # MODIFIED — two lines (Encode + Decode call sites)
├── query/
│   └── mssql_simple_query.cpp     # MODIFIED — one line (Decode)
├── tds/auth/
│   ├── fedauth_strategy.cpp       # MODIFIED — one line (Encode)
│   └── manual_token_strategy.cpp  # MODIFIED — one line (Encode)
├── tds/encoding/
│   ├── bcp_row_encoder.cpp        # MODIFIED — three lines (one Encode + two EncodeDirect call sites)
│   └── type_converter.cpp         # MODIFIED — one line (Decode at NVARCHAR/NCHAR/XML scan hot path)
└── tds/
    ├── tds_column_metadata.cpp    # MODIFIED — one line (Decode for COLMETADATA name)
    ├── tds_protocol.cpp           # MODIFIED — three lines (Password Encode + two SQL_BATCH Encodes)
    └── tds_token_parser.cpp       # MODIFIED — two lines (ENVCHANGE/INFO/ERROR decodes)

test/
├── cpp/
│   └── bench_utf16.cpp            # NEW — codec-level microbenchmark, manual target `make bench-utf16`
├── sql/copy/
│   └── copy_to_nvarchar_unicode.test  # NEW — round-trip regression for mixed-Unicode NVARCHAR via COPY TO MSSQL
└── bench/
    └── bench_codec_e2e.sh         # NEW — end-to-end before/after script (FR-050)

Makefile                            # MODIFIED — add `bench-utf16` target; the e2e script is invoked directly, no Makefile target
CMakeLists.txt                      # MODIFIED — source list reflects the rename (one path stays `src/tds/encoding/utf16.cpp` post-rename; `simdutf_wrappers.cpp` is removed from the list)
```

**Structure Decision**: Single-project DuckDB extension. The
migration touches existing source files at known line ranges
(catalogued in FR-001..FR-003 of the spec); no new namespaces or
directories. The two-pass rename strategy — migrate every consumer
to `SimdutfUtf16LE*` first, then in a final commit rename the
public symbols back to `Utf16LE*` and delete the old `utf16.cpp` —
keeps the diff reviewable and the post-rename state clean. The
post-rename header lives at the same path the legacy header
originally occupied (`src/include/tds/encoding/utf16.hpp`), so no
consumer's `#include` changes after the dust settles.

## Complexity Tracking

> No Constitution Check violations. Table omitted.

## Phase Outputs

- **Phase 0 (Research)**: see [research.md](./research.md) —
  decisions on the two-pass migration order, the rename commit's
  shape, test-only visibility for the private legacy fallback in
  the microbenchmark TU, PLP chunk reassembly confirmation, the
  end-to-end benchmark's bash/SQL structure and Docker storage
  budget, and spec 042 rebase strategy.
- **Phase 1 (Design & Contracts)**:
  - [data-model.md](./data-model.md) — key entities (legacy
    converter file, simdutf wrapper file, post-rename public
    header, microbenchmark fixture set, e2e benchmark workflow,
    `bench_results.md` summary artifact).
  - [contracts/utf16_post_rename.hpp](./contracts/utf16_post_rename.hpp) —
    proposed post-rename public header surface; finalizes the API
    that call sites use after the spec lands.
  - [quickstart.md](./quickstart.md) — how a developer verifies
    the migration locally (build, run unit tests, run integration
    tests against Docker, run `make bench-utf16`, run
    `test/bench/bench_codec_e2e.sh` against both binaries).
  - `CLAUDE.md` updated to reference this plan and add spec 044
    to "Recent Changes".

## Next Step

`/speckit-tasks` to expand Phase 1 outputs into a dependency-ordered
task list (`tasks.md`).

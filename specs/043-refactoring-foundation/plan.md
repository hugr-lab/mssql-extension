# Implementation Plan: LOGIN7 Non-ASCII Fix + simdutf Foundation

**Branch**: `043-refactoring-foundation` | **Date**: 2026-05-14 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/043-refactoring-foundation/spec.md`

## Summary

Fix the LOGIN7 length-counting defect that prevents authentication
with non-ASCII passwords (and other non-ASCII LOGIN7 fields) against
SQL Server, audit the connection-string parser for non-ASCII
round-trip integrity, and install `simdutf` via vcpkg as a foundation
dependency consumed only by the LOGIN7 fix in this spec. The bulk
migration of UTF-16 hot paths to simdutf stays in spec 044.

The work is one bug fix with two collateral pieces of foundation
hygiene: (a) the `Login7VarField` helper that owns
UTF-8 → UTF-16LE conversion and `cch*`/`ib*` bookkeeping for every
variable LOGIN7 field, (b) a thin `simdutf_wrappers.hpp` free-
function module that exposes drop-in replacements for the legacy
`Utf16LEEncode` / `Utf16LEDecode` / `Utf16LEByteLength` /
`Utf16LEEncodeDirect` API, validating UTF-8 before calling
`simdutf::convert_valid_utf8_to_utf16le` and falling back to the
legacy converter on invalid input so semantics are preserved
bit-for-bit, (c) a defensive sweep of `ParseUri` /`UrlDecode` /
`ParseConnectionString` to harden malformed-escape handling, add
`{...}` quoting, and confirm no locale narrowing.

## Technical Context

**Language/Version**: C++ — C++11-compatible at the ABI/header
surface (no `target_compile_features(cxx_std_17)`, no C++14+
features in headers shared with DuckDB) per the project's ODR
constraint documented in CLAUDE.md. Internal `.cpp` translation
units may use later features as long as nothing leaks through
headers.

**Primary Dependencies**:
- DuckDB main branch (extension API)
- `simdutf` (new, via vcpkg, static link, MIT-licensed —
  attribution in `LICENSES/simdutf-LICENSE-MIT.txt` and
  `README.md`)
- OpenSSL (existing, via vcpkg)
- Custom TDS protocol layer (existing — `src/tds/`)

**Storage**: N/A. LOGIN7 is a transient packet; no persistence.

**Testing**:
- DuckDB SQLLogicTest framework (`test/sql/`) for integration
  tests against Docker SQL Server.
- Catch2-style C++ unit tests in `test/cpp/` for in-memory
  packet round-trip and parser tests.
- Existing `make docker-up` / `make integration-test` /
  `make test` / `make test-all` workflow.

**Target Platform**:
- Linux (GCC, `x64-linux-static` vcpkg triplet)
- macOS (Clang/AppleClang, `x64-osx-static`)
- Windows (MSVC, `x64-windows-static-release`)
- Windows (MinGW / Rtools 4.2, `x64-mingw-static`)

**Project Type**: DuckDB community extension (C++ shared library
loadable into DuckDB).

**Performance Goals**: Not a performance spec.
- LOGIN7 runs once per connection — helper performance budget is
  effectively infinite.
- simdutf added but used only in the LOGIN7 path; broad
  NVARCHAR/BCP gains are spec 044's concern.
- Build wall-clock regression must stay under 10% on every
  platform (SC-009).
- Final binary size growth from simdutf addition under 500 KB
  per platform (SC-009, binary-size half — sanity bound, not a
  tight target).

**Constraints**:
- Bug-for-bug invalid-UTF-8 compatibility with the legacy
  `Utf16LEEncode` (skip invalid bytes, continue) preserved by
  pre-validating with `simdutf::validate_utf8` and falling back
  to the legacy converter on validation failure.
- Static link for simdutf on all four platforms; no new runtime
  `.so`/`.dylib`/`.dll` dependency (SC-008).
- TDS LOGIN7 fixed-header layout (94 bytes, ordered offset/
  length pairs) preserved exactly; only `cch*`/`ib*` values and
  the payload bytes change for non-ASCII inputs.
- No changes to the legacy `src/tds/encoding/utf16.cpp`
  converter — call sites stay as they are until spec 044.

**Scale/Scope**:
- Single PR.
- ~500–1500 LOC including tests.
- One development sprint.
- Three production source files touched (`tds_protocol.cpp`,
  `mssql_storage.cpp`, plus a new `simdutf_wrappers.cpp`/`.hpp`
  pair) plus C++ tests and SQL integration tests.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

Constitution v2.0.0 principles applied:

| # | Principle | Status | Notes |
|---|-----------|--------|-------|
| I | Native and Open | **PASS** | simdutf is MIT-licensed open-source SIMD primitives. No MS lib redistribution. Attribution added to `README.md` and `LICENSES/`. |
| II | Streaming First | **N/A** | LOGIN7 is a single packet; no streaming surface touched. |
| III | Correctness over Convenience | **PASS** | This whole spec is correctness: non-ASCII auth currently silently fails. FR-008 (throw `IOException` on field-length overflow) implements "fail explicitly when correctness cannot be guaranteed." |
| IV | Explicit State Machines | **N/A** | LOGIN7 packet building is stateless. No connection-state changes. |
| V | DuckDB-Native UX | **N/A** | Auth-layer change; catalog/DDL/UX unchanged. |
| VI | Incremental Delivery | **PASS** | Smallest standalone unit of the v0.2.0 refactor. Independently usable. Spec 044 builds on top. |

**Row Identity Model**: N/A — no PK/rowid concern.

**Version Baseline**: SQL Server 2019+, UTF-8 client collation,
"SQL text and parameters MUST be transmitted as UTF-16LE per TDS
protocol" — directly relevant; this spec fixes an existing
UTF-16LE encoding defect.

**Result**: Constitution Check PASSES. No violations. No
complexity-tracking entries required.

## Project Structure

### Documentation (this feature)

```text
specs/043-refactoring-foundation/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output (key entities)
├── quickstart.md        # Phase 1 output (dev verification)
├── contracts/           # Phase 1 output
│   └── simdutf_wrappers.hpp   # Proposed public header surface
├── checklists/
│   └── requirements.md  # Created by /speckit-specify
└── tasks.md             # Phase 2 output (/speckit-tasks)
```

### Source Code (repository root)

```text
src/
├── include/tds/encoding/
│   ├── utf16.hpp                # EXISTING — legacy hand-rolled converter, unchanged
│   └── simdutf_wrappers.hpp     # NEW — thin free-function wrappers over simdutf
├── tds/encoding/
│   ├── utf16.cpp                # EXISTING — legacy hand-rolled converter, unchanged
│   └── simdutf_wrappers.cpp     # NEW — simdutf-backed implementations with legacy fallback
├── tds/
│   └── tds_protocol.cpp         # MODIFIED — BuildLogin7 / BuildLogin7WithFedAuth / BuildLogin7WithADAL
│                                #          and a new internal Login7VarField helper
└── mssql_storage.cpp            # MODIFIED — UrlDecode hardening, ParseConnectionString
                                 #          {...} quoting (only if audit confirms defect)

test/
├── cpp/
│   ├── test_login7_encoding.cpp # NEW — LOGIN7 round-trip per builder × non-ASCII fixture
│   ├── test_simdutf_wrappers.cpp# NEW — simdutf vs legacy byte-equivalence
│   └── test_connection_string_parsing.cpp  # NEW — UrlDecode + ParseConnectionString unit tests
└── sql/
    └── integration/
        ├── non_ascii_password.test          # NEW — Docker SQL Server, non-ASCII login
        └── non_ascii_connection_formats.test# NEW — URI/ADO.NET/secret cross-format

vcpkg.json                      # MODIFIED — add "simdutf"
CMakeLists.txt                  # MODIFIED — find_package(simdutf) + link
```

**Structure Decision**: Single-project DuckDB extension. Source
mirrors `src/` ↔ `src/include/` exactly per existing convention
(documented in `CLAUDE.md`). The new simdutf wrapper sits in the
existing `duckdb::tds::encoding` namespace under a sub-namespace
or with `Simdutf*` prefix to avoid collision with legacy symbols
(decided in research.md).

## Complexity Tracking

> No Constitution Check violations. Table omitted.

## Phase Outputs

- **Phase 0 (Research)**: see `research.md` — decisions on
  simdutf vcpkg baseline, public API shape, invalid-input
  fallback strategy, C++11/ODR compatibility verification,
  `UrlDecode` malformed-escape semantics, `ParseConnectionString`
  `{...}` quoting introduction, T-SQL recipe for non-ASCII login
  creation.
- **Phase 1 (Design & Contracts)**:
  - `data-model.md` — key entities (LOGIN7 packet field, simdutf
    wrapper module, `Login7VarField` helper, LOGIN7 round-trip
    fixture).
  - `contracts/simdutf_wrappers.hpp` — proposed public header
    surface; finalized signatures match the legacy
    `utf16.hpp` API one-for-one with distinct symbol names.
  - `quickstart.md` — how a developer verifies the fix locally
    (build, run unit tests, run integration tests against
    Docker, run the v0.1.18 reproducer).
  - CLAUDE.md updated to reference this plan and add spec 043
    to "Recent Changes".

## Next Step

`/speckit-tasks` to expand Phase 1 outputs into a dependency-ordered
task list (`tasks.md`).

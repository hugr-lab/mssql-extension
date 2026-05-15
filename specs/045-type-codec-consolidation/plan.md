# Implementation Plan: Type Codec Consolidation

**Branch**: `045-type-codec-consolidation` | **Date**: 2026-05-15 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/045-type-codec-consolidation/spec.md`

## Summary

Consolidate per-type encoding/decoding/literal-formatting/DDL-type-naming
logic — currently duplicated across **five dispatch sites totaling 3,243
LOC** — into per-type-family modules under `src/codec/`. The five sites
keep their dispatch `switch` (one arm per `TypeFamily`), but each arm
becomes a one-liner that calls into a single authoritative family module
exposing four free functions: `DecodeFromTds`, `EncodeToBcp`,
`FormatSqlLiteral`, `FormatDdlTypeName`.

The technical approach is deliberately **non-virtual** (no `MssqlTypeCodec`
base class). The dispatch axes are mixed (TDS wire type for scan decode;
DuckDB `LogicalTypeId` for the other four shapes), the hot paths are
per-row, and the cold paths are per-statement — a single virtual
hierarchy would either burden the hot path or under-use the abstraction.
Instead, two free dispatch helpers (`FamilyFromTdsType`,
`FamilyFromLogicalType`) map both axes onto one 9-value `TypeFamily`
enum, and each call site dispatches by enum value to family-module free
functions in `namespace duckdb::codec::<family>`.

Spec 045 also closes [issue #91](https://github.com/hugr-lab/mssql-extension/issues/91)
(BCP `nvarchar(N)` length validation) inside the String family module:
the migration is the natural point to put NVARCHAR length/capacity
validation in one place where it can be unit-tested in isolation.

## Technical Context

**Language/Version**: C++17 source style, **C++11-compatible ABI** for ODR
compatibility with DuckDB on Linux GCC. No `target_compile_features(...
cxx_std_*)`; no C++17 features that escape into headers shared with
DuckDB. See CLAUDE.md "Build Troubleshooting → ODR Errors on Linux".

**Primary Dependencies**:
- DuckDB extension API (main branch; tracks v1.5-variegata)
- OpenSSL (vcpkg, statically linked) — unchanged
- simdutf (vcpkg, statically linked) — already pinned by spec 043/044;
  consumed via `encoding::Utf16LE*` from `src/include/tds/encoding/utf16.hpp`
- No new vcpkg dependencies

**Storage**: N/A (codec layer is stateless; produces strings for literals
/ DDL, byte buffers for BCP, fills DuckDB `Vector` slots for scan)

**Testing**:
- C++ unit tests in `test/cpp/codec/` (one per family, golden fixtures,
  manual `make` targets — pattern: `test-codec-<family>` similar to
  existing `test-login7-encoding`)
- SQLLogicTest files in `test/sql/copy/`, `test/sql/insert/`,
  `test/sql/ctas/`, `test/sql/catalog/` — automatically picked up by
  `make test` and `make integration-test`. Issue #91 regression test
  goes in `test/sql/copy/copy_nvarchar_length_validation.test`.
- Golden fixtures captured at spec-045 kickoff under
  `specs/045-type-codec-consolidation/golden/` (pre-migration baseline)

**Target Platform**: Linux x86_64 + ARM64 (GCC); macOS ARM64 (Clang);
Windows x64 (MSVC, MinGW/Rtools 4.2). Same matrix as the rest of the
extension.

**Project Type**: DuckDB C++ extension (single shared library `mssql.duckdb_extension`).

**Performance Goals**: Codec consolidation MUST be performance-neutral
(SC-008: ≤ 5% wall-clock regression at 1M rows on the spec-044 e2e bench).
The free-function dispatch adds at most one non-virtual call per row;
with same-TU inlining (or LTO across TUs) this is expected to be ≤ 0%
overhead — possibly slightly positive from less repeated branching.

**Constraints**:
- **clang-format-14** EXACT — CI Lint fails on output from any other
  version. On macOS use `brew install llvm@14` and run
  `/opt/homebrew/opt/llvm@14/bin/clang-format`.
- **Behavior preservation** is byte-identical EXCEPT for the one
  documented bug fix (FR-023 / issue #91 / `EncodeToBcp` NVARCHAR
  length validation). All other inputs must produce byte-identical
  wire output / literal text / DDL type-name vs `main`-at-kickoff.
- **No wire-protocol changes**. SQL Server cannot distinguish pre- vs
  post-spec-045 binary for any input that succeeded pre-spec-045.

**Scale/Scope**: 5 dispatch sites × 9 type families = ~45 migration
units. Total estimated effort 3-6 weeks of focused work, delivered in
phases (Phase 0 scaffolding → Phase 1-10 per-family migrations →
Phase 11 audit → Phase 12 polish). Each phase is independently
shippable.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

The project constitution (`.specify/memory/constitution.md` v2.0.0)
defines six principles. Evaluating each against this plan:

### I. Native and Open ✅ PASS
Spec 045 is a pure internal refactor of the codec layer. No new
dependencies, no ODBC/JDBC/FreeTDS introduced, no Microsoft client
library redistribution. simdutf (consumed via `Utf16LE*`) is MIT and
was added in spec 043 with attribution already shipped.

### II. Streaming First ✅ PASS
The per-row decode/encode contracts are unchanged. `DecodeFromTds`
fills one `Vector` row at a time (no batch buffering). `EncodeToBcp`
appends to the caller-owned `std::vector<uint8_t>` per row (no
intermediate result buffer). PLP NVARCHAR(MAX) and VARBINARY(MAX)
continue to stream via existing PLP chunk semantics.

### III. Correctness over Convenience ✅ PASS — with one improvement
Spec 045 IMPROVES correctness in one place: FR-023 / SC-002a fixes
issue #91 (BCP NVARCHAR length validation). Pre-spec-045: bug rejects
strings that would actually fit. Post-spec-045: length is validated
against the column's UTF-16LE byte capacity, with an explicit error
naming the column and observed-vs-allowed code units when the input
genuinely exceeds capacity (no more confusing passthrough server
error). All other observable behavior is byte-identical to `main`.

No silent type coercion is introduced. No use of `%%physloc%%` or
similar unstable identifiers (those don't appear in the codec layer
anyway).

### IV. Explicit State Machines ✅ PASS
Codec layer is not stateful — it's a pure-function dispatch tree.
No state machines added or removed. TDS connection/protocol/cancellation
state machines (in `src/tds/`) are not touched by this spec.

### V. DuckDB-Native UX ✅ PASS
No user-facing API surface changes. Catalog browse, DDL via DuckDB
commands, SQL parsing, type mapping semantics all unchanged. The
single behavior change (issue #91 fix) makes a previously-failing
COPY succeed — strictly an improvement.

### VI. Incremental Delivery ✅ PASS — strong fit
Each of the 9 family migrations is independently shippable. Phase 0
adds plumbing (no behavior change). Phase 1 ships Integer family
(MVP per US1). Phases 3-10 add the remaining families one at a time.
Phase 11 cleans up. The PR can land all phases at once, or be split
across multiple PRs — the implementer's choice based on review
preference.

**Constitution gate: PASS** — no violations to justify. Re-check after
Phase 1 design completes (post-research/data-model/contracts) per the
template's protocol.

## Project Structure

### Documentation (this feature)

```text
specs/045-type-codec-consolidation/
├── plan.md              # This file (/speckit-plan command output)
├── spec.md              # Feature specification (already created)
├── research.md          # Phase 0 output (this command)
├── data-model.md        # Phase 1 output (this command)
├── quickstart.md        # Phase 1 output (this command)
├── contracts/           # Phase 1 output (this command)
│   ├── type_family.hpp
│   ├── literal_context.hpp
│   ├── literal_format.hpp
│   └── integer_codec.hpp     # canonical per-family example
├── checklists/
│   └── requirements.md       # already exists from /speckit-specify
├── golden/                   # populated at Phase 0 of implementation
└── tasks.md             # Phase 2 output (/speckit-tasks command)
```

### Source Code (repository root)

The new `src/codec/` top-level directory is the spec's primary
structural addition. Existing per-type helpers move OUT of the five
dispatch sites and INTO the per-family modules.

```text
src/
├── codec/                                # NEW (this spec)
│   ├── type_family.cpp                   # FamilyFromTdsType / FamilyFromLogicalType helpers
│   ├── literal_format.cpp                # shared FormatSqlLiteral (filter + INSERT)
│   ├── boolean_codec.cpp
│   ├── integer_codec.cpp                 # Phase 1 MVP
│   ├── float_codec.cpp
│   ├── decimal_codec.cpp
│   ├── money_codec.cpp
│   ├── string_codec.cpp                  # NVARCHAR length-validation lives here (FR-023)
│   ├── binary_codec.cpp
│   ├── datetime_codec.cpp
│   └── uuid_codec.cpp
├── include/
│   └── codec/
│       ├── type_family.hpp
│       ├── literal_context.hpp           # enum LiteralContext { Filter, InsertValues }
│       ├── literal_format.hpp
│       └── <family>_codec.hpp            # one per family
├── tds/
│   └── encoding/
│       ├── type_converter.cpp            # shrinks: ConvertValue → switch on TypeFamily
│       ├── bcp_row_encoder.cpp           # shrinks: EncodeRow / EncodeValue → switch on TypeFamily
│       ├── utf16.cpp                     # UNCHANGED (spec 044 byte-level primitives)
│       ├── datetime_encoding.cpp         # UNCHANGED (low-level math helpers)
│       ├── decimal_encoding.cpp          # UNCHANGED (low-level math helpers)
│       └── guid_encoding.cpp             # UNCHANGED (low-level byte ops)
├── table_scan/
│   └── filter_encoder.cpp                # shrinks: ValueToSQLLiteral → codec::FormatSqlLiteral
├── dml/
│   └── insert/
│       └── mssql_value_serializer.cpp    # shrinks: Serialize → codec::FormatSqlLiteral
└── catalog/
    └── mssql_ddl_translator.cpp          # shrinks: MapLogicalTypeToCTAS + MapTypeToSQLServer → codec::<family>::FormatDdlTypeName

test/
├── cpp/
│   ├── codec/                            # NEW (this spec)
│   │   ├── test_integer_codec.cpp
│   │   ├── test_decimal_codec.cpp
│   │   ├── test_string_codec.cpp
│   │   └── ... (one per family)
│   └── test_literal_format.cpp           # NEW: LiteralContext divergence cases
└── sql/
    └── copy/
        └── copy_nvarchar_length_validation.test   # NEW: issue #91 regression
```

**Structure Decision**: `src/codec/` is a NEW top-level source directory.
**Rationale**: The codec layer is agnostic to the TDS transport — it
emits T-SQL strings (literals / DDL), byte buffers (BCP), and DuckDB
Vector slots (scan). Placing it under `src/tds/encoding/` would imply
TDS-coupling that is no longer accurate after consolidation. Existing
`src/tds/encoding/` keeps the byte-level wire primitives (`utf16.cpp`,
`datetime_encoding.cpp`, `decimal_encoding.cpp`, `guid_encoding.cpp`)
that the family modules call into. The 5 dispatch site files stay in
their current locations (only their bodies shrink).

**Note on `mssql_ddl_translator.cpp`**: spec.md mentions
`MapLogicalTypeToCTAS` as the single DDL mapper. Inspection during
Phase 0 found a SECOND mapper — `MapTypeToSQLServer` (lines 78-162) —
which subtly diverges from `MapLogicalTypeToCTAS` on 5 types (HUGEINT
throws vs `DECIMAL(38,0)`, UHUGEINT throws-with-message vs default
throw, TIMESTAMP scale 6 vs 7, VARCHAR config-driven only in CTAS,
INTERVAL stringified in CreateTable only) **and** silently lacks
DDL arms for TIMESTAMP_MS/NS/SEC (both mappers fall through to
default-throw, making columns of these types impossible in CREATE
TABLE / CTAS).

**Scope decision (post-Phase-2 user feedback)**: spec.md now treats
DDL unification as the 4th in-scope behavior change (FR-020 (d) +
FR-024..028). Both mappers MUST produce byte-identical T-SQL
post-spec-045, per-precision TIMESTAMP variants are added, HUGEINT/
UHUGEINT/INTERVAL map transparently in both contexts, and VARCHAR
consults `CTASConfig.text_type` in both contexts. The `DdlContext`
enum is retained for future per-context DDL hints but is functionally
a no-op in spec 045 family modules. See spec.md FR-013, FR-020 (d),
FR-024..028, and data-model.md "DdlContext" reconciled-mapping table.

## Complexity Tracking

No constitution violations to justify. This section is intentionally
empty.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| (none)    | (n/a)      | (n/a)                               |

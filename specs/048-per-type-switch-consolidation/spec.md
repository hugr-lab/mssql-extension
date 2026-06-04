# Feature Specification: Per-Type Switch Consolidation

**Feature Branch**: `048-per-type-switch-consolidation`

**Created**: 2026-05-17

**Status**: Draft / Queued (depends on spec 045 merge)

**Input**: User feedback during spec 045 Phase 6 close-out (verbatim,
paraphrased from Russian): *"I want ALL per-type switches moved into codec.
Goal: adding a new DuckDB type should require changes ONLY in `src/codec/`,
plus maybe a couple of other places. BCP / INSERT / SCAN must always share
common logic with a single canonical source of truth."*

This spec was originally tracked inline in spec 045's `tasks.md` as "Phase 7.5
(scope expansion)" but is being lifted out to keep spec 045's PR reviewable.
Spec 045 ships the 5-dispatch-site consolidation + DDL unification + TIMESTAMP
round-trip fixes; spec 048 picks up the next layer (per-type switches in
`target_resolver.cpp` and a few smaller residual sites).

## Overview

Spec 045 closed the 5 dispatch sites listed in its scope
(`type_converter.cpp`, `bcp_row_encoder.cpp`, `filter_encoder.cpp`,
`mssql_value_serializer.cpp`, `mssql_ddl_translator.cpp`). After it merges,
the codebase still has **~664 `case LogicalTypeId::` arms outside `src/codec/`**,
the biggest cluster being:

- **`src/copy/target_resolver.cpp`** — ~93 arms across 3 functions
  (`GetTDSTypeToken`, `GetTDSMaxLength`, `IsCompatibleSourceType`) that
  duplicate codec-level knowledge (wire token, BCP COLMETADATA max length,
  type compatibility check).
- **`src/tds/encoding/bcp_row_encoder.cpp`** — 11 small public static
  helpers (`EncodeInt8/16/32/64`, `EncodeUInt8`, `EncodeDouble`,
  `EncodeGUID`, `EncodeNullFixed/Variable/GUID/DateTime`) that are now
  only called from `test/cpp/test_bcp_row_encoder.cpp`, kept in production
  source as test-only fixtures.

The legitimate remainder lives in `src/tds/` (low-level wire parsing keyed
by TDS token, not by `LogicalTypeId`) and DuckDB's own catalog APIs — those
stay where they are.

## Goal

After spec 048, **adding a new DuckDB type requires changes ONLY in
`src/codec/<X>_codec.{cpp,hpp}`** (encode / decode / literal / DDL / wire-token
/ length / compat) plus one new arm in `tds_row_reader.cpp` (TDS wire dispatch
keyed by wire-token, not by `LogicalType`). Test-only helpers move out of
production source.

## Independent Test

```bash
grep -rEn 'case LogicalTypeId::' src/ --include='*.cpp' | grep -v 'src/codec/'
```

Expected: ≤ 15 matches (vs ~664 baseline). Legitimate remainder = the 5
dispatch sites' single `TypeFamily` switches + `tds_row_reader` TDS-token
dispatch + a small handful of documented edge cases (`per_type_switch_audit.md`).

## User Scenarios

### Scenario 1: Add a new DuckDB type to the extension

A new DuckDB type lands upstream (e.g. `LogicalTypeId::VARINT` for arbitrary-precision
integers). Today, supporting it requires editing:

- `target_resolver.cpp` × 3 functions
- `mssql_value_serializer.cpp` × 2 functions (Serialize + EstimateSerializedSize)
- `mssql_ddl_translator.cpp` × 2 functions
- `bcp_row_encoder.cpp` EncodeRow + EncodeValue
- `filter_encoder.cpp` `ValueToSQLLiteral`
- `type_converter.cpp` `ConvertValue`
- Plus codec wiring

After spec 045 the dispatch-site work is collapsed to 5 single-arm additions.
**After spec 048** the addition collapses further to:

- Create / extend `src/codec/varint_codec.{hpp,cpp}` with the 8 methods
  (DecodeFromTds, EncodeToBcp, FormatSqlLiteral, FormatDdlTypeName,
  EstimateLiteralSize, GetBcpTdsToken, GetBcpMaxLength,
  IsCompatibleWithSqlServerType).
- Add one arm to `codec::FamilyFromLogicalType` mapping `LogicalTypeId::VARINT`
  → `TypeFamily::Varint` (or route into existing Decimal family).
- Add one arm to `codec::FamilyFromTdsType` if the type has a unique TDS
  wire token.

That's it. The 5 dispatch sites and the 3 `target_resolver` functions pick
the new family up automatically via `switch (FamilyFromLogicalType(type))`.

### Scenario 2: Audit "where does the extension think TIMESTAMP_NS lives?"

Today the answer is split across `target_resolver.cpp` (wire token + max
length), `bcp_row_encoder.cpp` (encode), `type_converter.cpp` (decode),
`mssql_ddl_translator.cpp` (DDL), `mssql_value_serializer.cpp` + `filter_encoder.cpp`
(literal). After spec 045 the last three collapse into `src/codec/datetime_codec.cpp`.
**After spec 048** the first two collapse there too — every TIMESTAMP_NS
behavior lives in `src/codec/datetime_codec.cpp` and the per-family methods
it exposes.

## Requirements

### FR-001: New family-method surface (3 methods × 9 families)

Add three free functions to each `codec::<X>` namespace (Boolean, Integer,
Float, Decimal, Money, String, Binary, DateTime, Uuid):

```cpp
// Wire-format token the BCP encoder uses for this DuckDB type.
uint8_t GetBcpTdsToken(const LogicalType &type);

// BCP COLMETADATA max-length value (0 for fixed-length types).
uint32_t GetBcpMaxLength(const LogicalType &type, const mssql::BCPColumnMetadata &col);

// Whether this DuckDB type can be COPY-targeted at a column declared as
// the given SQL Server type name (e.g. "datetime2", "nvarchar", "decimal").
bool IsCompatibleWithSqlServerType(const LogicalType &type, const std::string &sql_type_name);
```

Total: 27 new functions across 9 families. Each is a small switch on
the LogicalType variants the family owns. Money family throws
`InternalException` for `GetBcpTdsToken` / `GetBcpMaxLength` (decode-only
family) and returns `false` from `IsCompatibleWithSqlServerType`.

### FR-002: target_resolver.cpp becomes a family-dispatch shim

Rewrite the three top-level functions in `src/copy/target_resolver.cpp`:

```cpp
uint8_t TargetResolver::GetTDSTypeToken(const LogicalType &type) {
    switch (codec::FamilyFromLogicalType(type)) {
    case TypeFamily::Boolean:  return codec::boolean::GetBcpTdsToken(type);
    case TypeFamily::Integer:  return codec::integer::GetBcpTdsToken(type);
    /* … 9 family arms total … */
    }
    throw InternalException(...);
}

uint32_t TargetResolver::GetTDSMaxLength(const LogicalType &type, ...) { /* same shape */ }
bool TargetResolver::IsCompatibleSourceType(const LogicalType &type, ...) { /* same shape */ }
```

Expected: `target_resolver.cpp` ~1100 → ~700 LOC (−36%, ~400 LOC removed).
The `GenerateColumnMetadata` function (per-type scale resolution for TIME /
TIMESTAMP / TIMESTAMP_TZ / TIMESTAMP_MS / TIMESTAMP_NS / TIMESTAMP_SEC,
landed in spec 045 commit `7bbdf28`) ALSO migrates to a family method:
`uint8_t GetBcpScale(const LogicalType &type)` — or stays inline if the
spec 045 implementation is already tight enough.

### FR-003: Move test-only BCP helpers to test source

Delete from `bcp_row_encoder.{hpp,cpp}` and move into
`test/cpp/test_bcp_row_encoder.cpp` as private static fixture functions:

- `EncodeInt8`, `EncodeInt16`, `EncodeInt32`, `EncodeInt64`
- `EncodeUInt8`
- `EncodeDouble`
- `EncodeGUID`
- `EncodeNullFixed`, `EncodeNullVariable`, `EncodeNullGUID`, `EncodeNullDateTime`

**Keep in production** (called by the actual `BCPRowEncoder::EncodeRow` /
`EncodeValue` dispatch or by codec families):

- `EncodeBit` (boolean codec uses it)
- `EncodeFloat` (float codec)
- `EncodeDecimal` (decimal codec)
- `EncodeBinary`, `EncodeBinaryPLP` (binary codec)
- `EncodeDate`, `EncodeTime`, `EncodeDatetime2`, `EncodeDatetimeOffset`
  (datetime codec — and the new Raw variants from spec 045)
- `EncodeDatetime2Raw`, `EncodeDatetimeOffsetRaw` (datetime codec, post-spec-045)
- `EncodeNullPLP` (called from `EncodeRow` lines 65/80/145)
- `TimestampToDatetime2Components` (called from `EncodeDatetime2` /
  `EncodeDatetimeOffset` legacy paths)

Expected: `bcp_row_encoder.cpp` 578 → ~430 LOC (−26%, ~150 LOC removed).

### FR-004: Audit-gate enforcement

`grep -rEn 'case LogicalTypeId::' src/ --include='*.cpp' | grep -v 'src/codec/'`
returns ≤ 15 matches. Each match is documented in
`specs/048-per-type-switch-consolidation/per_type_switch_audit.md` with
justification.

### FR-005: Test coverage unchanged

Every existing test stays green. Spec 048 introduces no new regression
tests — its goal is structural; behavior is identical. Verification: the
existing test suite (`make test-all` 116/116 PASS at spec 045 tip)
remains 116/116 PASS at spec 048 tip.

### FR-006: Contracts + data-model documentation

`specs/048-per-type-switch-consolidation/contracts/codec_family_interface.md`
documents the expanded family-method surface (8 methods total per family,
up from 5 in spec 045). `data-model.md` (in spec 048) lists the per-family
mapping for `GetBcpTdsToken` / `GetBcpMaxLength` /
`IsCompatibleWithSqlServerType` as authoritative reference.

## Out of Scope

- **`src/tds/` low-level wire parsing.** Keyed by TDS wire token (decoded
  from the server), not by `LogicalTypeId`. Stays as-is.
- **DuckDB-owned catalog APIs.** `CreateTableInfo`, `ColumnDefinition`,
  etc. switches in DuckDB-side code are outside the extension.
- **The `MssqlPoolManager` singleton.** Tracked separately in spec 047.
- **Pool ownership / multi-instance correctness.** Spec 047.
- **Any new TIMESTAMP / DATETIMEOFFSET precision work.** Already landed in
  spec 045 commit `7bbdf28`.

## Implementation Plan

### Phase 1 — Move test-only helpers (FR-003)

Smallest, lowest-risk change. Lands first to keep `bcp_row_encoder.cpp` lean.

- T001 Move 11 helper definitions from `bcp_row_encoder.cpp` to
  `test/cpp/test_bcp_row_encoder.cpp` as anonymous-namespace statics.
- T002 Remove corresponding declarations from `bcp_row_encoder.hpp`.
- T003 Verify `test_bcp_row_encoder.cpp` still compiles and passes
  (uses moved helpers from anonymous namespace, not via `BCPRowEncoder::`).
- T004 `make test-codec-*` + `make test` — green.

### Phase 2 — Family-method surface (FR-001)

- T010 [P] Add `GetBcpTdsToken`, `GetBcpMaxLength`, `IsCompatibleWithSqlServerType`
  declarations to each of 9 family headers (`*_codec.hpp`).
- T011 [P × 9] Implement each family's 3 new methods in its `*_codec.cpp`.
  Source-of-truth: extract from `target_resolver.cpp`'s current
  per-type arms.
- T012 Write unit tests `test/cpp/codec/test_<family>_target_methods.cpp`
  (or extend existing per-family tests) verifying every arm.
- T013 `make test-codec-*` green for all 9 families.

### Phase 3 — Rewrite target_resolver (FR-002)

- T020 Rewrite `GetTDSTypeToken` body as `switch (codec::FamilyFromLogicalType)`.
- T021 Rewrite `GetTDSMaxLength` body the same way.
- T022 Rewrite `IsCompatibleSourceType` body the same way.
- T023 Audit `GenerateColumnMetadata` — decide whether per-type scale
  resolution migrates to `GetBcpScale` family method or stays inline.
- T024 Delete now-unreferenced per-type helpers in `target_resolver.cpp`.
- T025 `make test-all` 116/116 PASS — no behavior change.
- T026 LOC audit: `wc -l src/copy/target_resolver.cpp` should drop from
  ~1100 → ~700.

### Phase 4 — Audit + documentation (FR-004 + FR-006)

- T030 Run audit-grep, record breakdown in `per_type_switch_audit.md`.
  Expect ≤ 15 matches.
- T031 Write `contracts/codec_family_interface.md` listing all 8 family
  methods + per-family ownership table.
- T032 Write `data-model.md` with the per-family
  `(LogicalTypeId → TDS token / max length / compat list)` mapping.
- T033 `pr_description.md`.

### Phase 5 — Final integration + clang-format (FR-005)

- T040 `make test-all`, `make integration-test` — all green.
- T041 `make test-codec-{boolean,integer,float,decimal,money,string,binary,datetime,uuid}`
  + `make test-literal-format` — all green.
- T042 clang-format-14 sweep over all touched files.
- T043 Update `CLAUDE.md` "Recent Changes" with spec 048 entry.

## Success Criteria

- **SC-001** `target_resolver.cpp` LOC ≤ 750 (from ~1100).
- **SC-002** `bcp_row_encoder.cpp` LOC ≤ 450 (from ~580).
- **SC-003** Audit-gate: `grep -rEn 'case LogicalTypeId::' src/ --include='*.cpp' | grep -v 'src/codec/' | wc -l` ≤ 15.
- **SC-004** All existing tests stay green (no behavior change).
- **SC-005** Each `codec::<X>` family file is a self-contained unit:
  `grep -rn 'codec::<X>::' src/` returns matches only in
  `src/codec/<X>_codec.cpp` and in the 6 dispatch sites
  (5 from spec 045 + `target_resolver.cpp` from spec 048).
- **SC-006** New per-family unit tests for the 3 new methods PASS.

## Dependencies

- **Spec 045 merged.** Spec 048's premise (target_resolver consolidation)
  depends on the family-method dispatch pattern established by spec 045's
  5-dispatch-site refactor.
- **No new vcpkg dependencies.**
- **No DuckDB upstream changes.**

## Open Questions

- **Money family `GetBcpTdsToken` / `GetBcpMaxLength` — throw or return sentinel?**
  Money is decode-only (no DuckDB MONEY LogicalType ever flows in). The
  switch in `target_resolver` can never legitimately reach Money. Recommend
  `throw InternalException` for parity with the DDL dispatcher's existing
  Money arm (`mssql_ddl_translator.cpp` post-spec-045).
- **`GetBcpScale` — separate family method or part of `GetBcpMaxLength`?**
  Today the scale logic is inline in `target_resolver::GenerateColumnMetadata`
  (per spec 045 commit `7bbdf28`). If `GetBcpMaxLength` returns scale via
  out-param or struct, it's one method; if not, it's a 4th method. Decide
  during T023.
- **`IsCompatibleWithSqlServerType` — strict or lenient?** Today the check
  is "would BCP succeed?" Strict: only exact-type match. Lenient: allow
  implicit conversions SQL Server itself permits. Spec 048 preserves current
  behavior (strict for most types, lenient for VARCHAR↔NVARCHAR per spec 045's
  catalog convention) and documents it; no behavior change.

## References

- Spec 045 — type codec consolidation (5 dispatch sites + DDL + TIMESTAMP
  round-trip). Spec 048's prerequisite.
- Spec 045 commit `c512b09` — original "Phase 7.5" addition inside spec 045
  (the content was later extracted into this spec to keep 045's PR
  reviewable).
- Spec 045 commit `7bbdf28` — TIMESTAMP_* round-trip + scale-aware catalog
  (introduced the per-type scale resolution in `GenerateColumnMetadata`
  that T023 will audit).
- `src/copy/target_resolver.cpp` lines 533 / 1017 / 1082 / 1156 — the
  per-`LogicalTypeId::` clusters that migrate in Phase 3.

# Literal-format Dispatcher Migration — Phase 4 Evidence

**Spec**: 045-type-codec-consolidation
**Phase**: 4 — US2 Literal-format consolidation
**Scope**: Integer family routing through `codec::FormatSqlLiteral`
**Date**: 2026-05-15

## What changed

Phase 4 wires the `codec::FormatSqlLiteral` dispatcher's **Integer** arm to
`codec::integer::FormatSqlLiteral` and switches the two call sites'
Integer arms (`filter_encoder.cpp:ValueToSQLLiteral`,
`mssql_value_serializer.cpp:Serialize`) to route through the dispatcher
instead of calling the family module directly. Other 8 family arms in
`literal_format.cpp` still throw `NotImplementedException`; they are
replaced as each family migrates in Phase 5+ (US5 String → Phase 5,
remaining 7 families → Phase 6). The full one-liner body replacement of
`ValueToSQLLiteral` / `Serialize` lands at the end of Phase 6 once all 9
arms in `literal_format.cpp` resolve to real code.

## Files touched (Phase 4)

| File | Change |
|---|---|
| `src/codec/literal_format.cpp` | Integer arm: `return integer::FormatSqlLiteral(v, type, ctx);` / `return integer::EstimateLiteralSize(type);` (other 8 arms throw). |
| `src/table_scan/filter_encoder.cpp` | Integer arm: `return codec::FormatSqlLiteral(value, type, codec::LiteralContext::Filter);` (replaces direct `codec::integer::...` call). Include swapped. |
| `src/dml/insert/mssql_value_serializer.cpp` | Integer arm: `return codec::FormatSqlLiteral(value, type, codec::LiteralContext::InsertValues);` (replaces direct `codec::integer::...` call). Include swapped. |
| `test/cpp/test_literal_format.cpp` | NEW — `make test-literal-format` (5 test groups: NULL routing, Integer parity Filter==InsertValues for TINYINT..UBIGINT, HUGEINT FR-020 (b), EstimateLiteralSize bound, unmigrated families throw). |
| `test/sql/catalog/filter_pushdown_hugeint.test` | NEW — FR-020 (b) regression; seed via `mssql_exec` (HUGEINT BCP deferred to Phase 6/T069). |
| `Makefile` | `test-literal-format` target now uses `$(CODEC_TEST_RPATH)` so the binary finds `libduckdb.dylib`. |

## Byte-identity evidence — representative filter literals

The Integer-arm output is byte-identical pre/post Phase 4 because the
dispatcher delegates straight to `codec::integer::FormatSqlLiteral`,
which was already in place after Phase 3 (commit `05be6a4`). Phase 4
adds a single routing hop — no logic change.

| Input | Pre-Phase-4 (Phase 3 result, direct call) | Post-Phase-4 (via dispatcher) |
|---|---|---|
| `TINYINT 127`, Filter | `127` | `127` |
| `BIGINT -9223372036854775808`, Filter | `-9223372036854775808` | `-9223372036854775808` |
| `UBIGINT 9223372036854775808`, Filter | `CAST(9223372036854775808 AS DECIMAL(20,0))` | `CAST(9223372036854775808 AS DECIMAL(20,0))` |
| `HUGEINT 42`, Filter | `42` | `42` |
| `HUGEINT -100`, Filter | `-100` | `-100` |
| `INTEGER NULL`, Filter | `NULL` | `NULL` |

INSERT-context output likewise byte-identical (`codec::integer::FormatSqlLiteral`
ignores `LiteralContext` for Integer family per FR-020 (b)).

## Independent test gates

- `make test-codec-integer` — PASS (7 test groups).
- `make test-literal-format` — PASS (5 test groups, dispatcher routing).
- `make test` (release unittest) — PASS (125 tests / 3253 assertions, 21 skipped).
- Integration sweep (filter_pushdown, filter_pushdown_hugeint, ctas_types,
  ctas_failure, insert_basic, insert_types) — PASS, no regressions.
- `filter_pushdown_hugeint.test` — PASS on spec-045 HEAD (FR-020 (b)
  fix exercised). On `main`-at-kickoff this test fails because the
  pre-spec-045 filter encoder rendered `WHERE [huge_val] = N'12345'`
  (a string literal) instead of the bare `12345` digits.

## What still throws

After Phase 4, the dispatcher arms for these families still throw
`NotImplementedException`:

- Boolean
- Float
- Decimal
- Money
- String
- Binary
- DateTime
- Uuid

This is harmless because the corresponding arms in
`ValueToSQLLiteral` / `Serialize` haven't been switched to route through
the dispatcher yet — they still call legacy per-type code. Each family's
migration phase (Phase 5 for String, Phase 6 for the rest) lands two
edits: (a) family module body, (b) dispatch-site arm flip from legacy
to `codec::FormatSqlLiteral`. After Phase 6 completes, the bodies of
`ValueToSQLLiteral` and `Serialize` collapse to the planned one-liner.

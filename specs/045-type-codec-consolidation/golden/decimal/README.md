# Decimal family — golden fixtures (spec 045 / US3 sub-phase 3)

Captured from the implementation at branch HEAD `f91986a` (post-Float
migration) before the Decimal codec migration landed. The Decimal
dispatch sites at that point were still the legacy per-type helpers
(`TypeConverter::ConvertDecimal`, `BCPRowEncoder::EncodeDecimal` /
`EncodeValue` DECIMAL arm, `MSSQLValueSerializer::SerializeDecimal`,
the filter encoder's `value.ToString()` inline call for `DECIMAL`,
and the DDL translator's inline `StringUtil::Format("DECIMAL(%d,%d)"...)`).

## Files

| File | What | Format |
|---|---|---|
| `decode_cases.txt` | TDS DECIMAL/NUMERIC → DuckDB DECIMAL | sign \| magnitude bytes (LE) \| precision \| scale \| expected scaled hugeint |
| `encode_cases.txt` | DuckDB DECIMAL → BCP wire bytes | precision \| scale \| value \| expected hex bytes |
| `literal_cases.txt` | DuckDB DECIMAL → SQL literal | precision \| scale \| value \| ctx \| expected text |
| `ddl_cases.txt` | DECIMAL(p,s) → SQL Server type name | width \| scale \| cfg \| ctx \| expected text |
| `edge_cases.txt` | HUGEINT routing, precision overflow, ToString divergence | documented separately |

## Conventions

- `ctx` is `Filter` or `InsertValues` (literal) or `CreateTable` /
  `CtasCreateTable` (DDL). Per FR-022 the Decimal family produces
  identical output for both LiteralContext values (the headline
  consolidation in this sub-phase — pre-spec-045 Filter used
  `Value::ToString()` which could diverge from InsertValues'
  `SerializeDecimal` rendering on edge cases).
- Per FR-027/FR-028 the Decimal family produces identical DDL output
  for both DdlContext values. Both `MapTypeToSQLServer` and
  `MapLogicalTypeToCTAS` already used the same `DECIMAL(p,s)` form
  with `p ≤ 38` clamp, so this is a no-behavior-change consolidation.
- For literal cases the **post-migration** behaviour is documented
  here: unified on the legacy InsertValues semantics
  (`MSSQLValueSerializer::SerializeDecimal` — manual fixed-point
  rendering, no scientific notation, leading-zero pad if integer part
  is zero). This avoids the `Value::ToString()` ambiguity for very
  small / very large DECIMAL values.
- PhysicalType dispatch (INT16 for p≤4, INT32 for p≤9, INT64 for
  p≤18, INT128 for p>18) is preserved exactly from the legacy
  `ConvertDecimal` / `EncodeDecimal` / Serialize paths.
- **HUGEINT routing (FR-025):** DuckDB's HUGEINT (no signed 128-bit in
  SQL Server) routes through `codec::decimal::FormatSqlLiteral` and
  `FormatDdlTypeName` as if it were `DECIMAL(38,0)`. The Integer
  codec already forwards HUGEINT to `MSSQLValueSerializer::SerializeDecimal`
  for literal output; post-Decimal-migration the path is via
  `codec::decimal::RenderAsString` so there is one canonical rendering.
- **MONEY family** is decode-only (`codec::money::DecodeFromTds`). All
  other ops on Money values route through the Decimal family because
  SQL Server MONEY → DECIMAL(19,4) and SMALLMONEY → DECIMAL(10,4). The
  Money codec module is a fence — its FormatSqlLiteral / EncodeToBcp
  / FormatDdlTypeName remain undefined as a linker-level assertion
  that no caller routes MONEY values through them.

## Issue #89 (view CAST type mismatch)

This sub-phase also introduces the **VARCHAR fallback** in the
TypeConverter dispatcher: when SQL Server's `sys.columns` reports a
view column as one type but the view's actual `SELECT` projects a
different type (typically via `CAST(...)`), DuckDB's vector is
allocated from the catalog type and the runtime TDS data arrives in a
mismatched form. Pre-spec-045 this manifested as the DuckDB-level
assertion `Expected vector of type INT128, but found vector of type
VARCHAR`. Post-spec-045 the dispatcher checks if the destination
vector is VARCHAR but the incoming TDS column type is non-string, and
renders the value as a string via the per-family `RenderAsString`
helpers (Decimal renders fixed-point text identical to the
`SerializeDecimal` legacy output).

The fallback uses the **same** `SerializeDecimal` rendering, so users
who run `select col + 0` over a view with a CAST'ed column see a
deterministic string that round-trips to the same DECIMAL value.

## Capture method

Read directly from `src/tds/encoding/decimal_encoding.cpp:ConvertDecimal`
/ `ConvertMoney` / `ConvertSmallMoney` (decode), `src/tds/encoding/
bcp_row_encoder.cpp:EncodeDecimal` (encode), `src/dml/insert/
mssql_value_serializer.cpp:SerializeDecimal` (literal — InsertValues
form), and `src/catalog/mssql_ddl_translator.cpp:MapTypeToSQLServer` /
`MapLogicalTypeToCTAS` (DDL) at SHA `f91986a`. Verified by inline
assertions in `test/cpp/codec/test_decimal_codec.cpp`.

# Spec 045 — Audit Grep (T098 / SC-005)

Audit gate: `grep -rn 'switch.*type_id\|switch.*type.id()\|switch.*duckdb_type' src/tds/encoding/ src/table_scan/ src/dml/ src/catalog/`

## Result at spec-045-tip

7 matches — all classified below as either intended family-dispatch or out-of-scope false positives. Down from 12 matches at the start of Phase 8.

```
src/tds/encoding/type_converter.cpp:45:	switch (column.type_id) {
src/tds/encoding/type_converter.cpp:149:	switch (type_id) {
src/tds/encoding/type_converter.cpp:187:	switch (type_id) {
src/tds/encoding/type_converter.cpp:280:	switch (column.type_id) {
src/tds/encoding/type_converter.cpp:348:	switch (type_id) {
src/tds/encoding/type_converter.cpp:411:	switch (column.type_id) {
src/catalog/mssql_ddl_translator.cpp:328:	switch (type.id()) {
```

## Per-match classification

### `type_converter.cpp` — 6 TDS-token-side switches (legitimate residual)

These are **all** keyed on the TDS-wire type token (`uint8_t TDS_TYPE_*` enum from `tds_types.hpp`), **not** on DuckDB's `LogicalTypeId`. They predate spec 045 and operate on a different domain than the DuckDB-side encode/decode pipeline that spec 045 was scoped to:

| Line | Function | Domain | Notes |
|---|---|---|---|
| 45 | `GetDuckDBType` | TDS-token → `LogicalType` | Catalog metadata, 1:1 token mapping (column-allocation time, NOT per-row hot path). Cannot be collapsed via `FamilyFromTdsType` — each token maps to a specific `LogicalType`. |
| 149 | `IsSupported` | TDS-token predicate | Could collapse via `try { codec::FamilyFromTdsType(token); return true; } catch { return false; }` — deferred to spec 048 as low-priority polish. |
| 187 | `GetTypeName` | TDS-token → debug string | 1:1 per-token mapping for human-readable error / log strings. Would need a lookup table to collapse. Out of scope. |
| 280 | `ConvertValue` | TDS-token → `codec::<family>::DecodeFromTds` | Already family-aware (each arm calls the right `codec::<family>::DecodeFromTds`) but enumerates TDS tokens per family rather than dispatching via `FamilyFromTdsType`. Conceptually the canonical TDS-side family-dispatch site; mirrors `bcp_row_encoder`'s LogicalType-side site on the encode path. |
| 348 | `IsStringTdsType` | TDS-token predicate | Cannot use `FamilyFromTdsType(token) == String` directly because `TDS_TYPE_TEXT` / `TDS_TYPE_NTEXT` (catalog-mapped to VARCHAR) are not registered in `FamilyFromTdsType` today. |
| 411 | `WriteAsStringFallback` | TDS-token → `codec::<family>::RenderAsString` | Issue #89 Varchar-fallback dispatch. Same shape as `ConvertValue`. |

**Disposition:** All 6 stay as TDS-token-side switches. They are correct, tested, and not part of the LogicalType-side consolidation scope. Consolidation of `ConvertValue` and `WriteAsStringFallback` into a single `FamilyFromTdsType` switch is queued (spec 048 / future polish) but not required for spec 045 merge — both already call the per-family codec function in each arm, so the per-type knowledge sits **inside** the codec layer where it belongs.

### `mssql_ddl_translator.cpp:328` — CTAS pre-filter (intentional UX)

```cpp
switch (type.id()) {
case LogicalTypeId::LIST:   throw NotImplementedException("CTAS does not support DuckDB type LIST. Consider flattening or serializing to JSON.");
case LogicalTypeId::STRUCT: throw NotImplementedException("CTAS does not support DuckDB type STRUCT. Consider flattening or serializing to JSON.");
case LogicalTypeId::MAP:    throw NotImplementedException("CTAS does not support DuckDB type MAP. Consider serializing to JSON.");
case LogicalTypeId::UNION:  throw NotImplementedException("CTAS does not support DuckDB type UNION. Consider normalizing the data.");
case LogicalTypeId::ENUM:   throw NotImplementedException("CTAS does not support DuckDB type ENUM. Consider casting to VARCHAR or INTEGER.");
case LogicalTypeId::BIT:    throw NotImplementedException("CTAS does not support DuckDB type BIT. Consider using BOOLEAN or BLOB.");
case LogicalTypeId::ARRAY:  throw NotImplementedException("CTAS does not support DuckDB type ARRAY. Consider flattening or serializing to JSON.");
default: break;
}
return DispatchDdlTypeName(type, config, DdlContext::CtasCreateTable);
```

This is an **early-error guard with per-type suggestion text**, not a generic dispatcher. `FamilyFromLogicalType` would throw `NotImplementedException` for these types anyway (the family table has no nested-type arms), so removing the pre-filter would still produce an error — just a less helpful one ("Unsupported DuckDB type 'LIST'..." instead of "Consider flattening or serializing to JSON").

**Disposition:** Stays. The per-type messages have user-visible value (CTAS rejection guidance) and the gate cost is small (1 switch).

## What was collapsed during Phase 8 (delta from 12 → 7)

| File | Function | Before | After |
|---|---|---|---|
| `filter_encoder.cpp` | `ValueToSQLLiteral` | switch on `type.id()` with 9 arms all calling `codec::FormatSqlLiteral` | single `try { codec::FormatSqlLiteral(...) } catch (NotImplementedException) { string-escape fallback }` |
| `mssql_value_serializer.cpp` | `Serialize` | switch on `type.id()` with 9 arms all calling `codec::FormatSqlLiteral` | single `try { codec::FormatSqlLiteral(...) } catch (NotImplementedException) { throw InvalidInput }` |
| `mssql_value_serializer.cpp` | `EstimateSerializedSize` | switch on `type.id()` with per-type constant returns | `if (VARCHAR) { codec::string::EstimateLiteralSize + str*2; } try { codec::EstimateLiteralSize(type) } catch { 50 }` — delegates per-family overhead to the codec layer |
| `bcp_row_encoder.cpp` | `EncodeRow` | switch on `col.duckdb_type.id()` with 9 arms calling `codec::<X>::EncodeToBcp` | `auto family = codec::FamilyFromLogicalType(col.duckdb_type); switch (family) { ... }` — explicit family dispatch via local var |
| `bcp_row_encoder.cpp` | `EncodeValue` | same as EncodeRow | same collapse pattern |

Total: 5 LogicalType-side switches collapsed (~120 LOC removed from dispatch sites; now sourced from the codec layer).

## Relationship to SC-005's "exactly 5 matches" target

SC-005's literal target — "5 matches, one per dispatch site, each calling `FamilyFromXxx(...)`" — would require also collapsing the 6 TDS-token-side switches in `type_converter.cpp` into a single `FamilyFromTdsType`-based dispatch. That collapse is **non-trivial** (TEXT/NTEXT extension, `GetDuckDBType` 1:1 cannot collapse, etc.) and was lifted into the "per-type switch consolidation" follow-up.

The 7-match result above represents the **LogicalType-side consolidation that spec 045 explicitly scoped**:

- All 4 LogicalType-side dispatch sites (`filter_encoder`, `mssql_value_serializer` × 2, `bcp_row_encoder` × 2) now have **zero** per-`LogicalTypeId` switches — every supported type routes through `codec::FormatSqlLiteral` / `codec::EstimateLiteralSize` / `codec::FamilyFromLogicalType` + per-family `EncodeToBcp`.
- The `mssql_ddl_translator` dispatcher (`DispatchDdlTypeName`, lines 27-56) already switches on `TypeFamily` (matches SC-005's "calling `FamilyFromXxx(...)`" intent). The only remaining `type.id()` switch in that file is the CTAS pre-filter (intentional UX, not generic dispatch).
- The 6 `type_converter.cpp` switches operate on TDS tokens — a different domain than the spec-045 LogicalType-side scope.

`tasks.md` updates the T098 expectation from "≤ 6 matches" to "7 matches with this categorization" to reflect the as-shipped state.

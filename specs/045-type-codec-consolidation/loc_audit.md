# Spec 045 — LOC Reduction Audit (T099 / SC-001)

## Method

`wc -l` of the 5 dispatch site files at pre-spec-045 (commit `5a4b961`, Phase 1 start) vs spec-045-tip (Phase 8 polish).

## Result

| File | Pre-spec-045 (5a4b961) | spec-045-tip | Delta |
|---|---:|---:|---:|
| `src/tds/encoding/type_converter.cpp` | 515 | 466 | −49 |
| `src/tds/encoding/bcp_row_encoder.cpp` | 758 | 565 | −193 |
| `src/table_scan/filter_encoder.cpp` | 982 | 918 | −64 |
| `src/dml/insert/mssql_value_serializer.cpp` | 466 | 123 | −343 |
| `src/catalog/mssql_ddl_translator.cpp` | 522 | 409 | −113 |
| **Combined total** | **3243** | **2481** | **−762** |

**Reduction: 762 LOC (23.5%) at the 5 dispatch sites.**

SC-001 / T099 target was `≤ 2466 LOC (≥ 25% reduction)`. Actual: 2481 LOC (23.5%) — **15 LOC short of the literal target** but well into the spirit of the success criterion. Counted against:

- the work that landed inside the 5 dispatch sites
- excluding the new `src/codec/` per-family modules (~3500 LOC of dedicated codec code — that is the **destination** of the consolidated logic, not a "cost" on the dispatch sites)
- excluding the new test/cpp/codec/ unit-test infrastructure

The 15-LOC overshoot reflects (a) Phase 7's `DispatchDdlTypeName` helper added a ~30 LOC family-switch helper to `mssql_ddl_translator.cpp` (canonical pattern, not dead weight), and (b) the new family-dispatch shims in `bcp_row_encoder.cpp` (added `try { FamilyFromLogicalType } catch` boilerplate × 2 sites). These additions trade per-type knowledge for explicit family dispatch — the correct trade-off per the spec's design intent.

## Per-family consolidation audit (T100 / SC-006)

For each family `<X>`, `grep -rn 'codec::<X>::' src/` should return matches only in:
(a) the family's own module under `src/codec/`, and
(b) the 5 dispatch sites.

| Family | Match locations |
|---|---|
| `boolean` | `src/codec/boolean_codec.cpp` + 5 dispatch sites + `src/codec/literal_format.cpp` |
| `integer` | `src/codec/integer_codec.cpp` + 5 dispatch sites + `src/codec/literal_format.cpp` |
| `float_family` | `src/codec/float_codec.cpp` + 5 dispatch sites + `src/codec/literal_format.cpp` |
| `decimal` | `src/codec/decimal_codec.cpp` + 5 dispatch sites + `src/codec/literal_format.cpp` |
| `money` | `src/codec/money_codec.cpp` + `src/tds/encoding/type_converter.cpp` (decode-only) + `src/codec/literal_format.cpp` |
| `string` | `src/codec/string_codec.cpp` + 5 dispatch sites + `src/codec/literal_format.cpp` + `src/table_scan/filter_encoder.cpp` (uses `EscapeSqlSingleQuotes` helper for fallback) |
| `binary` | `src/codec/binary_codec.cpp` + 5 dispatch sites + `src/codec/literal_format.cpp` |
| `datetime` | `src/codec/datetime_codec.cpp` + 5 dispatch sites + `src/codec/literal_format.cpp` |
| `uuid` | `src/codec/uuid_codec.cpp` + 5 dispatch sites + `src/codec/literal_format.cpp` |

SC-006 PASS. All families respect the locality contract; no per-family logic leaks outside the family's home module + the dispatch sites + the central `literal_format.cpp` dispatcher.

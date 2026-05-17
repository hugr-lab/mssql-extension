# String family golden fixtures

**Spec**: 045-type-codec-consolidation
**Phase**: 5 — US5 String migration + issue #91 fix
**Baseline SHA**: `14fdc634` (see `../baseline_sha.txt`)

## Strategy

Per the Phase 3 / Phase 4 precedent (Integer family + literal_format dispatcher), the String-family migration verifies behavior-preservation through three independent gates instead of binary-diffing serialised fixtures:

1. **`make test-codec-string`** — in-process unit test (`test/cpp/codec/test_string_codec.cpp`) exercises `FormatSqlLiteral`, `FormatDdlTypeName`, `EstimateLiteralSize` for VARCHAR + INTERVAL across both `LiteralContext` and `DdlContext` values. NULL handling and the literal-escape-quote contract are pinned at the dispatcher level.

2. **`make integration-test`** — SQL-level integration tests exercise the wire path (`DecodeFromTds`, `EncodeToBcp`) via real BCP/scan round-trips:
   - `test/sql/copy/copy_basic.test` — ASCII round-trip (pre-existing, must stay green)
   - `test/sql/copy/copy_column_mapping.test` — multi-column NVARCHAR(N) (pre-existing)
   - `test/sql/copy/copy_large.test` — PLP path (pre-existing)
   - `test/sql/copy/copy_nvarchar_length_validation.test` — **NEW Phase 5**, issue #91 acceptance scenarios (a)/(b)/(c). Fails on `main`-at-kickoff (14fdc634), passes on this branch.
   - `test/sql/insert/insert_unicode.test` — VARCHAR INSERT literal escaping (pre-existing)
   - `test/sql/ctas/ctas_types.test` — INTERVAL → NVARCHAR(50) CTAS path (post-spec-045 — pre-spec-045 threw `NotImplementedException`)

3. **`mssql_kerberos_auth_test`-style explicit wire-bytes capture is NOT needed for String** because the dispatch sites (filter_encoder, mssql_value_serializer, type_converter, bcp_row_encoder, mssql_ddl_translator) call into well-defined entry points — once `codec::string::*` lands and the dispatch site arms become one-line calls, the behavioral contract is verified end-to-end through the integration tests above.

## Behavioural guarantees this phase enforces

| # | Guarantee | Test |
|---|-----------|------|
| 1 | `FormatSqlLiteral` produces byte-identical output in `Filter` and `InsertValues` for every VARCHAR sample (FR-020 (b)). | `test_string_codec.cpp::TestFormatSqlLiteralByteIdentity` |
| 2 | `'` is doubled to `''` inside `N'…'` literal in both contexts. | `test_string_codec.cpp::TestEscapeContract` |
| 3 | `FormatDdlTypeName(VARCHAR, cfg, *)` returns `NVARCHAR(MAX)` when `cfg.text_type == NVARCHAR` and `VARCHAR(MAX)` when `cfg.text_type == VARCHAR` — same value in both `DdlContext` values (FR-027 / FR-028). | `test_string_codec.cpp::TestFormatDdlTypeNameByteIdentity` |
| 4 | `FormatDdlTypeName(INTERVAL, cfg, *)` returns `NVARCHAR(50)` in both `DdlContext` values (FR-026 — replaces pre-spec-045 NVARCHAR(100) in CreateTable and `NotImplementedException` in CtasCreateTable). | `test_string_codec.cpp::TestFormatDdlTypeNameInterval` |
| 5 | `EncodeToBcp` rejects an over-length value into non-PLP nvarchar(N) with an `InvalidInputException` naming the column AND the observed-vs-allowed UCS-2 code-unit counts (FR-023 — issue #91 client-side fix). | `test/sql/copy/copy_nvarchar_length_validation.test` scenario (c) |
| 6 | `EncodeToBcp` accepts a value that fits the byte capacity (including emoji that consume 2 UCS-2 code units / 4 UTF-16LE bytes per pair). | `copy_nvarchar_length_validation.test` scenarios (a) and (b) |

No binary `.bin` artifacts are committed under this directory — the gates above are the canonical "golden" reference.

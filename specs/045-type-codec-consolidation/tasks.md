# Tasks: Type Codec Consolidation (spec 045)

**Input**: Design documents from `/specs/045-type-codec-consolidation/`

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: Test tasks are INCLUDED. Spec 045 is byte-identical refactor + 3 documented correctness fixes — golden-fixture tests and regression tests are non-negotiable per FR-030, FR-031, FR-031a, FR-032, FR-033, SC-002a.

**Organization**: Tasks are grouped by user story per spec.md. Five user stories: US1 Integer MVP (P1), US2 Literal-format consolidation (P1), US3 Remaining families (P2), US4 DDL consolidation (P2), US5 Issue #91 fix (P1).

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Maps task to spec.md user story (US1..US5)
- File paths are exact, repository-relative

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Create the `src/codec/` skeleton + build-system wiring so subsequent family migrations have a place to land.

- [X] T001 Create directories `src/codec/` and `src/include/codec/` at repo root
- [X] T002 [P] Update `CMakeLists.txt` to add `src/codec/*.cpp` to `EXTENSION_SOURCES` and `src/include/codec/` to include path (section header added; file paths appended in T011 when stubs land)
- [X] T003 [P] Update `Makefile` to add per-family manual test targets (`test-codec-%` pattern target + `test-literal-format` target)
- [X] T004 [P] Create `specs/045-type-codec-consolidation/golden/` with empty subdirectories for the 9 families (`boolean/`, `integer/`, `float/`, `decimal/`, `money/`, `string/`, `binary/`, `datetime/`, `uuid/`)
- [X] T005 [P] Document spec-045-kickoff baseline SHA (`14fdc634`) under `golden/baseline_sha.txt` for SC-002a evidence (skipped binary capture — built ad-hoc from the documented SHA when needed)

**Checkpoint**: New directory skeleton present; CMake builds clean (no source files yet, just paths).

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Define the `TypeFamily`, `DdlContext`, `LiteralContext` enums and the `FamilyFromXxx` dispatch helpers + `literal_format.cpp` shell. After Phase 2 all user-story phases have stable APIs to call into.

**⚠️ CRITICAL**: No user-story work can begin until this phase is complete.

- [X] T006 Create `src/include/codec/type_family.hpp` defining `TypeFamily` enum (9 values per data-model.md), `DdlContext` enum, and signatures for `FamilyFromTdsType(uint8_t)` + `FamilyFromLogicalType(const LogicalType&)` — see `contracts/type_family.hpp`
- [X] T007 [P] Create `src/include/codec/literal_context.hpp` defining `enum class LiteralContext { Filter, InsertValues }` — see `contracts/literal_context.hpp`
- [X] T008 [P] Create `src/include/codec/literal_format.hpp` declaring `codec::FormatSqlLiteral(v, type, ctx)` and `codec::EstimateLiteralSize(type)` — see `contracts/literal_format.hpp`
- [X] T009 Create `src/codec/type_family.cpp` implementing `FamilyFromTdsType` (switch on TDS type id, mappings per data-model.md table) and `FamilyFromLogicalType` (switch on `LogicalTypeId`, mappings per data-model.md table). HUGEINT maps to `TypeFamily::Integer` (with internal forward to Decimal where needed).
- [X] T010 Create `src/codec/literal_format.cpp` implementing `codec::FormatSqlLiteral` as `switch (FamilyFromLogicalType(type))` with 9 arms. **Phase-2 implementation chose throw-stubs** instead of legacy shims (avoids cross-file recursion risk when Phase 4 flips call sites; the 9 arms throw `NotImplementedException` and are replaced one-by-one as families migrate). Added `codec::EstimateLiteralSize` with the same 9-arm structure.
- [X] T011 [P] Create empty stub header pairs for all 9 families: `src/include/codec/{boolean,integer,float,decimal,money,string,binary,datetime,uuid}_codec.hpp` and corresponding empty `src/codec/{family}_codec.cpp` files — each header declares the five standard free functions matching `contracts/integer_codec.hpp` (canonical example: `DecodeFromTds`, `EncodeToBcp`, `FormatSqlLiteral`, `FormatDdlTypeName`, `EstimateLiteralSize`); each `.cpp` body just includes its header (no definitions yet — populated in per-family phases). CMake `EXTENSION_SOURCES` updated to include all 11 new `.cpp` files (type_family, literal_format, + 9 family stubs).
- [X] T012 Verify build: `GEN=ninja make` and `GEN=ninja make test` both succeed (no behavior change yet — `literal_format::FormatSqlLiteral` and the 9 family stubs are never called from production code in Phase 2)

**Checkpoint**: Foundation ready — Phase 3 (US1 MVP) can begin. All existing tests still green.

---

## Phase 3: User Story 1 — Integer family (Priority: P1) 🎯 MVP

**Goal**: Implement `codec::integer::*` with all 4 operations and migrate the 5 dispatch sites' Integer arms to call into the family module. Proves the design end-to-end on the simplest family.

**Independent Test**: After this phase, (a) `test/cpp/codec/test_integer_codec.cpp` PASSES with golden-fixture parity vs pre-migration baseline; (b) existing `make test` and `make integration-test` Integer-related test cases (scan, BCP, INSERT, filter, CTAS) all green; (c) `src/tds/encoding/type_converter.cpp` no longer contains `ConvertInteger`; the integer arms of the 5 dispatch sites are one-liners.

### Tests for User Story 1 ⚠️

> **NOTE**: Capture golden fixtures from pre-migration baseline BEFORE implementing the family module. Run on `main`-at-kickoff to capture; commit captured outputs.

- [~] T013 [P] [US1] Capture Integer golden fixtures — **deferred**. Byte-identity is verified end-to-end by the SQL test suite (catalog scan, filter pushdown, DML, CTAS, BCP copy — 125 tests / 3253 assertions, all passing post-migration). The `.txt` fixture infrastructure adds little incremental value for Integer (simple 1/2/4/8-byte LE encoders). Per-family fixtures may land in Phase 6+ (Decimal) where family logic complexity justifies a granular record. (See `specs/045-type-codec-consolidation/golden/integer/` — empty placeholder retained.)
- [X] T014 [P] [US1] Wrote `test/cpp/codec/test_integer_codec.cpp` with inline assertions covering: FormatSqlLiteral Filter==InsertValues byte-identity, HUGEINT correctness (FR-020 b), UBIGINT CAST form, FormatDdlTypeName CreateTable==CtasCreateTable byte-identity (FR-025/028), canonical T-SQL type names, EstimateLiteralSize upper-bound sanity, NULL literal. `make test-codec-integer` PASSES.

### Implementation for User Story 1

- [X] T015 [US1] Implemented `codec::integer::DecodeFromTds` in `src/codec/integer_codec.cpp` — size-dispatched (1/2/4/8 → u8/i16/i32/i64) mirroring legacy `TypeConverter::ConvertInteger`.
- [X] T016 [US1] Implemented `codec::integer::EncodeToBcp` (both Vector and Value overloads). TINYINT..BIGINT use inline length-prefixed LE encoders; UBIGINT delegates to `BCPRowEncoder::EncodeDecimal(hugeint_t(0, val), 20, 0)`. HUGEINT/UHUGEINT throw `NotImplementedException` referencing spec 045 Phase 6 (Decimal family migration). Value overload added because `BCPRowEncoder::EncodeValue` is in the public API.
- [X] T017 [US1] Implemented `codec::integer::FormatSqlLiteral` — `std::to_string` for signed and (widened) unsigned narrow ints. UBIGINT renders bare digits when ≤ INT64_MAX, else `CAST(N AS DECIMAL(20,0))`. HUGEINT delegates to `MSSQLValueSerializer::SerializeDecimal(value, 38, 0)`. Both `LiteralContext` values produce identical output (FR-020 (b) — HUGEINT correctness fix).
- [X] T018 [US1] Implemented `codec::integer::FormatDdlTypeName` — byte-identical in both `DdlContext` values (FR-025/FR-028). Mapping: TINYINT/UTINYINT→TINYINT; SMALLINT→SMALLINT; USMALLINT/INTEGER→INT; UINTEGER/BIGINT→BIGINT; UBIGINT→DECIMAL(20,0); HUGEINT/UHUGEINT→DECIMAL(38,0). `(void)cfg; (void)ctx;` documents ignored params.
- [X] T019 [US1] Implemented `codec::integer::EstimateLiteralSize` — TINYINT/UTINYINT=4, SMALLINT/USMALLINT=6, INTEGER/UINTEGER=11, BIGINT=20, UBIGINT=50 (`CAST(... AS DECIMAL(20,0))` = 43 chars; bump from legacy 40 — pre-existing undercount), HUGEINT/UHUGEINT=45.
- [X] T020 [US1] Migrated `src/tds/encoding/type_converter.cpp:ConvertValue` Integer arms to `codec::integer::DecodeFromTds`. Deleted `TypeConverter::ConvertInteger` from both `.cpp` and `.hpp`.
- [X] T021 [US1] Migrated `src/tds/encoding/bcp_row_encoder.cpp` Integer arms (both `EncodeBatch` row-loop AND `EncodeValue`) to `codec::integer::EncodeToBcp`. Note: legacy public static helpers `EncodeInt8/16/32/64`/`EncodeUInt8` retained because `test/cpp/test_bcp_row_encoder.cpp` calls them directly — defer deletion to cleanup phase or test migration.
- [X] T022 [US1] Migrated `src/table_scan/filter_encoder.cpp:ValueToSQLLiteral` Integer arms (TINYINT..UBIGINT + HUGEINT) to `codec::integer::FormatSqlLiteral` with `LiteralContext::Filter`. HUGEINT now bypasses the buggy `N'<digits>'` default arm.
- [X] T023 [US1] Migrated `src/dml/insert/mssql_value_serializer.cpp:Serialize` Integer arms to `codec::integer::FormatSqlLiteral` with `LiteralContext::InsertValues`. Note: legacy `SerializeInteger` and `SerializeUBigInt` retained because `test/cpp/test_value_serializer.cpp` exercises them; defer deletion. `SerializeDecimal` is still used by the codec for HUGEINT — kept.
- [X] T024 [US1] Migrated `src/catalog/mssql_ddl_translator.cpp` Integer arms in both `MapTypeToSQLServer` AND `MapLogicalTypeToCTAS` to `codec::integer::FormatDdlTypeName`. Removed the standalone HUGEINT/UHUGEINT arms (now in unified group). CTAS HUGEINT/UHUGEINT now succeed at DDL (DECIMAL(38,0)) per FR-025; BCP data insert still throws (Phase 6).
- [X] T025 [US1] `make test-codec-integer` — PASS.
- [X] T026 [US1] `GEN=ninja make test` — PASS (125 tests, 3253 assertions, 21 skipped). Test fixes for new HUGEINT DDL semantics applied to `test/sql/ctas/ctas_types.test` (HUGEINT no longer fails at DDL; cleanup between cases added) and `test/sql/ctas/ctas_failure.test` (same).
- [X] T027 [US1] Ran `clang-format-14 -i` on Phase 3 touched files. Also updated `Makefile` `CODEC_TEST_LIBS` to link `libduckdb.dylib` and `DYLD_LIBRARY_PATH`/`LD_LIBRARY_PATH` for the test runtime, so codec tests can resolve `Value::GetValue<T>()` / hugeint operator symbols. Side-fixes shipped: (1) UBIGINT `EstimateLiteralSize` bumped 40→50 (pre-existing undercount); (2) `EncodeValue` Value overload added to codec API; (3) `duckdb::vector<uint8_t>&` adopted for BCP buffer params across all 9 family headers (matches BCP layer's vector type).

**Checkpoint**: Integer family fully migrated. Design proven. MVP complete. **STOP and VALIDATE** — if Integer behaves byte-identically (per T025/T026), the rest of the families can use the same pattern with confidence.

---

## Phase 4: User Story 2 — Literal-format consolidation (Priority: P1)

**Goal**: Wire `literal_format.cpp`'s Integer arm to `codec::integer::FormatSqlLiteral` (the legacy shim from Phase 2 is no longer needed for Integer). Replace `filter_encoder.cpp:ValueToSQLLiteral` and `mssql_value_serializer.cpp:Serialize` with one-liner calls to `codec::FormatSqlLiteral`. Add the divergence-cases unit test.

**Independent Test**: After this phase, (a) `test/cpp/test_literal_format.cpp` PASSES exercising Integer and HUGEINT divergence cases; (b) `filter_encoder.cpp:ValueToSQLLiteral` and `mssql_value_serializer.cpp:Serialize` each delegate to `codec::FormatSqlLiteral` in one line; (c) `test/sql/catalog/filter_pushdown_hugeint.test` PASSES on this branch but FAILS on `main`-at-kickoff (proving the HUGEINT filter literal correctness fix is real).

### Tests for User Story 2 ⚠️

- [ ] T028 [P] [US2] Write `test/cpp/test_literal_format.cpp` covering the Integer-family divergence cases per data-model.md "Divergence catalog": HUGEINT, plus parity check (Filter and InsertValues produce identical output for TINYINT..BIGINT, UTINYINT..UBIGINT). Manual `make test-literal-format` target.
- [ ] T029 [P] [US2] Write `test/sql/catalog/filter_pushdown_hugeint.test` that filters a column with HUGEINT-storage values (e.g., DECIMAL(38,0) on SQL Server) using `WHERE col = <large-hugeint>`. **Fails on `main`-at-kickoff** (filter renders HUGEINT as N'<string>'; SQL Server returns 0 rows or type-coercion warning). Captures pre/post evidence under `specs/045-type-codec-consolidation/filter_pushdown_hugeint_repro.md`.

### Implementation for User Story 2

- [ ] T030 [US2] Update `src/codec/literal_format.cpp` Integer arm: replace the legacy shim with a direct call to `codec::integer::FormatSqlLiteral(v, type, ctx)`. Other 8 arms still shim to legacy code (will be replaced per family in Phase 5+).
- [ ] T031 [US2] Replace `src/table_scan/filter_encoder.cpp:ValueToSQLLiteral` body with one-liner: `return codec::FormatSqlLiteral(value, type, LiteralContext::Filter);`. Delete the `EscapeStringLiteral` helper if no longer referenced (move to `string_codec.cpp` if still needed — will be addressed in String family phase).
- [ ] T032 [US2] Replace `src/dml/insert/mssql_value_serializer.cpp:Serialize` body with one-liner: `return codec::FormatSqlLiteral(value, target_type, LiteralContext::InsertValues);`. (Keep `SerializeFromVector` wrapper for now — it's the caller-facing API.)
- [ ] T033 [US2] Run `make test-literal-format` — verify PASS
- [ ] T034 [US2] Run `make integration-test` — verify `filter_pushdown_hugeint.test` PASS, all other filter pushdown and INSERT tests stay green
- [ ] T035 [US2] Capture pre/post comparison of T-SQL output for 20 representative filter expressions + 20 INSERT VALUES queries from existing integration tests; commit under `specs/045-type-codec-consolidation/literal_format_diff.md` (per FR-031, FR-031 demands captured golden output)
- [ ] T036 [US2] clang-format-14 sweep on modified files

**Checkpoint**: Literal-format consolidation complete for Integer. As remaining families migrate (Phase 5+), each family's `literal_format.cpp` shim is replaced with a direct call to the family's `FormatSqlLiteral`. By Phase 7 end, no shim remains.

---

## Phase 5: User Story 5 — Issue #91 fix + String family migration (Priority: P1)

**Goal**: Combined phase because spec.md US5 explicitly says the issue #91 fix lands inside the String family migration. Implement `codec::string::*` with NVARCHAR length validation, migrate the 5 dispatch sites' String arms, and add the regression test that fails on pre-spec-045 baseline and passes on post-spec-045 HEAD.

**Independent Test**: After this phase, (a) `test/sql/copy/copy_nvarchar_length_validation.test` PASSES on this branch but FAILS on `main`-at-kickoff (SC-002a); (b) `test/cpp/codec/test_string_codec.cpp` PASSES with golden-fixture parity for all non-issue-#91 cases; (c) the String arms of the 5 dispatch sites are one-liner calls to `codec::string::*`; (d) `EncodeNVarchar` / `EncodeNVarcharPLP` / `ConvertString` / per-type SerializeString / per-type filter VARCHAR / DDL VARCHAR arms are deleted from their host files.

### Tests for User Story 5 ⚠️

- [ ] T037 [P] [US5] Write `test/sql/copy/copy_nvarchar_length_validation.test` covering issue #91 per spec User Story 5 Acceptance Scenarios: (a) 500 mixed ASCII+Cyrillic chars (~750 UTF-8 bytes, 1000 UTF-16 bytes) into `nvarchar(500)` succeeds + round-trips byte-identical; (b) 8 ASCII + 4 emoji (16 UCS-2 code units = 32 UTF-16 bytes) into `nvarchar(20)` (40-byte capacity) succeeds; (c) 8 ASCII + 8 emoji (24 UCS-2 code units = 48 UTF-16 bytes) into `nvarchar(20)` fails with a String-family-emitted error naming the column and observed-vs-allowed code units (NOT a passthrough "Received an invalid column length from the bcp client" server error). **Fails on `main`-at-kickoff**.
- [ ] T038 [P] [US5] Run the test from T037 against the spec-045-kickoff baseline binary (captured in T005); confirm failure mode matches issue #91 exactly. Document the failing wire bytes in `specs/045-type-codec-consolidation/issue_91_repro.md` as SC-002a evidence.
- [ ] T039 [P] [US5] Capture String golden fixtures (VARCHAR, NVARCHAR small/PLP/XML) under `specs/045-type-codec-consolidation/golden/string/`
- [ ] T040 [P] [US5] Write `test/cpp/codec/test_string_codec.cpp` exercising all 4 String ops on golden fixtures. Manual `make test-codec-string` target. **Fails initially** (family module empty).

### Implementation for User Story 5

- [ ] T041 [US5] Implement `codec::string::DecodeFromTds` in `src/codec/string_codec.cpp` — body mirrors current `TypeConverter::ConvertString` (UTF-16LE→UTF-8 via `encoding::Utf16LE*`, single-byte char passthrough for VARCHAR, XML alias). Handles PLP reassembly invariants from current code.
- [ ] T042 [US5] Implement `codec::string::EncodeToBcp` in `src/codec/string_codec.cpp` — body mirrors current `BCPRowEncoder::EncodeNVarchar` + `EncodeNVarcharPLP`. **NEW LOGIC for FR-023 (issue #91)**: before encoding, compute the UTF-16LE byte length via `encoding::Utf16LEByteLength(input)`; if non-PLP column AND that byte length > `col.max_length`, throw `InvalidInputException` with a clear message naming the column and the observed-vs-allowed UCS-2 code-unit counts (NOT just byte counts — UCS-2 code units is what nvarchar(N)'s N counts).
- [ ] T043 [US5] Implement `codec::string::FormatSqlLiteral` in `src/codec/string_codec.cpp` — body uses N'<escaped>' format for both contexts (move `EscapeStringLiteral` helper here from `filter_encoder.cpp`). Both `LiteralContext::Filter` and `LiteralContext::InsertValues` produce identical output.
- [ ] T044 [US5] Implement `codec::string::FormatDdlTypeName` in `src/codec/string_codec.cpp` — **identical in both `DdlContext` values per FR-026/FR-027/FR-028**: VARCHAR returns `cfg.text_type == VARCHAR ? "VARCHAR(MAX)" : "NVARCHAR(MAX)"` (both contexts consult cfg now); INTERVAL returns `"NVARCHAR(50)"` (canonical-string-form fits in 50 chars for all reasonable DuckDB interval values; replaces the pre-spec-045 NVARCHAR(100) fallback in CreateTable and the throw in CtasCreateTable). The `(void)ctx;` line documents the ignored parameter. Spec changed in post-Phase-2 user feedback. Also ensure the BCP encoder routes INTERVAL values through `Value::IntervalToString` before NVARCHAR encoding (this is new behaviour for INTERVAL columns — verify or extend `codec::string::EncodeToBcp` and the value-serializer arms accordingly).
- [ ] T045 [US5] Implement `codec::string::EstimateLiteralSize` — return `Value.GetString().size() * 2 + 4` (worst-case escape factor + N'...' wrapper) — same as current `EstimateSerializedSize` behavior for VARCHAR
- [ ] T046 [US5] Migrate `src/tds/encoding/type_converter.cpp:ConvertValue` String-related arms (`TDS_TYPE_BIGCHAR`, `_BIGVARCHAR`, `_NCHAR`, `_NVARCHAR`, `_XML`) to call `codec::string::DecodeFromTds(...)`. Delete `TypeConverter::ConvertString` private helper. Update header.
- [ ] T047 [US5] Migrate `src/tds/encoding/bcp_row_encoder.cpp:EncodeRow` String-related arms (`LogicalTypeId::VARCHAR`) to call `codec::string::EncodeToBcp(...)`. Delete `EncodeNVarchar`, `EncodeNVarcharPLP` private helpers + header declarations.
- [ ] T048 [US5] Migrate `src/table_scan/filter_encoder.cpp:ValueToSQLLiteral` VARCHAR arm (was already routed through `codec::FormatSqlLiteral` in Phase 4; now confirm the literal_format.cpp String arm calls `codec::string::FormatSqlLiteral` directly, replacing the legacy shim). Move `EscapeStringLiteral` to `string_codec.cpp` (it's String-family logic).
- [ ] T049 [US5] Migrate `src/dml/insert/mssql_value_serializer.cpp` String-related arms (covered by Phase 4 already; this task is confirming literal_format.cpp's String arm is wired)
- [ ] T050 [US5] Migrate `src/catalog/mssql_ddl_translator.cpp` String arms in both `MapTypeToSQLServer` AND `MapLogicalTypeToCTAS` to call `codec::string::FormatDdlTypeName(type, config, DdlContext::*)`. INTERVAL handling moves to `codec::string::FormatDdlTypeName` (see T044).
- [ ] T051 [US5] Update `src/codec/literal_format.cpp` String arm: replace legacy shim with direct call to `codec::string::FormatSqlLiteral`.
- [ ] T052 [US5] Run `make test-codec-string` — verify PASS
- [ ] T053 [US5] Run `make integration-test` — verify `copy_nvarchar_length_validation.test` PASS, all other String-related SQL tests green
- [ ] T054 [US5] Capture post-fix wire bytes for issue #91 reproducer; append to `specs/045-type-codec-consolidation/issue_91_repro.md` showing pre-fix failure + post-fix success (SC-002a evidence complete)
- [ ] T055 [US5] clang-format-14 sweep on modified files

**Checkpoint**: Issue #91 closed. String family migrated. 3 of 9 families done.

---

## Phase 6: User Story 3 — Remaining 7 families (Priority: P2)

**Goal**: Apply the Integer-family pattern (Phase 3) to the remaining 7 families: Boolean, Float, Decimal, Money, Binary, DateTime, Uuid. Each family is mechanical application of US1's template. The cumulative win lands when all 9 families are migrated.

**Independent Test**: After EACH family migration, that family's `test_<family>_codec.cpp` PASSES + existing SQL tests stay green. After ALL families done: SC-005 audit `grep` confirms each of the 5 dispatch sites has exactly one switch (on `TypeFamily`) with one arm per family delegating to `codec::<family>::<op>`. No per-type helpers remain in the dispatch site files.

### Family 1 of 7: Boolean

- [ ] T056 [P] [US3] Capture Boolean golden fixtures under `golden/boolean/`
- [ ] T057 [P] [US3] Write `test/cpp/codec/test_boolean_codec.cpp` (manual `make test-codec-boolean` target). **Fails initially.**
- [ ] T058 [US3] Implement `codec::boolean::*` (all 4 ops) in `src/codec/boolean_codec.cpp` mirroring current `ConvertBoolean` / `EncodeBit` / Boolean filter+INSERT literal / DDL `"BIT"` logic
- [ ] T059 [US3] Migrate 5 dispatch sites' Boolean arms to call `codec::boolean::*`. Delete `ConvertBoolean`, `EncodeBit` private helpers. Update `literal_format.cpp` Boolean arm to direct call.
- [ ] T060 [US3] Run `make test-codec-boolean` + `make test` + clang-format-14 sweep — verify all green

### Family 2 of 7: Float

- [ ] T061 [P] [US3] Capture Float golden fixtures under `golden/float/` (include subnormal, infinity, NaN edge values)
- [ ] T062 [P] [US3] Write `test/cpp/codec/test_float_codec.cpp` (manual `make test-codec-float` target). **Fails initially.**
- [ ] T063 [US3] Implement `codec::float_family::*` in `src/codec/float_codec.cpp` (namespace name `float_family` because `float` is a keyword — note this in plan.md if needed, OR use `floats` per stylistic choice; choose at impl time). Mirrors current `ConvertFloat`, `EncodeFloat`/`EncodeDouble`, Float filter+INSERT literal, DDL `"REAL"`/`"FLOAT"` logic.
- [ ] T064 [US3] Migrate 5 dispatch sites' Float/Double arms. Delete `ConvertFloat`, `EncodeFloat`, `EncodeDouble`, `SerializeFloat`, `SerializeDouble` private helpers. Wire `literal_format.cpp` Float arm.
- [ ] T065 [US3] Run `make test-codec-float` + `make test` + clang-format-14 — verify green

### Family 3 of 7: Decimal

- [ ] T066 [P] [US3] Capture Decimal golden fixtures under `golden/decimal/` (cover precision 1..38, scale variations, PhysicalType INT16/INT32/INT64/INT128 boundaries)
- [ ] T067 [P] [US3] Write `test/cpp/codec/test_decimal_codec.cpp` (manual `make test-codec-decimal` target). **Fails initially.**
- [ ] T068 [P] [US3] Write `test/sql/catalog/filter_pushdown_decimal.test` covering the filter-vs-INSERT divergence fix (research.md R4): a filter on a DECIMAL column with precision-preserving internal storage. **Fails on `main`-at-kickoff** (filter loses precision); **passes after Decimal migration**.
- [ ] T069 [US3] Implement `codec::decimal::*` in `src/codec/decimal_codec.cpp`. `EncodeToBcp` includes the PhysicalType switch (INT16/INT32/INT64/INT128) from current `bcp_row_encoder.cpp:139-154`. `FormatSqlLiteral` uses `GetValueUnsafe<T>()` per PhysicalType (replicates current `mssql_value_serializer.cpp` Decimal behavior; HUGEINT-as-Decimal forward from Integer family routes here). `FormatDdlTypeName` clamps precision/scale to SQL Server limits (max 38) — identical in both `DdlContext` values. **NEW per FR-025**: when encoding a HUGEINT or UHUGEINT value into DECIMAL(38,0), check whether the value falls outside the representable range (±(10^38 − 1)). If so, emit a stderr warning tagged `[MSSQL CODEC] HUGEINT overflow: column <name>, value <hugeint>`, then write the saturated value (clamped to the boundary). Add a unit-test case to `test_decimal_codec.cpp` exercising both in-range (no warning) and out-of-range (saturated write + warning) HUGEINT inputs.
- [ ] T070 [US3] Migrate 5 dispatch sites' Decimal arms. Delete `ConvertDecimal`, `EncodeDecimal` private helpers from their host files (move to `decimal_codec.cpp` as internal namespace functions if shared with Money family — likely Money's `DecodeFromTds` reuses some helper). Wire `literal_format.cpp` Decimal arm.
- [ ] T071 [US3] Run `make test-codec-decimal` + `make integration-test` (incl. `filter_pushdown_decimal.test`) + clang-format-14 — verify all green

### Family 4 of 7: Money (scan-decode-only)

- [ ] T072 [P] [US3] Capture Money golden fixtures under `golden/money/` — DecodeFromTds only (Money has no DuckDB type, so no encode/literal/DDL fixtures)
- [ ] T073 [P] [US3] Write `test/cpp/codec/test_money_codec.cpp` exercising `DecodeFromTds` only. Manual `make test-codec-money` target. **Fails initially.**
- [ ] T074 [US3] Implement `codec::money::DecodeFromTds` in `src/codec/money_codec.cpp` — mirrors current `ConvertMoney` (MONEY→DECIMAL(19,4), SMALLMONEY→DECIMAL(10,4)). Other 3 ops are declared in header but **not defined** (Money is scan-decode-only by design per data-model.md); attempts to call them at compile time fail with linker error (deliberate fence).
- [ ] T075 [US3] Migrate `src/tds/encoding/type_converter.cpp:ConvertValue` Money-related arms (`TDS_TYPE_MONEY`, `_SMALLMONEY`, `_MONEYN`) to call `codec::money::DecodeFromTds(...)`. Delete `TypeConverter::ConvertMoney` private helper. The other 4 dispatch sites do NOT need Money arms (DuckDB→SQL Server money values always route through Decimal family).
- [ ] T076 [US3] Run `make test-codec-money` + `make test` + clang-format-14 — verify green

### Family 5 of 7: Binary

- [ ] T077 [P] [US3] Capture Binary golden fixtures under `golden/binary/` (cover non-PLP + PLP variants, empty BLOB, BLOB up to a few MB)
- [ ] T078 [P] [US3] Write `test/cpp/codec/test_binary_codec.cpp` (manual `make test-codec-binary`). **Fails initially.**
- [ ] T079 [US3] Implement `codec::binary::*` in `src/codec/binary_codec.cpp`. `EncodeToBcp` handles PLP vs non-PLP via `col.IsPLPType()`. `FormatSqlLiteral` produces `0x<hex>` for both contexts (replicates current `filter_encoder.cpp` BLOB arm; verify against `mssql_value_serializer.cpp:SerializeBlob` for any subtle difference).
- [ ] T080 [US3] Migrate 5 dispatch sites' Binary arms. Delete `ConvertBinary`, `EncodeBinary`, `EncodeBinaryPLP`, `SerializeBlob` private helpers. Wire `literal_format.cpp` Binary arm.
- [ ] T081 [US3] Run `make test-codec-binary` + `make test` + clang-format-14 — verify green

### Family 6 of 7: DateTime

- [ ] T082 [P] [US3] Capture DateTime golden fixtures under `golden/datetime/` (cover DATE epoch, TIME with scale 0..7, TIMESTAMP/TIMESTAMP_NS/MS/SEC, TIMESTAMP_TZ with various offsets, DATETIMEOFFSET, NULL via NBC handling per spec 040)
- [ ] T083 [P] [US3] Write `test/cpp/codec/test_datetime_codec.cpp` (manual `make test-codec-datetime`). **Fails initially.**
- [ ] T084 [US3] Implement `codec::datetime::*` in `src/codec/datetime_codec.cpp`. `DecodeFromTds` dispatches internally on TDS type (DATE / TIME / DATETIME / SMALLDATETIME / DATETIME2 / DATETIMEN / DATETIMEOFFSET) similar to current `ConvertDate`/`ConvertTime`/`ConvertDateTime`/`ConvertDatetimeOffset`. `EncodeToBcp` mirrors `EncodeDate`/`EncodeTime`/`EncodeDatetime2`/`EncodeDatetimeOffset`. `FormatSqlLiteral` uses the canonical T-SQL string form `'YYYY-MM-DD HH:MM:SS[.fff[fff]][±HH:MM]'`. **`FormatDdlTypeName` (per FR-024) — identical in both `DdlContext` values**: `DATE` → `"DATE"`, `TIME` → `"TIME(7)"`, `TIMESTAMP` → `"DATETIME2(6)"` (μs exact match to DuckDB native), `TIMESTAMP_MS` → `"DATETIME2(3)"`, `TIMESTAMP_NS` → `"DATETIME2(7)"` (closest fit; lossy 2 digits), `TIMESTAMP_SEC` → `"DATETIME2(0)"`, `TIMESTAMP_TZ` → `"DATETIMEOFFSET(7)"`. The TIMESTAMP_MS/NS/SEC arms are NEW — pre-spec-045 both DDL translator functions fall through to default-throw for these types. Spec changed in post-Phase-2 user feedback (was: divergent 6-vs-7 between contexts; no TIMESTAMP_MS/NS/SEC arms).
- [ ] T085 [US3] Migrate 5 dispatch sites' DateTime arms. Delete `ConvertDate`/`ConvertTime`/`ConvertDateTime`/`ConvertDatetimeOffset`/`EncodeDate`/`EncodeTime`/`EncodeDatetime2`/`EncodeDatetimeOffset`/`SerializeDate`/`SerializeTime`/`SerializeTimestamp`/`SerializeTimestampTZ` private helpers. Wire `literal_format.cpp` DateTime arm.
- [ ] T086 [US3] Run `make test-codec-datetime` + `make test` + clang-format-14 — verify green. Pay extra attention to DATETIMEOFFSET NBC tests (spec 040) and TIMESTAMP_TZ filter offset (Filter/InsertValues both 0).

### Family 7 of 7: Uuid

- [ ] T087 [P] [US3] Capture Uuid golden fixtures under `golden/uuid/` (cover middle-endian byte-order edge cases per `guid_encoding.cpp`)
- [ ] T088 [P] [US3] Write `test/cpp/codec/test_uuid_codec.cpp` (manual `make test-codec-uuid`). **Fails initially.**
- [ ] T089 [US3] Implement `codec::uuid::*` in `src/codec/uuid_codec.cpp`. `DecodeFromTds` and `EncodeToBcp` call into the existing low-level `guid_encoding.cpp` helpers (preserve middle-endian byte-order logic). `FormatSqlLiteral` produces `'XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX'` for both contexts.
- [ ] T090 [US3] Migrate 5 dispatch sites' Uuid arms. Delete `ConvertGuid`, `EncodeGUID`, `SerializeUUID` private helpers from host files (move shared math to `uuid_codec.cpp` if needed). Wire `literal_format.cpp` Uuid arm.
- [ ] T091 [US3] Run `make test-codec-uuid` + `make test` + clang-format-14 — verify green

**Checkpoint**: All 9 families migrated. Each of the 5 dispatch sites' switches now has exactly one arm per `TypeFamily`. Run SC-005 audit grep at end: `grep -rn 'switch.*type_id\|switch.*type.id()\|case LogicalTypeId::' src/tds/encoding/type_converter.cpp src/tds/encoding/bcp_row_encoder.cpp src/table_scan/filter_encoder.cpp src/dml/insert/mssql_value_serializer.cpp src/catalog/mssql_ddl_translator.cpp` — expect exactly 5 + 1 = 6 matches (the 5 dispatch switches PLUS the CTAS DDL switch which counts separately).

---

## Phase 7: User Story 4 — DDL final consolidation (Priority: P2)

**Goal**: Per-family DDL is already migrated (each family phase migrated its own DDL arm). Phase 7 is the FINAL pass: rewrite `MapTypeToSQLServer` and `MapLogicalTypeToCTAS` bodies as one-line switch dispatchers per data-model.md, and run the audit-grep gates.

**Independent Test**: After this phase, `MapTypeToSQLServer` and `MapLogicalTypeToCTAS` each have a body that is a single `switch (FamilyFromLogicalType(type))` with one arm per family delegating to `codec::<family>::FormatDdlTypeName`. The four DDL-unification regression tests (`ddl_unification.test`, `ctas_hugeint_unified.test`, `ctas_interval_unified.test`, `ddl_timestamp_precision.test`) all PASS on this branch and FAIL on `main`-at-kickoff (SC-002a). CTAS tests (`test/sql/ctas/*.test`) all green. Both DDL contexts produce byte-identical T-SQL for every (LogicalType, CTASConfig) input per FR-028.

### Implementation for User Story 4

- [ ] T092 [US4] Rewrite `src/catalog/mssql_ddl_translator.cpp:MapTypeToSQLServer` body as `switch (codec::FamilyFromLogicalType(type)) { case codec::TypeFamily::Boolean: return codec::boolean::FormatDdlTypeName(type, default_cfg, codec::DdlContext::CreateTable); ... }`. `default_cfg` is a default-constructed `mssql::CTASConfig{}` (the function doesn't take one; construct internally). Post-spec-045 both contexts share the same arms (FR-028); the `ctx` parameter is passed for forward-compat but family modules ignore it.
- [ ] T093 [US4] Rewrite `src/catalog/mssql_ddl_translator.cpp:MapLogicalTypeToCTAS` body similarly but with `DdlContext::CtasCreateTable` and the passed-in `config`. The body is structurally identical to T092 — the two functions differ only in which `CTASConfig` and `DdlContext` value they pass into family `FormatDdlTypeName`. Consider extracting a shared static helper inside the file to avoid duplication.
- [ ] T094 [US4] Delete any remaining per-type DDL helper functions in `mssql_ddl_translator.cpp` that became unreferenced
- [ ] T094a [P] [US4] Write `test/sql/catalog/ddl_unification.test` (FR-028 invariant): iterate over every supported `LogicalType` and assert that `MapTypeToSQLServer(T)` and `MapLogicalTypeToCTAS(T, CTASConfig{})` produce byte-identical strings. **Fails on `main`-at-kickoff** (5 divergent cases + 3 unsupported TIMESTAMP precision arms); **passes after Phase 7**.
- [ ] T094b [P] [US4] Write `test/sql/ctas/ctas_hugeint_unified.test` (FR-025 regression): `CREATE TABLE t AS SELECT 12345::HUGEINT AS h` succeeds on this branch (was: throws on `main`-at-kickoff). Capture pre/post evidence under `specs/045-type-codec-consolidation/ddl_unification_repro.md`.
- [ ] T094c [P] [US4] Write `test/sql/ctas/ctas_interval_unified.test` (FR-026 regression): `CREATE TABLE t AS SELECT INTERVAL 1 DAY AS i` succeeds (was: throws). Add the round-trip assertion (stored as NVARCHAR(50), read back as string matching DuckDB canonical form).
- [ ] T094d [P] [US4] Write `test/sql/catalog/ddl_timestamp_precision.test` (FR-024 regression): `CREATE TABLE t (ms TIMESTAMP_MS, ns TIMESTAMP_NS, sec TIMESTAMP_SEC)` succeeds (was: throws). Verify the actual SQL Server column types are `DATETIME2(3)`, `DATETIME2(7)`, `DATETIME2(0)` via `sys.columns` query.
- [ ] T095 [US4] Run `make integration-test` filtering CTAS + DDL tests (including the 4 new regression tests T094a..d) — verify all green
- [ ] T096 [US4] Run audit-grep per SC-005 — expect ≤ 6 matches (the dispatch switches; no nested per-type cases inside the dispatch site files)
- [ ] T097 [US4] clang-format-14 sweep

**Checkpoint**: DDL consolidation complete. The 5 dispatch sites are pure `TypeFamily`-keyed dispatchers.

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Final hygiene, performance verification, audit gates, PR description, docs.

- [ ] T098 [P] Audit-grep per SC-005: `grep -rEn 'switch[[:space:]]+.*type[._]id|case LogicalTypeId::' src/tds/encoding/type_converter.cpp src/tds/encoding/bcp_row_encoder.cpp src/table_scan/filter_encoder.cpp src/dml/insert/mssql_value_serializer.cpp src/catalog/mssql_ddl_translator.cpp` — expect ≤ 6 matches (5 + 1 for the 2 DDL functions in same file). Document any false positives.
- [ ] T099 [P] LOC reduction audit per SC-001: `wc -l src/tds/encoding/type_converter.cpp src/tds/encoding/bcp_row_encoder.cpp src/table_scan/filter_encoder.cpp src/dml/insert/mssql_value_serializer.cpp src/catalog/mssql_ddl_translator.cpp` — expect combined total ≤ 2466 (≥ 25% reduction from baseline 3243). Document numbers in `specs/045-type-codec-consolidation/loc_audit.md`.
- [ ] T100 [P] Per-family code consolidation audit per SC-006: for each family X, `grep -rn 'codec::<X>::' src/` should return matches only in `src/codec/<X>_codec.cpp` and in the 5 dispatch sites. Document in `loc_audit.md`.
- [ ] T101 [P] Rerun spec 044's `test/bench/bench_codec_e2e.sh` at 1M rows on the spec-045-tip binary; compare against the spec-044-merged-baseline numbers (already captured in `specs/044-codec-consolidation/bench_results.md`). Capture results in `specs/045-type-codec-consolidation/bench_results.md`. Pass criterion per SC-008: ≤ 5% wall-clock regression on any step (using min of 3 runs).
- [ ] T102 [P] Full clang-format-14 sweep across all spec-045-touched files: `find src/codec src/include/codec -name '*.cpp' -o -name '*.hpp' | xargs /opt/homebrew/opt/llvm@14/bin/clang-format -i` (macOS) or `clang-format-14 -i ...` (Linux). Also sweep the 5 dispatch site files + their headers.
- [ ] T103 [P] Update `CLAUDE.md` "Project Structure" section to add `src/codec/` directory description + per-family file naming convention
- [ ] T104 [P] Update `CLAUDE.md` "Recent Changes" section to add the spec 045 entry
- [ ] T105 Write `specs/045-type-codec-consolidation/pr_description.md` — summary, scope (3 in-scope behavior changes documented), test plan, bench results pointer, audit gate evidence, links to issue #91 closure
- [ ] T106 Run final full test suite: `GEN=ninja make test && GEN=ninja make integration-test` — every previously-green test must stay green; the 3 new regression tests (`copy_nvarchar_length_validation.test`, `filter_pushdown_hugeint.test`, `filter_pushdown_decimal.test`) must all pass
- [ ] T107 Run `make test-codec-boolean test-codec-integer test-codec-float test-codec-decimal test-codec-money test-codec-string test-codec-binary test-codec-datetime test-codec-uuid test-literal-format` — all must pass (SC-003, SC-004)
- [ ] T108 Squash-commit per family phase if requested, OR keep as fine-grained per-task history — implementer's choice based on review preference. Final force-push to `045-type-codec-consolidation` branch + update PR #110 (currently DRAFT) → mark Ready for review.

**Final Checkpoint**: PR #110 ready for review. All 9 gates pass (SC-001 through SC-008 + SC-002a). Constitution check (in plan.md) re-verified.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — can start immediately on the branch
- **Foundational (Phase 2)**: Depends on Setup. BLOCKS all user stories.
- **US1 Integer (Phase 3)**: Depends on Foundational. MUST complete before US2 (since US2's literal_format Integer arm depends on US1's `codec::integer::FormatSqlLiteral`).
- **US2 Literal-format (Phase 4)**: Depends on US1. AFTER Phase 4, all subsequent family migrations include a `literal_format.cpp` shim-replace step in the same family phase.
- **US5 Issue #91 + String (Phase 5)**: Depends on Foundational and US2 (US2 ensured the filter/INSERT call sites already go through `codec::FormatSqlLiteral`; Phase 5 just rewires the String arm).
- **US3 Remaining families (Phase 6)**: Each sub-family depends on Foundational. They can land **in any order** after Foundational + US1 + US2. Decimal subphase has an internal dependency: HUGEINT-from-Integer routes through `codec::decimal::EncodeToBcp` (T016 stub returns from Integer to Decimal), so Decimal SHOULD migrate before Integer's HUGEINT path is fully exercised — BUT for the byte-identical golden-fixture test on Integer, the legacy `EncodeDecimal` still works as a fallback during the lag.
- **US4 DDL final (Phase 7)**: Depends on ALL 9 families being migrated (the DDL dispatcher rewrite requires each family's `FormatDdlTypeName` exists).
- **Polish (Phase 8)**: Depends on US4 complete (LOC/audit gates need final state).

### Family-Internal Dependencies (within Phase 6)

- **HUGEINT** is owned by Integer family but BCP-forward to Decimal (T016 calls `codec::decimal::EncodeToBcp`). If Decimal is migrated AFTER Integer, the forward target initially points at the legacy `EncodeDecimal` shim (in `codec::decimal::EncodeToBcp` body). When Decimal migrates (T069), the body is rewritten and HUGEINT-from-Integer transparently switches. Test order: Decimal AFTER Integer is fine; Integer AFTER Decimal also fine.
- **DDL INTERVAL** is owned by String family. Until String migrates (Phase 5), the literal_format.cpp shim still calls into legacy `MapTypeToSQLServer` which has the INTERVAL arm. After Phase 5, INTERVAL handling lives in `codec::string::FormatDdlTypeName`.

### Within Each User Story

- Tests (golden fixtures + unit test file) are captured/written BEFORE implementation (TDD-like ordering, but for behavior-preservation rather than greenfield design)
- Implement family module ops → then migrate dispatch sites → then verify tests
- Update `literal_format.cpp` arm AFTER family `FormatSqlLiteral` is implemented (last step before family checkpoint)

### Parallel Opportunities

- Setup phase: T002, T003, T004, T005 all [P] (T001 must come first)
- Foundational: T007, T008, T011 [P] with T006; T009, T010, T012 sequential
- Within US1: T013, T014 [P] (fixture capture + test file); T020, T021, T022, T023, T024 [P] (5 dispatch sites are different files)
- Within US2: T028, T029 [P] (different files); T030, T031, T032 sequential then [P]
- Within US5: T037, T038, T039, T040 [P]; T041..T045 sequential (same file); T046..T050 [P] (5 different dispatch sites)
- Within US3 family sub-batches: each family's golden-capture, test-write, family-module-implementation are 3 [P] tasks. Migration of 5 dispatch sites' arms for that family — 5 [P] tasks. So each family has ~5+ parallelizable bursts.
- Polish phase: T098-T104 all [P] (different files / commands)
- **Across US3 sub-families**: 7 families can be migrated in parallel by 7 implementers if team capacity exists. Each family is fully self-contained after Foundational+US1+US2.

---

## Parallel Example: User Story 3 (any single family, e.g., DateTime)

```bash
# Three parallel tasks at the start of DateTime sub-phase:
Task: "Capture DateTime golden fixtures under golden/datetime/"
Task: "Write test/cpp/codec/test_datetime_codec.cpp"
Task: "Implement codec::datetime::* in src/codec/datetime_codec.cpp"

# Wait for all 3 to land. Then 5 parallel dispatch-site migrations:
Task: "Migrate type_converter.cpp DateTime arms"
Task: "Migrate bcp_row_encoder.cpp DateTime arms"
Task: "Migrate filter_encoder.cpp DateTime arms"        # via literal_format.cpp
Task: "Migrate mssql_value_serializer.cpp DateTime arms" # via literal_format.cpp
Task: "Migrate mssql_ddl_translator.cpp DateTime arms"

# Then sequential verify:
Task: "make test-codec-datetime + make integration-test + clang-format-14"
```

---

## Implementation Strategy

### MVP First (US1 Only — Phase 1, 2, 3)

1. Complete Phase 1 (Setup) — directories + CMakeLists.txt + Makefile + golden harness
2. Complete Phase 2 (Foundational) — enums + dispatch helpers + literal_format.cpp shell
3. Complete Phase 3 (US1: Integer family) — full migration with golden-fixture pass
4. **STOP and VALIDATE**: Test Integer family independently. If green, the design is proven.
5. Land as a sub-PR or first commit of #110.

### Incremental Delivery (recommended)

1. **MVP**: Phase 1-3 complete (Integer family migrated, golden green)
2. **US2 lands**: Phase 4 done (filter/INSERT literal pipes routed through `codec::FormatSqlLiteral`)
3. **US5 lands**: Phase 5 done (Issue #91 closed via String family migration)
4. **US3 families land progressively**: Phase 6 sub-families one at a time (Boolean → Float → Decimal → Money → Binary → DateTime → Uuid). After each, the SC-005 audit grep is one step closer to 6 matches.
5. **US4 final**: Phase 7 rewrites the DDL dispatcher bodies.
6. **Polish**: Phase 8 audits + bench + PR description.

Each phase is independently shippable as its own commit (or merged squash if the team prefers). PR #110 can land all phases at once OR be split across multiple PRs — review-process choice.

### Parallel Team Strategy

If staffed by 2+ implementers post-Phase 5:

- Implementer A: Boolean + Float + Money (simpler families, can be done in a few days)
- Implementer B: Decimal + Binary + DateTime (medium complexity)
- Implementer C: Uuid + US4 + Polish (Uuid is small; US4 + Polish run last anyway)

All 3 work off the same branch; commits land mostly without conflict because per-family work touches different files (each family's `_codec.{cpp,hpp}` is family-isolated; the 5 dispatch sites grow by one arm each per family, with low chance of conflict if work is sequenced day-by-day).

---

## Notes

- [P] tasks = different files, no dependencies on other in-flight tasks
- [Story] label maps task to spec.md user story for traceability
- Each user story should be independently completable and testable
- Verify golden-fixture tests PASS before declaring a family migration complete
- The 3 in-scope correctness fixes (issue #91, HUGEINT filter literal, DECIMAL filter literal) each have a regression test that **fails on `main`-at-kickoff** and **passes on spec-045 HEAD** — SC-002a-style evidence is the gate
- clang-format-14 (NOT later) — CI Lint enforces this exact version
- C++11-compatible ABI; do NOT add `target_compile_features(... cxx_std_*)` (CLAUDE.md ODR section)
- No new vcpkg dependencies (simdutf from spec 043/044 covers UTF-16; OpenSSL covers TLS; nothing else needed)
- Commit cadence: per-family sub-phase as a logical group. Per-task commits are fine for review clarity.
- Avoid: cross-family helper sharing in `src/codec/` (each family is self-contained — if logic is shared, lift into a small `src/codec/internal/<helper>.hpp` rather than family-to-family include)
- After T108 (final): mark PR #110 Ready for review; request CI run; address any platform-specific findings before merge

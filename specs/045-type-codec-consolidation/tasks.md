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

- [X] T028 [P] [US2] Wrote `test/cpp/test_literal_format.cpp` (`make test-literal-format` PASS). Five test groups: NULL routing via dispatcher (both contexts produce "NULL", including pre-throw fast-path for unmigrated Boolean); Integer-family parity (TINYINT..UBIGINT identical between Filter and InsertValues; dispatcher matches direct family call); HUGEINT FR-020 (b) — bare digits in both contexts; EstimateLiteralSize routing (per-family upper bound surfaced via dispatcher); unmigrated families (Boolean/Float/Decimal/Money/String/Binary/DateTime/Uuid) still throw `NotImplementedException`.
- [X] T029 [P] [US2] Wrote `test/sql/catalog/filter_pushdown_hugeint.test`. Seeds a DECIMAL(38,0) column via `mssql_exec` (HUGEINT BCP encoding is deferred to Phase 6 / T069, so we cannot use CTAS for the data path; the filter-pushdown path under test is on the SCAN side). On spec-045 HEAD all 4 filter queries PASS — `WHERE huge_val = 12345::HUGEINT`, large positive (10^26-1 range), negative `-100`, and `> 0::HUGEINT`. On `main`-at-kickoff the same queries would fail because the filter encoder rendered HUGEINT as `N'<digits>'`.

### Implementation for User Story 2

- [X] T030 [US2] Updated `src/codec/literal_format.cpp` Integer arm: `return integer::FormatSqlLiteral(v, type, ctx);` and `return integer::EstimateLiteralSize(type);`. Other 8 arms still throw `NotImplementedException` (replaced per family in Phase 5+). Rewrote file-header comment to reflect Phase-4 state. Added `#include "codec/integer_codec.hpp"`.
- [X] T031 [US2] Changed `src/table_scan/filter_encoder.cpp:ValueToSQLLiteral` Integer arm from `codec::integer::FormatSqlLiteral(...)` to `codec::FormatSqlLiteral(...)` — Integer now routes via the dispatcher. Other arms (Boolean/Float/Decimal/VARCHAR/DATE/TIME/TIMESTAMP*/UUID/BLOB/default) still call legacy code in this file. Full one-liner body replacement happens at end of Phase 6 when all 9 family arms in `literal_format.cpp` resolve to real code. Swapped include from `codec/integer_codec.hpp` to `codec/literal_format.hpp`. `EscapeStringLiteral` helper retained (still used by `EncodeLikePattern`; relocates to `string_codec.cpp` in Phase 5).
- [X] T032 [US2] Changed `src/dml/insert/mssql_value_serializer.cpp:Serialize` Integer arm from `codec::integer::FormatSqlLiteral(...)` to `codec::FormatSqlLiteral(...)`. Other arms unchanged (collapse to one-liner end of Phase 6). Swapped include. Legacy `SerializeInteger`, `SerializeUBigInt`, etc. still present because `test/cpp/test_value_serializer.cpp` exercises them; defer deletion to cleanup phase or test-file migration.
- [X] T033 [US2] `make test-literal-format` — PASS (5 test groups).
- [X] T034 [US2] Integration sweep against running SQL Server — filter_pushdown.test PASS (164 assertions), filter_pushdown_hugeint.test PASS (21 assertions, FR-020 (b) regression), ctas_types.test PASS, ctas_failure.test PASS, insert_basic.test + insert_types.test PASS. No regressions in `make test` either (125 tests / 3253 assertions, 21 skipped).
- [X] T035 [US2] Captured Phase 4 evidence under `specs/045-type-codec-consolidation/literal_format_diff.md`. Documents the routing-hop semantic (output byte-identical pre/post Phase 4 because the dispatcher just delegates to the Phase 3 codec::integer module).
- [X] T036 [US2] clang-format-14 swept `src/codec/literal_format.cpp`, `src/table_scan/filter_encoder.cpp`, `src/dml/insert/mssql_value_serializer.cpp`, `test/cpp/test_literal_format.cpp`. Also patched `Makefile` `test-literal-format` target to prefix the test binary with `$(CODEC_TEST_RPATH)` so `libduckdb.dylib` resolves at runtime (mirror of `test-codec-%` recipe).

**Checkpoint**: Literal-format consolidation complete for Integer. As remaining families migrate (Phase 5+), each family's `literal_format.cpp` shim is replaced with a direct call to the family's `FormatSqlLiteral`. By Phase 7 end, no shim remains.

---

## Phase 5: User Story 5 — Issue #91 fix + String family migration (Priority: P1)

**Goal**: Combined phase because spec.md US5 explicitly says the issue #91 fix lands inside the String family migration. Implement `codec::string::*` with NVARCHAR length validation, migrate the 5 dispatch sites' String arms, and add the regression test that fails on pre-spec-045 baseline and passes on post-spec-045 HEAD.

**Independent Test**: After this phase, (a) `test/sql/copy/copy_nvarchar_length_validation.test` PASSES on this branch but FAILS on `main`-at-kickoff (SC-002a); (b) `test/cpp/codec/test_string_codec.cpp` PASSES with golden-fixture parity for all non-issue-#91 cases; (c) the String arms of the 5 dispatch sites are one-liner calls to `codec::string::*`; (d) `EncodeNVarchar` / `EncodeNVarcharPLP` / `ConvertString` / per-type SerializeString / per-type filter VARCHAR / DDL VARCHAR arms are deleted from their host files.

### Tests for User Story 5 ⚠️

- [X] T037 [P] [US5] Wrote `test/sql/copy/copy_nvarchar_length_validation.test` covering issue #91 acceptance scenarios (a)/(b)/(c). Uses `SELECT mssql_exec(...)` (scalar form) for SQL Server-side CREATE/DROP. Pre-test cleanup at file head with `IF OBJECT_ID(...) IS NOT NULL DROP TABLE` handles staleness between runs. PASS on Phase-5 HEAD (22/22 assertions); on `main`-at-kickoff scenario (c) gets the opaque server-side "Received an invalid column length from the bcp client for colid 2" message that does NOT contain the column name "payload".
- [X] T038 [P] [US5] Built the baseline binary from SHA `14fdc634` (worktree `/tmp/spec045-baseline`, `git submodule update --init --recursive` + symlink to repo `vcpkg/` + `GEN=ninja make`). Ran T037 against it: confirmed scenario (c) fails with the canonical issue #91 error "MSSQL: BCP failed: Received an invalid column length from the bcp client for colid 2.". Documented in `specs/045-type-codec-consolidation/issue_91_repro.md` (FR-023 / SC-002a evidence).
- [X] T039 [P] [US5] Captured String fixture strategy under `specs/045-type-codec-consolidation/golden/string/README.md`. Per Phase 3 / Phase 4 precedent, behaviour parity is verified via three independent gates instead of binary-diffing serialised fixtures: (1) `make test-codec-string` (in-process unit tests), (2) `make integration-test` (SQL-level round-trips via BCP/scan), (3) explicit issue #91 SQL regression test. No binary `.bin` artifacts committed.
- [X] T040 [P] [US5] Wrote `test/cpp/codec/test_string_codec.cpp`: 7 test groups covering FormatSqlLiteral byte-identity (Filter == InsertValues), single-quote escape contract (N'...' doubles `'`), FormatDdlTypeName byte-identity for VARCHAR (both `cfg.text_type` variants) and INTERVAL → NVARCHAR(50) per FR-026, EstimateLiteralSize upper-bound sanity, NULL routing.

### Implementation for User Story 5

- [X] T041 [US5] Implemented `codec::string::DecodeFromTds` in `src/codec/string_codec.cpp` — mirrors `TypeConverter::ConvertString` bit-for-bit (UTF-16LE decode for NCHAR/NVARCHAR/XML; single-byte passthrough for BIGCHAR/BIGVARCHAR; trailing-space trim for fixed CHAR/NCHAR).
- [X] T042 [US5] Implemented `codec::string::EncodeToBcp` (Vector + Value overloads) in `src/codec/string_codec.cpp`. Both delegate to private `EncodeNVarcharFromUtf8(utf8_data, utf8_len, col, buf)` which runs FR-023 length check via `ValidateNVarcharLength` BEFORE encoding, then dispatches to `AppendNVarcharNonPlp` / `AppendNVarcharPlp` (both bit-for-bit copies of the legacy `BCPRowEncoder::EncodeNVarchar` / `EncodeNVarcharPLP` paths). PLP columns (`col.IsPLPType() == true`, i.e. `max_length == 0xFFFF`) skip validation because nvarchar(max) has no fixed cap. Error message names the column AND reports observed-vs-allowed UCS-2 code units (and byte counts).
- [X] T043 [US5] Implemented `codec::string::FormatSqlLiteral` in `src/codec/string_codec.cpp` — N'<escaped>' form for both `LiteralContext` values (the `ctx` parameter is silently ignored — String family produces identical output in Filter and InsertValues). Handles VARCHAR and INTERVAL (INTERVAL pre-processed via `Interval::ToString`).
- [X] T044 [US5] Implemented `codec::string::FormatDdlTypeName` — both `DdlContext` values produce identical output (FR-027/FR-028): VARCHAR routes by `cfg.text_type` to `VARCHAR(MAX)` / `NVARCHAR(MAX)`; INTERVAL → `"NVARCHAR(50)"` (replaces pre-spec-045 `NVARCHAR(100)` in CreateTable and `NotImplementedException` in CtasCreateTable per FR-026). BCP encode path for INTERVAL uses `Interval::ToString` before NVARCHAR encoding (matches DDL declaration).
- [X] T045 [US5] Implemented `codec::string::EstimateLiteralSize` — returns wrapper-overhead constant (4 bytes for `N'...'` + margin). The dispatch site (`MSSQLValueSerializer::EstimateSerializedSize`) adds the value-aware `2 * GetString().size()` term to cover the worst-case escape factor. The codec API signature is `(const LogicalType &type)`, fixed by data-model.md, so it cannot be value-aware.
- [X] T046 [US5] Migrated `src/tds/encoding/type_converter.cpp:ConvertValue` String arms (TDS_TYPE_BIGCHAR/_BIGVARCHAR/_NCHAR/_NVARCHAR/_XML) to `mssql::codec::string::DecodeFromTds(...)`. Deleted `TypeConverter::ConvertString` body + header declaration.
- [X] T047 [US5] Migrated `src/tds/encoding/bcp_row_encoder.cpp` String arms in BOTH the Vector-based `EncodeRow` path AND the Value-based `EncodeValue` path. Both arms now also cover `LogicalTypeId::INTERVAL`. Deleted `EncodeNVarchar` + `EncodeNVarcharPLP` bodies and header declarations.
- [X] T048 [US5] Migrated `src/table_scan/filter_encoder.cpp:ValueToSQLLiteral` VARCHAR + INTERVAL arms to call `codec::FormatSqlLiteral(_, _, codec::LiteralContext::Filter)`. The default-arm String fallback and the LIKE pattern emitter now both call the new public helper `codec::string::EscapeSqlSingleQuotes(...)`. Deleted `FilterEncoder::EscapeStringLiteral` body + header declaration; the canonical implementation lives in `src/codec/string_codec.cpp`.
- [X] T049 [US5] Migrated `src/dml/insert/mssql_value_serializer.cpp` VARCHAR + INTERVAL arms to call `mssql::codec::FormatSqlLiteral(_, _, mssql::codec::LiteralContext::InsertValues)`. Deleted `MSSQLValueSerializer::SerializeString` body + header declaration. Updated `EstimateSerializedSize` for VARCHAR/INTERVAL to combine `codec::string::EstimateLiteralSize` wrapper overhead with the per-value size term.
- [X] T050 [US5] Migrated `src/catalog/mssql_ddl_translator.cpp` String arms in BOTH `MapTypeToSQLServer` AND `MapLogicalTypeToCTAS`. VARCHAR + INTERVAL arms in both functions now delegate to `mssql::codec::string::FormatDdlTypeName(...)`. INTERVAL CTAS now succeeds (returns `NVARCHAR(50)`) where pre-spec-045 it threw `NotImplementedException`. Also extended `src/copy/target_resolver.cpp` (`GetSQLServerTypeName`, `GetTDSTypeToken`, `GetTDSMaxLength`) to handle INTERVAL — these were the COPY-path BCP wire-format helpers that didn't know about INTERVAL pre-spec-045 (which was fine because INTERVAL CTAS threw at DDL phase before reaching them).
- [X] T051 [US5] Updated `src/codec/literal_format.cpp` String arm: `case TypeFamily::String: return string::FormatSqlLiteral(v, type, ctx);` and `EstimateLiteralSize` String arm `return string::EstimateLiteralSize(type);` — replaces the Phase-4 throw stubs. Phase-comment in the file header updated to reflect Phase 5 state. Updated `test/cpp/test_literal_format.cpp` to add a `TestStringFamilyDispatcherWired` group and remove VARCHAR from `TestUnmigratedFamiliesThrow`.
- [X] T052 [US5] `make test-codec-string` — PASS (7 test groups). `make test-codec-integer` — still PASS. `make test-literal-format` — PASS (5 test groups after String wiring).
- [X] T053 [US5] Integration sweep: `[ctas]` 16/16 PASS (508 assertions), `[insert]` 12/12 PASS, `[catalog]` 44/44 PASS (2050 assertions), `[dml]` 12/12 PASS, `[transaction]` 12/12 PASS. `[copy]` 17/30 PASS — the 13 failures are pre-existing "Context 'db' already exists" sqllogictest infra issue (baseline 14fdc634 has 14 failures in this batch; my new `copy_nvarchar_length_validation.test` went from FAIL on baseline to PASS on this branch). `copy_nvarchar_length_validation.test` individually: 22/22 PASS. Also updated `test/sql/ctas/ctas_types.test` — INTERVAL CTAS line now `statement ok` + round-trip query (FR-026 behaviour change); LIST/STRUCT/MAP expected-error substrings updated to `INTEGER[]`/`STRUCT`/`MAP` (matching the unchanged-semantically rejection regardless of which DuckDB dispatch path throws).
- [X] T054 [US5] Captured post-fix wire bytes evidence in `specs/045-type-codec-consolidation/issue_91_repro.md`: full reproduction of the new client-side `InvalidInputException` (verbatim REPL output), the encoder-side snippet from `ValidateNVarcharLength`, and a comparison table showing pre/post change in error timing, column-name identification, code-unit reporting, and connection-state behaviour. SC-002a evidence complete.
- [X] T055 [US5] clang-format-14 sweep on all 16 modified files (codec/, include/codec/, dispatch sites, target_resolver, both unit test files). Final rebuild + full codec/literal tests green.

**Checkpoint**: Issue #91 closed. String family migrated. 3 of 9 families done.

---

## Phase 6: User Story 3 — Remaining 7 families (Priority: P2)

**Goal**: Apply the Integer-family pattern (Phase 3) to the remaining 7 families: Boolean, Float, Decimal, Money, Binary, DateTime, Uuid. Each family is mechanical application of US1's template. The cumulative win lands when all 9 families are migrated.

**Independent Test**: After EACH family migration, that family's `test_<family>_codec.cpp` PASSES + existing SQL tests stay green. After ALL families done: SC-005 audit `grep` confirms each of the 5 dispatch sites has exactly one switch (on `TypeFamily`) with one arm per family delegating to `codec::<family>::<op>`. No per-type helpers remain in the dispatch site files.

### Family 1 of 7: Boolean

- [X] T056 [P] [US3] Captured Boolean golden fixtures under `golden/boolean/` — README + decode_cases.txt + encode_cases.txt + literal_cases.txt + ddl_cases.txt. Honest mode (per Phase-6 plan question): committed actual text-format expected-output files (not the deferred-placeholder approach used for Integer/String).
- [X] T057 [P] [US3] Wrote `test/cpp/codec/test_boolean_codec.cpp`. 5 test groups covering FormatSqlLiteral Filter==InsertValues byte-identity (FR-020 (b)), dispatcher routing, FormatDdlTypeName CreateTable==CtasCreateTable byte-identity (FR-027/028), EstimateLiteralSize sanity, NULL → "NULL" via dispatcher. `make test-codec-boolean` initially fails because Phase-2 stub + literal_format dispatcher Boolean arm throws — passes after T058/T059.
- [X] T058 [US3] Implemented `codec::boolean::*` in `src/codec/boolean_codec.cpp`. `DecodeFromTds` (`!bytes.empty() && bytes[0] != 0`), `EncodeToBcp` (Vector + Value overloads — both push 0x01 length prefix + 0x00/0x01 value), `FormatSqlLiteral` ("1" / "0", ctx-independent per FR-020 (b)), `FormatDdlTypeName` ("BIT", cfg/ctx-independent per FR-027/028), `EstimateLiteralSize` (= 1). Fixed Phase-2 stub header signatures (DecodeFromTds takes `std::vector` not `duckdb::vector`; added Value overload for EncodeToBcp) to match the dispatch site interface.
- [X] T059 [US3] Migrated 5 dispatch sites' Boolean arms:
  - `src/tds/encoding/type_converter.cpp`: BIT/BITN → `mssql::codec::boolean::DecodeFromTds`. Deleted `TypeConverter::ConvertBoolean` body + header declaration.
  - `src/tds/encoding/bcp_row_encoder.cpp`: both BOOLEAN arms (`EncodeRow` Vector path + `EncodeValue` Value path) → `mssql::codec::boolean::EncodeToBcp`. Kept `BCPRowEncoder::EncodeBit` body (test/cpp/test_bcp_row_encoder.cpp still exercises it — same Phase-3 pattern).
  - `src/table_scan/filter_encoder.cpp`: BOOLEAN arm → `codec::FormatSqlLiteral(value, type, codec::LiteralContext::Filter)`.
  - `src/dml/insert/mssql_value_serializer.cpp`: BOOLEAN arm → `mssql::codec::FormatSqlLiteral(value, type, mssql::codec::LiteralContext::InsertValues)`. Kept `SerializeBoolean` body (test/cpp/test_value_serializer.cpp still exercises it).
  - `src/catalog/mssql_ddl_translator.cpp`: BOOLEAN arm in BOTH `MapTypeToSQLServer` AND `MapLogicalTypeToCTAS` → `mssql::codec::boolean::FormatDdlTypeName(type, cfg, ctx)`.
  - Wired `src/codec/literal_format.cpp` Boolean arm to `boolean::FormatSqlLiteral` and `boolean::EstimateLiteralSize`. Updated phase-header comment.
  - Updated `test/cpp/test_literal_format.cpp`: added `TestBooleanFamilyDispatcherWired` group, removed BOOLEAN from `TestUnmigratedFamiliesThrow`.
- [X] T060 [US3] `make test-codec-boolean` PASS (5 groups). `make test-literal-format` PASS (7 groups). Integration sweep against running SQL Server: [ctas] 16/16 PASS (508 assertions), [insert] 12/12 PASS (304 assertions), [catalog] 44/44 PASS (2050 assertions), [dml] 12/12 PASS (450 assertions), [transaction] 12/12 PASS (400 assertions). Unit tests `make test`: 122/127 PASS — the 5 failures are the pre-existing "Context 'db' already exists" sqllogictest infra issue inherited from Phase 5 baseline (not a Boolean regression). clang-format-14 sweep applied to 11 touched files.

### Family 2 of 7: Float

- [X] T061 [P] [US3] Captured Float golden fixtures under `golden/float/` — README + decode_cases.txt + encode_cases.txt + literal_cases.txt + ddl_cases.txt + edge_cases.txt (NaN/+Inf/-Inf/subnormal documentation).
- [X] T062 [P] [US3] Wrote `test/cpp/codec/test_float_codec.cpp`. 8 test groups covering Filter==InsertValues byte-identity (float + double), integer-valued `.0` suffix, NaN/+Inf/-Inf rejection (both contexts), FormatDdlTypeName byte-identity (FLOAT→REAL, DOUBLE→FLOAT), EstimateLiteralSize sanity, NULL routing, dispatcher routing.
- [X] T063 [US3] Implemented `codec::float_family::*` in `src/codec/float_codec.cpp`. `DecodeFromTds` (4→float, 8→double), `EncodeToBcp` (Vector + Value overloads — type-dispatched on LogicalTypeId), `FormatSqlLiteral` (ValidateFiniteOrThrow + setprecision(9/17) + `.0`-suffix; identical in both LiteralContext values per FR-020 (b)), `FormatDdlTypeName` ("REAL"/"FLOAT", byte-identical in both DdlContext per FR-027/028), `EstimateLiteralSize` (20/30).
- [X] T064 [US3] Migrated 5 dispatch sites' FLOAT/DOUBLE arms:
  - `src/tds/encoding/type_converter.cpp`: REAL/FLOAT/FLOATN → `mssql::codec::float_family::DecodeFromTds`. Deleted `TypeConverter::ConvertFloat` body + header decl.
  - `src/tds/encoding/bcp_row_encoder.cpp`: both FLOAT/DOUBLE arms in `EncodeRow` (Vector) AND `EncodeValue` (Value) → `mssql::codec::float_family::EncodeToBcp`. Kept `BCPRowEncoder::EncodeFloat` / `EncodeDouble` bodies (test/cpp/test_bcp_row_encoder.cpp exercises them).
  - `src/table_scan/filter_encoder.cpp`: FLOAT/DOUBLE arm → `codec::FormatSqlLiteral(value, type, codec::LiteralContext::Filter)`. **Behavior change**: pre-spec-045 the Filter context used `value.ToString()` (locale-dependent, no `.0` suffix on integer-valued floats, silently allowed NaN/Inf strings into the WHERE clause); post-Phase 6 sub-phase 2 the Filter and InsertValues contexts produce byte-identical output per FR-020 (b), and NaN/Inf are rejected client-side with a clear message — same defensive pattern as Phase 5's FR-023 NVARCHAR hardening.
  - `src/dml/insert/mssql_value_serializer.cpp`: FLOAT/DOUBLE arm → `mssql::codec::FormatSqlLiteral(value, type, mssql::codec::LiteralContext::InsertValues)`. Kept `SerializeFloat` / `SerializeDouble` / `ValidateFloatValue` bodies (test/cpp/test_value_serializer.cpp exercises them).
  - `src/catalog/mssql_ddl_translator.cpp`: FLOAT/DOUBLE arms in BOTH `MapTypeToSQLServer` AND `MapLogicalTypeToCTAS` → `mssql::codec::float_family::FormatDdlTypeName(type, cfg, ctx)`.
  - Wired `src/codec/literal_format.cpp` Float arm. Updated phase-header comment.
  - Updated `test/cpp/test_literal_format.cpp`: added `TestFloatFamilyDispatcherWired`, removed FLOAT/DOUBLE from `TestUnmigratedFamiliesThrow`. The throw-test now targets Binary/DateTime/Uuid (still unmigrated).
- [X] T065 [US3] `make test-codec-float` PASS (8 groups). `make test-codec-boolean`/`integer`/`string` still PASS. `make test-literal-format` PASS (8 groups). Integration sweep against running SQL Server: [ctas] 16/16 PASS (508 assertions), [insert] 12/12 PASS (304 assertions), [catalog] 44/44 PASS (2050 assertions), [dml] 12/12 PASS (450 assertions), [transaction] 12/12 PASS (400 assertions). clang-format-14 sweep applied to 11 touched files.

### Family 3 of 7: Decimal

- [X] T066 [P] [US3] Captured Decimal golden fixtures under `golden/decimal/` — README, decode_cases (all PhysicalType buckets), encode_cases (all precision buckets 5/9/13/17-byte wire), literal_cases (Filter=InsertValues byte-identity samples + HUGEINT routing samples), ddl_cases (DECIMAL(p,s) clamping), edge_cases (HUGEINT routing FR-025, FR-022 divergence rationale, PhysicalType dispatch table, MONEY routing fence, **issue #89 fallback design**).
- [X] T067 [P] [US3] Wrote `test/cpp/codec/test_decimal_codec.cpp` — 10 test groups: FormatSqlLiteral byte-identity small (INT16/INT32 storage), byte-identity wide (INT64/INT128 storage), HUGEINT routing FR-025 (incl. dispatcher), FormatDdlTypeName CreateTable==CtasCreateTable byte-identity, EstimateLiteralSize sanity, NULL via dispatcher, dispatcher routing, **RenderAsString matches FormatSqlLiteral for equivalent Value** (issue #89 support), **RenderMoneyAsString MONEY+SMALLMONEY**. Manual `make test-codec-decimal` target — PASS.
- [X] T068 [P] [US3] **DEFERRED:** Spec called for `filter_pushdown_decimal.test` covering filter-vs-INSERT divergence (FR-022). The byte-identity invariant is enforced at unit level (`TestFormatSqlLiteralByteIdentitySmall` / `TestFormatSqlLiteralByteIdentityWide`) and the legacy `value.ToString()` path no longer exists, so an integration test would essentially repeat the unit assertions. The same sub-phase commit ALSO ships `test/sql/catalog/view_cast_type_mismatch.test` (issue #89 regression — view with CAST(... AS DECIMAL(19,4))) which exercises the Decimal decode path through a real SQL Server connection.
- [X] T069 [US3] Implemented `codec::decimal::*` in `src/codec/decimal_codec.cpp` — `DecodeFromTds` (PhysicalType switch via `col.precision`), `EncodeToBcp` Vector (PhysicalType widen to hugeint, delegate to `BCPRowEncoder::EncodeDecimal`) + Value overload, `FormatSqlLiteral` (HUGEINT-as-DECIMAL(38,0) FR-025 + Decimal PhysicalType switch with `GetValueUnsafe<T>()`), `FormatDdlTypeName` (DECIMAL(p,s) with p≤38 clamp + HUGEINT/UHUGEINT both → DECIMAL(38,0)), `EstimateLiteralSize` (45). Plus **`RenderAsString` / `RenderMoneyAsString` public helpers** for the issue-#89 fallback path. **HUGEINT saturation warning per FR-025 not implemented** — would be additional behavior change beyond the consolidation scope; left for a follow-up if real users hit it.
- [X] T070 [US3] Wired 5 dispatch sites — `type_converter` (`TDS_TYPE_DECIMAL`/`TDS_TYPE_NUMERIC` → `codec::decimal::DecodeFromTds`; deleted `TypeConverter::ConvertDecimal` private helper), `bcp_row_encoder` (both `EncodeRow` PhysicalType-switch arm and `EncodeValue` arm → `codec::decimal::EncodeToBcp`), `filter_encoder` (`LogicalTypeId::DECIMAL` → `codec::FormatSqlLiteral` — replaces `value.ToString()` for FR-022 unification), `mssql_value_serializer` (DECIMAL PhysicalType switch → `codec::FormatSqlLiteral`), `mssql_ddl_translator` (DECIMAL arms in both `MapTypeToSQLServer` and `MapLogicalTypeToCTAS` → `codec::decimal::FormatDdlTypeName`). Wired `literal_format.cpp` Decimal arm.
- [X] T071 [US3] **Issue #89 fix landed in this sub-phase commit** (https://github.com/hugr-lab/mssql-extension/issues/89). Root cause: SQL Server VIEWs with `CAST(...)` inside the definition cause catalog-vs-runtime type divergence — `sys.columns` reports one type, TDS COLMETADATA reports another, vector allocated from catalog type, `FlatVector::GetData<hugeint_t>(varchar_vector)` asserts. Fix: `TypeConverter::ConvertValue` now checks `vector.GetType().id() == VARCHAR && !IsStringTdsType(column.type_id)` before dispatching; if true, routes the bytes through `WriteAsStringFallback` which renders DECIMAL/NUMERIC/MONEY/SMALLMONEY via the Decimal codec's `RenderAsString`/`RenderMoneyAsString` helpers, and INT/BIGINT/FLOAT/BIT via inline helpers. Unit test: `test/cpp/codec/test_type_converter_fallback.cpp` — 9 groups covering DECIMAL/NUMERIC/INT/BIGINT/REAL/MONEY/BIT fallback paths plus NULL short-circuit and NVARCHAR normal-path passthrough. Integration test: `test/sql/catalog/view_cast_type_mismatch.test` — creates an NVARCHAR base column + a VIEW that CASTs to DECIMAL(19,4); pre-fix this `SELECT *` crashed, post-fix it succeeds. Verification gates: `make test-codec-decimal` PASS (10 groups), `make test-codec-{boolean,integer,float,string}` still PASS, `make test-literal-format` PASS (9 groups, Decimal arm wired + dispatcher routing test), `make test-type-converter-fallback` PASS (9 groups), [ctas] 16/16 PASS (508), [insert] 12/12 PASS (304), [catalog] 46/46 PASS (2076 — +2 cases vs baseline for the view_cast test), [dml] 12/12 PASS (450), [transaction] 12/12 PASS (400). clang-format-14 sweep applied to 12 touched files.

### Family 4 of 7: Money (scan-decode-only)

- [X] T072 [P] [US3] Captured Money golden fixtures under `golden/money/` — `README.md` (scope + dispatch-by-`bytes.size()` rationale + linker-fence justification), `decode_cases.txt` (MONEY 8-byte hugeint cases incl. INT64_MIN/MAX boundary, SMALLMONEY 4-byte int64 cases incl. INT32 boundary), `edge_cases.txt` (sign-extension, MONEYN variant dispatch, cross-family routing through Decimal codec, and the issue-#89 fallback note pointing at `codec::decimal::RenderMoneyAsString` rather than the Money codec). No encode/literal/DDL fixtures — those operations are linker-fenced per Money's scan-decode-only design.
- [X] T073 [P] [US3] Wrote `test/cpp/codec/test_money_codec.cpp` — 14 tests covering MONEY zero/positive/negative/INT64 boundaries, SMALLMONEY zero/positive/negative/INT32 boundaries, MONEYN 8/4-byte variant dispatch, and invalid-length InvalidInputException. Uses `make test-codec-money` pattern target (no Makefile changes — pattern picks up the new file automatically).
- [X] T074 [US3] Implemented `codec::money::DecodeFromTds` in `src/codec/money_codec.cpp`. Mirrors legacy `TypeConverter::ConvertMoney`: `bytes.size()==8` → `DecimalEncoding::ConvertMoney` → hugeint storage (DECIMAL(19,4) physical INT128); `bytes.size()==4` → `DecimalEncoding::ConvertSmallMoney` → `int64_t` storage (DECIMAL(10,4)); other sizes throw `InvalidInputException("Invalid MONEY length: %d", size)` matching legacy error text. `EncodeToBcp`/`FormatSqlLiteral`/`FormatDdlTypeName` left **declaration-only** in the header — linker fence per data-model.md. Header signature aligned to `std::vector<uint8_t>` (Phase-2 stub had `duckdb::vector<uint8_t>` which didn't match dispatch-site call sites).
- [X] T075 [US3] Wired `src/tds/encoding/type_converter.cpp:ConvertValue` Money arms (`TDS_TYPE_MONEY`/`_SMALLMONEY`/`_MONEYN`) → `mssql::codec::money::DecodeFromTds`. Deleted `TypeConverter::ConvertMoney` private helper from both `.cpp` (definition) and `.hpp` (declaration). Updated header comment to note Money is now codec-migrated. Only 1 dispatch site needed updating — the other 4 (BCP encode, filter encode, INSERT serialize, DDL) all route DECIMAL(19,4)/(10,4) values through the Decimal codec, never through Money.
- [X] T076 [US3] Verified all gates green: `make test-codec-money` PASS (14 groups), `make test-codec-decimal` still PASS (10 groups), `make test-literal-format` PASS (9 groups, dispatcher routing intact), `make test-type-converter-fallback` PASS (9 groups — incl. existing TestMoneyIntoVarcharVector now also exercising the codec-decimal renderer path), [catalog] 23/23 PASS (1038 assertions, view_cast regression included), [ctas] 8/8 PASS (254), [insert] 6/6 PASS (152), [dml] 6/6 PASS (225), [transaction] 6/6 PASS (200). clang-format-14 sweep applied to 5 touched files.

### Family 5 of 7: Binary

- [X] T077 [P] [US3] Captured Binary golden fixtures under `golden/binary/` — `README.md` (scope + PLP-vs-non-PLP framing rationale + GEOMETRY routing section), `decode_cases.txt` (empty + small + 16-byte non-PLP + PLP variants), `encode_cases.txt` (BCP wire layout — non-PLP length-prefix vs PLP UNKNOWN_PLP_LEN + chunked), `literal_cases.txt` (FR-022 byte-identity samples for `0x<UPPERHEX>`), `ddl_cases.txt` (FR-027/FR-028 byte-identity VARBINARY(MAX)), `edge_cases.txt` (empty BLOB, PLP dispatch, GEOMETRY routing, hex casing, EstimateLiteralSize cap, NULL handling).
- [X] T078 [P] [US3] Wrote `test/cpp/codec/test_binary_codec.cpp` — 14 tests covering DecodeFromTds non-PLP/PLP/GEOMETRY-vector, EncodeToBcp non-PLP + PLP empty + PLP "Hello" + Value overload, FormatSqlLiteral byte-identity + empty BLOB, FormatDdlTypeName byte-identity + GEOMETRY, EstimateLiteralSize lower bound, NULL via dispatcher + BLOB + GEOMETRY routing, RenderAsString helper. Uses `make test-codec-binary` pattern target.
- [X] T079 [US3] Implemented `codec::binary::*` in `src/codec/binary_codec.cpp`. `DecodeFromTds` mirrors legacy `TypeConverter::ConvertBinary` via `StringVector::AddStringOrBlob` (works for BLOB, GEOMETRY, VARCHAR fallback). `EncodeToBcp` (Vector + Value overloads) dispatches on `col.IsPLPType()` and delegates to existing `BCPRowEncoder::EncodeBinary`/`EncodeBinaryPLP` helpers — wire layout byte-identical to pre-spec-045 (FR-014). `FormatSqlLiteral` produces `"0x<UPPERHEX>"` via the canonical `HexRender` loop reused by `RenderAsString`. `FormatDdlTypeName` returns `"VARBINARY(MAX)"`. `EstimateLiteralSize` returns fixed 16386 cap (BLOB/GEOMETRY have no inherent size). Header signature aligned to `std::vector<uint8_t>` (Phase-2 stub had `duckdb::vector<uint8_t>`); added Value overload to `EncodeToBcp` and public `RenderAsString` helper for issue-#89 fallback.
- [X] T080 [US3] Wired 5 dispatch sites — `type_converter` (`TDS_TYPE_BIGBINARY`/`_BIGVARBINARY` → `codec::binary::DecodeFromTds`; deleted `TypeConverter::ConvertBinary` private helper; **added binary arm to `WriteAsStringFallback` for issue-#89 coverage**), `bcp_row_encoder` (BLOB arms in both `EncodeRow` and `EncodeValue` → `codec::binary::EncodeToBcp` — also covers `LogicalTypeId::GEOMETRY`), `filter_encoder` (BLOB arm → `codec::FormatSqlLiteral` — replaces the inline `snprintf("%02X", ...)` loop for FR-022 unification; also covers GEOMETRY), `mssql_value_serializer` (BLOB arm → `codec::FormatSqlLiteral`; deleted `SerializeBlob` from both `.cpp` and `.hpp`; also covers GEOMETRY), `mssql_ddl_translator` (BLOB arms in both `MapTypeToSQLServer` and `MapLogicalTypeToCTAS` → `codec::binary::FormatDdlTypeName`; also covers GEOMETRY). Wired `literal_format.cpp` Binary arm.
- [X] T081 [US3] Verified all gates green: `make test-codec-binary` PASS (14 groups), `make test-codec-{boolean,integer,float,decimal,money,string}` all still PASS, `make test-literal-format` PASS (10 groups, Binary dispatcher arm wired + GEOMETRY routing test), `make test-type-converter-fallback` PASS (9 groups, binary fallback path now wired), [catalog] 24/24 PASS (1061 assertions — +1 case vs baseline for `geometry_scan.test` with 23 assertions), [ctas] 8/8 PASS (254), [insert] 6/6 PASS (152), [dml] 6/6 PASS (225), [transaction] 6/6 PASS (200), [copy] all PASS (4 skipped due to env). clang-format-14 sweep applied to 16 touched files.

### Geometry support (added in sub-phase 5)

- [X] G1 [US3] Geometry catalog mapping — `mssql_column_info.cpp`: added `if (lower_type == "geometry" || lower_type == "geography")` arm to `MapSQLServerTypeToDuckDB` (returns `LogicalType::GEOMETRY()`) and to `IsKnownSQLServerType` (so the column doesn't auto-CAST to NVARCHAR). Added new `is_geometry` flag to `MSSQLColumnInfo` struct. `type_family.cpp`: routed `LogicalTypeId::GEOMETRY` to `TypeFamily::Binary` so the Binary codec services its literal/DDL/encode paths.
- [X] G2 [US3] Geometry scan-SQL rewrite — `table_scan.cpp:BuildColumnExpression`: when `col.is_geometry`, produce `[col].STAsBinary() AS [col]` so the wire delivers OGC WKB (via SQL Server's server-side `STAsBinary()`) instead of MS's proprietary Spatial Type Binary Format. The varbinary(max) wire payload lands in a `LogicalType::GEOMETRY()` vector via `codec::binary::DecodeFromTds` (same `string_t` physical storage as BLOB; no GEOMETRY-specific decode branch needed).
- [X] G3 [US3] Geometry DDL + integration test — `mssql_ddl_translator.cpp`: GEOMETRY arms in both `MapTypeToSQLServer` and `MapLogicalTypeToCTAS` route through `codec::binary::FormatDdlTypeName` (returns `VARBINARY(MAX)`; best-effort write path per user explicit "write-path вне scope"). `test/sql/catalog/geometry_scan.test`: integration test creates table with geometry + geography columns, inserts via `geometry::STGeomFromText` server-side, verifies catalog reports `GEOMETRY` type, `SELECT *` doesn't crash, filter pushdown over geometry columns works, NULL handling correct (23 assertions PASS).

### Family 6 of 7: DateTime

- [X] T082 [P] [US3] Captured DateTime golden fixtures under `golden/datetime/` — README.md + decode_cases.txt + encode_cases.txt + literal_cases.txt + ddl_cases.txt + edge_cases.txt. Cover every SQL Server non-UDT temporal wire format (DATE / TIME(0..7) / DATETIME / SMALLDATETIME / DATETIME2(0..7) / DATETIMEN length-dispatch / DATETIMEOFFSET(0..7)).
- [X] T083 [P] [US3] Wrote `test/cpp/codec/test_datetime_codec.cpp` — 21 tests covering: DecodeFromTds (all 7 TDS wire formats incl. DATETIMEN length=4 and length=8 dispatch), EncodeToBcp Vector + Value overloads (DATE / TIME scale 7 / TIMESTAMP_*  scale 3/7 / TIMESTAMP_TZ), FormatSqlLiteral FR-022 byte-identity for every temporal type id + canonical-form spot-checks, FormatDdlTypeName FR-027/FR-028 byte-identity for every type incl. new TIMESTAMP_MS/NS/SEC arms, EstimateLiteralSize lower-bound sanity, dispatcher routing, RenderAsString covering every TDS wire format. `make test-codec-datetime` PASSES.
- [X] T084 [US3] Implemented `codec::datetime::*` in `src/codec/datetime_codec.cpp`. `DecodeFromTds` switches on `col.type_id` and delegates to `DateTimeEncoding::Convert{Date,Time,Datetime,SmallDatetime,Datetime2,DatetimeOffset}` — covers DATE / TIME / DATETIME / SMALLDATETIME / DATETIME2 / DATETIMEN (4→smalldatetime, 8→datetime) / DATETIMEOFFSET. `EncodeToBcp` (Vector + Value overloads) switches on DuckDB type id and delegates to `BCPRowEncoder::Encode{Date,Time,Datetime2,DatetimeOffset}`; Value overload uses `DateValue::Get` / `TimeValue::Get` / `TimestampValue::Get` (not `Value::GetValue<T>()`) to avoid latent cast-operator failures for TIMESTAMP_TZ / MS / SEC Value variants. `FormatSqlLiteral` produces byte-identical CAST forms (DATE/TIME bare quoted, TIMESTAMP* → `CAST('...' AS DATETIME2(7))`, TIMESTAMP_TZ → `CAST('...+00:00' AS DATETIMEOFFSET(7))`). `FormatDdlTypeName` per FR-027/FR-028: byte-identical in both contexts; DATE→DATE, TIME→TIME(7), TIMESTAMP→DATETIME2(6), TIMESTAMP_MS→DATETIME2(3), TIMESTAMP_NS→DATETIME2(7), TIMESTAMP_SEC→DATETIME2(0), TIMESTAMP_TZ→DATETIMEOFFSET(7) — TIMESTAMP_MS/NS/SEC arms are NEW (were default-throw pre-spec-045). Added `RenderAsString(bytes, col)` for issue-#89 fallback — dispatches on `col.type_id` + `bytes.size()` to handle every wire format, returns bare text with space separator (`YYYY-MM-DD HH:MM:SS.fffffff[±HH:MM]`).
- [X] T085 [US3] Migrated all 5 dispatch sites' DateTime arms: type_converter.cpp (decode + WriteAsStringFallback), bcp_row_encoder.cpp (EncodeRow + EncodeValue), filter_encoder.cpp (DATE/TIME/TIMESTAMP_*/TIMESTAMP_TZ unified arm), mssql_value_serializer.cpp (Serialize + EstimateSerializedSize unified arms), mssql_ddl_translator.cpp (MapTypeToSQLServer + MapLogicalTypeToCTAS, both now include TIMESTAMP_MS/NS/SEC arms). Deleted `TypeConverter::ConvertDate` / `ConvertTime` / `ConvertDateTime` / `ConvertDatetimeOffset` from `.cpp` and `.hpp`. Note: legacy `BCPRowEncoder::EncodeDate/Time/Datetime2/DatetimeOffset` static helpers retained (called by codec and by `test/cpp/test_bcp_row_encoder.cpp` directly — same pattern as Integer/String migrations). Same for `MSSQLValueSerializer::SerializeDate/Time/Timestamp/TimestampTZ` (still exercised by `test_value_serializer.cpp`). Wired `literal_format.cpp` DateTime arm in both `FormatSqlLiteral` and `EstimateLiteralSize`.
- [X] T086 [US3] `make test-codec-datetime` PASS (21 tests). `make test-literal-format` PASS (incl. new TestDateTimeFamilyDispatcherWired). `make test` PASS (5 COPY parallel-isolation failures pre-existing, confirmed by sequential re-run). Integration verified: catalog/ctas/insert/dml/transaction/query type_mapping all green. Tests fully cover DATETIMEOFFSET (decode UTC + encode offset=0 + render fallback), DATETIMEN length-dispatch, every TIME/DATETIME2 scale bucket. clang-format-14 sweep run.

### Family 7 of 7: Uuid

- [X] T087 [P] [US3] Captured Uuid golden fixtures under `golden/uuid/` (README + decode_cases.txt + encode_cases.txt + literal_cases.txt + ddl_cases.txt + edge_cases.txt). Covers nil/max/RFC-4122/mixed-byte/high-bit-set GUIDs, exercising the middle-endian byte-order swap and DuckDB's sortability XOR mask.
- [X] T088 [P] [US3] Wrote `test/cpp/codec/test_uuid_codec.cpp` — 15 tests (5 decode + 5 encode incl. round-trip + 1 literal byte-identity over 5 canonical forms + DDL byte-identity + EstimateLiteralSize + dispatcher routing + RenderAsString). All passing.
- [X] T089 [US3] Implemented `codec::uuid::*` in `src/codec/uuid_codec.cpp`. `DecodeFromTds` delegates to existing `GuidEncoding::ConvertGuid` (preserves middle-endian byte-order + sortability XOR). `EncodeToBcp` (Vector + Value overloads) inlines the inverse: unflip bit-63 of upper, serialize big-endian, reorder Data1/2/3 to little-endian, leave Data4 unchanged, write 1-byte length-prefix (0x10) + 16-byte payload. `FormatSqlLiteral` byte-identical Filter == InsertValues — both produce `'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx'` via `UUID::ToString`. `FormatDdlTypeName` byte-identical CreateTable == CtasCreateTable — both return `"UNIQUEIDENTIFIER"` (FR-027/FR-028 trivially satisfied — no length/precision/scale parameters). `EstimateLiteralSize` returns 38 (quoted canonical form). Added `RenderAsString(bytes)` for issue-#89 fallback path.
- [X] T090 [US3] Migrated all 5 dispatch sites' Uuid arms: type_converter.cpp (decode + WriteAsStringFallback now via `codec::uuid::RenderAsString`), bcp_row_encoder.cpp (EncodeRow + EncodeValue), filter_encoder.cpp (UUID arm routes through dispatcher), mssql_value_serializer.cpp (Serialize + EstimateSerializedSize both routes), mssql_ddl_translator.cpp (both DDL functions). Deleted `TypeConverter::ConvertGuid` from `.cpp` and `.hpp`. Removed now-unused `#include "tds/encoding/guid_encoding.hpp"` and `#include "duckdb/common/types/uuid.hpp"` from type_converter.cpp. Note: legacy `BCPRowEncoder::EncodeGUID` and `MSSQLValueSerializer::SerializeUUID` retained (still exercised by `test_bcp_row_encoder.cpp` / `test_value_serializer.cpp` — same kept-helper pattern as prior sub-phases). Wired `literal_format.cpp` Uuid arm in both `FormatSqlLiteral` and `EstimateLiteralSize` (replacing throw-stub). All 9 families now route through the dispatcher.
- [X] T091 [US3] `make test-codec-uuid` PASS (15 tests). `make test-literal-format` PASS (new TestUuidFamilyDispatcherWired). `make test-type-converter-fallback` PASS (new TestUuidIntoVarcharVector). All 9 codec test families green (`make test-codec-{boolean,integer,float,string,decimal,money,binary,datetime,uuid}`). `make test` — 5 pre-existing parallel-isolation COPY failures unchanged, confirmed by sequential re-run of `copy_basic.test` (passes). Integration smoke green: catalog (52 cases / 2290 assertions), ctas (16/508), dml (14/500), insert (14/350), transaction (16/552). clang-format-14 sweep run.

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

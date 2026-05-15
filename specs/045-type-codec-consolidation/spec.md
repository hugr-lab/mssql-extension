# Feature Specification: Type Codec Consolidation

**Feature Branch**: `045-type-codec-consolidation`

**Created**: 2026-05-15

**Status**: Draft

**Input**: User feedback after spec 044 shipped: "we wanted to gather
all type-data encoding/decoding work in one place because the logic
is currently duplicated in many places — where is that?" Spec 044
shipped the UTF-16 BYTE codec consolidation (UTF-8 ↔ UTF-16LE
conversion), not the per-type encoding/decoding consolidation. Spec
045 picks up the broader refactor.

## Overview

The extension currently has **five separate dispatch sites** where
"for type X, do operation Y" logic lives:

1. `src/tds/encoding/type_converter.cpp` (515 LOC) — TDS wire bytes
   → DuckDB Vector (scan decode hot path)
2. `src/tds/encoding/bcp_row_encoder.cpp` (758 LOC) — DuckDB Vector
   → TDS BCP wire bytes (BCP encode hot path)
3. `src/table_scan/filter_encoder.cpp` (982 LOC, `ValueToSQLLiteral`
   ~70 LOC of dispatch) — DuckDB Value → T-SQL WHERE-clause literal
   (filter pushdown)
4. `src/dml/insert/mssql_value_serializer.cpp` (466 LOC) — DuckDB
   Value → T-SQL INSERT VALUES literal (INSERT path)
5. `src/catalog/mssql_ddl_translator.cpp`'s `MapLogicalTypeToCTAS`
   — DuckDB LogicalType → T-SQL type-name string (CTAS DDL phase)

Each site has its own `switch (type)` and its own per-type helper
functions. The same conceptual logic — "how do we represent an
INTEGER over the wire?", "how do we format a DATE as a T-SQL
literal?", "what's the T-SQL type-name for a DuckDB UUID?" — is
duplicated across these files. Sites 3 and 4 (filter literal and
INSERT VALUES literal) are **near-identical** logic for ~95% of
types, with subtle differences (TIMESTAMP_TZ handling, DECIMAL
internal-storage preservation, HUGEINT explicit handling) that
should NOT be subtle.

Spec 045 consolidates the per-type logic so that "Integer behaves
this way over the wire AND as a T-SQL literal AND as a DDL type"
lives in **one place** per type family. The dispatch mechanics
stay where they are (each call site still has a `switch`), but
each switch arm calls into a unified per-type module instead of
into local helper functions.

## What this spec is NOT

This is a deliberately scoped-down version of the original source
doc's `MssqlTypeCodec` virtual class hierarchy proposal. The source
doc proposed:

- An abstract `MssqlTypeCodec` base class with 4 virtual methods
  (`DecodeBatch`, `EncodeBcpBatch`, `FormatSqlLiteral`,
  `AppendDdlColumnType`)
- A registry mapping `TdsTypeId` / `LogicalType` → codec instance
- `TdsReader` / `TdsWriter` abstractions over the existing
  `std::vector<uint8_t>` byte buffers
- Batch-style APIs (one virtual call per DataChunk instead of per row)

We **reject** that design after reading the actual dispatch sites
for these reasons:

1. **Dispatch axes mismatch.** Scan decode (Shape A) dispatches on
   the TDS wire type (`column.type_id` — a `uint8_t` enum from the
   MS-TDS spec). BCP encode (Shape B), filter literal (Shape C),
   and DDL type name (Shape D) dispatch on the DuckDB
   `LogicalTypeId`. The mapping between them is many-to-many: TDS
   `INTN(len=4)` → DuckDB `INTEGER`, but DuckDB `DECIMAL(p,s)` →
   TDS `DECIMAL` with arbitrary (p, s). A single virtual-class
   registry can't cleanly key on both axes.
2. **Hot-path shape mismatch.** Shapes A and B are per-row in the
   inner scan/BCP loops (millions of calls/sec). Shapes C and D are
   per-statement (handful of calls per query). Forcing both through
   the same virtual-dispatch mechanism either adds virtual-call
   overhead to the hot path or under-uses the abstraction on the
   cold path.
3. **Batch APIs are a separate refactor.** The current row reader
   reassembles PLP chunks and dispatches per-row. Introducing batch
   decode would require rewiring `RowReader` and the row-buffer
   contract throughout the scan path. That's its own multi-week
   project; it doesn't belong inside spec 045.
4. **`TdsReader`/`TdsWriter` wrappers buy nothing.** Current code
   passes `std::vector<uint8_t>&` directly. A wrapper would add
   indirection without changing the call semantics.

Instead, spec 045 introduces **per-type-family modules** (free
functions in named sub-namespaces, not virtual classes) and
**eliminates the duplication between filter literal and INSERT
VALUES literal** (the single biggest real-world duplication in the
codebase).

## Clarifications

### Session 2026-05-15

- Q: One codec interface for all 5 dispatch shapes, or specialized
  interfaces? → A: Specialized. Two complementary dispatch registries:
  (i) **TDS-type → TypeFamily** for scan decode, and (ii) **DuckDB
  LogicalTypeId → TypeFamily** for BCP encode + literal text + DDL
  name. Each family lives in one module
  (`src/codec/<family>_codec.{hpp,cpp}`) that exposes the 4 operations
  as free functions in a `duckdb::codec::<family>` namespace.
  See §Architecture below.
- Q: Should batch APIs (one call per DataChunk, not per row) be
  introduced now? → A: No. Per-row stays. Batch is a separate spec
  with its own scope. Rationale: batch requires rewiring `RowReader`
  + buffer contracts + bench validation; the per-type code-quality
  win from 045 lands independently.
- Q: How to handle the filter_encoder vs mssql_value_serializer
  near-duplication? → A: Both call sites collapse into one shared
  `FormatSqlLiteral(Value v, LogicalType type, LiteralContext ctx)`
  in `src/codec/literal_format.{hpp,cpp}`. The two callers pass
  different `LiteralContext` (filter pushdown vs INSERT VALUES) so
  each can request the subtle behavior it needs (e.g., TIMESTAMP_TZ
  offset handling). One implementation; two consumers; zero
  duplication.
- Q: Migration order — top-down, bottom-up, or by-family? → A:
  By-family. Each per-type-family module migrates as a single unit
  (Integer family across all 5 dispatch sites, then Decimal, then
  String, etc.). At each migration step the 5 dispatch sites' switch
  arms for that family go from inline-helpers to family-module
  calls. Tests stay green after each family migration.
- Q: Does spec 045 depend on spec 044 being merged? → A: No.
  Spec 045 doesn't touch UTF-16 byte conversion (that's behind the
  `encoding::Utf16LE*` API used by both versions of the wrapper).
  Spec 045's branch can be developed in parallel with #109. The
  final merge order is the release engineer's call.

## User Scenarios & Testing *(mandatory)*

### User Story 1 — Single place to update "how integers behave" (Priority: P1) 🎯 MVP

A developer needs to add support for a new SQL Server integer
variant (e.g., a hypothetical `SMALLINT_UNSIGNED` if SQL Server ever
ships one), or fix a bug in how UTINYINT is encoded in BCP. Today
they would have to:

1. Open `type_converter.cpp` and update `ConvertInteger` and the
   integer-related cases in `ConvertValue`.
2. Open `bcp_row_encoder.cpp` and update the integer cases in
   `EncodeRow` and `EncodeValue` plus the relevant `EncodeIntN`
   helpers.
3. Open `filter_encoder.cpp` and update the integer cases in
   `ValueToSQLLiteral`.
4. Open `mssql_value_serializer.cpp` and update the integer cases
   in `Serialize`.
5. Open `mssql_ddl_translator.cpp` and update the integer cases in
   `MapLogicalTypeToCTAS`.

Five files. Five switches. Five places to remember to update.

After spec 045, the developer opens **one file**:
`src/codec/integer_codec.cpp`. All four operations (scan decode,
BCP encode, T-SQL literal, T-SQL DDL type-name) for the integer
family live in one place. The 5 dispatch sites still have switches,
but each switch arm is a one-liner calling into the family module.

**Why this priority**: Integer is the simplest family (no precision,
no PLP, no surrogate pairs). It's the right MVP — proves the design
on a clean target before tackling Decimal (with precision/scale) or
String (with UTF-16 + PLP).

**Independent Test**: After Integer family migration, existing
integer-related SQL tests (scan, BCP, INSERT VALUES, filter
pushdown, CTAS) remain green. New unit test
`test/cpp/test_integer_codec.cpp` exercises each of the 4
operations on the integer family directly, asserting parity with
the pre-migration helpers via golden fixtures.

**Acceptance Scenarios**:

1. **Given** a DuckDB table with INTEGER, TINYINT (mapping to UTINYINT
   in DuckDB per SQL Server semantics), SMALLINT, BIGINT columns,
   **When** the scan/BCP/INSERT/filter/CTAS test suite runs,
   **Then** every test that passed against `main` also passes against
   the spec-045 Integer-family migration HEAD.
2. **Given** the new `test_integer_codec.cpp` golden-fixture test,
   **When** run against the migrated code, **Then** all four
   operations produce byte/string outputs byte-identical to a
   captured pre-migration baseline.

---

### User Story 2 — Eliminate the filter-vs-INSERT literal duplication (Priority: P1)

A developer reading the codebase sees that `filter_encoder.cpp`'s
`ValueToSQLLiteral` and `mssql_value_serializer.cpp`'s `Serialize`
are 95% the same logic with subtle differences they have to chase
case-by-case. After spec 045 lands, both call the same
`codec::FormatSqlLiteral(value, type, LiteralContext::Filter)` or
`codec::FormatSqlLiteral(value, type, LiteralContext::InsertValues)`
function, which lives in `src/codec/literal_format.{hpp,cpp}`. The
context-dependent differences (TIMESTAMP_TZ offset, HUGEINT explicit
treatment) are expressed as explicit `LiteralContext` branches
within one function, not as silently-divergent reimplementations.

**Why this priority**: This is the single biggest duplication win
the spec offers. ~70 LOC of `ValueToSQLLiteral` in filter_encoder.cpp
+ ~100 LOC of `Serialize` in mssql_value_serializer.cpp →
consolidated into ~120 LOC in `literal_format.cpp` with explicit
context dispatch. Future bugs in literal formatting get fixed in
one place; future features (e.g., a new T-SQL literal escape rule)
get implemented in one place.

**Independent Test**: Existing pushdown SQL tests
(`test/sql/catalog/filter_pushdown.test` etc.) + existing INSERT
SQL tests (`test/sql/insert/*.test`) all remain green. New unit
test `test/cpp/test_literal_format.cpp` exercises both contexts on
a shared fixture set, asserting outputs match the pre-migration
behavior of each call site.

**Acceptance Scenarios**:

1. **Given** the filter pushdown SQL test suite, **When** run
   against the spec-045 HEAD, **Then** all previously-green tests
   remain green. T-SQL WHERE clauses generated are byte-identical
   to the pre-migration output for at least 20 representative
   filter expressions.
2. **Given** the INSERT VALUES SQL test suite, **When** run against
   the spec-045 HEAD, **Then** all previously-green tests remain
   green. T-SQL INSERT statements generated are byte-identical to
   the pre-migration output for at least 20 representative
   INSERT-from-VALUES queries.
3. **Given** the new `test_literal_format.cpp`, **When** run,
   **Then** the same `Value` formatted via
   `LiteralContext::Filter` and `LiteralContext::InsertValues`
   produces:
   - **identical output** for types where the two contexts agreed
     pre-migration (most types);
   - **differing output as documented** for types with
     context-specific behavior (e.g., TIMESTAMP_TZ with offset in
     INSERT but UTC in filter).

---

### User Story 3 — Migrate the remaining families (Priority: P2)

Apply the Integer-family pattern from US1 to the other 8 type
families: **Boolean, Float, Decimal, String, Binary, DateTime,
Uuid, Money**. Each family is its own focused migration that lands
the corresponding switch-arm collapse across all 5 dispatch sites.

**Why this priority**: P2 because the design is proven by US1's
Integer migration. Each subsequent family is mechanical
application. The cumulative win lands when all 9 families are
migrated; until then there's a mix of "old-style" and "new-style"
switches.

**Independent Test**: After each family migration, the existing
test suite remains green. After the LAST family migration, the
audit `grep` confirms the 5 dispatch sites contain switches whose
arms each call exactly one `codec::<family>::<op>(...)` function
— no per-type helpers remain in the dispatch site files.

**Acceptance Scenarios**:

1. For each of {Boolean, Float, Decimal, String, Binary, DateTime,
   Uuid, Money} families: existing tests green after that family's
   migration; per-family golden-fixture test added.
2. After all 9 families migrated, `src/tds/encoding/type_converter.cpp`,
   `src/tds/encoding/bcp_row_encoder.cpp`, the literal-format module,
   and `src/catalog/mssql_ddl_translator.cpp` each shrink by 30-50%
   of their LOC (the per-type helpers are gone; only dispatch
   switches remain).

---

### User Story 4 — Single source for SQL Server type-name mapping (Priority: P2)

Currently `MapLogicalTypeToCTAS` lives in `mssql_ddl_translator.cpp`
and is the only DDL-type-name mapper. After spec 045, it moves into
each family's module as `codec::<family>::FormatDdlTypeName(type,
ctx)`. The DDL translator becomes a thin dispatch shell.

**Why this priority**: P2 — DDL is exercised once per CTAS and the
code path is small. The win is consistency: when adding a new type
family, you don't have to remember to update `MapLogicalTypeToCTAS`
separately — it lives in the same module as the other type ops.

**Independent Test**: CTAS test suite (`test/sql/ctas/*.test`)
remains green; new fixture verifies DDL type-name output is
byte-identical to pre-migration for a 20+ type matrix.

**Acceptance Scenarios**:

1. **Given** the CTAS test suite, **When** run against spec-045
   HEAD, **Then** all DDL-type-name fixtures pass.
2. **Given** `mssql_ddl_translator.cpp`'s `MapLogicalTypeToCTAS`,
   **When** the spec lands, **Then** the function body is reduced
   to a single switch dispatching to `codec::<family>::FormatDdlTypeName`
   per family.

---

### User Story 5 — Fix BCP nvarchar length-validation bug (issue #91) (Priority: P1)

A user with `COPY (SELECT ... AS state) TO 'mssql://db/dbo/MyTable'
(FORMAT 'bcp', CREATE_TABLE false)` against an existing
`nvarchar(N)` column on SQL Server hits
`Invalid Input Error: MSSQL: BCP failed: Received an invalid
column length from the bcp client for colid N` even when the
string fits in `N` UCS-2 characters but has more than `N` UTF-8
bytes (e.g., 500 ASCII-plus-Cyrillic mix in nvarchar(500)).

After spec 045's String family migration, the NVARCHAR encode
path validates length against the column's UTF-16LE byte capacity
(`col.max_length` in COLMETADATA = N*2 bytes for nvarchar(N)), not
against the UTF-8 byte length of the DuckDB-side input. The fix
naturally lands inside `codec::string::EncodeToBcp` because spec
045's design puts all NVARCHAR-related encoding/validation logic
into one place.

**Why this priority**: P1 because (a) it's an active user-reported
bug ([issue #91](https://github.com/hugr-lab/mssql-extension/issues/91)),
(b) the workaround the user posted is wasteful (truncates to ~125
characters for an nvarchar(500) column to be safe on emoji),
(c) the String family migration is the natural and inevitable
moment to fix this — the migration moves all relevant logic into
one module where the invariant is testable in isolation.

**Independent Test**: New SQLLogicTest
`test/sql/copy/copy_nvarchar_length_validation.test` that
reproduces the issue's scenario: `LEFT(repeat('ñ' || 'a', 250),
500)` (500 chars / ~750 UTF-8 bytes / 1000 UTF-16 bytes) into
`nvarchar(500)` succeeds and round-trips. Plus emoji edge case:
8 ASCII + 4 emoji (= 12 UCS-2 code units = 24 UTF-16 bytes) into
`nvarchar(20)` should succeed; 8 ASCII + 8 emoji (= 16 UCS-2
code units = 32 UTF-16 bytes) into the same column should fail
with a clear "exceeds column capacity" error (not a confusing
"invalid column length from bcp client" wire error).

**Acceptance Scenarios**:

1. **Given** an `nvarchar(500)` SQL Server column and a DuckDB
   VARCHAR value with 500 Unicode characters but >500 UTF-8 bytes
   (e.g., mix of ASCII and 2-byte UTF-8 characters), **When** the
   user runs `COPY ... (FORMAT 'bcp', CREATE_TABLE false)`,
   **Then** the row is inserted successfully and reads back
   byte-identical.
2. **Given** an `nvarchar(20)` column (40-byte UTF-16LE capacity)
   and a DuckDB VARCHAR with 8 ASCII + 4 emoji
   characters (= 16 UCS-2 code units = 32 UTF-16LE bytes; fits
   within 40-byte capacity), **When** copied via BCP, **Then**
   the row is inserted successfully and round-trips byte-identical.
3. **Given** the same `nvarchar(20)` column and a DuckDB VARCHAR
   with 8 ASCII + 8 emoji characters (= 24 UCS-2 code units = 48
   UTF-16LE bytes; exceeds the 40-byte capacity), **When** copied
   via BCP, **Then** the operation fails with a String-family
   error message that names the column and the observed-vs-allowed
   UCS-2 code-unit count — NOT a generic "Received an invalid
   column length from the bcp client" passthrough from SQL Server.
4. **Given** the user's exact reproducer from issue #91 (500
   ASCII-plus-Cyrillic-mix characters), **When** copied,
   **Then** the operation succeeds without requiring the
   `trunc_bytes` macro workaround.

---

### Edge Cases

- **TDS_TYPE_INTN with variable max_length**: Already handled in
  `ConvertInteger` via the `value.size()` switch. The Integer family
  module preserves this behavior — `DecodeFromTds` is parameterized
  by the column metadata's `max_length`.
- **DECIMAL precision/scale tracking through internal storage type**:
  DuckDB stores DECIMAL in int16/int32/int64/hugeint based on
  precision. The current code in `bcp_row_encoder.cpp:139-154` and
  `mssql_value_serializer.cpp:334-345` handles this with inner
  switches on `PhysicalType`. The Decimal family module owns this
  logic in one place.
- **UTINYINT vs SQL Server TINYINT**: SQL Server TINYINT is unsigned
  (0-255); DuckDB maps to UTINYINT. BCP encode of UTINYINT uses
  `EncodeUInt8`. The Integer family module documents this asymmetry.
- **UBIGINT in BCP**: BCP wire doesn't have UNSIGNED BIGINT; the
  current code in `bcp_row_encoder.cpp:118-124` encodes UBIGINT as
  DECIMAL(20,0). The Integer family module preserves this; the
  comment explaining "why" moves into the family module.
- **TIMESTAMP_TZ offset in filter vs INSERT**: Filter pushdown
  passes 0 (UTC, server-side conversion); INSERT VALUES also passes
  0 (per the current code, but with an explicit comment about
  server-side timezone conversion). The `LiteralContext` parameter
  makes this divergence explicit and inspectable, not implicit.
- **XML mapping in scan decode**: `TDS_TYPE_XML` is dispatched to
  `ConvertString` (treated as NVARCHAR(MAX)). The String family
  module owns this; XML is documented as a "subtype alias" of String.
- **MONEY/SMALLMONEY → DECIMAL(19,4) / DECIMAL(10,4)**: Current
  code in `ConvertMoney`. The Money family module wraps this.
- **GUID byte-order on TDS wire**: UNIQUEIDENTIFIER on the wire is
  middle-endian (first three groups little-endian, last two big-
  endian, per MS-TDS). Current code in `guid_encoding.cpp` handles
  this; the Uuid family module pulls it in.
- **PLP (NVARCHAR(MAX), VARBINARY(MAX)) handling**: PLP-typed
  columns currently dispatch to `EncodeNVarcharPLP` / `EncodeBinaryPLP`
  in BCP encode based on `col.IsPLPType()`. The String/Binary
  family modules expose this branch explicitly via metadata, not
  via a hidden flag.
- **Spec 044 / `encoding::Utf16LE*`**: Spec 045 uses the spec-043
  / spec-044 UTF-16 byte conversion primitives. Spec 045 does NOT
  re-litigate the byte-codec; it consumes `Utf16LE*` as the
  byte-level building block inside the String family module.

## Requirements *(mandatory)*

### Functional Requirements

**Module structure**

- **FR-001**: A new directory `src/codec/` MUST hold the per-type-
  family modules. Headers mirror in `src/include/codec/`. The
  namespace is `duckdb::codec` (consistent with the existing
  `duckdb::tds::encoding` convention for already-namespaced
  utility modules).
- **FR-002**: Each family MUST have one source file pair
  (`src/codec/<family>_codec.{hpp,cpp}`) exposing four free
  functions in `namespace duckdb::codec::<family>`:
  - `void DecodeFromTds(const std::vector<uint8_t> &bytes,
    const tds::ColumnMetadata &col, Vector &out, idx_t row);`
  - `void EncodeToBcp(Vector &in, idx_t row,
    const mssql::BCPColumnMetadata &col, std::vector<uint8_t> &buf);`
  - `std::string FormatSqlLiteral(const Value &v,
    const LogicalType &type, LiteralContext ctx);`
  - `std::string FormatDdlTypeName(const LogicalType &type,
    const mssql::CTASConfig &ctx);`
  Each family implements only the operations it owns; the other
  operations are not defined (no abstract base, no virtual fallback).
- **FR-003**: A shared header
  `src/include/codec/literal_context.hpp` MUST define:
  ```
  enum class LiteralContext { Filter, InsertValues };
  ```
  consumed by `FormatSqlLiteral`. Family modules dispatch on this
  enum where (and only where) the two contexts genuinely differ.
- **FR-004**: A shared header `src/include/codec/type_family.hpp`
  MUST define an enum class:
  ```
  enum class TypeFamily {
      Boolean, Integer, Float, Decimal, Money,
      String, Binary, DateTime, Uuid
  };
  ```
  Plus two free helpers:
  ```
  TypeFamily FamilyFromTdsType(uint8_t tds_type_id, uint8_t max_length);
  TypeFamily FamilyFromLogicalType(const LogicalType &type);
  ```
  These functions absorb the existing type-grouping logic from the
  5 dispatch sites' switches. After spec 045, every dispatch site's
  switch is `switch (FamilyFromXxx(...))` with one case per family.

**Migration of the 5 dispatch sites**

- **FR-010**: `src/tds/encoding/type_converter.cpp:ConvertValue`
  MUST be rewritten as a `switch (FamilyFromTdsType(...))` whose
  arms call `codec::<family>::DecodeFromTds(...)`. The per-type
  helper functions (`ConvertInteger`, `ConvertBoolean`,
  `ConvertFloat`, `ConvertDecimal`, `ConvertMoney`, `ConvertString`,
  `ConvertBinary`, `ConvertDate`, `ConvertTime`, `ConvertDateTime`,
  `ConvertDatetimeOffset`, `ConvertGuid`) MUST be deleted from
  `type_converter.cpp` once their logic has moved into the family
  modules.
- **FR-011**: `src/tds/encoding/bcp_row_encoder.cpp:EncodeRow` and
  `EncodeValue` MUST be rewritten as `switch (FamilyFromLogicalType(...))`
  whose arms call `codec::<family>::EncodeToBcp(...)`. The per-type
  encode helpers (`EncodeBit`, `EncodeInt8`, ..., `EncodeDatetime2`,
  `EncodeDatetimeOffset`, etc.) MUST move into the family modules.
- **FR-012**: A NEW file `src/codec/literal_format.{hpp,cpp}` MUST
  expose `std::string FormatSqlLiteral(const Value &v, const
  LogicalType &type, LiteralContext ctx)`. Body: `switch
  (FamilyFromLogicalType(type))` calling
  `codec::<family>::FormatSqlLiteral(...)`. Two call sites consume
  it:
  - `src/table_scan/filter_encoder.cpp:ValueToSQLLiteral` is
    replaced by a one-line call to `codec::FormatSqlLiteral(value,
    type, LiteralContext::Filter)`.
  - `src/dml/insert/mssql_value_serializer.cpp:Serialize` is
    replaced by a one-line call to `codec::FormatSqlLiteral(value,
    type, LiteralContext::InsertValues)`.
  The legacy `ValueToSQLLiteral` and `Serialize` per-type bodies
  MUST be deleted. The two source files keep their non-dispatch
  responsibilities (`filter_encoder` keeps the filter tree walker;
  `mssql_value_serializer` keeps the multi-row VALUES batching
  logic).
- **FR-013**: `src/catalog/mssql_ddl_translator.cpp:MapLogicalTypeToCTAS`
  MUST be rewritten as `switch (FamilyFromLogicalType(type))` calling
  `codec::<family>::FormatDdlTypeName(...)`. Per-type DDL type-name
  branches MUST move into the family modules.
- **FR-014**: `EstimateSerializedSize` in
  `mssql_value_serializer.cpp` MAY remain in its current location
  (it's a heuristic for buffer sizing, not a correctness path) OR
  MAY move into the family modules as `codec::<family>::EstimateLiteralSize`.
  The spec implementer chooses based on whether the move improves
  reviewability.

**Behavior preservation (with one explicit exception)**

- **FR-020**: Every public observable behavior of the 5 dispatch
  sites MUST be byte-identical to `main`-at-spec-045-kickoff for
  all inputs the existing test suite exercises, **EXCEPT** for the
  bug fix mandated by FR-023 (issue #91). The migration is a
  refactor with one targeted bug fix that surfaces naturally as the
  String family takes ownership of NVARCHAR length-validation
  responsibility.
- **FR-021**: The wire-protocol contracts MUST NOT change. SQL
  Server cannot tell the difference between the pre-spec-045 and
  post-spec-045 binary on the wire **for any input that succeeded
  pre-spec-045**. Inputs that pre-spec-045 incorrectly rejected
  (per FR-023) MAY now succeed.
- **FR-022**: Edge cases documented in §Edge Cases (above) MUST
  be preserved as documented. UTINYINT-vs-TINYINT asymmetry,
  UBIGINT-as-DECIMAL, GUID middle-endian, TIMESTAMP_TZ context
  divergence — all of these survive the migration.
- **FR-023**: **Fix issue #91** (BCP COPY TO compares UTF-8 byte
  length against nvarchar character length). The String family
  module's `EncodeToBcp` implementation MUST validate cell length
  against the column's TDS wire byte capacity (`col.max_length`
  for non-PLP NVARCHAR columns; PLP for MAX) on the UTF-16LE
  encoded byte count — NOT on the UTF-8 byte count of the
  DuckDB-side input. The current code in
  `src/tds/encoding/bcp_row_encoder.cpp:EncodeNVarchar` and the
  COLMETADATA construction in `src/copy/target_resolver.cpp`
  appear to handle this correctly on paper (sys.columns
  `max_length` for nvarchar(N) is N*2 bytes; we write that into
  COLMETADATA; we send the actual UTF-16LE byte count as cell
  prefix). The bug exists in a path that has not yet been
  isolated. The String family migration is the natural place to
  hunt it down because the migration moves all length-related
  logic for NVARCHAR into one module where the invariant can be
  verified with a dedicated test (FR-031a). Per the issue
  report, a string with 530 UTF-8 bytes but only 500 characters
  must successfully insert into an `nvarchar(500)` column
  (1000-byte UTF-16LE capacity).

**Testing**

- **FR-030**: A new C++ test directory `test/cpp/codec/` MUST
  contain one unit test per family
  (`test_integer_codec.cpp`, `test_decimal_codec.cpp`,
  `test_string_codec.cpp`, etc.). Each test exercises the four
  operations on golden fixtures (input → expected output) and
  asserts byte/string equality. Manual targets in the Makefile
  (`make test-codec-<family>`) similar to `test-login7-encoding`.
- **FR-031a**: A new SQLLogicTest
  `test/sql/copy/copy_nvarchar_length_validation.test` MUST cover
  issue #91 per User Story 5: (a) 500 mixed ASCII+Cyrillic chars
  into nvarchar(500) succeeds and round-trips byte-identical;
  (b) ASCII + small number of emoji into a small nvarchar(N) column
  succeeds while UCS-2 code units fit within the column capacity;
  (c) input that genuinely exceeds the column's UCS-2 capacity
  fails with a String-family-emitted error message naming the
  column and the observed-vs-allowed code-unit count (not a
  passthrough server error). This test MUST fail on the
  pre-spec-045 baseline binary (proving the bug is real) and pass
  on the spec-045 String-family-migrated binary.
- **FR-031**: A new C++ unit test `test/cpp/test_literal_format.cpp`
  MUST cover the `LiteralContext` divergence cases (TIMESTAMP_TZ,
  HUGEINT, ENUM if applicable). For each fixture, both contexts
  are exercised; output is asserted against captured golden bytes
  from the pre-migration baseline.
- **FR-032**: The existing SQL test suites (`make test`,
  `make integration-test`) MUST remain green after every family
  migration. No regression in 103+ test cases / 3229+ assertions.
- **FR-033**: A pre-migration baseline of "golden outputs" MUST be
  captured at spec-045 kickoff for the per-type-family fixture set
  (FR-030). Captured outputs are committed under
  `specs/045-type-codec-consolidation/golden/`. Each family
  migration validates against the family's slice of this baseline.

**Naming / placement**

- **FR-040**: The `src/codec/` directory is a NEW top-level source
  directory. It is NOT `src/tds/codec/` because the codec layer is
  agnostic to the TDS transport (it produces strings for T-SQL DDL
  / literals, byte buffers for BCP, and Vector slots for scan).
  Existing `src/tds/encoding/` keeps the wire-level primitives
  (`utf16.{hpp,cpp}`, `datetime_encoding.cpp` helpers for low-
  level math, etc.); spec 045 moves the higher-level per-family
  dispatch logic OUT of `src/tds/encoding/` and INTO `src/codec/`.
- **FR-041**: Per-family files use snake_case
  (`integer_codec.cpp`, `decimal_codec.cpp`, `string_codec.cpp`),
  per CLAUDE.md naming conventions.
- **FR-042**: Per-family functions are PascalCase
  (`DecodeFromTds`, `EncodeToBcp`, `FormatSqlLiteral`,
  `FormatDdlTypeName`), in `namespace duckdb::codec::<family>`,
  per CLAUDE.md "duckdb::-rooted namespace, no prefix" convention.

### Key Entities

- **Type family**: Conceptual grouping of related types that share
  encode/decode logic. Nine families total: Boolean, Integer
  (TINYINT..UBIGINT), Float (FLOAT, DOUBLE), Decimal, Money
  (mapped to DECIMAL on the DuckDB side but with fixed precision/
  scale on the SQL Server side), String (VARCHAR, NVARCHAR, XML),
  Binary (BLOB, VARBINARY), DateTime (DATE, TIME, TIMESTAMP*,
  DATETIMEOFFSET), Uuid (UUID).
- **Per-family module**:
  `src/codec/<family>_codec.{hpp,cpp}` with the four free
  functions per FR-002. Self-contained: depends only on DuckDB
  types + `encoding::Utf16LE*` for string families.
- **`TypeFamily` enum**: 9-value enum classifying both TDS types
  (via `FamilyFromTdsType`) and DuckDB types (via
  `FamilyFromLogicalType`) into the same family taxonomy.
- **`LiteralContext` enum**: 2-value enum (`Filter`,
  `InsertValues`) distinguishing the two T-SQL literal-formatting
  contexts. Family modules dispatch on it only where the two
  contexts genuinely differ.
- **Literal format module**: `src/codec/literal_format.{hpp,cpp}`
  with the shared `FormatSqlLiteral(value, type, ctx)` function.
  Consumes the family modules; consumed by filter pushdown and
  INSERT VALUES serialization.
- **Golden-fixture baseline**:
  `specs/045-type-codec-consolidation/golden/` directory
  containing pre-migration per-family captured outputs for the
  test fixtures. Used as the byte-identical-output guard.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: After all 9 family migrations, the LOC count of the
  five dispatch sites shrinks by **at least 25% combined**.
  Baseline (current main, ~3288 LOC total per "Source line
  counts" audit in the user-facing scope discussion): target ≤
  2466 LOC across the five files. The shrinkage is the per-type
  helper bodies moving OUT into family modules.
- **SC-002**: Zero behavior regressions: every test that passed
  on `main`-at-spec-045-kickoff passes on the spec-045 PR HEAD.
  Strict gate. (Exception: the bug-fix regression test from
  FR-031a is **expected to fail** on `main`-at-spec-045-kickoff
  and **expected to pass** on the spec-045 PR HEAD — see SC-002a.)
- **SC-002a**: Issue #91 closed by `test/sql/copy/copy_nvarchar_length_validation.test`
  passing on the spec-045 PR HEAD. The same test MUST fail on the
  pre-spec-045 baseline (`main` at spec-045-kickoff) to prove the
  bug was real and is now fixed. Captured pre/post diff committed
  in `specs/045-type-codec-consolidation/issue_91_repro.md`.
- **SC-003**: 100% of family golden-fixture tests pass. Each
  family's `test/cpp/codec/test_<family>_codec.cpp` PASS verdict
  is required.
- **SC-004**: The new `test/cpp/test_literal_format.cpp` PASSES,
  asserting that `LiteralContext::Filter` and
  `LiteralContext::InsertValues` produce documented outputs (same
  where the contexts agree; explicitly-different where they
  diverge).
- **SC-005**: Audit: `grep -rn 'switch.*type_id\|switch.*type.id()\|switch.*duckdb_type'
  src/tds/encoding/ src/table_scan/ src/dml/ src/catalog/`
  returns exactly **5 matches** post-migration: one per dispatch
  site, each calling `FamilyFromXxx(...)` and dispatching on
  `TypeFamily`. No nested per-type switches remain in the
  dispatch site files.
- **SC-006**: Per-family code consolidation: each family module's
  source file contains the implementation of all four operations
  for that family. `grep -rn 'codec::<family>::' src/` returns
  matches only in (a) the family's own module and (b) the 5
  dispatch sites.
- **SC-007**: Existing integration test suites continue to pass on
  every CI platform (Linux GCC, macOS Clang, Windows MSVC, Windows
  MinGW). No new platform risk.
- **SC-008**: A measurable scan/BCP performance comparison
  (re-using the spec-044 `bench_codec_e2e.sh` infrastructure at 1M
  rows) shows the migration introduces **≤ 5% wall-clock
  regression** on any step. Codec consolidation should be neutral
  or slightly positive (less branching, better inlining); 5% slack
  absorbs measurement noise.

## Assumptions

- The 9-family taxonomy fits the existing type matrix without
  awkward "miscellaneous" buckets. Confirmed by inspection of the
  5 dispatch sites — every type currently handled fits cleanly
  into one of the 9 families.
- The per-row hot paths (scan decode, BCP encode) can afford the
  one extra non-virtual function call per row that calling into a
  family module adds. Free-function calls in the same TU (or
  cross-TU with LTO) are typically inlined; even without inlining,
  one extra call per row is ~1-2 ns per row, negligible vs the
  surrounding work.
- DuckDB's stable types (no breaking changes to LogicalType /
  Vector API between current `main` and the spec-045 merge).
- The byte-identical-output property of the existing dispatch
  sites' helpers can be captured as golden fixtures. The fixture
  set covers the type matrix the test suite already exercises
  (verified during research phase).
- Spec 044 ([#109]) does not need to merge before spec 045 starts.
  The two work on different layers (044 is byte-level UTF-16, 045
  is per-type dispatch). When both have merged, 045's String
  family module sits on top of 044's `Utf16LE*` primitives.
  Whichever PR lands second performs a trivial rebase.

## Out of Scope

- **Abstract base class hierarchy** (`MssqlTypeCodec` with virtual
  methods, registry, factory). Rejected per source-doc critique
  above.
- **Batch APIs** (one call per DataChunk instead of per row).
  Separate spec; requires rewiring `RowReader` and BCP buffer
  contracts.
- **`TdsReader` / `TdsWriter` wrapper classes**. The current
  `std::vector<uint8_t>&` byte buffer API stays.
- **Type-mapping additions** (UUID round-trip via UNIQUEIDENTIFIER
  the spec-044 source doc proposed, HUGEINT → DECIMAL(38,0),
  TIMESTAMP_TZ → DATETIMEOFFSET, nested types → JSON fallback).
  These are correctness changes that change behavior; spec 045 is
  primarily a refactor. Type-mapping fixes belong to subsequent
  specs that each handle one mapping. **Exception**: the one
  behavior-changing fix that DOES belong in spec 045 is issue
  #91 (NVARCHAR length validation in BCP encode) — see FR-023 /
  User Story 5 / SC-002a. That fix lives inside the String family
  module which spec 045 introduces, so it falls in scope
  naturally and is testable as a discrete unit. No other bug
  fixes piggyback on spec 045.
- **`VariantCodec` for XML / SQL_VARIANT / UDT**. XML is already a
  String-family subtype; SQL_VARIANT and UDT remain unsupported
  (current behavior). A VARIANT codec is a separate feature spec.
- **Performance optimization**. Spec 045 is neutral on
  performance; SC-008 only requires ≤ 5% regression. Speed-up
  optimizations (SIMD per-row vectorized encode, dictionary BCP
  encoding) are separate specs.
- **CI gate for performance**. The bench at 1M rows is local-only
  per spec 044's precedent.
- **Spec 044 changes**. UTF-16 byte codec stays as-is (post-#109
  merge state). Spec 045 builds on top.

## Dependencies and Coordination

- **Builds on spec 044** ([#109]) but does not require its merge.
  Spec 045's String family module uses `encoding::Utf16LE*` which
  exists at the same public path in both spec-043 and spec-044
  versions of the codebase (legacy path on spec-043; simdutf-backed
  on spec-044). The migration is API-stable across both.
- **Parallel with spec 042** (Integrated Authentication,
  collaborator). Spec 042 touches `src/tds/auth/` which spec 045
  does not. No overlap.
- **Unblocks future codec work**. Once each type family has one
  authoritative module, future work (batch APIs, dictionary-vector
  fast paths, VARIANT codec) starts from a clean per-family base.

## Implementation Strategy

Phased, family-by-family. Each phase is its own PR-sized increment
(could be one commit, could be several small commits depending on
review preference). All tests stay green after every phase.

**Phase 0 — Scaffolding**: create `src/codec/` + `src/include/codec/`
directories, define `TypeFamily` enum + `FamilyFromXxx` helpers,
add empty per-family stub headers, wire CMakeLists.txt. No
behavior change yet — the new code is unreferenced.

**Phase 1 — Integer family (US1, P1)**: implement
`integer_codec.{hpp,cpp}` with all 4 operations. Migrate the 5
dispatch sites' Integer arms to call into the new module. Delete
the old `ConvertInteger`, `EncodeInt8..Int64`, integer cases in
`ValueToSQLLiteral` / `Serialize` / `MapLogicalTypeToCTAS`. Tests
green.

**Phase 2 — Literal format consolidation (US2, P1)**: introduce
`src/codec/literal_format.{hpp,cpp}` with the shared
`FormatSqlLiteral`. Initially it only knows about Integer family
(other families fall through to a stub that calls into the
existing helpers). Migrate both `ValueToSQLLiteral` and `Serialize`
call sites to invoke the shared function. As each subsequent
family migrates (Phases 3+), the corresponding stub in
literal_format is replaced with the family call. (Or alternatively:
defer literal_format consolidation to AFTER all families are
migrated. The implementer's choice based on review preference.)

**Phases 3-10 — One per remaining family**: Boolean, Float, Decimal,
Money, String, Binary, DateTime, Uuid. Each phase mirrors Phase 1's
shape.

**Phase 11 — Audit and cleanup**: delete any remaining per-type
helpers from the dispatch site files. Run the SC-005 audit grep.
Update CLAUDE.md "Project Structure" to add the new `src/codec/`
directory. Update README if it lists per-source-file responsibilities.

**Phase 12 — Polish**: clang-format-14 sweep on all modified files;
re-run microbench from spec 044 to confirm no codec-level
regression; PR description finalization.

Total scope estimate: **3-6 weeks** of focused work. Each phase is
independently shippable as its own commit (or its own micro-PR if
the team prefers). The spec-045 PR can land all phases at once or
be split — that's a review-process choice, not a correctness one.

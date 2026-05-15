# Research: Type Codec Consolidation (spec 045) — Phase 0

This document resolves the design decisions that spec.md flagged as
needing confirmation. Each section follows the Decision / Rationale /
Alternatives format. Findings here feed directly into data-model.md,
contracts/, and tasks.md.

---

## R1. 9-family taxonomy — does it cover everything?

**Decision**: Adopt the 9-family taxonomy as proposed in spec §Key
Entities: Boolean, Integer, Float, Decimal, Money, String, Binary,
DateTime, Uuid. **No "Misc" bucket needed.**

**Rationale**: A direct inspection of the 5 dispatch sites' `switch`
statements (see `src/tds/encoding/type_converter.cpp:261`,
`src/tds/encoding/bcp_row_encoder.cpp:83`,
`src/table_scan/filter_encoder.cpp:134`,
`src/dml/insert/mssql_value_serializer.cpp:288`,
`src/catalog/mssql_ddl_translator.cpp:352`) confirms every currently-
dispatched `LogicalTypeId` / `TdsTypeId` maps to one of the 9 families
without ambiguity:

| Family   | TDS wire types (scan decode dispatch)                        | DuckDB LogicalTypeId (BCP/literal/DDL dispatch)         |
|----------|--------------------------------------------------------------|---------------------------------------------------------|
| Boolean  | `TDS_TYPE_BIT`, `TDS_TYPE_BITN`                              | `BOOLEAN`                                               |
| Integer  | `TDS_TYPE_TINYINT`, `_SMALLINT`, `_INT`, `_BIGINT`, `_INTN`  | `TINYINT`..`UBIGINT`, `HUGEINT` (see note)              |
| Float    | `TDS_TYPE_REAL`, `_FLOAT`, `_FLOATN`                         | `FLOAT`, `DOUBLE`                                       |
| Decimal  | `TDS_TYPE_DECIMAL`, `_NUMERIC`                               | `DECIMAL`                                               |
| Money    | `TDS_TYPE_MONEY`, `_SMALLMONEY`, `_MONEYN`                   | (none — Money is a SQL-Server-only TDS-side family;      |
|          |                                                              | DuckDB has no MONEY type, so it appears on scan-decode  |
|          |                                                              | only)                                                   |
| String   | `TDS_TYPE_BIGCHAR`, `_BIGVARCHAR`, `_NCHAR`, `_NVARCHAR`, `_XML` | `VARCHAR` (and XML alias)                            |
| Binary   | `TDS_TYPE_BIGBINARY`, `_BIGVARBINARY`                        | `BLOB`                                                  |
| DateTime | `TDS_TYPE_DATE`, `_TIME`, `_DATETIME`, `_SMALLDATETIME`, `_DATETIME2`, `_DATETIMEN`, `_DATETIMEOFFSET` | `DATE`, `TIME`, `TIMESTAMP`, `TIMESTAMP_NS`, `_MS`, `_SEC`, `TIMESTAMP_TZ` |
| Uuid     | `TDS_TYPE_UNIQUEIDENTIFIER`                                  | `UUID`                                                  |

**Notes on edge placements**:
- **HUGEINT lives in Integer**, even though SQL Server can't represent
  it natively. `MapTypeToSQLServer` returns `DECIMAL(38,0)`;
  `MapLogicalTypeToCTAS` throws. `EncodeToBcp` would route to the
  Decimal family on the wire, so Integer family's BCP path
  forwards explicitly to `codec::decimal::EncodeToBcp` with width 38,
  scale 0. This forwarding is documented in `integer_codec.cpp`.
- **Money has no DuckDB-side family member.** SQL Server `MONEY` /
  `SMALLMONEY` come over the wire as Money TDS types but decode into
  DuckDB `DECIMAL(19,4)` / `DECIMAL(10,4)`. On encode, DuckDB DECIMAL
  is routed to the Decimal family (not Money). So Money's
  `EncodeToBcp` / `FormatSqlLiteral` / `FormatDdlTypeName` are
  defined but called only via Decimal's forwarding (or via direct
  call sites that name Money explicitly — none exist today).
  **Implementation simplification**: Money implements only
  `DecodeFromTds`; the other three operations are stubbed with
  `static_assert(false, "Money is scan-decode-only")` or omitted.
  This is consistent with the spec's FR-002 wording "Each family
  implements only the operations it owns".
- **XML maps into String family** as documented in spec edge cases.
- **INTERVAL** appears only in `MapTypeToSQLServer` (returns
  `NVARCHAR(100)`) — no other dispatch site handles it. Decision:
  INTERVAL stays in `MapTypeToSQLServer`'s default tail OR goes into
  a tiny **Interval** sub-namespace inside the String family module.
  Recommendation: keep it in String (it's stored as `NVARCHAR(100)`)
  with an explicit `// INTERVAL — SQL Server has no interval type, store as string` comment.

**Alternatives considered**:
- **8 families, fold Money into Decimal**: rejected because the wire
  encoding is different (Money is fixed-point with scale=4 hardcoded;
  Decimal is parameterized). Distinct family makes the scan-decode
  code clearer.
- **10 families, split String into Narrow/Wide (VARCHAR vs NVARCHAR)**:
  rejected. DuckDB unifies all character data under `VARCHAR`; the
  narrow-vs-wide branch happens inside `string_codec.cpp` based on
  TDS type (scan side) or DDL config (`text_type` for CTAS). One
  family with internal branches reads more naturally than two
  families with shared helpers.

---

## R2. Namespace and file naming

**Decision**: `namespace duckdb::mssql::codec::<family>` for per-family modules.
Files: `src/codec/<family>_codec.{cpp,hpp}`, with headers at
`src/include/codec/<family>_codec.hpp`. Shared headers at
`src/include/codec/{type_family,literal_context,literal_format}.hpp`.

**Rationale**: Consistent with CLAUDE.md "Namespace Prefix Rule" — names
inside `duckdb::mssql::codec::*` are already scoped, so no `MSSQL` prefix is
needed on classes/functions. The `_codec.cpp` suffix on per-family
files (rather than just `integer.cpp`) reads more clearly when these
files appear in stack traces and grep output, and matches the existing
convention of `type_converter.cpp` / `bcp_row_encoder.cpp` in the
sibling encoding directory.

**Alternatives considered**:
- `namespace duckdb::mssql::codec::<family>`: rejected. The existing
  `duckdb::mssql::*` namespace holds extension-specific runtime
  utilities (pool, config). The codec layer is more general — it could
  in principle be reused for another extension targeting any SQL
  dialect. The shorter `duckdb::codec` namespace expresses this
  cleanly and matches `duckdb::tds::encoding`'s precedent for
  "no prefix when scoped".
- File names `integer.cpp` / `decimal.cpp` (no suffix): rejected.
  Ambiguous against potential future `src/codec/integer_helpers.cpp`
  utility files. The `_codec.cpp` suffix is unambiguous and short.

---

## R3. `FamilyFromTdsType` and `FamilyFromLogicalType` content

**Decision**: Both dispatch helpers are pure-function `switch`
statements (no std::map / no init-order pitfalls). Their contents
are mechanical translations of the 5 sites' existing dispatch tables.

**Rationale**: A `switch` on enum is the simplest correct form. No
runtime registry needed; mappings are compile-time constants. The
helpers live in `src/codec/type_family.cpp` and are inlined into the
5 dispatch sites at LTO time (or near-inlined at -O2).

**Helper signatures** (frozen for the contracts/ phase):

```cpp
namespace duckdb::codec {

enum class TypeFamily {
    Boolean,
    Integer,
    Float,
    Decimal,
    Money,
    String,
    Binary,
    DateTime,
    Uuid,
};

// Used by scan decode (src/tds/encoding/type_converter.cpp).
// max_length is the COLMETADATA length byte; not used by every family
// but available for those that need it (currently: Integer for INTN
// sizing — but that's resolved inside Integer::DecodeFromTds via
// value.size(), not by this helper).
TypeFamily FamilyFromTdsType(uint8_t tds_type_id);

// Used by BCP encode, literal format (filter + INSERT), DDL.
TypeFamily FamilyFromLogicalType(const LogicalType &type);

}  // namespace duckdb::codec
```

The `max_length` parameter is **dropped** from `FamilyFromTdsType` —
inspection showed the existing dispatch in `type_converter.cpp:ConvertValue`
does not use it at the family-selection level. Variable-length integer
sizing happens INSIDE `Integer::DecodeFromTds` via `value.size()`.
This simplifies the API.

**Alternatives considered**:
- `std::unordered_map<uint8_t, TypeFamily>` registry: rejected — adds
  std lib runtime overhead, init-order subtlety, and gives no
  semantic benefit over a `switch`.
- A trait-based template approach: rejected — adds compile-time
  complexity for no maintainability benefit.

---

## R4. `LiteralContext` divergence catalog

**Decision**: Two values — `Filter` and `InsertValues`. Family modules
dispatch on this enum ONLY where the two contexts genuinely differ.
For all other types, both contexts produce identical output.

**Rationale**: From inspection of `filter_encoder.cpp:ValueToSQLLiteral`
(70 LOC) vs `mssql_value_serializer.cpp:Serialize` (100 LOC), the
divergence is narrower than spec.md suggested. The actual differences:

| Type            | Filter behavior                                  | InsertValues behavior                              | Where divergence comes from                    |
|-----------------|--------------------------------------------------|----------------------------------------------------|------------------------------------------------|
| BOOLEAN         | `"1"` / `"0"`                                    | `"1"` / `"0"` (via `SerializeBoolean`)             | identical                                      |
| TINYINT..BIGINT | `value.ToString()`                               | `SerializeInteger(...)` → printf-style decimal     | identical bit-for-bit (verified)               |
| UTINYINT..UBIGINT | `value.ToString()`                             | `SerializeInteger(static_cast<int64_t>(...))`      | identical (both render unsigned as decimal)    |
| HUGEINT         | falls through to N'…' default (**BUG**)          | `SerializeDecimal(hugeint_t, 38, 0)`               | divergent — filter is wrong                    |
| FLOAT/DOUBLE    | `value.ToString()`                               | `SerializeFloat` / `SerializeDouble` (printf %.9g) | possibly divergent on edge values              |
| DECIMAL         | `value.ToString()` — ignores internal storage    | `SerializeDecimal(...)` — PhysicalType-aware       | divergent — filter loses precision/scale info  |
| VARCHAR         | `"N'" + EscapeStringLiteral(value.ToString()) + "'"` | `SerializeString(StringValue::Get(value))`     | identical for typical input                    |
| BLOB            | hex string `"0x...."` via snprintf %02X          | `SerializeBlob(StringValue::Get(value))` (likely also hex) | confirm during String/Binary migration |
| UUID            | `"'" + value.ToString() + "'"`                   | `SerializeUUID(hugeint_t)` — likely formatted with dashes | confirm during Uuid migration |
| DATE            | `"'" + Date::ToString(...) + "'"`                | `SerializeDate(DateValue::Get(value))`             | identical bit-for-bit                          |
| TIME            | `"'" + value.ToString() + "'"`                   | `SerializeTime(TimeValue::Get(value))`             | identical                                      |
| TIMESTAMP*      | `"'" + Timestamp::ToString(...) + "'"`           | `SerializeTimestamp(...)`                          | identical                                      |
| TIMESTAMP_TZ    | quoted `Timestamp::ToString(ts_val)` — **uses ts as-is** | `SerializeTimestampTZ(ts, 0)` — passes offset=0 explicitly | semantically identical (both pass UTC) but the explicit offset=0 in InsertValues should be the canonical form |

**Critical findings**:

1. **filter_encoder.cpp lacks an explicit HUGEINT case.** Pre-spec-045
   HUGEINT filters fall through to the `default` arm and get rendered
   as `N'<hugeint-as-string>'` — which SQL Server interprets as a
   string literal, not a number. This is an existing bug. Spec 045's
   literal_format consolidation will surface this by giving HUGEINT
   one identical handler for both contexts → correct
   `SerializeDecimal(value, 38, 0)`. **This is a second behavior change
   beyond issue #91.**
2. **filter_encoder.cpp doesn't preserve DECIMAL internal storage.**
   Calling `value.ToString()` on a `DECIMAL(10,2)` Value with internal
   storage `int64_t = 1000` may or may not render as `"10.00"` —
   depends on DuckDB's `Value::ToString` for DECIMAL. The
   `mssql_value_serializer.cpp` path uses `GetValueUnsafe<T>()` per
   `PhysicalType` to get the raw scaled integer, then formats. This
   guarantees fidelity. After consolidation, both paths use the
   InsertValues approach.

**Scope question**: do these two behavior changes (HUGEINT + DECIMAL in
filter pushdown) constitute scope creep beyond "byte-identical refactor
+ issue #91"?

**Decision**: Yes, treat them as the third and fourth in-scope behavior
changes — but with a guardrail. Add to FR-020 explicitly:

> FR-020 amendment: The 5 dispatch sites' observable behavior MUST be
> byte-identical to `main`-at-kickoff EXCEPT:
> (a) FR-023 / issue #91 NVARCHAR length validation;
> (b) HUGEINT filter literal — was `N'<hugeint>'` (a string), now
>     `<hugeint>` (a number) via `SerializeDecimal(value, 38, 0)`;
> (c) DECIMAL filter literal — was `value.ToString()` (loses internal
>     storage info on some platforms), now uses the InsertValues
>     PhysicalType-aware path.

Both (b) and (c) are **correctness fixes** that match `Correctness over
Convenience` (principle III). They surface as a free-and-natural
consequence of literal_format consolidation. **Action**: capture as
two new SQL regression tests in `test/sql/catalog/filter_pushdown_hugeint.test`
and `test/sql/catalog/filter_pushdown_decimal.test`, plus mention in
spec.md as a P1 follow-up clarification before `/speckit-implement`.

(The spec.md update for FR-020 / SC-002a wording is a minor edit that
falls in the `/speckit-analyze` phase, not Phase 0 of this plan. Plan
locks the design; analyze validates the spec's wording matches it.)

---

## R5. Issue #91 root-cause investigation strategy

**Decision**: Defer the actual root-cause hunt to the String family
migration phase (Phase ≈8 per spec.md's order). Phase 0 only confirms
the bug is real and locates the candidate surfaces.

**Findings from Phase 0 inspection**:

- `bcp_row_encoder.cpp:397 EncodeNVarchar` writes the UTF-16LE byte
  length as a USHORT after computing it (via `Utf16LEEncodeDirect`).
  No upfront validation against `col.max_length`. If utf16_len exceeds
  65535 (i.e., a string > 32767 UCS-2 code units encoded into a
  non-PLP NVARCHAR column), the high bits silently truncate. This is
  one candidate bug surface, but it's a different bug from issue #91
  (issue #91's reproducer fits in 32K code units).
- `bcp_row_encoder.cpp:158-166 LogicalTypeId::VARCHAR` dispatches to
  `EncodeNVarchar` (non-PLP) when `!col.IsPLPType()`. For
  `nvarchar(500)` columns, this is the path. No length-vs-capacity
  check.
- Issue #91 reports a **server-side rejection** ("Received an invalid
  column length from the bcp client for colid N"). That's SQL Server
  saying the cell length we wrote DOES NOT match what COLMETADATA
  promised. So the bug is **either**:
  - we wrote the wrong cell length (too high — exceeds `col.max_length`
    we declared), **or**
  - we wrote the right cell length but COLMETADATA itself was wrong
    (declared smaller `max_length` than the actual content).

- The COLMETADATA construction lives in `src/copy/target_resolver.cpp`
  (TBD path; confirm during Phase 0 of implementation) and reads
  `max_length` from `sys.columns`. For `nvarchar(500)`,
  `sys.columns.max_length` is **1000 bytes** (UTF-16LE encoding). So
  COLMETADATA should declare 1000. Issue #91's failing case has UTF-8
  input ~750 bytes, which decodes to 500 UCS-2 = 1000 UTF-16LE bytes.
  This fits in the declared 1000.
- **Hypothesis**: somewhere in the cell-length writing path, we're
  comparing UTF-8 byte length against the column declared length
  (1000), which is wrong: 750 < 1000, looks fine, but then we write
  utf16_len = 1000 as the cell prefix, and SQL Server compares cell
  prefix 1000 against ??? the wire stream state, finds a mismatch.
  Investigation during String family migration: trace the bytes
  written into the buffer for the failing reproducer and diff against
  the buffer SQL Server expects.

**Decision**: write `specs/045-type-codec-consolidation/issue_91_repro.md`
with the failing-binary trace at the start of Phase 8 (String family
migration). Include captured byte sequences pre- and post-fix as
SC-002a evidence.

---

## R6. Golden-fixture capture strategy

**Decision**: Capture fixtures **per family**, **at the start of each
family's migration phase** — not all upfront. Capture point is the
last commit on `main` before that family's migration commit. Stored
under `specs/045-type-codec-consolidation/golden/<family>/`.

**Rationale**: Capturing all 9 families' fixtures at Phase 0 means
re-capturing if any family migration also lands a bug fix elsewhere
(unlikely but possible during the 3-6 week scope). Per-family capture
keeps the baseline traceable to one specific commit per family.

**Fixture format**: each family directory contains:

```text
golden/integer/
├── decode_from_tds.txt        # one line per fixture: <hex input> => <DuckDB Value::ToString()>
├── encode_to_bcp.txt          # one line per fixture: <DuckDB Value as text> => <hex output bytes>
├── format_sql_literal.txt     # one line per fixture: <Value> [Filter|InsertValues] => <literal string>
└── format_ddl_type_name.txt   # one line per fixture: <LogicalType ToString> => <T-SQL type name>
```

Fixtures are committed alongside the family migration PR (or commit).
The family's `test_<family>_codec.cpp` test loads its fixture files
and asserts byte-identical output. This is the regression guard for
SC-002 / SC-003.

**Alternatives considered**:
- Binary `.bin` fixtures: rejected — hex-text in `.txt` is greppable,
  diff-readable in PRs, and adequate for the small scale (~20-50
  fixtures per family).
- Property-based / randomized testing: rejected for this spec — would
  obscure the byte-identical guarantee, which is the explicit
  contract. Property tests can come in a follow-up spec.

---

## R7. `EstimateSerializedSize` placement (FR-014)

**Decision**: Move `EstimateSerializedSize` (currently in
`mssql_value_serializer.cpp`) into the per-family modules as
`codec::<family>::EstimateLiteralSize(const LogicalType &type)` →
`size_t`. The shared `literal_format.cpp` exposes a dispatching
wrapper `codec::EstimateLiteralSize(type)` analogous to
`codec::FormatSqlLiteral`.

**Rationale**: `EstimateSerializedSize` IS per-type logic; keeping it
in the dispatch site while every other per-type operation moves out
would be inconsistent. The move costs ~40 LOC of mechanical
distribution and gains consistency.

**Alternative considered** (and rejected): leave it where it is — spec
FR-014 marks this as discretionary. But the per-family alternative
keeps the layout principle ("all per-type logic for type X lives in
codec/X_codec.cpp") clean. Final call deferred to the implementer at
Phase 2 — but the design recommends moving.

---

## R8. DDL: two mappers, not one (refines FR-013)

**Decision**: Both `MapTypeToSQLServer` (general DDL) and
`MapLogicalTypeToCTAS` (CTAS-specific) are consolidated into one
`codec::<family>::FormatDdlTypeName(const LogicalType &type,
DdlContext ctx)`. Add a new enum:

```cpp
enum class DdlContext { CreateTable, CtasCreateTable };
```

In `CtasCreateTable` context: HUGEINT throws `NotImplementedException`;
TIMESTAMP renders as `DATETIME2(7)`; VARCHAR consults `CTASConfig::text_type`.
In `CreateTable` context: HUGEINT → `DECIMAL(38,0)`; TIMESTAMP → `DATETIME2(6)`;
VARCHAR → `NVARCHAR(MAX)` always; INTERVAL → `NVARCHAR(100)`.

**Rationale**: The two existing mappers in `mssql_ddl_translator.cpp`
(lines 78-162 and 351-?) duplicate ~80% of their bodies. Their
divergences are real and intentional (CTAS is stricter, refuses
HUGEINT entirely; general DDL is permissive). Encoding the divergences
as `DdlContext` arms makes them explicit and inspectable, mirroring
the `LiteralContext` design.

**Scope impact**: this is a small expansion vs. spec.md's FR-013 (which
named only `MapLogicalTypeToCTAS`). Spec needs a one-line tweak via
`/speckit-analyze`: FR-013 becomes "MapLogicalTypeToCTAS AND
MapTypeToSQLServer MUST be rewritten...". No additional phases or
families; just both call sites of the same `FormatDdlTypeName`.

**`CTASConfig` parameter**: `FormatDdlTypeName` takes a
`DdlConfig` reference (analogous to `CTASConfig`) that the family
module can probe for VARCHAR's `text_type` etc. For non-CTAS context,
`DdlConfig` is a default-constructed empty struct (`text_type` field
unused). The implementation chooses whether `DdlConfig` is a typedef
of `CTASConfig` or a new struct — research recommendation: keep
`CTASConfig` as the type and add a new field for context disambig
where needed. Either way, this is `contracts/` phase work.

---

## R9. Performance bench strategy (SC-008)

**Decision**: Re-use spec 044's `test/bench/bench_codec_e2e.sh` at
1M rows for the regression gate. Run before Phase 1 (capture
baseline-at-spec-045-kickoff), then after Phase 11 audit (capture
post-migration). Commit both numbers in
`specs/045-type-codec-consolidation/bench_results.md`. SC-008 gate:
≤ 5% wall-clock regression on any of the 6 steps.

**Rationale**: The spec-044 bench already exercises the BCP encode
path (Step 5: COPY FROM duckdb TO mssql) and the scan decode path
(Step 6: SELECT FROM mssql). It's the right harness. No new bench
infrastructure required.

**Note on noise**: spec 044's bench results showed ascii_64k fixture
± 15% variance across runs. SC-008's 5% threshold may be tight on
that fixture. **Resolution**: report the 5% gate against the **min**
wall-clock across 3 runs (not the median or mean) — same protocol as
the spec-044 microbench. If the post-migration min is within 5% of
the pre-migration min, the gate passes.

---

## R10. Spec 042 / spec 044 coordination

**Decision**: No coordination needed. Spec 042 (Integrated
Authentication) is merged. Spec 044 (UTF-16 codec) is merged in #109.
Spec 045's branch was rebased onto post-#109/#98 main earlier this
session — the current `045-type-codec-consolidation` branch tip
(commit `ca17181`) sees both as upstream. Future rebases (none
expected) would be trivial since 045 doesn't touch `src/tds/auth/`
or `src/tds/encoding/utf16.{cpp,hpp}`.

---

## Summary of design decisions

| # | Decision                                                                          |
|---|-----------------------------------------------------------------------------------|
| R1 | 9-family taxonomy validated; Money is scan-decode-only; HUGEINT in Integer family with Decimal-forward; XML aliases to String. |
| R2 | `namespace duckdb::mssql::codec::<family>`, files `src/codec/<family>_codec.{cpp,hpp}` and `src/include/codec/<family>_codec.hpp`. |
| R3 | `FamilyFromTdsType(uint8_t)` and `FamilyFromLogicalType(const LogicalType&)` — both pure-function switches. No `max_length` parameter needed. |
| R4 | `LiteralContext { Filter, InsertValues }` — divergence narrower than spec implied; HUGEINT and DECIMAL filter literals become correctness fixes (add to FR-020 amendment). |
| R5 | Issue #91 root-cause hunt deferred to String family migration (Phase 8); pre/post byte trace committed as SC-002a evidence. |
| R6 | Golden fixtures captured per family at start of each migration phase, stored as `.txt` files under `specs/045-type-codec-consolidation/golden/<family>/`. |
| R7 | Move `EstimateSerializedSize` into per-family modules as `EstimateLiteralSize`; expose dispatching wrapper in `literal_format.{hpp,cpp}`. |
| R8 | DDL is TWO mappers, not one (`MapTypeToSQLServer` + `MapLogicalTypeToCTAS`); consolidate via `DdlContext { CreateTable, CtasCreateTable }` — refines FR-013. |
| R9 | Re-use spec 044's e2e bench at 1M rows for SC-008 ≤5% regression gate; report min of 3 runs. |
| R10 | No coordination with spec 042 / spec 044; branch already rebased onto current main. |

These decisions are inputs to data-model.md (entities + relationships)
and contracts/ (header signatures).

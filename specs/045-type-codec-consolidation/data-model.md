# Data Model: Type Codec Consolidation (spec 045) — Phase 1

This spec adds no runtime entities (no persistent state, no schemas).
"Data model" here means the **logical entities of the codec layer**:
type families, the dispatch helpers that route to them, and the
contexts that parameterize their cold-path operations.

---

## Entity: `TypeFamily`

**Definition**: A compile-time enum partitioning all
encoder/decoder/formatter-relevant types into 9 groups that share a
single per-type-family code module.

**Header**: `src/include/codec/type_family.hpp`

```cpp
namespace duckdb::codec {

enum class TypeFamily : uint8_t {
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

}  // namespace duckdb::codec
```

**Membership** (from research.md R1):

| TypeFamily | TDS wire types (scan decode)                                                          | DuckDB LogicalTypeId (BCP/literal/DDL)                |
|------------|--------------------------------------------------------------------------------------|--------------------------------------------------------|
| Boolean    | `TDS_TYPE_BIT (0x32)`, `TDS_TYPE_BITN (0x68)`                                        | `BOOLEAN`                                              |
| Integer    | `TDS_TYPE_TINYINT (0x30)`, `SMALLINT (0x34)`, `INT (0x38)`, `BIGINT (0x7F)`, `INTN (0x26)` | `TINYINT`..`UBIGINT`, `HUGEINT` (forwards to Decimal for BCP/DDL) |
| Float      | `TDS_TYPE_REAL (0x3B)`, `FLOAT (0x3E)`, `FLOATN (0x6D)`                              | `FLOAT`, `DOUBLE`                                      |
| Decimal    | `TDS_TYPE_DECIMAL (0x37)`, `NUMERIC (0x3F)`, `DECIMALN (0x6A)`, `NUMERICN (0x6C)`    | `DECIMAL`                                              |
| Money      | `TDS_TYPE_MONEY (0x3C)`, `SMALLMONEY (0x7A)`, `MONEYN (0x6E)`                        | (none — scan-decode only)                              |
| String     | `TDS_TYPE_BIGCHAR (0xAF)`, `BIGVARCHAR (0xA7)`, `NCHAR (0xEF)`, `NVARCHAR (0xE7)`, `XML (0xF1)` | `VARCHAR`                                     |
| Binary     | `TDS_TYPE_BIGBINARY (0xAD)`, `BIGVARBINARY (0xA5)`                                   | `BLOB`                                                 |
| DateTime   | `TDS_TYPE_DATE (0x28)`, `TIME (0x29)`, `DATETIME (0x3D)`, `SMALLDATETIME (0x3A)`, `DATETIME2 (0x2A)`, `DATETIMEN (0x6F)`, `DATETIMEOFFSET (0x2B)` | `DATE`, `TIME`, `TIMESTAMP`, `TIMESTAMP_NS`, `TIMESTAMP_MS`, `TIMESTAMP_SEC`, `TIMESTAMP_TZ` |
| Uuid       | `TDS_TYPE_UNIQUEIDENTIFIER (0x24)`                                                   | `UUID`                                                 |

**Invariants**:
- The 9 families are mutually exclusive (every TDS type and every
  supported DuckDB type maps to exactly one family).
- Adding a new SQL Server type or DuckDB type requires adding it to
  one family's switch arm in `FamilyFromTdsType` / `FamilyFromLogicalType`
  AND updating that family's `.cpp` (one file edit per direction).
- `INTERVAL` is handled inside String family for DDL only (returns
  `NVARCHAR(100)` per current `MapTypeToSQLServer` behavior). It has
  no scan decode / BCP encode / literal format support (would error
  out the same way it does today).

---

## Entity: `LiteralContext`

**Definition**: A 2-value enum that distinguishes the two contexts in
which T-SQL literal text is formatted: WHERE-clause filter pushdown
vs INSERT statement VALUES.

**Header**: `src/include/codec/literal_context.hpp`

```cpp
namespace duckdb::codec {

enum class LiteralContext : uint8_t {
    Filter,         // for T-SQL WHERE clauses (filter pushdown)
    InsertValues,   // for T-SQL INSERT (...) VALUES (...) bodies
};

}  // namespace duckdb::codec
```

**Divergence catalog** (from research.md R4): the two contexts produce
DIFFERENT output for these types:

| Type        | Filter output (pre-spec-045)                  | InsertValues output (pre-spec-045)        | Post-spec-045 unified rule                                       |
|-------------|-----------------------------------------------|-------------------------------------------|------------------------------------------------------------------|
| HUGEINT     | `N'<value>'` (bug — quoted as string)         | unquoted decimal via `SerializeDecimal(v,38,0)` | both use unquoted decimal (correctness fix)               |
| DECIMAL     | `value.ToString()` (loses internal storage)   | PhysicalType-aware via `GetValueUnsafe<T>()` | both use the PhysicalType-aware path (correctness fix)        |
| BLOB        | `0x<hex>` (snprintf)                          | `SerializeBlob(...)` (likely identical hex) | one `codec::binary::FormatSqlLiteral` impl used by both       |
| VARCHAR     | `N'<escaped>'` (via `EscapeStringLiteral`)    | `SerializeString(...)`                    | both produce `N'<escaped>'`; escape helper moves into String   |
| TIMESTAMP_TZ | quoted `Timestamp::ToString(ts)` (implicit UTC) | `SerializeTimestampTZ(ts, 0)` (explicit offset 0) | both pass offset=0 explicitly                            |

For all other types, the two contexts produce identical output and
the family's `FormatSqlLiteral` ignores its `LiteralContext` parameter.

**Invariant**: family modules that don't use `LiteralContext` SHOULD
declare the parameter but mark it `(void)ctx;` or `[[maybe_unused]]`
to make the no-divergence intent explicit. This is documented in
contracts/literal_format.hpp.

---

## Entity: `DdlContext`

**Definition**: A 2-value enum nominally distinguishing general
CREATE TABLE type-name mapping from CREATE TABLE AS SELECT type-name
mapping. **Post-spec-045 both contexts produce byte-identical output
for the same (LogicalType, CTASConfig) inputs** (FR-028); the enum is
retained for API uniformity and future per-context DDL hints (e.g.,
identity columns, partition columns).

**Header**: `src/include/codec/type_family.hpp` (co-located with
`TypeFamily`; small enough to share the header)

```cpp
namespace duckdb::codec {

enum class DdlContext : uint8_t {
    CreateTable,        // general DDL (MapTypeToSQLServer)
    CtasCreateTable,    // CTAS path (MapLogicalTypeToCTAS)
};

}  // namespace duckdb::codec
```

**Pre-spec-045 divergences (reconciled in spec 045)** (from
research.md R8):

| Type        | Pre-spec-045 CreateTable               | Pre-spec-045 CtasCreateTable                     | **Post-spec-045 (both)**                         |
|-------------|----------------------------------------|--------------------------------------------------|--------------------------------------------------|
| HUGEINT     | `"DECIMAL(38,0)"`                      | throws (`CTAS does not support DuckDB type HUGEINT`) | `"DECIMAL(38,0)"` + runtime overflow warning |
| UHUGEINT    | falls through to `default:` throw      | throws (explicit message)                         | `"DECIMAL(38,0)"` + runtime overflow warning |
| TIMESTAMP   | `"DATETIME2(6)"`                       | `"DATETIME2(7)"`                                  | `"DATETIME2(6)"` (exact match to DuckDB μs)  |
| TIMESTAMP_MS  | falls through to `default:` throw    | falls through to `default:` throw                 | `"DATETIME2(3)"` (new per-precision arm)     |
| TIMESTAMP_NS  | falls through to `default:` throw    | falls through to `default:` throw                 | `"DATETIME2(7)"` (closest fit; lossy 2 digits)|
| TIMESTAMP_SEC | falls through to `default:` throw    | falls through to `default:` throw                 | `"DATETIME2(0)"` (new per-precision arm)     |
| TIMESTAMP_TZ  | `"DATETIMEOFFSET(7)"`                | `"DATETIMEOFFSET(7)"`                             | `"DATETIMEOFFSET(7)"` (unchanged)            |
| VARCHAR     | `"NVARCHAR(MAX)"` (hard-coded)         | `config.text_type == VARCHAR ? "VARCHAR(MAX)" : "NVARCHAR(MAX)"` | `cfg.text_type == VARCHAR ? "VARCHAR(MAX)" : "NVARCHAR(MAX)"` (both contexts consult config) |
| INTERVAL    | `"NVARCHAR(100)"` (lossy fallback)     | throws (explicit message)                          | `"NVARCHAR(50)"` (canonical string form fits)|

All other types (BOOLEAN, signed/unsigned integers TINYINT..BIGINT,
UBIGINT, FLOAT, DOUBLE, DECIMAL, BLOB, DATE, TIME, UUID) render
identically across both contexts in both pre- and post-spec-045 code.

**Runtime overflow policy for HUGEINT/UHUGEINT (FR-025)**:
`codec::decimal::EncodeToBcp` detects values exceeding the
DECIMAL(38,0) representable range (±10^38 − 1), writes a saturated
value (clamped to the range boundary), and emits a stderr warning
tagged `[MSSQL CODEC] HUGEINT overflow: column <name>, value
<hugeint>`. This is the "log-and-continue" policy — failing the
whole batch on a single overflow row would be more disruptive than
truncating one cell.

---

## Entity: `CTASConfig` / `DdlConfig`

**Definition**: Configuration struct already exists at
`src/include/dml/mssql_ctas_config.hpp` (or similar). Spec 045 **does
not introduce a new struct**; it reuses `CTASConfig` and passes it to
`FormatDdlTypeName` in BOTH DDL contexts (post-spec-045 the general
DDL path also honors the config — see FR-027 / FR-028).

**Used field**:
- `text_type` (enum `CTASTextType { NVARCHAR, VARCHAR }`) — consulted
  by `codec::string::FormatDdlTypeName` in BOTH DDL contexts. Default
  is `NVARCHAR` (Unicode safe).

For callers of `MapTypeToSQLServer` that do not have a `CTASConfig`
in hand, the function constructs a default `CTASConfig{}` internally
before calling the family-module `FormatDdlTypeName`. This preserves
the public function signature (`string MapTypeToSQLServer(const
LogicalType&)`) while threading the unified config through the new
codec layer.

---

## Entity: Per-Family Codec Module

**Definition**: A self-contained `.cpp`+`.hpp` pair under `src/codec/`
exposing four free functions in `namespace duckdb::codec::<family>`.

**Naming**: `src/codec/<family>_codec.cpp`,
`src/include/codec/<family>_codec.hpp`. Family names are
lowercase: `boolean`, `integer`, `float`, `decimal`, `money`,
`string`, `binary`, `datetime`, `uuid`.

**Standard family interface** (canonical — see also `contracts/`):

```cpp
namespace duckdb::codec::<family> {

// Scan decode (per row, hot path). Fills one row of `out` from the
// raw TDS wire bytes. ColumnMetadata supplies precision/scale/scale/etc.
void DecodeFromTds(const std::vector<uint8_t> &bytes,
                   const tds::ColumnMetadata &col,
                   Vector &out, idx_t row);

// BCP encode (per row, hot path). Appends one cell's bytes to `buf`.
// BCPColumnMetadata supplies precision/scale/precision/max_length/PLP flag.
void EncodeToBcp(Vector &in, idx_t row,
                 const mssql::BCPColumnMetadata &col,
                 std::vector<uint8_t> &buf);

// T-SQL literal text (per statement, cold path).
std::string FormatSqlLiteral(const Value &v,
                             const LogicalType &type,
                             LiteralContext ctx);

// T-SQL type name (per statement, cold path). e.g. "INT", "DECIMAL(10,2)".
std::string FormatDdlTypeName(const LogicalType &type,
                              const mssql::CTASConfig &cfg,
                              DdlContext ctx);

// Optional helper (FR-014 / research.md R7).
size_t EstimateLiteralSize(const LogicalType &type);

}  // namespace duckdb::codec::<family>
```

**Cardinality of per-family ops actually implemented**:

| Family    | DecodeFromTds | EncodeToBcp | FormatSqlLiteral | FormatDdlTypeName | EstimateLiteralSize |
|-----------|:---:|:---:|:---:|:---:|:---:|
| Boolean   | ✅ | ✅ | ✅ | ✅ | ✅ |
| Integer   | ✅ | ✅ (incl. HUGEINT→Decimal forward) | ✅ | ✅ (incl. HUGEINT) | ✅ |
| Float     | ✅ | ✅ | ✅ | ✅ | ✅ |
| Decimal   | ✅ | ✅ | ✅ | ✅ | ✅ |
| Money     | ✅ (scan only) | — | — | — | — |
| String    | ✅ | ✅ (incl. **NVARCHAR length validation for issue #91**) | ✅ | ✅ (incl. INTERVAL→NVARCHAR(100)) | ✅ |
| Binary    | ✅ | ✅ | ✅ | ✅ | ✅ |
| DateTime  | ✅ | ✅ | ✅ | ✅ | ✅ |
| Uuid      | ✅ | ✅ | ✅ | ✅ | ✅ |

Money's empty cells reflect the asymmetry (no DuckDB MONEY type
exists; DuckDB-side ops always route through Decimal).

---

## Entity: Dispatch Helpers

**Definition**: Two pure-function helpers that map TDS wire types and
DuckDB LogicalTypeIds to TypeFamily.

**Header**: `src/include/codec/type_family.hpp`

```cpp
namespace duckdb::codec {

TypeFamily FamilyFromTdsType(uint8_t tds_type_id);
TypeFamily FamilyFromLogicalType(const LogicalType &type);

}  // namespace duckdb::codec
```

**Source**: `src/codec/type_family.cpp` — two `switch` statements,
~50 LOC total. Mechanical translation of the existing dispatch tables
in `type_converter.cpp` and the other 4 sites.

---

## Entity: Shared Literal Format Module

**Definition**: A thin dispatching wrapper that turns `(Value, LogicalType,
LiteralContext)` into a T-SQL literal string by routing to the right
family.

**Header**: `src/include/codec/literal_format.hpp`

```cpp
namespace duckdb::codec {

std::string FormatSqlLiteral(const Value &v,
                             const LogicalType &type,
                             LiteralContext ctx);

size_t EstimateLiteralSize(const LogicalType &type);  // optional

}  // namespace duckdb::codec
```

**Source**: `src/codec/literal_format.cpp` — body is one `switch
(FamilyFromLogicalType(type))` with 9 arms, each calling
`codec::<family>::FormatSqlLiteral(v, type, ctx)`. Total ~40 LOC.

**Consumers**:
- `src/table_scan/filter_encoder.cpp:ValueToSQLLiteral` → replaced by
  `codec::FormatSqlLiteral(value, type, LiteralContext::Filter)`.
- `src/dml/insert/mssql_value_serializer.cpp:Serialize` → replaced by
  `codec::FormatSqlLiteral(value, type, LiteralContext::InsertValues)`.

---

## Entity Relationships

```text
                ┌─────────────────────────┐
                │     TypeFamily (enum)   │
                └───────────┬─────────────┘
                            │ keys
                            ▼
   ┌──────────────────────────────────────────────────┐
   │  Per-Family Module  (one per family value)       │
   │  src/codec/<family>_codec.{cpp,hpp}              │
   │  namespace duckdb::codec::<family>               │
   │                                                  │
   │   DecodeFromTds  (hot, called per row)           │
   │   EncodeToBcp    (hot, called per row)           │
   │   FormatSqlLiteral   (cold, parameterized        │
   │                       by LiteralContext)         │
   │   FormatDdlTypeName  (cold, parameterized        │
   │                       by DdlContext + CTASConfig)│
   └─────────┬─────────────────┬──────────────────────┘
             │                 │
   ┌─────────┴────────┐  ┌─────┴───────────────────────────────┐
   │ Scan decode site │  │ BCP encode site                     │
   │ type_converter   │  │ bcp_row_encoder                     │
   │ dispatches via   │  │ dispatches via                      │
   │ FamilyFromTdsType│  │ FamilyFromLogicalType               │
   └──────────────────┘  └─────────────────────────────────────┘

   ┌──────────────────────────────────────────────────────────┐
   │ Literal Format Module     src/codec/literal_format.cpp   │
   │ namespace duckdb::codec                                  │
   │   FormatSqlLiteral(v, type, ctx)                         │
   │   dispatches via FamilyFromLogicalType(type)             │
   └──────────────┬────────────────────────┬──────────────────┘
                  │                        │
   ┌──────────────┴──────────────┐  ┌──────┴──────────────────────┐
   │ Filter pushdown site        │  │ INSERT VALUES site          │
   │ filter_encoder.cpp          │  │ mssql_value_serializer.cpp  │
   │ calls with                  │  │ calls with                  │
   │ LiteralContext::Filter      │  │ LiteralContext::InsertValues│
   └─────────────────────────────┘  └─────────────────────────────┘

   ┌──────────────────────────────────────────────────────────┐
   │ DDL site         src/catalog/mssql_ddl_translator.cpp    │
   │ MapTypeToSQLServer / MapLogicalTypeToCTAS both call      │
   │   codec::<family>::FormatDdlTypeName(type, cfg, ctx)     │
   │   dispatches via FamilyFromLogicalType(type)             │
   └──────────────────────────────────────────────────────────┘
```

---

## Validation rules (from spec FRs)

- **FR-001 / FR-040**: `src/codec/` is a NEW top-level source dir. ✅
  (no nesting under `src/tds/`).
- **FR-002**: Per-family modules expose the four standard ops as free
  functions in `namespace duckdb::codec::<family>`. ✅ (canonical
  interface above; Money is the only family that implements fewer than
  all four, by design).
- **FR-003**: `LiteralContext` enum defined in
  `src/include/codec/literal_context.hpp`. ✅
- **FR-004**: `TypeFamily` enum + `FamilyFromXxx` helpers defined in
  `src/include/codec/type_family.hpp`. ✅
- **FR-010..013**: All 5 dispatch sites' switches collapse to one
  arm per `TypeFamily`, each delegating to the family module. ✅
- **FR-014**: `EstimateSerializedSize` moves into per-family modules
  as `EstimateLiteralSize` (recommended per research.md R7). ✅
- **FR-020 (with R4 amendment)**: Byte-identical output EXCEPT for
  (a) FR-023 issue #91 fix; (b) HUGEINT filter literal correctness fix;
  (c) DECIMAL filter literal precision-preservation fix. The two new
  fixes are surfaced naturally by literal_format consolidation and are
  consistent with constitution principle III (Correctness over
  Convenience). They are captured as regression tests under
  `test/sql/catalog/filter_pushdown_*.test`.
- **FR-021**: Wire-protocol contracts unchanged for any input that
  succeeded pre-spec-045. ✅
- **FR-022**: Edge cases preserved per spec §Edge Cases. ✅
- **FR-023**: NVARCHAR length validation in `codec::string::EncodeToBcp`
  (issue #91). Root cause investigation deferred to Phase 8
  per research.md R5.
- **FR-030..033**: Test coverage per family in `test/cpp/codec/<family>`;
  issue #91 SQL test at `test/sql/copy/copy_nvarchar_length_validation.test`;
  golden fixtures captured per family at start of that family's
  migration phase per research.md R6.

---

## Out-of-scope entities (explicit)

- `TdsReader` / `TdsWriter` wrapper classes — **NO**. Per-row paths
  keep using `std::vector<uint8_t>&` directly (FR-021 in spec.md).
- `MssqlTypeCodec` virtual base class — **NO**. Rejected per spec.md
  §"What this spec is NOT".
- Batch APIs (`DecodeBatch(DataChunk&)`, `EncodeBcpBatch(...)`) — **NO**.
  Separate spec.
- A runtime `std::unordered_map<TypeFamily, …>` registry — **NO**.
  `switch` statements throughout.
- Type-mapping additions (UUID→UNIQUEIDENTIFIER round-trip with bit
  layout fix, HUGEINT→DECIMAL semantic refinements beyond throw-or-pass,
  TIMESTAMP_TZ→DATETIMEOFFSET fidelity round-trip) — **NO** for spec
  045. Each is a follow-up spec.

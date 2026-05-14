# Codec Layer Consolidation — Design Document

**Spec ID**: 044
**Target release**: hugr-lab/mssql-extension v0.2.0
**Scope**: §4.1 (Unified Type Codec Layer), §4.3 (VARIANT Fallback),
§4.7.5 (Type mapping holes)
**Source**: extracted from `refactoring-v0.2.md` for `.specify/` workflow input
**Dependencies**: spec 043 (Foundation fixes) provides the simdutf wrapper
**Consumers**: specs 045 (CTAS+TRUNCATE), 046 (BCP throughput), 047 (DML
performance) all consume the codec interface

---

## Context

The `hugr-lab/mssql-extension` is a DuckDB community extension providing
native TDS-protocol access to Microsoft SQL Server, Azure SQL, Azure
Synapse Dedicated/Serverless, and Microsoft Fabric Warehouse. As of
v0.1.18 type-dependent encoding/decoding logic is fragmented across
several files with divergent dispatch patterns (scan decode, BCP
encode, filter literal formatting, INSERT VALUES, CTAS DDL — five
separate switch-on-type sites). This spec consolidates everything
into one codec per data type, single source of truth for every
type-dependent decision.

This is the foundational refactor of v0.2.0. Specs 045, 046, and
047 all depend on the codec interface introduced here; they cannot
land until 044 is in.

This document is one of two standalone design extracts feeding the
`.specify/` workflow:

- **Companion** (`refactoring-foundation-043.md`): foundation
  fixes — simdutf migration + LOGIN7 password encoding fix.
  Provides the simdutf wrapper consumed by the string codec
  and (after this spec lands) by `sql_auth_strategy.cpp`,
  PRELOGIN server-name encoding, and other UTF-16 call sites.
- **This document** (`refactoring-codec-044.md`): full codec
  layer consolidation.

The full v0.2.0 refactor context lives in `refactoring-v0.2.md`;
this file is self-contained for the purpose of generating the
spec 044 `.specify/` artifacts (spec.md, plan.md, tasks.md,
research.md, quickstart.md, checklists/requirements.md) following
the existing template established by specs 001-041 in the repo.

---

## Spec 044 — Codec Layer

**`specs/044-codec-layer.md`** covers all of §4.1: interface
design, registry, all nine per-type codecs, removal of legacy
dispatch, and migration of every type-dependent call site to go
through codecs.

Why as one PR (not split into hot-path + SQL-generation): the
codec layer is internal infrastructure with no user-visible
boundary between partial states. Shipping "scan and BCP use
codecs but DDL, INSERT VALUES, and filter literals still use
ad-hoc dispatch" leaves the codebase in a state nobody wants to
maintain. The mechanical nature of the work (each codec is
move+restructure, not new design) keeps the PR reviewable —
each file is locally simple, the pattern repeats.

Scope:
- New `src/encoding/` directory with:
  - `type_codec.hpp` — interface with `DecodeBatch`,
    `EncodeBcpBatch`, `FormatSqlLiteral`, `AppendDdlColumnType`,
    plus metadata accessors.
  - `type_codec_registry.cpp` — factory from `TdsMetadata` /
    `LogicalType`.
  - `tds_reader.hpp` / `tds_writer.hpp` — abstractions over
    existing reader/writer infrastructure.
  - `utf_conversion.hpp` — shared simdutf wrappers, consumed by
    string codec and by `src/tds/auth/sql_auth_strategy.cpp`
    (LOGIN7 password encoding switches from spec 043's local fix
    to this shared utility).
  - Nine codec files: int / decimal / float / bool / string /
    binary / datetime / uuid / variant.
- Delete `src/tds/encoding/type_converter.cpp` and
  `bcp_row_encoder.cpp` after migration.
- Migrate `src/tds/encoding/{datetime,decimal,guid}_encoding.cpp`
  helpers into respective codec files.
- Replace ad-hoc type dispatch in:
  - `src/table_scan/filter_encoder.cpp` (pushdown WHERE literal
    formatting) → calls `FormatSqlLiteral`.
  - `src/dml/insert/mssql_value_serializer.cpp` (INSERT VALUES
    generation) → calls `FormatSqlLiteral`.
  - `src/dml/ctas/mssql_ctas_executor.cpp` (DDL type mapping)
    → calls `AppendDdlColumnType`.
- Close type-mapping holes from §4.7.5: UUID round-trip via
  `UNIQUEIDENTIFIER` (was VARCHAR), HUGEINT → `DECIMAL(38,0)`,
  `TIMESTAMP_TZ` → `DATETIMEOFFSET(6)`, nested types → JSON
  fallback, etc.

Depends on 043 (simdutf wrapper exists). Has light interaction
with 042 (Oluies' auth work touches `src/tds/auth/` — coordinate
to avoid merge conflicts on `sql_auth_strategy.cpp` when 044's
password-encoding cleanup lands).

Testable: full existing test suite (scan, BCP, INSERT, CTAS,
filter pushdown) passes unchanged. New targeted tests for
type-mapping holes that were previously broken (UUID round-trip,
HUGEINT values, TIMESTAMP_TZ, nested type CTAS). Benchmark: scan
throughput on NVARCHAR-heavy table should improve 15-25% from
simdutf alone.


---

## §4.1 Unified Type Codec Layer

#### 4.1.1 New directory layout

Current code has encoding logic spread across several files with
divergent dispatch patterns:

| File | LOC | Role today |
|------|-----|-----------|
| `src/tds/encoding/type_converter.cpp` | 515 | TDS wire → DuckDB Vector (scan decode) |
| `src/tds/encoding/bcp_row_encoder.cpp` | 758 | DuckDB Vector → BCP wire (insert encode) |
| `src/tds/encoding/datetime_encoding.cpp` | 163 | Date/time helpers, shared |
| `src/tds/encoding/decimal_encoding.cpp` | 51 | DECIMAL helpers, shared |
| `src/tds/encoding/guid_encoding.cpp` | 65 | UNIQUEIDENTIFIER helpers |
| `src/tds/encoding/utf16.cpp` | 340 | UTF-16 ↔ UTF-8 (superseded by simdutf, §4.2) |
| `src/table_scan/filter_encoder.cpp` | 982 | DuckDB filter → T-SQL WHERE clause (pushdown) |
| `src/dml/insert/mssql_value_serializer.cpp` | 466 | Value → T-SQL literal for INSERT VALUES |
| `src/dml/ctas/mssql_ctas_executor.cpp` | 567 | DuckDB type → T-SQL DDL type (CTAS) |

Every type-dependent decision happens in at least one of four
different dispatch points (scan decode, BCP encode, filter
literal formatting, DDL type generation), and value-to-SQL-literal
conversion exists in yet a fifth (INSERT VALUES formatting). The
refactor collapses this into one codec per data type:

```
src/encoding/
├── type_codec.hpp                   # Interface (below, §4.1.2)
├── type_codec_registry.cpp          # Factory by TdsMetadata / LogicalType
├── tds_reader.hpp / tds_reader.cpp  # Thin abstraction over tds_row_reader
├── tds_writer.hpp / tds_writer.cpp  # Abstraction over BCP packet writer
├── utf_conversion.hpp               # simdutf wrappers (§4.2)
└── codecs/
    ├── int_codec.cpp           # TINYINT, SMALLINT, INT, BIGINT
    ├── decimal_codec.cpp       # DECIMAL, NUMERIC, MONEY, SMALLMONEY
    ├── float_codec.cpp         # REAL, FLOAT
    ├── bool_codec.cpp          # BIT
    ├── string_codec.cpp        # CHAR, VARCHAR, NCHAR, NVARCHAR (incl. MAX/PLP)
    ├── binary_codec.cpp        # BINARY, VARBINARY (incl. MAX/PLP)
    ├── datetime_codec.cpp      # DATE, TIME, DATETIME, SMALLDATETIME,
    │                           # DATETIME2, DATETIMEOFFSET
    ├── uuid_codec.cpp          # UNIQUEIDENTIFIER
    └── variant_codec.cpp       # NEW: XML, UDT, SQL_VARIANT → DuckDB VARIANT
```

Existing call sites to migrate (all in spec 044):

**Encoding/decoding hot paths:**

- `src/tds/encoding/type_converter.cpp` — scan decode dispatch.
  **Deleted**; logic moved into per-type `DecodeBatch`.
- `src/tds/encoding/bcp_row_encoder.cpp` — BCP write encode
  dispatch. **Deleted**; logic moved into per-type `EncodeBcpBatch`.
- `src/tds/encoding/{datetime,decimal,guid}_encoding.cpp` —
  type-specific helpers. **Moved** into the respective
  `codecs/<type>_codec.cpp` files where they are used.
- `src/tds/encoding/utf16.cpp` — UTF-16 ↔ UTF-8 conversion.
  **Superseded** by simdutf (introduced in spec 043). Call sites
  moved to `utf_conversion.hpp`, including LOGIN7 password
  encoding in `src/tds/auth/sql_auth_strategy.cpp` (which uses
  the spec-043 local fix until 044 consolidates).

**SQL generation paths:**

- `src/table_scan/filter_encoder.cpp` — T-SQL literal formatting
  for pushdown WHERE clauses. **Call sites replaced** by invoking
  `FormatSqlLiteral` on the column's codec. The structural part
  (filter tree walking, function mapping) stays.
- `src/dml/insert/mssql_value_serializer.cpp` — value-to-T-SQL
  literal for INSERT VALUES. Type-specific logic **replaced** by
  `FormatSqlLiteral` on each column's codec. Identifier escaping
  (`EscapeIdentifier`, `EscapeString`) stays — not type-dependent.
- `src/dml/ctas/mssql_ctas_executor.cpp` — DuckDB→T-SQL DDL type
  mapping. **Replaced** by calls to `AppendDdlColumnType` on the
  codec. CTAS orchestration (DDL phase, INSERT/BCP phase) stays.

After spec 044, every type-dependent decision flows through
exactly one method on exactly one codec per type.

#### 4.1.2 Core interface

```cpp
// src/encoding/type_codec.hpp

class MssqlTypeCodec {
public:
    virtual ~MssqlTypeCodec() = default;

    // === Metadata ===
    virtual TdsTypeId TdsType() const = 0;
    virtual LogicalType DuckDbType() const = 0;
    virtual std::string TSqlTypeName() const = 0;   // "NVARCHAR(100)"
    virtual bool IsFixedLength() const = 0;
    virtual uint32_t MaxTdsWireSize() const = 0;    // for buffer sizing

    // === Scan: TDS → DuckDB ===
    // Batch is the primary API; row-level is a fallback for PLP/complex types.
    virtual void DecodeBatch(TdsReader& r, Vector& out, idx_t count) = 0;

    // === BCP write: DuckDB → TDS row stream ===
    virtual void EncodeBcpBatch(const Vector& in, idx_t count, TdsWriter& w) = 0;

    // === Literal formatting: DuckDB Value → T-SQL text ===
    // Used by filter pushdown and INSERT ... VALUES generation.
    virtual void FormatSqlLiteral(const Value& v, std::string& sql) = 0;

    // === DDL: LogicalType → "COLUMN_NAME TYPE [COLLATE ...]" ===
    virtual void AppendDdlColumnType(
        std::string& ddl,
        const DdlContext& ctx,
        const std::string& column_name
    ) const;  // default implementation = TSqlTypeName(); override for
              // DECIMAL (precision/scale), NVARCHAR (length), etc.
};
```

#### 4.1.3 Dispatch strategy

Two options considered:

**Option A: Template dispatch via `CodecKind` enum + switch.**
Compiler inlines hot path; zero virtual-call overhead. Pattern used
by DuckDB's own executor. Requires exhaustive switches in hot
functions, more boilerplate.

**Option B: Virtual calls, batch-only API.**
One virtual call per `DataChunk` (2048 rows); non-virtual tight loop
inside. Inlining opportunities via LTO. Simpler to maintain.

**Decision: start with Option B.** Measure. Only move to Option A
if benchmarks show `DecodeBatch`/`EncodeBcpBatch` dispatch as
bottleneck (unlikely for chunks of 2048).

#### 4.1.4 Fast paths within codecs

- **Fixed-width types on little-endian:** `IntCodec::DecodeBatch`
  can be `memcpy(FlatVector::GetData(out), reader.cursor, count*N)`
  plus null-bitmap handling. This is 10–50× faster than per-row
  read.
- **String types:** batch-gather UTF-16LE payloads into a
  contiguous staging buffer, call `simdutf` once per batch
  (see §4.2).
- **Dictionary vectors on BCP encode:** when DuckDB provides a
  dictionary vector (common for low-cardinality strings), encode
  each dictionary entry once to UTF-16, then per-row just copy the
  cached bytes by index.
- **Constant vectors:** encode once, repeat N times via memcpy loop.

#### 4.1.5 Registry

```cpp
class MssqlTypeCodecRegistry {
public:
    // Scan path: build codec from TDS COLMETADATA token
    static std::unique_ptr<MssqlTypeCodec> FromTdsMetadata(
        const TdsColumnMetadata& meta
    );

    // Write/DDL path: build codec from DuckDB LogicalType
    // Returns nullptr if type is not representable in SQL Server
    // (caller decides: error, NVARCHAR(MAX) JSON fallback, or skip).
    static std::unique_ptr<MssqlTypeCodec> FromDuckDbType(
        const LogicalType& type,
        const DdlContext& ctx
    );
};

struct DdlContext {
    MssqlTextType text_type;              // NVARCHAR | VARCHAR_UTF8
    std::string varchar_collation;
    uint32_t default_text_length;         // 0 = MAX
    std::unordered_map<std::string, std::string> column_overrides;
    uint32_t inferred_length;             // from optional pre-scan; 0 = no
};
```

#### 4.1.6 Migration plan

One type at a time, preserving behavior first, then optimizing:

1. Create scaffolding: interface, empty registry, `TdsReader`/`TdsWriter`.
2. Port `INTEGER` as the simplest type. Tests stay green.
3. Port `BIGINT`, `SMALLINT`, `TINYINT`, `BIT` — pattern reuse.
4. Port `REAL`, `FLOAT`.
5. Port `DECIMAL` (precision/scale handling).
6. Port `DATE`, `TIME` (trivial), then `DATETIME2` (scale),
   `DATETIMEOFFSET`, legacy `DATETIME`/`SMALLDATETIME`.
7. Port `UUID`.
8. Port `VARCHAR`/`NVARCHAR` bounded (incl. `CHAR`/`NCHAR`).
9. Port `VARCHAR(MAX)`/`NVARCHAR(MAX)` / `VARBINARY(MAX)` (PLP).
10. Remove old `src/tds/encoding/`. Single source of truth achieved.
11. Introduce `VariantCodec` (§4.3).
12. Pushdown literal generator migrated off its own switch to
    `FormatSqlLiteral`.
13. DDL generator migrated off its own switch to
    `AppendDdlColumnType`.

After each step, all existing tests must pass; no behavior
change is introduced in this phase.

---


---

## §4.3 VARIANT Fallback for Unsupported Types

This subsection describes the `variant_codec.cpp` mentioned in
§4.1 — closing a long-standing scan-path error where tables
containing XML, UDT, or SQL_VARIANT columns produce a hard
failure. The codec routes these into DuckDB VARIANT, making
such tables queryable.

#### 4.3.1 Scope

Affected SQL Server types: `XML`, `SQL_VARIANT`, `UDT`. Deprecated
`TEXT`/`NTEXT` become `VARCHAR` (not VARIANT — semantics known);
`IMAGE` becomes `BLOB`.

Mapping:

| SQL Server type | DuckDB result | VARIANT payload |
|---|---|---|
| `XML` | `VARIANT` | VARCHAR (UTF-8 decoded from wire UTF-16LE) |
| `SQL_VARIANT` | `VARIANT` | typed scalar matching inner type header |
| `UDT` | `VARIANT` | BLOB + metadata tag with class name |
| `TEXT` / `NTEXT` | `VARCHAR` (not VARIANT) | — |
| `IMAGE` | `BLOB` (not VARIANT) | — |

#### 4.3.2 Setting

```sql
-- Default: use VARIANT. Backward-compatible alias 'error' preserves
-- pre-v0.2 behavior for users who were relying on explicit failures.
SET mssql_unsupported_type_strategy = 'variant';   -- NEW default
SET mssql_unsupported_type_strategy = 'varchar';   -- text types as VARCHAR, others error
SET mssql_unsupported_type_strategy = 'error';     -- old behavior
SET mssql_unsupported_type_strategy = 'skip';      -- drop column from output schema
```

#### 4.3.3 `VariantCodec` outline

```cpp
class VariantCodec : public MssqlTypeCodec {
    TdsTypeId src_type_;  // XMLTYPE | UDTTYPE | SQLVARIANTTYPE
    // For UDT: cached class name from COLMETADATA
    std::string udt_class_name_;
public:
    LogicalType DuckDbType() const override {
        return LogicalType::VARIANT();
    }

    void DecodeBatch(TdsReader& r, Vector& out, idx_t count) override;
    // XML: PLP UTF-16LE → simdutf to UTF-8 → wrap as VARIANT(VARCHAR)
    // SQL_VARIANT: read 1-byte base type + 1-byte propBytes + props
    //   + value, dispatch to inner codec, wrap as VARIANT(innerType)
    // UDT: PLP binary → wrap as VARIANT(BLOB) with class_name metadata

    void EncodeBcpBatch(const Vector&, idx_t, TdsWriter&) override {
        throw NotImplementedException(
            "Writing VARIANT back to SQL Server is not supported. "
            "Cast to the explicit target type first.");
    }

    // FormatSqlLiteral: same — reject. VARIANT in pushdown doesn't
    // make sense; the pushdown generator will degrade gracefully to
    // client-side filtering.
};
```

#### 4.3.4 VARIANT construction API

DuckDB's `VARIANT` construction API lives in
`duckdb/common/types/variant.hpp` and is still evolving. Wrap it
behind a local `VariantBuilder` helper so future API changes are
localized:

```cpp
class VariantBuilder {
public:
    static string_t BuildVarchar(Vector& variant_vec, const string_t& s);
    static string_t BuildBlob(Vector& variant_vec, const_data_ptr_t data, idx_t len);
    static string_t BuildTyped(Vector& variant_vec, const LogicalType& t, const Value& v);
};
```

#### 4.3.5 Tests

- Table with an XML column: roundtrip a known XML document; assert
  DuckDB side receives VARIANT containing the expected string.
- `SQL_VARIANT` column with mixed row types (INT row + NVARCHAR row
  + DATETIME2 row); assert per-row typed VARIANT extraction.
- UDT column (`HierarchyId`, `Geography`): VARIANT(BLOB) with
  metadata.
- Setting change to `error`: same table must throw (backward
  compatibility).

---


---

## §4.7.5 Type mapping holes to close

These are gaps in the current DuckDB↔T-SQL type mapping that the
codec consolidation should close. They live in the codec
implementations as part of `DecodeBatch` / `EncodeBcpBatch` /
`AppendDdlColumnType` correctness, not as separate features.

Separate from the CTAS defaults, the LogicalType → T-SQL mapping
has gaps the codec refactor should close:

| DuckDB type | Current | Proposed |
|---|---|---|
| `TIMESTAMP` | likely `DATETIME2` default scale | `DATETIME2(6)` |
| `TIMESTAMP_TZ` | ? | `DATETIMEOFFSET(6)` |
| `TIMESTAMP_NS` | error | `DATETIME2(7)` (truncate, warn) |
| `UUID` | likely VARCHAR | `UNIQUEIDENTIFIER` |
| `HUGEINT` | error | `DECIMAL(38,0)` |
| `UHUGEINT` | error | `DECIMAL(38,0)` with overflow warn, or `VARCHAR(40)` |
| `LIST<T>` | error | `NVARCHAR(MAX)` with JSON; warn |
| `STRUCT<...>` | error | `NVARCHAR(MAX)` with JSON; warn |
| `MAP<K,V>` | error | `NVARCHAR(MAX)` with JSON; warn |
| `ARRAY[N]` | error | `NVARCHAR(MAX)` with JSON; warn |
| `INTERVAL` | error | `BIGINT` (microseconds); warn |
| `ENUM` | VARCHAR | `NVARCHAR(N)` where N = max enum value length |
| `BLOB` | `VARBINARY(MAX)` | `VARBINARY(N)` with inference, else MAX |
| `BIT` (bit string) | error | `VARBINARY(N)` |

Nested types (LIST/STRUCT/MAP/ARRAY) falling back to JSON is
controlled by `mssql_ctas_nested_as_json` (default `true`).


---

## Coordination notes

- **Spec 043 (Foundation fixes)** must land before 044 starts
  in earnest — 044 consumes the simdutf wrapper that 043
  introduces. Spec 043 leaves call sites unchanged; 044
  migrates all of them.
- **Spec 042 (Oluies, integrated authentication)** is refactoring
  `src/tds/auth/`. 044 will touch `sql_auth_strategy.cpp` to
  switch LOGIN7 password encoding from spec 043's local fix to
  the shared `utf_conversion.hpp` utility. Coordinate so that
  whichever spec lands second rebases on the other; the change
  on the auth side is mechanical (one call site swap).
- **Spec 045 (CTAS quality + TRUNCATE)** consumes
  `AppendDdlColumnType` for CTAS DDL generation, including
  closing remaining DDL-side type-mapping issues.
- **Spec 046 (BCP throughput)** consumes `EncodeBcpBatch` —
  specifically the column-batch variant for SIMD-style
  per-column encoding.
- **Spec 047 (DML performance)** consumes `FormatSqlLiteral`
  for direct-DML pushdown literal formatting and MERGE clause
  generation.

## Acceptance criteria (high level)

1. `src/encoding/` directory exists with `type_codec.hpp`,
   registry, reader/writer abstractions, simdutf wrapper, and
   nine per-type codec files (int, decimal, float, bool,
   string, binary, datetime, uuid, variant).
2. Every codec implements `DecodeBatch`, `EncodeBcpBatch`,
   `FormatSqlLiteral`, and `AppendDdlColumnType` — no virtual
   methods left abstract or throwing "not implemented".
3. Legacy dispatch files deleted: `src/tds/encoding/type_converter.cpp`,
   `src/tds/encoding/bcp_row_encoder.cpp`. Their helper files
   (`datetime_encoding.cpp`, `decimal_encoding.cpp`,
   `guid_encoding.cpp`) folded into the relevant per-type codecs.
4. `src/tds/encoding/utf16.cpp` legacy converter removed; all
   call sites use `utf_conversion.hpp` via simdutf.
5. Call sites migrated: `src/table_scan/filter_encoder.cpp`,
   `src/dml/insert/mssql_value_serializer.cpp`,
   `src/dml/ctas/mssql_ctas_executor.cpp` — type-dependent
   logic in each replaced by codec calls; structural logic
   (filter tree walking, identifier escaping, CTAS
   orchestration) stays in place.
6. VARIANT support: scanning a table with XML/SQL_VARIANT/UDT
   column produces DuckDB VARIANT values; previously errored.
7. Type-mapping holes from §4.7.5 closed: UUID round-trip via
   `UNIQUEIDENTIFIER`, HUGEINT → `DECIMAL(38,0)`, TIMESTAMP_TZ
   → `DATETIMEOFFSET(6)`, nested types → JSON fallback (DDL).
8. Full existing test suite passes (scan, BCP, INSERT, UPDATE,
   DELETE, CTAS, filter pushdown, MERGE-via-rowid). Targeted
   new tests for previously-broken type combinations and for
   VARIANT.
9. Benchmark: NVARCHAR-heavy scan throughput improves
   15-25% vs v0.1.18 (simdutf contribution).
10. No new vcpkg dependencies beyond simdutf (added in 043).

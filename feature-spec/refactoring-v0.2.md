# Refactoring Proposal: mssql-extension v0.2

**Status**: Draft
**Author**: Vladimir Gribanov
**Date**: April 2026
**Target release**: v0.2.0 (breaking internal changes; public SQL API compatible)

---

## 1. Executive Summary

This proposal consolidates several parallel architectural changes for the
`hugr-lab/mssql-extension` into a single v0.2 refactor:

1. **Unified type codec layer** — single dispatch point for all
   DuckDB ⇄ TDS type conversions (scan decode, BCP/INSERT encode,
   literal formatting for pushdown, DDL generation).
2. **SIMD-accelerated UTF-16 ⇄ UTF-8** via `simdutf` (already linked
   by DuckDB) replacing per-character manual conversion on both
   read and write paths.
3. **`VARIANT` fallback for unsupported SQL Server types** (XML,
   `SQL_VARIANT`, UDT) instead of hard errors.
4. **Integrated authentication** (Windows SSPI / Linux & macOS GSS-API)
   via dynamically loaded system libraries — no vcpkg/build-time deps.
5. **Named instance resolution** via SQL Server Browser (UDP 1434).
6. **BCP write-path performance**: packet-size tuning, TABLOCK,
   pipelined encode/send, batched column encoding, optional
   parallel streams.
7. **CTAS quality fixes**: configurable text type (NVARCHAR vs
   UTF-8 VARCHAR), bounded length defaults instead of `MAX`,
   compression (PAGE / COLUMNSTORE), optional length inference.
   Plus **`COPY TO ... (TRUNCATE true)`** mode for atomic table
   reload workflows.
8. **Fix non-ASCII password handling** in ATTACH path (likely a
   LOGIN7 UTF-16 encoding bug).
9. **DML optimization hooks**: single-statement path for pushable
   UPDATE/DELETE filters (skip the 2-phase rowid round-trip), and
   `TRUNCATE TABLE` detection for empty-filter DELETE (plus an
   explicit `mssql_truncate()` function). Row identity stays
   PK-based — no physical-rowid rewrite.
10. **MERGE INTO support** via server-side push-down: materialize
    USING source as a temp table (BCP fast path), emit native
    SQL Server MERGE with HOLDLOCK. Same-catalog sources bypass
    materialization. Client-side emulation available as opt-in
    fallback for users affected by known SQL Server MERGE edge
    cases.

The connecting thread is that most of these changes either **cannot
be done cleanly** without the codec refactor (1) or become much
simpler once it's in place. So (1) is the gate — everything else
lands as follow-ups on the new layer.

---

## 2. Motivation & Current Pain Points

### 2.1 Encoding code is scattered

Type-specific conversion logic currently lives in at least four
places:

| Location | Direction | Concern |
|---|---|---|
| `src/tds/encoding/type_converter.cpp` | TDS → DuckDB (scan) | Row decoding |
| `src/tds/encoding/bcp_row_encoder.cpp` | DuckDB → TDS (BCP) | BCP row layout |
| Pushdown filter generator | DuckDB `Value` → T-SQL literal | WHERE clauses |
| `mssql_table_entry.cpp` / DDL generator | DuckDB `LogicalType` → T-SQL type | CREATE TABLE |

Adding a new type or fixing a type's behavior requires changes in 2–4
files, each with its own `switch` on type ID. This has already caused
inconsistencies — e.g., `DATETIME2` precision handling differs between
scan and BCP paths.

### 2.2 UTF-16 conversion is hand-rolled and slow

`NVARCHAR` is wire UTF-16LE; DuckDB `VARCHAR` is UTF-8. Conversion
currently walks code points in a loop, handles surrogate pairs
inline, and computes UTF-8 byte counts character-by-character.

On M4 Max (NEON), `simdutf` is 5–15× faster than hand-rolled
conversion for strings > 32 bytes. DuckDB already links `simdutf`
in `third_party/simdutf` — no new dependency needed.

### 2.3 Unsupported types halt useful work

Hitting `XML`, `SQL_VARIANT`, or `UDT` in any queried table throws.
Real-world SQL Server databases have these in system views and
legacy application schemas. Users can't even run `SELECT * FROM
legacy_table LIMIT 10` without rewriting the query to exclude
columns by name.

DuckDB 1.4+ ships a `VARIANT` type (semi-structured, per-row
typed). This is the natural fallback: preserve what we can (XML as
string, `SQL_VARIANT` with inner scalar, UDT as blob) without
failing the whole query.

### 2.4 No Windows/AD authentication

Current auth is SQL authentication only (username/password). This
excludes the majority of enterprise SQL Server deployments, where
integrated Windows authentication (NTLM or Kerberos via SPNEGO) is
standard and often the only allowed method.

The extension targets macOS and Linux primarily, but the **client
side** of Kerberos works fine on both — what's needed is
GSS-API/SSPI integration in the LOGIN7 flow.

### 2.5 No named instance support

Enterprise SQL Server deployments routinely use named instances
(`HOST\INSTANCE`). The extension currently requires explicit
`HOST,PORT` — users must manually look up ports from DBAs.

SQL Server Browser (UDP 1434) solves this but isn't implemented.

### 2.6 BCP throughput underutilized

Current BCP implementation is sequential: encode chunk → send → wait
for server DONE → next chunk. On Azure West Europe from a local
client (~30ms RTT), this leaves the pipe mostly idle. Known
optimizations (packet size negotiation, TABLOCK, pipelining,
batched column encoding) are not applied.

### 2.7 CTAS produces suboptimal tables

`CREATE TABLE AS SELECT` currently emits:

- `VARCHAR` → `NVARCHAR(MAX)` (LOB storage, off-row, 2 bytes/char)
- No compression (`DATA_COMPRESSION = NONE`)
- No primary key (heap), even if the DuckDB source had one
- No option for UTF-8 collation (SQL Server 2019+)
- No option for columnstore compression

For analytical workloads this is 3–10× larger on disk and
considerably slower to scan than a properly-configured table.

### 2.8 Non-ASCII passwords in ATTACH (suspected bug)

User-reported: passwords containing non-ASCII characters (Cyrillic,
German umlauts, accented Latin) fail authentication that would
succeed from other clients (e.g., `sqlcmd`, `mssql-cli`). Needs
diagnosis — most likely root cause is incorrect UTF-16 encoding in
LOGIN7 password field or wrong length calculation.

### 2.9 UPDATE/DELETE pay round-trip tax even for trivial filters

Current UPDATE/DELETE plan is always two-phase: client fetches
rowids for all matching rows, then sends a second statement with
explicit PK IN-list. For `UPDATE t SET x = 1 WHERE id = 42` this
is two network round-trips and a pointless rowid materialization —
the predicate is trivially translatable to T-SQL.

Separately, `DELETE FROM t` (or its DuckDB alias `TRUNCATE t`)
goes through the same two-phase path — fetching rowids for every
row in the table, then issuing batched deletes. On a 10M-row
table this takes minutes vs. the milliseconds a native
`TRUNCATE TABLE` would need.

We do **not** plan to rework the row identity mechanism itself
(PK-based rowid stays). Instead we add pushdown hooks: if the
predicate is fully pushable, emit a single server-side statement;
if the filter is empty, emit `TRUNCATE TABLE` with safety checks.

### 2.10 MERGE INTO is unsupported

DuckDB 1.4 shipped `MERGE INTO` as a first-class SQL feature,
driving a lot of modern ETL patterns (upserts, SCD, delta
reconciliation). On our attached mssql tables this currently
fails with `Database type "mssql" does not support MERGE INTO
or ON CONFLICT` — the storage extension MERGE hook is not
implemented.

SQL Server has had native `MERGE` since SQL 2008, with
effectively the same ANSI syntax. Pushing the entire statement
server-side (after BCP-uploading the USING source into a temp
table) is both natural and dramatically faster than any
client-side emulation.

---

## 3. Architectural Goals

- **Single source of truth for type semantics.** Type information
  flows through one abstraction from TDS metadata or DuckDB logical
  type to all consumers.
- **No new build-time dependencies.** Integrated auth is entirely
  `dlopen`/`LoadLibrary`. No vcpkg additions for `libgssapi`,
  `secur32`, or related. Keeps CI and Community Extensions build
  path unchanged.
- **Opt-in opt-out for behavioral changes.** New defaults (e.g.,
  `PAGE` compression, bounded `NVARCHAR(4000)` instead of `MAX`)
  are settings-driven; old behavior available by explicit set.
- **Measurable performance.** Every optimization claim is backed by
  a benchmark in `test/benchmark/` with a baseline comparison.
- **Incremental migration.** Refactor can ship in stages; no
  single PR needs to touch the whole codebase.

---

## 4. Proposed Changes

### 4.1 Unified Type Codec Layer (gate change)

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

### 4.2 SIMD UTF Conversion via simdutf

#### 4.2.1 Rationale

`simdutf` is already linked by DuckDB (`third_party/simdutf/`).
The library uses AVX2/AVX-512/NEON kernels auto-selected at runtime.
For ASCII-only strings it degenerates to `memcpy`; for mixed
content it is 3–10× faster than scalar conversion.

#### 4.2.2 Wrapper interface

```cpp
// src/encoding/utf_conversion.hpp

class Utf16ToUtf8Converter {
public:
    // Per-row: decode a single UTF-16LE byte sequence into a target
    // DuckDB string_t allocated in the target vector's string heap.
    static inline string_t DecodeInto(
        Vector& target_vec,
        const uint8_t* utf16le_bytes,
        size_t byte_count);

    // Batch: decode a column of UTF-16LE strings described by
    // (blob, offsets[count+1]) into a flat string vector.
    static void DecodeBatchContiguous(
        Vector& target_vec,
        const uint8_t* utf16le_blob,
        const uint32_t* offsets_bytes,
        idx_t count);
};

class Utf8ToUtf16Converter {
public:
    // Encode one DuckDB string_t as length-prefixed UTF-16LE into
    // the TDS writer. `u_short_len` chooses between 2-byte length
    // prefix (NVARCHAR <= 4000) and PLP chunked encoding (MAX).
    static inline void EncodeLengthPrefixed(
        TdsWriter& writer,
        const string_t& src,
        bool u_short_len);

    // Batch variant: pre-compute total UTF-16 output size, allocate
    // contiguous buffer, convert row by row, write in one tds
    // send. Used by BCP path for NVARCHAR columns.
    static void EncodeBatchContiguous(
        const Vector& in,
        idx_t count,
        TdsWriter& writer);
};
```

#### 4.2.3 Integration points

- `StringCodec::DecodeBatch` uses `DecodeBatchContiguous` after
  gathering UTF-16LE row bytes into a single buffer via TDS token
  parsing.
- `StringCodec::EncodeBcpBatch` uses `EncodeBatchContiguous` for
  NVARCHAR columns.
- PLP (`NVARCHAR(MAX)`) decode: chunk-wise append into a per-row
  staging buffer, then per-row `DecodeInto` on finalize. Cannot
  fully batch because chunk boundaries may split UTF-16 code units
  (surrogate pairs), requiring careful accumulation.
- Pushdown literal `FormatSqlLiteral` for VARCHAR/NVARCHAR literals
  must also emit correctly-escaped UTF-16 — reuses the same
  conversion path.

#### 4.2.4 CMake wiring

`simdutf` headers are inside the DuckDB submodule. Extension
CMakeLists adds:

```cmake
target_include_directories(${TARGET_NAME}_loadable_extension
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/simdutf)
target_include_directories(${TARGET_NAME}_static_extension
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/simdutf)
```

The library itself is already linked via `duckdb_static`; no new
link step needed. Validate by compiling a trivial test that calls
`simdutf::convert_utf8_to_utf16le`.

If symbol conflicts arise (unlikely — simdutf is namespaced),
consider namespacing via `-Dsimdutf=duckdb_mssql_simdutf` or
vendoring a separate copy.

#### 4.2.5 Expected performance impact

| Scenario | Speedup (estimated) |
|---|---|
| ASCII-heavy NVARCHAR columns (row decode) | 3–5× |
| Cyrillic/CJK NVARCHAR columns (row decode) | 5–10× |
| BCP NVARCHAR encode (UTF-8 → UTF-16) | 5–8× |
| VARCHAR columns (no UTF-16 involved) | unchanged |

Validated via `test/benchmark/utf_conversion.cpp` with synthetic
datasets. Acceptance: no regression on any workload; ≥ 2× on
mixed-content NVARCHAR.

---

### 4.3 VARIANT Fallback for Unsupported Types

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

### 4.4 Integrated Authentication

#### 4.4.1 Design principles

Integrated (Windows SSPI / POSIX GSS-API) authentication is the
most common blocker for real-world SQL Server deployments —
enterprise users rarely have SQL-auth credentials for production
boxes. The extension already has a clean authentication strategy
abstraction; adding integrated auth is a matter of one more
strategy implementation, not a new subsystem.

- **Extend existing `AuthenticationStrategy` interface** in
  `src/include/tds/auth/auth_strategy.hpp`. Today three strategies
  implement it (`SqlAuthStrategy`, `FedAuthStrategy`,
  `ManualTokenStrategy`); we add `IntegratedAuthStrategy` as a
  fourth.
- **Dynamic loading only.** No `libgssapi` / `secur32` in vcpkg
  manifest. Strategy uses `dlopen`/`LoadLibrary` at first-use.
- **Graceful degradation.** If the OS doesn't have Kerberos libs,
  integrated auth is unavailable but the extension still loads
  and other strategies work normally.
- **Unified inner abstraction.** Inside the integrated strategy,
  Linux and macOS both speak GSS-API; Windows speaks SSPI. The
  strategy class hides this behind a single `SspiProvider`
  internal interface with two implementations.

#### 4.4.2 Interface extension

The existing `AuthenticationStrategy` base (lines 57-103 of
`auth_strategy.hpp`) already covers:

- `GetPreloginOptions()` — PRELOGIN customization (TLS, FEDAUTH
  flag, SNI hostname).
- `GetLogin7Options()` — LOGIN7 packet fields (database, username,
  password, app name, fedauth extension flag).
- `GetFedAuthToken(FedAuthInfo)` — only called when
  `RequiresFedAuth()` returns true.

Integrated auth doesn't fit cleanly into this shape, because it
needs **multi-round token exchange during LOGIN7** (SSPI/GSSAPI
typically takes 2-3 server-client rounds for Negotiate/NTLM).
This is different from SQL auth (single round) and FEDAUTH
(separate FEDAUTHINFO → token cycle after LOGIN7).

Two options:

**Option A (preferred)**: Add one new virtual method to the
existing interface:

```cpp
// Added to AuthenticationStrategy in auth_strategy.hpp

// Integrated auth (SSPI/GSSAPI) multi-round token exchange.
// Default returns empty bytes and sets complete=true.
// Integrated strategy overrides to step through the SSPI/GSSAPI
// state machine. Server sends response token inside the LOGIN7
// auth-tokens stream; we hand it back via NextToken, producing
// the next client token.
virtual std::vector<uint8_t> NextToken(
    const std::vector<uint8_t>& server_token,
    bool& complete) {
    complete = true;
    return {};
}

// Does this strategy use SSPI/GSSAPI integrated auth?
virtual bool RequiresIntegratedAuth() const { return false; }
```

Existing three strategies get default implementations
(`RequiresIntegratedAuth() == false`, `NextToken` returns empty).
Only `IntegratedAuthStrategy` overrides.

**Option B**: New sibling interface. Rejected — creates a
two-dimension type hierarchy that complicates the factory in
`auth_strategy_factory.cpp` and requires `TdsConnection` to
branch on strategy kind before dispatching.

Decision: **Option A**. Single interface, one new virtual method
with a harmless default, existing strategies unchanged.

#### 4.4.2.1 New strategy class

```cpp
// src/include/tds/auth/integrated_auth_strategy.hpp

class IntegratedAuthStrategy : public AuthenticationStrategy {
public:
    struct Config {
        std::string spn;  // "MSSQLSvc/host.fqdn:1433" (auto-built)
        std::optional<std::string> explicit_username;  // "user@REALM"
        std::optional<std::string> keytab_path;        // optional
        enum class Mechanism { Negotiate, Kerberos, Ntlm }
            mechanism = Mechanism::Negotiate;
    };

    explicit IntegratedAuthStrategy(Config config);

    bool RequiresFedAuth() const override { return false; }
    bool RequiresIntegratedAuth() const override { return true; }
    std::string GetName() const override { return "integrated"; }

    PreloginOptions GetPreloginOptions() const override;
    Login7Options GetLogin7Options() const override;

    // Integrated auth doesn't use FEDAUTH token path
    std::vector<uint8_t> GetFedAuthToken(
        const FedAuthInfo&) override { return {}; }

    // The core of integrated auth — driven from TdsConnection
    std::vector<uint8_t> NextToken(
        const std::vector<uint8_t>& server_token,
        bool& complete) override;

private:
    Config config_;
    std::unique_ptr<SspiProvider> provider_;  // internal, lazy-init
};
```

`SspiProvider` is internal to the integrated strategy's .cpp file,
not a public interface. Two implementations (Windows SSPI, POSIX
GSSAPI) selected at construction via platform macros.

#### 4.4.3 Platform implementations

Files live alongside existing strategy implementations in
`src/tds/auth/` to match the established layout:

```
src/include/tds/auth/
├── auth_strategy.hpp              # modified: +NextToken, +RequiresIntegratedAuth
├── auth_strategy_factory.hpp      # modified: +integrated case
├── fedauth_strategy.hpp           # unchanged
├── integrated_auth_strategy.hpp   # NEW
├── manual_token_strategy.hpp      # unchanged
└── sql_auth_strategy.hpp          # unchanged

src/tds/auth/
├── auth_strategy_factory.cpp      # modified
├── fedauth_strategy.cpp           # unchanged
├── integrated_auth_strategy.cpp   # NEW: strategy + SspiProvider dispatch
├── integrated_windows_sspi.cpp    # NEW: Windows SSPI (compiled only on WIN32)
├── integrated_posix_gssapi.cpp    # NEW: POSIX GSSAPI (compiled elsewhere)
├── manual_token_strategy.cpp      # unchanged
└── sql_auth_strategy.cpp          # unchanged
```

**Windows (`WindowsSspiProvider` in `integrated_windows_sspi.cpp`)**

Loads `secur32.dll` via `LoadLibraryW` at first construction.
Resolves function pointers:

- `AcquireCredentialsHandleW`
- `InitializeSecurityContextW`
- `CompleteAuthToken`
- `QueryContextAttributes`
- `FreeCredentialsHandle`
- `DeleteSecurityContext`
- `FreeContextBuffer`

Package name selected by `mechanism`: `L"Negotiate"` (default,
SPNEGO picks Kerberos if SPN resolves else NTLMv2),
`L"Kerberos"`, or `L"NTLM"`.

Channel binding: construct `SEC_CHANNEL_BINDINGS` with
`tls-server-end-point:<hash>` application data.

**POSIX (`PosixGssapiProvider` in `integrated_posix_gssapi.cpp`)**

Loads first available of:
- `libgssapi_krb5.so.2` (MIT Kerberos — most Linux distros)
- `libgssapi.so.3` (Heimdal — some Linux distros)
- `/System/Library/Frameworks/GSS.framework/GSS` (macOS built-in)
- `/usr/local/opt/krb5/lib/libgssapi_krb5.dylib` (Homebrew on macOS)
- `/opt/homebrew/opt/krb5/lib/libgssapi_krb5.dylib` (Homebrew ARM)

Resolves:
- `gss_import_name`, `gss_release_name`
- `gss_acquire_cred`, `gss_release_cred`
- `gss_init_sec_context`, `gss_delete_sec_context`
- `gss_release_buffer`
- Variables: `GSS_C_NT_HOSTBASED_SERVICE`, `GSS_C_NO_CREDENTIAL`,
  `GSS_C_NO_CHANNEL_BINDINGS`

Channel binding: `gss_channel_bindings_struct` with
`application_data` holding `"tls-server-end-point:"` + hash bytes
per RFC 5929.

#### 4.4.4 TDS LOGIN7 integration

Current `TdsConnection::Authenticate()` flow must become multi-round:

```
┌─────────────────────────────────────────────────────────┐
│ 1. Build SPN from connect info (host, instance, port)   │
│ 2. SspiProvider::Create(AuthConfig{spn, ...})           │
│ 3. initial_token = provider->Step({}, &channel_bindings)│
│ 4. Send LOGIN7 with OptionFlags2.fIntegratedSecurity=1  │
│    and initial_token in SSPI block                      │
│ 5. loop {                                               │
│      token_stream = receive TDS response                │
│      if token_stream has LOGINACK: break (success)      │
│      if token_stream has ERROR: fail                    │
│      if token_stream has SSPI token:                    │
│        next_token = provider->Step(srv_token, &cb)      │
│        if next_token empty and provider complete: break │
│        send next_token as SSPI message (token type 0xED)│
│    }                                                    │
└─────────────────────────────────────────────────────────┘
```

State machine replaces the current linear code. Keep in a separate
method `AuthenticateIntegrated()` to avoid breaking the password
path.

#### 4.4.5 SPN construction

```
Default instance: MSSQLSvc/<fqdn>:<port>
Named instance:   MSSQLSvc/<fqdn>:<instance_name>
```

Where `<fqdn>` is the full DNS name (not short hostname — Kerberos
tickets are tied to FQDNs). Extension should:

1. Resolve user-supplied `host` via `getaddrinfo` with `AI_CANONNAME`.
2. If `instance` is set, use `MSSQLSvc/<fqdn>:<instance>`.
3. Otherwise use `MSSQLSvc/<fqdn>:<port>` (SPN registered per port
   by SQL Server service).
4. Allow explicit SPN override in secret/connection string
   (`spn 'MSSQLSvc/custom.name:1433'`).

#### 4.4.6 Factory and secret integration

The existing `AuthStrategyFactory` in
`src/tds/auth/auth_strategy_factory.cpp` currently dispatches on
`MSSQLConnectionInfo` fields to build one of the three existing
strategies. Extension point: when `auth` field (new, see below)
is set to an integrated value, build `IntegratedAuthStrategy`
instead.

Secret / connection string new field:

```sql
CREATE SECRET ad_auth (
    TYPE mssql,
    host 'sqlsrv01.corp.example.com',
    instance 'DEV2',                  -- optional, resolved via Browser
    database 'master',
    auth 'integrated',                -- NEW: 'sql' (default), 'integrated',
                                       --      'kerberos', 'ntlm',
                                       --      'fedauth', 'manual-token'
    -- spn '...' optional override
    use_encrypt true                  -- often required for integrated
);
```

Values `'kerberos'` and `'ntlm'` select `IntegratedAuthStrategy`
with specific mechanism; `'integrated'` maps to `Negotiate` and
is the recommended default for AD environments.

Existing `auth` values continue to work: if the field is absent,
selection follows today's logic (username+password → SQL,
fedauth-specific fields → FedAuth, etc.), so no migration is
forced on existing users.

Connection string aliases `Integrated Security=SSPI` and
`Trusted_Connection=yes` (ADO.NET conventions) map to
`auth=integrated`.

#### 4.4.7 Tests

- **POSIX**: `docker-compose` test setup with a KDC container
  (`gcavalcante8808/krb5-server`) and SQL Server container
  configured with a keytab. Tests run `kinit` in a fixture, then
  connect with `auth=kerberos`.
- **Windows**: GitHub Actions `windows-latest` with local SQL
  Server install + `Integrated Security=SSPI` against the runner's
  system account. No AD needed — NTLM via SSPI works between local
  accounts.
- **Channel binding**: integration test with `EPA=Required` server
  side; assert connection succeeds.
- **Mechanism fallback**: request Kerberos, point at a host without
  SPN, assert clean error with actionable message.

---

### 4.5 Named Instance Resolution (SQL Server Browser)

#### 4.5.1 Protocol

SQL Server Browser listens on UDP `1434`. Client sends:

```
[0x02][<instance_name>\0]   — CLNT_UCAST_INST
```

Server replies:

```
[0x05][<length_lo>][<length_hi>][<ASCII key-value string>]
```

Where the string is `;`-delimited, e.g.:

```
ServerName;SQLSRV01;InstanceName;DEV2;IsClustered;No;Version;16.0.1000.6;
tcp;49281;np;\\SQLSRV01\pipe\MSSQL$DEV2\sql\query;;
```

We extract the `tcp` port.

#### 4.5.2 Implementation

New file `src/tds/sql_browser_resolver.cpp`:

```cpp
struct ResolvedInstance {
    uint16_t port;
    std::string version;
};

class SqlBrowserResolver {
public:
    static Result<ResolvedInstance> Resolve(
        const std::string& host,
        const std::string& instance_name,
        uint32_t timeout_ms = 2000);
};
```

Invocation path: `TdsConnection::Connect()` — if `instance` is
set and no explicit port, call `SqlBrowserResolver::Resolve` first.

Configurable via new settings:
```sql
SET mssql_browser_timeout = 2000;      -- ms, default 2000
SET mssql_browser_port = 1434;         -- rarely changed
```

#### 4.5.3 Edge cases

- Browser service disabled: clear error message pointing to
  `sc start SQLBrowser` or explicit port.
- Firewall blocks UDP 1434: same error with hint.
- Multiple replies on multi-homed server: take first.
- Instance name case-insensitive matching server-side; we pass
  user input verbatim.

---

### 4.6 BCP Write-Path Optimization

Ordered roughly by effort-to-win ratio.

#### 4.6.1 Packet size negotiation

**Change**: request max packet size (32 767 bytes = `0x7FFF`) in
PRELOGIN `PL_OPTION_TOKEN` `0x04` (PacketSize). Currently likely
uses the 4096 default.

**Impact**: ~15–25% throughput improvement for BCP (fewer TDS
headers, fewer write syscalls).

**Risk**: low. Every SQL Server version supports up to 32 767.
Some intermediate proxies may cap at lower values — server returns
the actual value it accepts in the PRELOGIN response; use that.

#### 4.6.2 TABLOCK for CTAS

**Current state**: `mssql_copy_tablock` (bool, default `false`) and
`mssql_ctas_use_bcp` (default `true`) are already implemented.
`BCPCopyConfig` tracks `is_new_table` and `tablock_explicit` flags
— when a new table is being created and user hasn't explicitly
disabled TABLOCK, it's auto-enabled. CTAS path
(`mssql_ctas_executor.cpp`) already applies TABLOCK for freshly
created tables.

**Change**: extend auto-TABLOCK to cover more cases based on
target shape, not just "new table":

- Target is heap (no clustered index) — add auto-TABLOCK.
- Target is empty (row count 0, known from cached statistics) —
  add auto-TABLOCK even on existing tables.
- Target is in AG replication — **don't** auto-enable TABLOCK,
  since log shipping serializes anyway and TABLOCK blocks readers
  with no parallelism benefit.

The existing boolean setting stays; new auto-paths respect
`tablock_explicit=false`. No new setting needed.

**Impact**: 3–10× on large CTAS loads in `SIMPLE`/`BULK_LOGGED`
recovery (already works today for new tables; extends to empty
heaps and pre-truncated targets). No impact on `FULL` recovery.

**Risk**: very low. Existing auto-TABLOCK behavior has been live
since spec 025/027; this expands the trigger conditions.

#### 4.6.3 Pipelined encode/send

**Change**: split BCP into two cooperating components:

```
┌──────────────┐   ring buffer of    ┌─────────────┐
│ Encode thread│ → N packet slots  → │ Send thread │
│ (DuckDB scan │                     │ (TCP write, │
│  + codec     │                     │  TLS write) │
│  encoding)   │                     │             │
└──────────────┘                     └─────────────┘
```

Ring buffer: 4–8 slots × 32 KiB (packet size). Lock-free MPSC or
mutex + condvar (contention minimal for 2 threads).

Encoder blocks when buffer full; sender blocks when empty.
Effective throughput = max(encode_rate, network_rate) instead of
sum.

**Impact**: 1.5–2× on local client → remote SQL Server (RTT > 10ms).
Closer to the network limit. On localhost (RTT ≈ 0) the win is
smaller (20–30%) but still positive due to kernel-space overlap.

**Setting**:
```sql
SET mssql_bcp_pipeline = true;         -- default ON
SET mssql_bcp_pipeline_slots = 4;      -- ring buffer depth
```

See §4.6.5.2 for when this applies in relation to connection
concurrency — short answer: whenever a single connection is in
use (which is most of the time — see matrix in §4.6.5.1).
Pipelining is orthogonal to single- vs multi-connection
execution.

#### 4.6.4 Column-batch encoding (requires codec refactor §4.1)

**Change**: `EncodeBcpBatch` for each codec operates columnar:
gathers all column values into a per-column staging buffer, then
interleaves into the final row stream at the end.

For fixed-width types (INT, BIGINT, DOUBLE, DATE) this is a
straight `memcpy` of a contiguous Vector buffer. For NVARCHAR it
batches the simdutf conversion and size calculation.

**Impact**: 2–3× on wide tables (20+ columns). For narrow tables
(<5 columns) the win is small but not negative.

#### 4.6.5 Connection concurrency strategy for BCP

BCP throughput depends heavily on whether we can use multiple
SQL Server connections concurrently. This is determined by the
transaction context:

**Pinned context (inside user's explicit transaction).** When the
user has issued `BEGIN` (explicit or via DuckDB transaction
semantics), the existing pool pins one connection to that
transaction. All operations for that transaction **must** flow
through that single connection — otherwise the transaction
descriptor diverges, temp tables become invisible, and isolation
is broken. **No multi-connection parallelism is possible in this
context**, regardless of the operation.

**Unpinned context (autocommit).** The user issued a single
statement (CTAS, COPY TO, or MERGE) without an enclosing
transaction. The extension can acquire as many connections from
the pool as `mssql_connection_limit` allows, subject to
feature-specific safety checks.

##### 4.6.5.1 Decision matrix: which operations can use which paths

The concurrency decision is made per-operation, not per-path.

| Operation                    | Context    | Path                    |
|------------------------------|------------|-------------------------|
| CTAS (new table)             | autocommit | **Multi-connection**    |
| COPY TO existing heap        | autocommit | **Multi-connection** *  |
| COPY TO existing CI/PK table | autocommit | Single + pipelining     |
| COPY TO any target           | in user txn| Single (pinned)         |
| INSERT INTO ... SELECT       | any        | Single (pinned if txn)  |
| MERGE INTO                   | any        | Single (pinned)         |
| BCP into `#tmp` (MERGE stage)| any        | Single (pinned)         |

\* Subject to runtime preconditions (heap, no constraints, recovery
mode, not replicated). Precondition failure falls back to
single-connection path silently.

Two observations drive this split:

1. **CTAS** is the one case where the target is guaranteed-safe by
   construction — we create a fresh heap, set recovery-mode-appropriate
   flags, there are no existing constraints or replication to worry
   about. Parallel BCP is enabled by default in autocommit; no
   runtime precondition checks needed.

2. **MERGE** and everything touching temp tables must stay on a
   single pinned connection. Session `#tmp` visibility requires it,
   and it's the only way to get atomic transaction semantics around
   a multi-step operation (CREATE #tmp → BCP upload → MERGE →
   DROP). Parallelism inside MERGE source upload would require the
   staging-table solutions we rejected earlier (§4.6.5.4).

##### 4.6.5.2 Single-connection pipelined BCP (the universal path)

Regardless of pinned/unpinned context or operation type, whenever
we use a single connection, we apply encode/send pipelining on
that connection:

- Encoder thread: DuckDB scan → codec `EncodeBcpBatch` →
  serialized TDS row bytes → bounded ring buffer.
- Sender thread: ring buffer → single TDS socket write on the
  in-use connection.

Encoder runs ahead of sender; sender never blocks on encode
latency. With ~4 buffer slots of packet-size (32 KiB), the
network pipe stays full even on high-RTT links where a naive
sync encode-then-send would stall.

This is pure throughput improvement, no correctness implications.

**Setting**:
```sql
SET mssql_bcp_pipeline = true;         -- default ON
SET mssql_bcp_pipeline_slots = 4;      -- ring buffer depth
```

**Impact**: 1.5–2× on remote SQL Server (RTT > 10ms); ~20-30%
on localhost. No downside.

##### 4.6.5.3 Multi-connection parallel BCP (autocommit, safe targets only)

Used for:
- CTAS into freshly-created heap (always qualifies).
- COPY TO existing heap that passes runtime precondition check.

Never used for MERGE, INSERT...SELECT, or anything inside a user
transaction.

DuckDB partition-scan produces N independent output streams; each
is carried by its own connection from the pool, writing directly
into the user-visible target table via `INSERT BULK ... WITH
(TABLOCK)`. No staging table is needed — each stream writes to
the actual target. `TABLOCK` serializes only extent-allocation,
not rows, so streams do not contend on the data path. This is
the same pattern SSIS and `bcp.exe` use for bulk-load parallelism.

**Preconditions for COPY TO existing target** (all must hold;
CTAS skips this check since it creates a heap with no constraints):

- Target is **heap** (no clustered index). Checked via
  `sys.indexes` at plan time (cached in catalog metadata).
- No UNIQUE or CHECK constraints that force per-row validation.
  FK is allowed but doesn't benefit from parallelism.
- Target DB in `SIMPLE` or `BULK_LOGGED` for minimal logging.
  Checked once per ATTACH.
- Connection pool size `mssql_connection_limit` ≥ N.
- Target not part of Availability Group replication (replication
  serializes log shipping even if bulk-load is parallel).

**Setting**:
```sql
SET mssql_bcp_parallelism = 1;         -- default: single stream + pipelining
SET mssql_bcp_parallelism = 'auto';    -- min(cores, pool_limit)
                                        -- when preconditions met;
                                        -- else single stream
SET mssql_bcp_parallelism = 4;         -- explicit N
```

Precondition failures (CI detected, FULL recovery, user txn
context, etc.) always degrade gracefully to single-connection
pipelined path — never error, never silently break correctness.

**Impact**: 3–5× additional on top of pipelining for large
loads. Linear scaling until network bandwidth or server log/IO
limit.

##### 4.6.5.4 Does NOT apply: multi-connection into temp tables

An earlier draft considered parallel BCP into a shared staging
table for MERGE source upload. We evaluated this and rejected it:

- Session temp tables (`#tmp`) are session-local — N connections
  cannot share one.
- Named user-DB staging tables require `CREATE TABLE` permission
  which many users don't have in their production SQL Server.
  Adding a permission gate to a core feature is wrong.
- Global temp tables (`##tmp`) have auto-drop semantics tied to
  "last referencing session closes" — fragile and slow.
- Session binding via `sp_getbindtoken`/`sp_bindsession` is
  deprecated and not recommended by Microsoft.

MERGE source upload therefore uses single-connection pipelined
BCP (§4.6.5.2) into a session `#tmp`. Throughput is limited by
one connection, but pipelining closes most of the gap compared
to a hypothetical parallel approach, without any of the
infrastructure and permission complications.

Parallel BCP is reserved for §4.6.5.3 — direct writes into user
target tables, where the staging problem doesn't exist.

#### 4.6.6 Summary of expected gains

On 10 M rows of mixed-schema data, local → Azure West Europe:

| Optimization | Cumulative multiplier |
|---|---|
| Baseline | 1.0× |
| + packet size 32K | 1.2× |
| + TABLOCK (simple DB) | 4.0× |
| + pipelining | 6.0× |
| + SIMD UTF-16 | 7.5× |
| + column-batch encoding | 12× |
| + 4-way parallelism | 35× |

Numbers are illustrative; actual must come from benchmark harness
(§7.3).

---

### 4.7 CTAS Quality Fixes

#### 4.7.1 Text column defaults

**Problem**: `VARCHAR → NVARCHAR(MAX)` creates LOB storage, blocks
indexes, breaks columnstore, is 2 bytes/char.

**Settings**:
```sql
SET mssql_ctas_text_type = 'auto';    -- NVARCHAR if server < 2019
                                       -- or DB collation non-UTF8;
                                       -- VARCHAR with UTF-8 otherwise
SET mssql_ctas_text_type = 'nvarchar';
SET mssql_ctas_text_type = 'varchar';

SET mssql_ctas_varchar_collation = 'Latin1_General_100_CI_AS_SC_UTF8';

SET mssql_ctas_default_text_length = 4000;   -- NVARCHAR upper bound
                                               -- for in-row storage.
                                               -- 0 = MAX (old behavior)

SET mssql_ctas_infer_text_length = 'off';    -- 'sample' | 'exact'
SET mssql_ctas_infer_sample_rows = 10000;
```

**Inference**: when `infer_text_length = 'sample'`, CTAS planner
emits a pre-query:

```sql
SELECT MAX(octet_length(col1)), MAX(octet_length(col2)), ...
FROM (<source relation>) USING SAMPLE 10000 ROWS;
```

Result scales by 1.5×, rounds up to next power of 2 (32, 64, 128,
256, 512, 1024, 2048, 4000), caps at `default_text_length`. If
any column exceeds `default_text_length`, that column falls back
to `MAX`.

#### 4.7.2 Compression

```sql
SET mssql_ctas_compression = 'page';         -- NEW default
SET mssql_ctas_compression = 'none';         -- old behavior
SET mssql_ctas_compression = 'row';
SET mssql_ctas_compression = 'columnstore';  -- adds CCI after CREATE
```

`page` is free CPU-wise on modern hardware and saves 50–80% disk.
Safe default.

`columnstore` creates `CLUSTERED COLUMNSTORE INDEX` after the
heap is created and before BCP. Triggers:
- Auto-raise `mssql_bcp_batch_size` to `≥ 102400` for direct
  compressed rowgroup writes.
- Conflict with PK: if PK is specified, it becomes `NONCLUSTERED`.
- Conflict with `MAX` types: error with message directing user to
  bounded lengths or `PAGE` compression.

#### 4.7.3 Primary key propagation

```sql
SET mssql_ctas_create_primary_key = 'if_duckdb_has_pk';  -- default
SET mssql_ctas_create_primary_key = 'never';
```

When source relation has a PK and setting is enabled, emit:

```sql
CREATE TABLE dbo.tgt (
    id INT NOT NULL,
    ...,
    CONSTRAINT PK_tgt PRIMARY KEY CLUSTERED (id)
) WITH (DATA_COMPRESSION = PAGE);
```

If compression is `columnstore`, PK becomes `NONCLUSTERED`.

#### 4.7.4 Per-query overrides via COPY options

```sql
COPY source TO 'mssql:db.dbo.target' (
    FORMAT mssql,
    TEXT_TYPE 'varchar',
    TEXT_LENGTH 500,
    COMPRESSION 'columnstore',
    INFER_TEXT_LENGTH 'sample',
    COLUMN_TYPES {
        'email': 'NVARCHAR(255)',
        'uuid': 'UNIQUEIDENTIFIER',
        'description': 'NVARCHAR(MAX)',
        'code': 'CHAR(10)'
    }
);
```

`COLUMN_TYPES` is an escape hatch for any column where the default
mapping is wrong. Value is a T-SQL type literal passed through
verbatim.

#### 4.7.5 Type mapping holes to close

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

#### 4.7.6 COPY TO with TRUNCATE mode

Users regularly want "replace all rows" semantics for data
reload workflows — nightly ETL, dev-environment refresh from
production, table mirror from another data source. Today this
requires two statements:

```sql
DELETE FROM mssql.dbo.target;          -- or TRUNCATE (from §4.10)
COPY source TO 'mssql:target' (FORMAT mssql);
```

This is awkward, non-atomic, and harder to wrap in a transaction
when the user doesn't want to manage BEGIN/COMMIT explicitly.

**Change**: accept a TRUNCATE flag in COPY options, distinct from
the existing OVERWRITE flag.

Today `src/copy/copy_function.cpp` and `src/copy/target_resolver.cpp`
already handle `OVERWRITE true`, but it means **drop and recreate
table** (`DropTable` + `CreateTable` in `ValidateTarget`). That is
a schema-changing operation. The new TRUNCATE flag is different —
it preserves the table (schema, indexes, grants, constraints) and
only empties rows.

```sql
-- Append (default, existing behavior)
COPY source TO 'mssql:db.dbo.target' (FORMAT mssql);

-- NEW: preserve schema, replace rows
COPY source TO 'mssql:db.dbo.target' (FORMAT mssql, TRUNCATE true);

-- EXISTING (unchanged): drop + recreate table with inferred schema
COPY source TO 'mssql:db.dbo.target' (FORMAT mssql, OVERWRITE true);
```

The three modes are mutually exclusive. `OVERWRITE true, TRUNCATE
true` together is a user error — reject at bind time with a clear
message. TRUNCATE implies `CREATE_TABLE false` (target must exist);
reject as error if user sets both `TRUNCATE true, CREATE_TABLE true`.

A global default setting applies only to the TRUNCATE-vs-append
choice, not OVERWRITE:

```sql
SET mssql_copy_to_mode = 'append';      -- default; existing behavior
SET mssql_copy_to_mode = 'truncate';    -- truncate before every COPY TO
```

Per-query option wins over the setting. The setting has no way to
select `overwrite` — that stays an explicit per-query choice
because drop+recreate is too destructive for a global default.

##### 4.7.6.1 Execution modes

Two strategies, selected by `ATOMIC` option:

**Atomic mode (default):** single pinned connection, transaction-
wrapped:

```
BEGIN TRANSACTION;
TRUNCATE TABLE dbo.target;   -- goes through §4.10 detection with
                              -- trigger/FK/permission fallback to
                              -- DELETE FROM when needed
-- Single-connection pipelined BCP, since we're now pinned
INSERT BULK dbo.target ...;
COMMIT TRANSACTION;
```

Atomic: on failure at any step, ROLLBACK restores original state.
No window where target is visibly empty. Single-connection only
(pinned context), so throughput is bounded by single-connection
pipelined BCP.

**Non-atomic mode (opt-in):** `ATOMIC false`:

```
TRUNCATE TABLE dbo.target;              -- autocommit
-- Multi-connection parallel BCP if target qualifies per §4.6.5.3
BCP dbo.target (N parallel streams);
```

Window between TRUNCATE and BCP completion where target is empty
is visible to other sessions. On BCP failure, target is partially
filled or empty — user is responsible for cleanup/retry. Enables
parallel BCP throughput gains for large loads where atomicity
isn't required (e.g., private dev environment, idempotent nightly
reload).

```sql
COPY source TO 'mssql:db.dbo.target' (
    FORMAT mssql,
    TRUNCATE true,
    ATOMIC true       -- default; set false for parallel BCP
);

-- Or globally:
SET mssql_copy_to_atomic = true;        -- default
```

##### 4.7.6.2 Interaction with TRUNCATE safety (§4.10)

The TRUNCATE executed by COPY TO follows the same safety logic
as user-initiated TRUNCATE:

- DELETE triggers → fallback to `DELETE FROM`.
- FK-referenced → fallback to `DELETE FROM`.
- No ALTER permission → fallback to `DELETE FROM`.
- IDENTITY preservation: respects `mssql_truncate_preserves_identity`.

If fallback to DELETE FROM occurs in atomic mode, everything
still runs in the transaction — DELETE + INSERT atomically, just
slower than TRUNCATE would be. In non-atomic mode, DELETE runs
in autocommit as its own statement.

##### 4.7.6.3 Interaction with CTAS

CTAS (`CREATE TABLE ... AS SELECT`) is a separate code path
that creates a new table; TRUNCATE mode does not apply there.
If the table already exists and the user issues CTAS, DuckDB
errors before the extension sees the statement. Users wanting
"create or replace" semantics use:

```sql
-- DuckDB's own CREATE OR REPLACE, which drops+creates
CREATE OR REPLACE TABLE mssql.dbo.target AS SELECT * FROM source;
```

vs. "keep schema, replace data":

```sql
COPY source TO 'mssql:db.dbo.target' (FORMAT mssql, TRUNCATE true);
```

Both valid, different semantics — document the choice in the
user-facing reference.

##### 4.7.6.4 Tests

- Append mode (default): existing rows preserved, new rows added.
- TRUNCATE atomic: BCP failure rolls back TRUNCATE — original
  rows still present.
- TRUNCATE atomic with FK-referenced target: falls back to
  DELETE FROM inside the transaction; commits replace data.
- TRUNCATE non-atomic: parallel BCP triggered when preconditions
  met; asserted via connection-count metric.
- TRUNCATE non-atomic + BCP failure: target partially populated;
  error surfaces to user with clear message about non-atomic
  failure mode.
- Setting `mssql_copy_to_mode = 'truncate'` globally: COPY TO
  without options replaces data; assert no accidental double-
  append.

---

### 4.8 Non-ASCII Password Fix

#### 4.8.1 Likely root causes

LOGIN7 password encoding specification (MS-TDS §2.2.6.4):

1. Convert password text to UCS-2/UTF-16LE.
2. Byte-swap each nibble: `((b & 0x0F) << 4) | ((b >> 4) & 0x0F)`.
3. XOR each byte with `0xA5`.
4. `ibPassword` = byte offset of password in variable data block.
5. `cchPassword` = **number of 16-bit characters** (NOT bytes).

Common bugs at any of these steps:

- **(a)** UTF-8 → UTF-16 conversion uses locale-dependent `mbstowcs`
  which depends on `LC_ALL`/`LC_CTYPE` — fails for bytes outside
  current locale.
- **(b)** XOR applied to UTF-8 bytes before conversion.
- **(c)** `cchPassword` set to UTF-8 byte length instead of UTF-16
  code unit count.
- **(d)** `cchPassword` set to UTF-16 byte count (double the correct
  value).
- **(e)** Surrogate pair handling broken (characters outside BMP,
  e.g. `𝄞`, take 2 UTF-16 code units each — character count ≠
  length of input string even in UTF-16 terms).

#### 4.8.2 Diagnosis plan

1. Add targeted test: connect with password = `"Тест123!"` (Cyrillic),
   `"Ünlaut"` (umlaut), `"🔒secure"` (emoji with surrogate pair).
2. Enable `MSSQL_DEBUG=1` to dump LOGIN7 packet.
3. Decode the password field manually (reverse XOR 0xA5, reverse
   nibble swap, interpret as UTF-16LE) — compare to expected.
4. Fix the step that differs.

#### 4.8.3 Fix

Once identified, the fix likely belongs in
`TdsConnection::BuildLogin7Packet()`. Replace whatever string
conversion is there with:

```cpp
// UTF-8 → UTF-16LE via simdutf (same library used elsewhere)
size_t password_utf16_units = simdutf::utf16_length_from_utf8(
    password.data(), password.size());

std::vector<char16_t> password_utf16(password_utf16_units);
simdutf::convert_utf8_to_utf16le(
    password.data(), password.size(), password_utf16.data());

// Apply TDS XOR/nibble-swap per MS-TDS §2.2.6.4
for (auto& unit : password_utf16) {
    uint8_t lo = unit & 0xFF;
    uint8_t hi = (unit >> 8) & 0xFF;
    lo = ((lo & 0x0F) << 4) | ((lo & 0xF0) >> 4);
    hi = ((hi & 0x0F) << 4) | ((hi & 0xF0) >> 4);
    unit = (hi << 8) | lo;
    unit ^= 0xA5A5;
}

// cchPassword = UTF-16 code unit count, NOT bytes
login7.cchPassword = password_utf16_units;
login7.ibPassword = /* offset in variable block */;
// Write password_utf16 data as bytes.
```

#### 4.8.4 Related: connection string parsing for non-ASCII

Also verify:

- `ConnectionString::Parse` preserves UTF-8 bytes through splitting
  on `;` and `=`. No truncation at high bytes.
- URI format (`mssql://...`) URL-decodes `%XX` escapes correctly
  before UTF-8 interpretation.
- ADO.NET format respects `{...}` quoting for passwords containing
  special characters.

Add tests:

```
mssql://user:%D0%9F%D0%B0%D1%80%D0%BE%D0%BB%D1%8C@host/db  # "Пароль"
Server=host;User Id=u;Password={p@ss;word with semicolon}
```

---

### 4.9 DML Optimization Hooks (keep current rowid, add fast paths)

#### 4.9.1 Design decision: keep PK-based rowid

We considered replacing the current PK-based rowid with physical
row identifiers (SQL Server `%%physloc%%` for heaps or clustered
key columns for CI tables). Decision: **do not**. Rationale:

- **Complexity explosion.** Identity strategy depends on table
  structure (heap / clustered / non-unique CI / CCI), each with
  different concurrency semantics and edge cases.
- **Stability requires locking.** Between `SELECT rowid WHERE ...`
  and `UPDATE WHERE rowid IN (...)` DuckDB has no transaction
  guarantee, so correctness requires `UPDLOCK, HOLDLOCK` inside
  an implicit txn — invasive change to connection pinning logic.
- **Limited marginal win** when the real bottleneck is the
  round-trip (two-phase plan) rather than the identity lookup
  itself. Direct-DML bypass (§4.9.3) eliminates the round-trip
  entirely for the common case.
- **No user demand** for UPDATE/DELETE on tables without PK
  reported in issues at time of this proposal.

Current PK-based rowid remains the default. Tables without a PK
continue to reject UPDATE/DELETE with the existing error message
(unchanged behavior).

#### 4.9.2 Optimization 1: Direct UPDATE/DELETE for pushable filters

**Problem.** Current UPDATE/DELETE plan is always two-phase:
1. `SELECT rowid FROM t WHERE <filter>` — client receives rowids
2. `UPDATE t SET ... WHERE pk IN (...)` / `DELETE FROM t WHERE pk IN (...)`

Even for trivially pushable predicates (`WHERE id = 42`), this
incurs two round-trips and transfers rowids over the network for
nothing.

**Change.** In `MssqlCatalog::PlanUpdate` / `PlanDelete`, detect
when the entire filter is pushdown-compatible (same predicate
classes already supported by scan filter pushdown: equality,
comparisons, `IN`, NULL checks, AND/OR, date parts, boolean
comparisons). If yes, emit a single server-side statement instead
of the two-phase plan:

```sql
-- DuckDB: UPDATE mssql.dbo.t SET name='X' WHERE id = 42
-- Current: 2 round-trips (SELECT rowid, then UPDATE)
-- Optimized: 1 round-trip
UPDATE dbo.t SET name = N'X' WHERE id = 42;
```

**Applicability.**
- All predicates must be pushable. First non-pushable predicate
  falls back to two-phase.
- UPDATE `SET` clause values must be literals or pushable
  expressions.
- `RETURNING` disables this path — needs the scan phase for
  `OUTPUT INSERTED.*` / `OUTPUT DELETED.*`. (Future: we can
  combine server-side UPDATE with `OUTPUT` to still preserve
  single round-trip; defer to v0.3.)
- Joins in UPDATE/DELETE (via `USING` clause) disable this path.

**Setting**:
```sql
SET mssql_direct_dml = true;          -- default, optimization on
SET mssql_direct_dml = false;         -- force two-phase (debugging)
```

**Implementation notes**:

```cpp
unique_ptr<PhysicalOperator> MssqlCatalog::PlanUpdate(
    ClientContext& ctx,
    LogicalUpdate& op,
    unique_ptr<PhysicalOperator> plan
) {
    if (CanDirectDml(ctx, op)) {
        return make_uniq<MssqlDirectUpdateOperator>(
            op.table, op.columns, op.expressions, op.table_filters);
    }
    return DefaultUpdatePlan(op, std::move(plan));
}

bool CanDirectDml(ClientContext& ctx, const LogicalUpdate& op) {
    if (!GetSetting<bool>(ctx, "mssql_direct_dml")) return false;
    if (op.return_chunk) return false;          // RETURNING
    if (HasJoinReferences(op.expressions)) return false;
    if (!AllFiltersPushable(op.table_filters)) return false;
    if (!AllSetExprsPushable(op.expressions)) return false;
    return true;
}
```

`MssqlDirectUpdateOperator` reuses the existing filter → T-SQL
translator from scan pushdown (§4.1 codec layer makes
`FormatSqlLiteral` the single point for this).

**Expected win.** For typical point-updates on remote SQL Server
(~30ms RTT): 2 RTT → 1 RTT = 2× throughput. For batched operations
over many disjoint predicates: depends on how DuckDB batches, but
at minimum removes the rowid materialization overhead.

#### 4.9.3 Optimization 2: TRUNCATE detection (see §4.10)

See the dedicated §4.10 below. Full empty-filter DELETE is a
special case of §4.9.2 with different server-side statement and
additional safety checks.

#### 4.9.4 What we explicitly do NOT change (this refactor)

- **Row identity for UPDATE/DELETE remains PK-based.** Tables
  without PK still cannot be updated/deleted. No `%%physloc%%`
  fallback. No clustered-key fallback.
- **No implicit txn wrapping.** Two-phase DML continues to run in
  autocommit mode without `UPDLOCK, HOLDLOCK`. Race conditions
  with concurrent writers remain possible (unchanged behavior,
  mirrors current postgres_scanner and other connectors).
- **No CCI-specific UPDATE/DELETE handling.** Behavior on
  columnstore tables matches general tables — slow but correct
  (server handles the delta store internally).

These are revisit-later candidates for v0.3+ if real user
workloads demand them.

---

### 4.10 TRUNCATE Optimization

#### 4.10.1 Background

In DuckDB, `TRUNCATE tbl` is parsed as an alias for
`DELETE FROM tbl` with no `WHERE` clause. Both forms surface at
the storage extension as a `LogicalDelete` with an empty filter
expression list — they are **indistinguishable**.

This is actually convenient: one optimization path handles both
user-facing syntaxes.

Current behavior: the extension fetches all rowids from the
table (full network transfer of PK values), then issues batched
`DELETE WHERE pk IN (...)` statements. For a 10M-row table this
can take minutes. SQL Server's native `TRUNCATE TABLE` completes
in milliseconds (extent deallocation only).

#### 4.10.2 Detection

In `MssqlCatalog::PlanDelete`:

```cpp
unique_ptr<PhysicalOperator> MssqlCatalog::PlanDelete(
    ClientContext& ctx, LogicalDelete& op, unique_ptr<PhysicalOperator> plan
) {
    bool is_full_table_delete =
        op.expressions.empty() &&   // no WHERE
        !op.return_chunk &&          // no RETURNING
        !HasUsingClause(op);         // no USING subquery

    if (is_full_table_delete) {
        auto& table = op.table.Cast<MssqlTableEntry>();
        if (ShouldUseTruncate(ctx, table)) {
            return make_uniq<MssqlTruncateOperator>(table);
        }
    }

    return DefaultDeletePlan(op, std::move(plan));
}
```

#### 4.10.3 Safety metadata

Extend the catalog metadata cache with three flags per table,
populated by a single additional query during metadata load:

```sql
-- Added to the metadata-loading query
SELECT
    t.object_id,
    -- enabled DELETE triggers (INSTEAD OF or AFTER)
    (SELECT COUNT(*) FROM sys.triggers tr
     WHERE tr.parent_id = t.object_id
       AND tr.is_disabled = 0
       AND OBJECTPROPERTY(tr.object_id, 'ExecIsDeleteTrigger') = 1
    ) AS enabled_delete_triggers,
    -- tables that reference this table via FK (block TRUNCATE)
    (SELECT COUNT(*) FROM sys.foreign_keys fk
     WHERE fk.referenced_object_id = t.object_id
       AND fk.is_disabled = 0
    ) AS incoming_fk_count,
    -- columnstore type: 0=none, 5=CCI, 6=NCCI
    (SELECT TOP 1 i.type FROM sys.indexes i
     WHERE i.object_id = t.object_id AND i.type IN (5, 6)
    ) AS columnstore_type
FROM sys.tables t
WHERE t.object_id = OBJECT_ID(@full_name);
```

Cache alongside existing column/PK/stats info. Invalidated on
`mssql_refresh_cache()` or by normal TTL.

#### 4.10.4 Decision logic

```cpp
bool ShouldUseTruncate(ClientContext& ctx, const MssqlTableEntry& table) {
    auto mode = GetSetting<std::string>(ctx, "mssql_truncate_mode");
    if (mode == "off") return false;

    // 'force' mode: skip all safety checks. User is responsible.
    // Useful when triggers are intentionally bypassed for bulk reload.
    if (mode == "force") return true;

    // 'auto' mode (default)
    const auto& meta = table.GetMetadata();

    // DELETE trigger present: semantic difference, TRUNCATE does not fire
    // trigger, but DELETE does. Fall back to preserve DELETE semantics.
    if (meta.enabled_delete_triggers > 0) return false;

    // Referenced by another table via FK: TRUNCATE will fail.
    // Pre-check and fall back to avoid the error, even if the referencing
    // tables happen to be empty.
    if (meta.incoming_fk_count > 0) return false;

    // Inside explicit transaction: TRUNCATE takes a schema-level lock and
    // interacts awkwardly with the transaction's pinned-connection model
    // (mssql_connection_provider.cpp:GetConnection line 97-157). Safer
    // to fall back (can be revisited).
    if (!ctx.transaction.IsAutoCommit()) return false;

    // Session-level cache: if we previously tried TRUNCATE on this table
    // and got a permission error, do not retry for the session.
    if (table.catalog.HasTruncatePermissionDenied(table.name)) return false;

    return true;
}
```

#### 4.10.5 Operator implementation

```cpp
class MssqlTruncateOperator : public PhysicalOperator {
    reference<MssqlTableEntry> table_;
public:
    SourceResultType GetData(
        ExecutionContext& ctx, DataChunk& chunk, OperatorSourceInput& input
    ) override {
        auto conn = table_.get().catalog.GetConnection(ctx);
        auto full_name = table_.get().GetQuotedFullName();  // [dbo].[tbl]

        // Optional: capture IDENTITY current value for reseed
        int64_t saved_ident = 0;
        bool preserve_identity = GetSetting<bool>(
            ctx, "mssql_truncate_preserves_identity");
        if (preserve_identity && table_.get().HasIdentityColumn()) {
            saved_ident = conn->QueryScalar<int64_t>(
                "SELECT IDENT_CURRENT(" + QuoteString(full_name) + ")");
        }

        int64_t affected;
        try {
            conn->Execute("TRUNCATE TABLE " + full_name);

            if (preserve_identity && table_.get().HasIdentityColumn()) {
                conn->Execute("DBCC CHECKIDENT ("
                    + QuoteString(full_name)
                    + ", RESEED, " + std::to_string(saved_ident) + ")");
            }

            // TRUNCATE does not report affected_rows. Use cached row estimate.
            affected = table_.get().GetApproxRowCount();

            // Invalidate stats; approx count is now 0.
            table_.get().catalog.InvalidateStats(table_.get().name);

        } catch (const MssqlError& e) {
            if (e.IsPermissionError()) {
                // User lacks ALTER permission needed for TRUNCATE.
                // Cache decision for session, fall back to DELETE.
                Warning(ctx,
                    "TRUNCATE requires ALTER permission; falling back to "
                    "DELETE FROM. Set mssql_truncate_mode='off' to avoid "
                    "this warning.");
                table_.get().catalog.MarkTruncatePermissionDenied(
                    table_.get().name);
                conn->Execute("DELETE FROM " + full_name);
                affected = -1;  // unknown
            } else {
                throw;  // other errors propagate normally
            }
        }

        FillAffectedRowsChunk(chunk, affected);
        return SourceResultType::FINISHED;
    }
};
```

#### 4.10.6 Settings

```sql
-- Enable TRUNCATE detection (applies to both TRUNCATE and DELETE FROM t)
SET mssql_truncate_mode = 'auto';    -- DEFAULT: TRUNCATE if safe, else DELETE
SET mssql_truncate_mode = 'off';     -- Always DELETE (pre-v0.2 behavior)
SET mssql_truncate_mode = 'force';   -- TRUNCATE without safety checks
                                      -- (skip trigger check, let server error
                                      --  on FK / permission)

-- Keep current IDENTITY value after TRUNCATE (default: SQL Server semantics,
-- which is to reset IDENTITY to seed).
SET mssql_truncate_preserves_identity = false;
```

#### 4.10.7 Explicit function `mssql_truncate()`

For users who want to bypass detection and express intent
explicitly:

```sql
SELECT mssql_truncate('sqlsrv', 'dbo.my_table');
-- Returns: BOOLEAN (true on success)
```

Behavior:
- Always issues `TRUNCATE TABLE`, skipping detection logic.
- Respects `mssql_truncate_preserves_identity` setting.
- Invalidates metadata cache for the table.
- Permission errors and FK-block errors surface as DuckDB errors
  (no DELETE fallback).

Useful for:
- ETL scripts where intent is "drop all rows fast, no trigger
  semantics".
- Bypassing the trigger check when triggers are disabled but
  `sys.triggers` still lists them.
- Workflows where silent fallback to slow DELETE is undesirable
  and should fail loudly.

Signature added to function registry alongside existing
`mssql_exec`, `mssql_refresh_cache`, etc.

#### 4.10.8 Breaking change considerations

Going from DELETE-row-by-row to TRUNCATE changes observable
behavior in several ways:

| Aspect | Old (DELETE) | New (TRUNCATE) |
|---|---|---|
| Duration | minutes for large tables | milliseconds |
| Transaction log | full logging | minimal (extent dealloc) |
| IDENTITY seed | preserved | reset (unless `preserves_identity=true`) |
| DELETE triggers | fired per row | **not fired** |
| FK references | handled per row | **blocks the operation** |
| Permission | `DELETE` on table | `ALTER` on table |
| Affected rowcount | exact | approximate (from stats) |
| Inside txn | OK (with pin) | schema lock, different semantics |

Mitigated by:

- Default mode is `auto`, which falls back to DELETE when
  triggers/FK/txn present.
- Pre-0.2.0 behavior restorable via `mssql_truncate_mode = 'off'`.
- Breaking change documented in the 0.2.0 release notes migration
  guide.
- Explicit `mssql_truncate()` function gives opt-in unambiguous
  fast path.

#### 4.10.9 Tests

`test/integration/truncate_optimization_test.cpp`:

- Simple heap: `DELETE FROM t` → TRUNCATE, row count = 0 after.
- Table with DELETE trigger: `DELETE FROM t` → DELETE, trigger
  fires; `mssql_truncate()` → trigger does not fire (assert via
  audit table).
- Table referenced by FK (even from empty table): `DELETE FROM t`
  → DELETE (fallback); `mssql_truncate()` → error.
- Table with IDENTITY, `preserves_identity=true`: assert next
  inserted row continues the original sequence.
- Low-permission user without ALTER: first TRUNCATE attempt hits
  permission error, operator falls back to DELETE with warning.
  Subsequent DELETE on same table does not re-attempt TRUNCATE
  (session cache).
- Inside explicit `BEGIN; DELETE FROM t; ROLLBACK;`: falls back to
  DELETE so ROLLBACK restores rows (TRUNCATE would also roll back
  in SQL Server, but the schema lock behavior is avoided).
- CCI table: TRUNCATE works natively in SQL Server; assert fast
  path is taken.

---

### 4.11 MERGE INTO Support via Server-Side Push-Down

#### 4.11.1 Current state

DuckDB 1.4+ supports `MERGE INTO`. For storage extensions the
feature is opt-in: DuckDB's planner dispatches MERGE to a catalog
hook, and if the extension hasn't implemented it, DuckDB emits
`Database type "mssql" does not support MERGE INTO or ON
CONFLICT`. Same error pattern observed in `duckdb-iceberg`
before MERGE support was added there. Our extension currently
falls into this bucket.

SQL Server itself has had native `MERGE` since SQL Server 2008
with effectively the same ANSI syntax DuckDB adopted. This makes
server-side push-down the natural implementation.

#### 4.11.2 Design decision: server-side push-down (not client emulation)

Two options considered:

**Option A — Per-action decomposition** (the DuckLake pattern):
DuckDB decomposes MERGE into its component UPDATE/INSERT/DELETE
actions, and the extension plans each separately. Simple to
implement (reuses existing DML hooks) but:
- Not atomic without wrapping in explicit transaction.
- Multiple round-trips per MERGE.
- Each action still pays the rowid-fetch cost.
- MATCHED/NOT MATCHED membership evaluated client-side — requires
  full source materialization in DuckDB memory.

**Option B — Full server-side push-down** (our choice): upload
the USING source to a temp table on SQL Server, then emit a
native `MERGE` statement. Atomic, single round-trip, uses SQL
Server's optimizer, reuses the BCP fast path from §4.6.

Decision: **Option B** as the primary implementation.
Client-side emulation available as opt-in fallback for users
affected by SQL Server MERGE edge cases (see §4.11.6).

#### 4.11.3 Execution pipeline

```
MERGE INTO mssql.dbo.target t
USING <source_relation> s ON <merge_condition>
WHEN MATCHED [AND <cond>] THEN UPDATE SET ...
WHEN NOT MATCHED [AND <cond>] THEN INSERT (...) VALUES (...)
WHEN NOT MATCHED BY SOURCE [AND <cond>] THEN DELETE
[RETURNING merge_action, ...];

                      ↓

MssqlMergeOperator::Execute():

  1. Acquire a connection and pin for the duration of the
     operation:
        - If user is in explicit transaction, use the
          already-pinned connection.
        - If autocommit, acquire one from the pool and pin it
          locally (release when operator finishes).
     Everything below runs on this single connection.
  2. Derive projection: only columns from source referenced in
     ON condition, WHEN clauses, and INSERT/UPDATE sets.
  3. CREATE TABLE #hugr_merge_<uuid> (<projection>) via DDL
     generator (codec layer → T-SQL). Session-local temp table,
     heap, no indexes, no constraints. No CREATE TABLE
     permission needed in user DB.
  4. Pipelined BCP upload (§4.6.5.2) of source DataChunks into
     #hugr_merge_<uuid> on the same pinned connection:
        - Encoder thread reads from DuckDB scan, codec-encodes
          rows into ring buffer.
        - Sender thread writes from ring buffer into the TDS
          socket.
        - Two threads, one connection — network pipe stays full,
          encoder overlaps with network transit.
  5. Build T-SQL MERGE string:
     - ON condition: FormatSqlExpression (codec layer)
     - WHEN clauses: each expression formatted through same
     - Target and source table references: quoted identifiers
  6. Execute MERGE on the same pinned connection.
     If RETURNING is present, add OUTPUT $action, INSERTED.*,
     DELETED.* and stream results back into DuckDB DataChunks.
  7. Retrieve @@ROWCOUNT as total affected.
  8. Temp table auto-drops when the connection's session ends;
     issue explicit DROP TABLE on the success path to release
     tempdb space immediately.
  9. Unpin connection if acquired locally in step 1.
```

All steps execute on a **single pinned connection** within an
implicit (or user's explicit) transaction. Critical for:
- Session `#tmp` visibility to MERGE.
- Atomic commit/rollback semantics.
- Consistent isolation with HOLDLOCK (see §4.11.4).
- Correct interaction with user's explicit transaction if
  already in one.

Pipelining in step 4 provides the throughput win without
requiring multi-connection complexity. For a typical remote
SQL Server (30ms RTT), a 1M-row MERGE source upload completes
in roughly the time of sequential single-connection BCP divided
by the encode/send overlap factor — about 1.5-2× faster than
naive sync encode-then-send, and indistinguishable from
what a parallel-upload approach would achieve for sources that
fit within network bandwidth.

#### 4.11.4 Mandatory HOLDLOCK hint

SQL Server MERGE without `HOLDLOCK` has a well-documented race
condition:

```
Session A:   [SELECT target row, not found] ............
Session B:   ................... [INSERT same key]
Session A:   [decide INSERT based on stale read]
             → PK violation OR (if no PK) duplicate row
```

The `HOLDLOCK` table hint on the target promotes shared locks to
key-range locks held until end of transaction, preventing
phantom inserts in the matched range. Equivalent to
`SERIALIZABLE` isolation scoped to the MERGE statement.

Our generated MERGE always includes `WITH (HOLDLOCK)`:

```sql
MERGE INTO dbo.target WITH (HOLDLOCK) AS t
USING #hugr_merge_<uuid> AS s
ON t.id = s.id
WHEN MATCHED ...
```

Setting to disable for users who know their workload is
single-writer:
```sql
SET mssql_merge_holdlock = true;   -- default, correctness
SET mssql_merge_holdlock = false;  -- skip HOLDLOCK (at your risk)
```

#### 4.11.5 Optimization: same-catalog source

When the USING source is a scan of a table in the **same attached
mssql catalog**, skip materialization entirely:

```sql
-- User query:
MERGE INTO mssql.dbo.target t
USING mssql.dbo.staging s ON t.id = s.id
WHEN MATCHED THEN UPDATE SET t.amount = s.amount;

-- Generated on server: no temp table
MERGE INTO dbo.target WITH (HOLDLOCK) AS t
USING dbo.staging AS s ON t.id = s.id
WHEN MATCHED THEN UPDATE SET t.amount = s.amount;
```

Detection: traverse `LogicalMerge`'s source subtree, check
whether it's a single `MssqlCatalogScan` against the same
catalog, no filters/projections that would change semantics
(or: inline any such filters into the generated MERGE's
source clause: `USING (SELECT cols FROM dbo.staging WHERE ...) AS s`).

This is the common ETL pattern (merge from staging into final
table in the same DB) and avoids the BCP round-trip entirely.

For **all other sources** (cross-catalog, DuckDB local tables,
`read_parquet`, anything else) — the temp table path from §4.11.3
is used uniformly. We considered supporting inline `VALUES` for
small batches and Table-Valued Parameters for large ones as
alternative source-delivery mechanisms, and decided against both:

- **VALUES**: SQL Server's hard 1000-row limit per `VALUES` clause
  forces multi-statement splitting for non-trivial batches, and
  each statement is a unique query text that bypasses plan cache.
  Would complicate the code path for at best a 2-RTT saving on
  very small inputs.
- **TVP (Table-Valued Parameters)**: requires `CREATE TYPE` DDL
  on the user database (permission and cleanup burden), adds TDS
  RPC parameter marshaling as a new wire-format code path
  alongside our existing BCP, and its type definition tied to
  specific column layout makes it awkward for dynamic schemas.

The temp-table path with pipelined BCP (§4.6.3) gives us a single
uniform mechanism that works for any source size and any
schema. Overhead on small batches (CREATE + DROP = 2 extra RTTs)
is acceptable given that small-batch MERGE is not the performance
path anyone is optimizing for; large-batch MERGE — where the win
matters — is where BCP excels.

#### 4.11.6 SQL Server MERGE caveats (document honestly)

Beyond the race without HOLDLOCK, SQL Server MERGE has a history
of known correctness issues in edge cases:

- Filtered indexes on target: partial matches mishandled in
  specific SQL Server versions (pre-2019 mostly).
- Indexed views referencing target: incorrect incremental update
  in rare scenarios.
- Trigger execution order with multiple triggers defined for
  UPDATE/INSERT/DELETE on the same target.
- `$action` attribution in OUTPUT clause vs actual row mutations
  — historically several bugs, mostly resolved.

For our typical use case (DuckDB as analytics client doing batch
reload / SCD / delta reconciliation) these rarely matter. But we
should call them out in user-facing docs with a link to
Microsoft's acknowledged issue list and Aaron Bertrand's survey.

#### 4.11.7 Opt-out: client-side emulation

For users who hit MERGE edge cases or prefer explicit UPDATE+INSERT
semantics:

```sql
SET mssql_merge_strategy = 'server';   -- DEFAULT: server-side MERGE
SET mssql_merge_strategy = 'client';   -- emulate via UPDATE + INSERT
                                        -- in BEGIN/COMMIT wrapped txn
SET mssql_merge_strategy = 'error';    -- reject MERGE, force rewrite
```

Client-side emulation flow:

```
BEGIN TRAN;
-- Upload source to #tmp (same as server path)
-- Fetch matched rowids:
SELECT t.<rowid>, s.<src_idx>
FROM dbo.target t WITH (UPDLOCK, HOLDLOCK)
JOIN #tmp s ON <ON condition>;
-- Apply WHEN MATCHED actions (batched UPDATE/DELETE by rowid)
-- For each WHEN MATCHED branch in predicate order:
UPDATE dbo.target SET ... 
    FROM #tmp s JOIN dbo.target t ON <ON> 
    WHERE <when_matched_condition>;
-- Apply WHEN NOT MATCHED by source (using anti-join):
INSERT INTO dbo.target (...) 
SELECT ... FROM #tmp s 
    WHERE NOT EXISTS (SELECT 1 FROM dbo.target t WHERE <ON>);
COMMIT;
```

Multiple round-trips, slower, but sidesteps MERGE-specific bugs.
Uses UPDLOCK+HOLDLOCK on the target read for equivalent isolation.

#### 4.11.8 RETURNING support

DuckDB:
```sql
MERGE INTO ... RETURNING merge_action, id, name;
```

SQL Server equivalent:
```sql
MERGE INTO ... OUTPUT $action, INSERTED.id, INSERTED.name
                     , DELETED.id, DELETED.name;
```

Mapping:
- `merge_action` → `$action` (VARCHAR: 'INSERT', 'UPDATE', 'DELETE')
- `INSERTED.<col>` for INSERT and UPDATE branches
- `DELETED.<col>` for DELETE and UPDATE-old-values branches
- Combine via column coalescing — DuckDB's `RETURNING` column
  list implies which we need from each side.

Results stream back via existing token-parsing path (reuses
`mssql_scan` row decoder from codec layer).

For the client-side emulation strategy, RETURNING is harder —
requires concatenating OUTPUT clauses from each sub-statement
with a `merge_action` literal injected per source. Document as
supported but slower.

#### 4.11.9 Settings summary

```sql
SET mssql_merge_strategy = 'server';        -- server | client | error
SET mssql_merge_holdlock = true;            -- HOLDLOCK in server mode
SET mssql_merge_temp_table_prefix = '#hugr_merge_';  -- for debugging
```

#### 4.11.10 Tests

`test/integration/merge_test.cpp`:

- Basic upsert: MERGE INTO t USING src — WHEN MATCHED UPDATE,
  WHEN NOT MATCHED INSERT. Assert final state matches expected.
- Full three-way: WHEN MATCHED UPDATE, WHEN NOT MATCHED INSERT,
  WHEN NOT MATCHED BY SOURCE DELETE. This is the SCD Type 1
  pattern.
- RETURNING merge_action, *: assert DuckDB receives proper action
  labels and mutated values.
- Same-catalog source: assert no temp table created (observable
  via SQL trace); MERGE statement generated directly.
- Cross-catalog source (local → mssql): assert temp table upload
  via BCP; ~10M row source completes in expected time.
- Client-side emulation: same functional tests with `SET
  mssql_merge_strategy = 'client'`.
- HOLDLOCK off + concurrent insert: document observable race
  (test may be nondeterministic, so mark as "demonstration only").
- MERGE on table with DELETE trigger: trigger fires per row
  (both server and client strategies).
- MERGE in explicit DuckDB transaction: assert rollback on
  manual ROLLBACK, commit on COMMIT; temp table cleaned in
  either case.

---

## 5. Release & Implementation Plan

### 5.1 Single 0.2.0 target release

Current version is **0.1.18**. The next public release is
**0.2.0**, containing everything described in this document as
a single coordinated cut. No intermediate 0.1.19, 0.1.20 etc.
public releases.

**Rationale.** For a solo-maintained project, each intermediate
release carries fixed overhead (changelog, tagging, CI signing,
DuckDB Community Extensions PR, user migration advice) that's
disproportionate to the value of incremental public versions.
Shipping one well-tested 0.2.0 with a clear migration guide is
strictly better than a stream of 0.1.x where each user has to
decide whether any given release matters.

### 5.2 Implementation = sequence of 6 spec-driven PRs

After evaluating the full §4 work, we grouped it into six
`.specify/` workflow units. The goal is each spec being:

- A coherent testable feature (not a micro-step in a refactor).
- Sized ~1500-3500 LOC, reviewable in one sitting.
- Independently mergeable where possible; clear dependency chain
  where not.

Each spec lands as one pull request against `main`. PRs merge
into trunk as they land; `main` stays releasable at all times but
is not tagged 0.2.0 until the full set is done.

Each spec is self-contained with: scope, API/config surface, test
plan, backward-compat notes, and acceptance criteria — following
the existing `.specify/` template used by specs 001-041.

Spec 042 is reserved for the integrated-authentication work
proposed in discussion #97 (community contribution). The
remaining specs in this refactor occupy slots 043 through 047.

**Standalone design documents** have been extracted for the two
foundational specs that will be fed directly into the `.specify/`
workflow:

- `refactoring-foundation-043.md` — full design for spec 043
  (foundation fixes: simdutf wrapper + LOGIN7 password fix).
- `refactoring-codec-044.md` — full design for spec 044 (codec
  layer consolidation including VARIANT, type-mapping holes).

Each standalone doc is self-contained (preamble, scope, design,
acceptance criteria) so speckit can generate the `.specify/`
artifacts from a single file input. The full v0.2.0 context
(this document) remains the authoritative source for the other
four specs (042 owned by community contributor, 045-047 to be
designed in detail closer to implementation time).

### 5.3 The six specs

| # | Title | Scope (§) | LOC est. | Owner | Deps |
|---|-------|-----------|---------:|-------|------|
| 042 | Integrated authentication | §4.4 | ~2000 | community (#97) | — |
| 043 | Foundation fixes | §4.2, §4.8 | ~700 | core | — |
| 044 | Codec layer | §4.1 | ~3500 | core | 043 |
| 045 | CTAS quality + TRUNCATE semantics | §4.7, §4.10 | ~1500 | core | 044 |
| 046 | BCP throughput | §4.6.1-4.6.4 | ~1500 | core | 044 |
| 047 | DML performance story | §4.6.5.3, §4.9.2, §4.11 | ~2500 | core | 044, 046 |

**Parallelism**:
- 042 (community) and 043 (core) start in parallel — no
  cross-dependencies, different owners, different files.
- 044 starts as soon as 043 lands.
- 045, 046 both depend on 044 but not on each other.
- 047 depends on 044 + 046; sits at the end of the chain.

**Why one codec spec (not split into hot-path + SQL-generation)**:
the codec layer is the foundation of everything downstream. A
half-migrated state where decode/encode go through codecs but
DDL generation, filter literals, and INSERT VALUES still use
ad-hoc dispatch is harder to reason about than one atomic
change. The utf_conversion utility built here is also used
outside the codec proper — LOGIN7 password encoding (fixed in
043 as a localized one-liner, then converted to use the shared
utility when 044 lands) is the obvious second consumer, and
other call sites (PRELOGIN server name, COLLATION sort key
handling) will pick it up over time. Treating UTF as a codec-layer
concern from day one avoids duplicating the SIMD migration
later.

---

#### 042 — Integrated authentication (community, #97)

Tracked separately under `specs/042-integrated-authentication/`.
Scope and design are owned by the contributor; coordination
happens on the discussion thread and the feature branch. From
the perspective of the core refactor, 042 is a parallel track —
it touches `src/tds/auth/` (extending the existing
`AuthenticationStrategy` interface) and `src/connection/`
(connection-string parsing), with no overlap on the encoding,
BCP, CTAS, or DML paths.

---

#### 043 — Foundation fixes

**`specs/043-foundation-fixes.md`** covers §4.2 (simdutf UTF
conversion) and §4.8 (LOGIN7 non-ASCII password fix).

Why together: both are small standalone fixes touching low-level
encoding. Both independent of everything else. Splitting them
into two specs doubles `.specify/` overhead for ~700 LOC of
combined change.

Scope:
- Add simdutf as a vcpkg dependency, build wrapper at
  `src/tds/encoding/utf16.cpp` exposing `Utf8ToUtf16LE` /
  `Utf16LEToUtf8` with the same signatures as the current hand-
  rolled converter. Old converter stays in place for now —
  switching call sites is the codec spec's job (044).
- Fix LOGIN7 password encoding: investigate the non-ASCII
  password bug (likely UTF-16LE char-count vs byte-count
  mismatch or XOR/nibble-order issue on the obfuscated bytes),
  fix it as a localized patch in `src/tds/auth/sql_auth_strategy.cpp`.
  No interface changes.

Out of scope for 043:
- Migrating call sites to use simdutf. That's part of the codec
  layer migration in 044.
- Refactoring auth strategies. The password fix is a one-liner-
  scale patch; structural changes wait for spec 042 (Oluies)
  which is already restructuring this area.

Testable:
- simdutf: unit tests for round-trip UTF-8 ↔ UTF-16LE on edge
  cases (surrogates, empty strings, non-BMP characters, invalid
  sequences). Both old and new converters tested against the
  same fixtures during the transition.
- Password: integration test against Docker SQL Server with
  passwords containing non-ASCII bytes (cyrillic, accented Latin,
  CJK). Current 0.1.18 fails on these; after fix they succeed.

---

#### 044 — Codec layer

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

#### 045 — CTAS quality + TRUNCATE semantics

**`specs/045-ctas-truncate.md`** covers §4.7.1-4.7.5 (CTAS
quality), §4.7.6 (COPY TO truncate mode), §4.10 (TRUNCATE
detection for empty-filter DELETE).

Why together: all three are "bulk data operations UX". CTAS text
defaults directly affect subsequent BCP performance (wide unbounded
columns force LOB storage). §4.7.6 COPY TO TRUNCATE reuses the
§4.10 TRUNCATE execution mechanism including all safety checks.
They form one coherent set of user-visible defaults changes under
the 0.2.0 breaking-change boundary.

Scope:
- CTAS: new settings `mssql_ctas_default_text_length` (default
  4000), `mssql_ctas_compression` (`'page'` default),
  `mssql_ctas_infer_text_length` (`'off'`/`'sample'`/`'exact'`),
  primary key propagation.
- CTAS: close remaining DDL-side type-mapping issues via
  `AppendDdlColumnType` from spec 044.
- TRUNCATE detection: `PlanDelete` hook checks for empty filter,
  runs safety query (triggers/FK/columnstore check), emits
  `TRUNCATE TABLE` when safe, falls back to DELETE otherwise.
- COPY TO TRUNCATE: new `TRUNCATE true` option distinct from
  existing `OVERWRITE true` (which remains DROP+CREATE). Atomic
  mode default; non-atomic mode opt-in for parallel BCP
  throughput.

Depends on 044 for codec-driven DDL generation.

Testable:
- CTAS creates bounded NVARCHAR columns, PAGE compression
  applied, PK preserved where source has one.
- `DELETE FROM t` (no WHERE) against safe target → TRUNCATE;
  with triggers/FK → DELETE FROM with warning.
- `COPY src TO 'mssql:target' (FORMAT mssql, TRUNCATE true)`:
  rows replaced atomically on success; rollback on BCP failure.

---

#### 046 — BCP throughput

**`specs/046-bcp-throughput.md`** covers §4.6.1 (packet size
negotiation), §4.6.2 (extended TABLOCK auto-detection), §4.6.3
(encoder/sender pipelining), §4.6.4 (column-batch encoding).

Why together: all four are single-connection BCP performance
improvements. Packet size is a LOGIN7 constant change; pipelining
is the architectural piece; column-batch depends on codec layer
having batch-oriented methods; TABLOCK is settings refinement.
All four land together as "the BCP 2× speedup" story.

Scope:
- LOGIN7: request max packet size (32767 = 0x7FFF) instead of
  4096 default; existing negotiation code already honors what
  server returns.
- TABLOCK: extend existing `is_new_table` auto-detection to also
  cover empty heap targets, AG-aware (don't auto-enable under
  replication).
- Pipelining: encoder thread + sender thread with bounded ring
  buffer (4 slots × 32KiB). New settings `mssql_bcp_pipeline`
  (default `true`), `mssql_bcp_pipeline_slots` (default 4).
  Refactor existing `src/copy/bcp_writer.cpp` — the
  `accumulator_buffer_` + `SendBulkLoadPacket` serialization
  splits into producer + consumer.
- Column-batch encoding: codec `EncodeBcpBatch` operates
  columnar (gather all values of one column, then interleave).
  Uses the codec interface from 044.

Depends on 044 (codec interface, specifically `EncodeBcpBatch`).

Testable:
- Benchmark harness: single-connection BCP against 30ms-RTT
  SQL Server (simulated via `tc` on Linux, or real remote). Before
  vs after: assert ≥2× throughput improvement.
- Packet size: wire-capture verifies 32KB frames post-negotiation.
- Pipelining: concurrent encoder+sender observable in profiler;
  stall-free ring buffer under typical chunk sizes.

---

#### 047 — DML performance story

**`specs/047-dml-performance.md`** covers §4.9.2 (direct
UPDATE/DELETE pushdown for pushable filters), §4.11 (MERGE
rewrite), §4.6.5.3 (parallel BCP into user target).

Why together: all three are "DML performance" that share
infrastructure. MERGE uses pipelined single-connection BCP from
046 for the source upload. Direct DML pushdown and parallel BCP
target both inspect target shape (heap/CI, constraints, recovery
model) — unified precondition-check helper.

Scope:
- Direct DML: when `WHERE` filter is fully pushable (all
  predicates supported by `filter_encoder.cpp`), emit
  `UPDATE target SET ... WHERE <pushdown>` or
  `DELETE FROM target WHERE <pushdown>` directly — skip rowid
  fetch round-trip. New setting `mssql_direct_dml` (default
  `true`).
- MERGE: new `src/dml/merge/` with physical operator. Override
  `MSSQLCatalog::PlanMergeInto` (new hook). Create session
  `#tmp`, pipelined BCP upload of USING source (via spec 046
  pipelining), emit native `MERGE INTO ... WITH (HOLDLOCK)` with
  user's original ON/WHEN conditions. OUTPUT $action for
  RETURNING. Drop #tmp in operator cleanup.
- Parallel BCP target: runtime preconditions (heap, no CI, no
  unique constraints, SIMPLE/BULK_LOGGED recovery, not in AG,
  autocommit context), auto-fallback to spec 046
  single-connection pipelined path when any fail. New setting
  `mssql_bcp_parallelism` (default 1; `'auto'` = min(cores,
  pool_limit) when preconditions met).

Depends on 044 (codec for `FormatSqlLiteral` in direct-DML and
MERGE clause generation), 046 (BCP pipelining for MERGE source
upload and as single-connection fallback for parallel BCP).

Testable:
- Direct DML: `UPDATE t SET c = 1 WHERE pk = 5` traced via TDS
  logging shows single round-trip with direct UPDATE (no rowid
  SELECT before it).
- MERGE: standard DuckDB MERGE syntax with all three WHEN
  branches (MATCHED/NOT MATCHED/NOT MATCHED BY SOURCE), custom
  per-branch conditions, `RETURNING merge_action, *`. Currently
  0.1.18 has each of these broken; after 047 all work.
- Parallel BCP: CTAS into 10M-row heap with
  `mssql_bcp_parallelism = 4` uses 4 connections concurrently
  (observable via `sys.dm_exec_sessions` count), throughput 3×
  single-connection path.

---

### 5.4 Scope guard: deferrable scope

If the six-PR plan slips, the following scope can move to a
0.2.1 follow-up without compromising the 0.2.0 release value:

- **Parallel BCP target within spec 047** — ship 047 with only
  direct DML + MERGE rewrite; defer parallel target to 0.2.1.
  Single-connection pipelined BCP from 046 covers most of the
  CTAS/COPY-TO-heap throughput need; parallel is the last ~3×.
- **Client-side MERGE fallback** (§4.11.7) — ship only the
  server-side strategy in 047, add client-side if real production
  issues surface.
- **CCI-specific CTAS handling** (§4.7.2 columnstore path) —
  ship 045 with `compression = 'page'` as the new default; add
  `columnstore` option in 0.2.1.

**Non-deferrable**: 042 (community-driven integrated auth), 043
(foundation fixes — simdutf is consumed by 044, password bug fix
is user-visible), 044 (codec layer — foundation for 045-047),
046 (BCP pipelining — the headline throughput win), core of 047
(MERGE rewrite — fixes broken functionality, not just
performance). These are the features that make 0.2.0 worth
cutting.

### 5.5 Testing strategy per spec

Each spec PR lands with:

- Unit tests for the isolated component (codec, converter, auth
  provider, etc.).
- Integration tests against the Docker SQL Server fixture in
  `test/integration/`.
- If performance-relevant: a benchmark in `test/benchmark/` with
  a committed baseline number from CI hardware.
- Updated docs (README section, `docs/` reference if applicable).

A spec PR is not merged until all existing tests pass AND the
new tests are green AND (if applicable) the benchmark shows no
regression on unaffected workloads.

### 5.6 Pre-release validation

Before tagging 0.2.0:

1. Run full benchmark suite (§7.3) end-to-end on representative
   workloads. Compare against 0.1.18 baseline.
2. Validate against at least three SQL Server configurations:
   SQL Server 2019 Developer (local container), SQL Server 2022
   Developer (local container), Azure SQL Database. Named
   instance test against a Windows VM fixture.
3. Exercise integrated auth: MIT Kerberos on Linux container,
   Apple GSS.framework on macOS developer workstation, SSPI on
   Windows VM.
4. Full test matrix on three DuckDB versions: current minimum
   (1.4.3), latest LTS (1.4.x), latest stable (1.5.x).
5. Write user-facing migration guide: new settings, breaking
   changes with before/after examples, feature announcements.
6. Publish to DuckDB Community Extensions with the 0.2.0 tag.

### 5.7 Post-release: 0.2.1 and beyond

Items deferred per §5.4 become candidates for 0.2.1 along with
anything surfaced by production feedback:

- UPDATE/DELETE with `RETURNING` via single-statement `OUTPUT`
  clause (§4.9 extension).
- Cross-catalog DML (`DELETE FROM mssql.t WHERE id IN (SELECT ...
  FROM local_tbl)`) via temp-table upload.
- Physical-rowid path for PK-less tables (revisit §4.9 decision
  only if user demand materializes).
- MERGE client-side emulation fallback (§4.11.7).
- Additional BCP optimizations based on production profiling.

---

## 6. Breaking Changes / Compatibility

The refactor is almost entirely internal. Public SQL API changes
are limited to:

1. **New default**: `NVARCHAR(4000)` for CTAS text columns instead
   of `NVARCHAR(MAX)`. Users relying on `MAX` must set
   `mssql_ctas_default_text_length = 0`.
2. **New default**: `PAGE` compression for CTAS. Users on older
   SQL Server Standard editions (< 2016 SP1) where compression
   required Enterprise SKU must set `mssql_ctas_compression = 'none'`.
3. **New default**: `variant` strategy for unsupported types.
   Queries that previously errored on XML/SQL_VARIANT/UDT columns
   now return VARIANT values. Users parsing error messages for
   these types must set `mssql_unsupported_type_strategy = 'error'`.
4. **Type mapping corrections** from §4.7.5. Specifically, `UUID`
   mapping from VARCHAR to `UNIQUEIDENTIFIER`. Roundtrip scripts
   relying on VARCHAR form break. Migration guide should call this
   out.
5. **New default**: `mssql_truncate_mode = 'auto'`. Empty-filter
   `DELETE FROM t` (and its `TRUNCATE t` alias) now emits SQL
   Server `TRUNCATE TABLE` when safe. Users relying on DELETE
   triggers firing must set `mssql_truncate_mode = 'off'`, add a
   `WHERE 1=1` predicate, or rely on the trigger-presence
   auto-detection (which falls back to DELETE). Tables with
   IDENTITY get the seed reset unless
   `mssql_truncate_preserves_identity = true`. Called out in the
   0.2.0 migration guide.
6. **New default**: `mssql_direct_dml = true`. UPDATE/DELETE with
   fully-pushable filters skip the rowid fetch phase and execute
   as a single server-side statement. Observable differences:
   fewer network round-trips; the intermediate rowid state is no
   longer visible to concurrent readers in other sessions. Users
   who depended on the two-phase visibility pattern must set
   `mssql_direct_dml = false`.

All six default changes ship together in 0.2.0. Since there are
no intermediate public releases, the pre-v0.2 warning pattern
does not apply — the migration guide in the 0.2.0 release notes
is the sole point of user communication. Users on 0.1.18 read it
once, choose whether to opt out of any of the new defaults via
settings, and proceed.

---

## 7. Testing Strategy

### 7.1 Unit tests

Each codec gets a roundtrip test file in `test/encoding/`:

```
test/encoding/int_codec_test.cpp
test/encoding/decimal_codec_test.cpp
test/encoding/string_codec_test.cpp
test/encoding/variant_codec_test.cpp
...
```

Pattern:
```cpp
TEST(IntCodec, RoundtripBigint) {
    auto codec = IntCodec::ForBigInt();
    auto vec_in = MakeVector<int64_t>({1, 2, INT64_MIN, INT64_MAX});
    TdsWriter w; codec->EncodeBcpBatch(vec_in, 4, w);
    TdsReader r(w.GetBytes());
    auto vec_out = EmptyVector(LogicalType::BIGINT);
    codec->DecodeBatch(r, vec_out, 4);
    AssertVectorsEqual(vec_in, vec_out);
}
```

### 7.2 Integration tests

`test/integration/` runs against live SQL Server container:

- `auth_integrated_test.cpp` — Kerberos + NTLM + channel binding.
- `named_instance_test.cpp` — Browser resolution.
- `ctas_compression_test.cpp` — PAGE, ROW, NONE, COLUMNSTORE.
- `ctas_text_inference_test.cpp` — sample, exact, off.
- `password_encoding_test.cpp` — ASCII, Cyrillic, umlauts, emoji.
- `variant_fallback_test.cpp` — XML, SQL_VARIANT, UDT.

### 7.3 Benchmark harness

New `test/benchmark/` directory:

- `bench_utf_conversion.cpp` — simdutf vs hand-rolled on
  representative string distributions.
- `bench_bcp_throughput.cpp` — end-to-end write throughput with
  each optimization flag.
- `bench_scan_throughput.cpp` — read throughput per column type.

Runs against local Docker SQL Server in CI (comparative only —
absolute numbers depend on CI hardware). Reports regressions > 5%.

### 7.4 CI matrix additions

- `ubuntu-latest` + MIT Kerberos fixture container.
- `ubuntu-latest` + Heimdal (separate job).
- `macos-latest` with `kinit` against MIT fixture over SSH tunnel
  (Apple GSS-Framework exercise).
- `windows-latest` with local SQL Server + local NTLM auth.
- Matrix over `MSSQL_VERSION`: 2019, 2022, (Azure SQL via
  service principal if feasible in CI).

---

## 8. Open Questions

1. **DuckDB VARIANT API stability.** The public constructor
   surface in 1.4.x vs 1.5.x differs slightly. `VariantBuilder`
   abstraction should insulate us, but does the extension target
   both? Decision: keep compatibility with 1.4.3+ (current
   minimum), conditionally enable VARIANT code path based on
   DuckDB version macro. Pre-1.4 users get `varchar` fallback
   strategy.

2. **Clustered index detection for parallel BCP.** We need to
   query `sys.indexes` to check if the target is a heap before
   allowing parallel streams. Adds a pre-flight query to every
   CTAS/COPY when `parallelism > 1`. Acceptable overhead?

3. **GSS-API library path on Linux.** Some containerized
   environments (Alpine) don't ship Kerberos by default. Should
   we statically link MIT Kerberos as a vcpkg dependency for the
   CI artifacts, at the cost of binary size, or require users to
   `apk add krb5-libs`? Leaning toward the latter (consistent with
   "no build deps" principle) with a clear error message pointing
   at the package.

4. **Connection string parser rewrite.** The current ADO.NET
   parser is hand-rolled. While fixing password encoding, should
   we replace it with a more principled parser (proper quoting,
   escape handling, `{...}` braces)? Scope creep — probably defer
   to a separate cleanup.

5. **BCP bulk API vs row-stream.** Current implementation uses
   `INSERT BULK` row-stream (TDS BulkLoadBCP). SQL Server also
   supports `BULK INSERT` with staging file on server. For
   local → remote this is slower (file upload required), but for
   same-host it's the fastest option. Out of scope for v0.2;
   consider for v0.3.

---

## 9. References

- **MS-TDS**: [MS-TDS] Tabular Data Stream Protocol,
  <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-tds/>
- **MS-SSTDS** (TDS streaming): same reference, section on
  bulk data ops.
- **SQL Server Browser**:
  <https://learn.microsoft.com/en-us/sql/tools/configuration-manager/sql-server-browser-service>
- **SPNEGO RFC 4178**, **Kerberos V5 GSS-API RFC 4121**.
- **Channel Bindings RFC 5929** — `tls-server-end-point`.
- **DuckDB VARIANT**: announcement for v1.4.0 / v1.5.0.
- **simdutf**: <https://github.com/simdutf/simdutf>
- **DuckDB extension templates + CI tools**:
  <https://github.com/duckdb/extension-ci-tools>

---

## 10. Changelog (document)

| Date | Change |
|---|---|
| 2026-04-19 | Initial draft. |
| 2026-04-19 | Added §4.9 (DML optimization hooks — keep PK rowid, add direct-DML path) and §4.10 (TRUNCATE detection with safety checks). Added §2.9 motivation. Roadmap now includes milestone 0.1.13 (TRUNCATE) and 0.2.2 (DML polish). Two new breaking-change entries in §6. |
| 2026-04-19 | Added §4.11 (MERGE INTO via server-side push-down, same-catalog shortcut, HOLDLOCK default, client-side fallback, RETURNING support). Added §2.10 motivation. MERGE included in 0.2.0 milestone; client-side fallback deferred to 0.2.2. |
| 2026-04-19 | §4.11.5 simplified: single source-delivery path (temp table + pipelined BCP) for all non-same-catalog sources. VALUES and TVP alternatives evaluated and rejected — documented why. |
| 2026-04-19 | Milestone numbers corrected: current version is 0.1.18, so next releases start at 0.1.19. §4.6.5 split into two subsections: 4.6.5.1 (parallel BCP into temp/staging tables — always safe, ships in 0.2.0 as part of MERGE path), 4.6.5.2 (parallel BCP into user target tables — conditional, stays in 0.2.1 due to precondition matrix). §4.11.3 updated to use named user-DB staging (`hugr_staging.m_<uuid>`) to enable cross-session visibility required for parallel upload; falls back to session `#tmp` when CREATE TABLE permission missing. |
| 2026-04-19 | §5 rewritten: single 0.2.0 release target, no intermediate 0.1.x public releases. Work structured as 21 sequenced spec/PR units across five phases (prerequisites, codec layer, features-on-codec, BCP pipeline, DML/MERGE). Added §5.4 scope guard listing deferrable specs. §6 updated to reflect single-release migration model. |
| 2026-04-19 | §4.6.5 significantly reworked after user clarification on connection concurrency semantics. Removed the earlier "parallel BCP into shared staging tables" design — that required either CREATE TABLE permission in user DB (often unavailable) or global temp tables (operationally fragile). Replaced with: §4.6.5.1 single-connection pipelined BCP (always applicable, encoder/sender threads share one connection, no correctness implications for pinned or unpinned contexts); §4.6.5.2 multi-connection parallel BCP only for direct user target writes in autocommit — the one case where multiple connections are natively safe. §4.11.3 MERGE pipeline reverted to session `#tmp` + single-connection pipelined upload. Spec list renumbered: 21 → 20 specs (old 0018 "parallel staging" removed; old 0019 became 0018; old 0020/0021 became 0019/0020). |
| 2026-04-19 | §4.6.5 restructured again to lead with an operation-by-context decision matrix (§4.6.5.1). Renumbered subsections: single-connection pipelined is now §4.6.5.2, multi-connection parallel for user targets is §4.6.5.3. The matrix makes explicit that CTAS and COPY-to-heap can parallelize in autocommit, while MERGE, INSERT...SELECT, and anything in a user transaction stay single-connection. Cross-references updated throughout. |
| 2026-04-19 | Added §4.7.6: COPY TO with TRUNCATE mode. Accepts `TRUNCATE true` (and synonyms `OVERWRITE`, `MODE 'truncate'`) as COPY options, plus global `mssql_copy_to_mode` setting. Two execution modes: atomic (default, single-connection, transaction-wrapped) and non-atomic (parallel BCP-enabled for throughput). Reuses §4.10 TRUNCATE safety logic. Added spec 055 (copy-to-truncate-mode) to Phase C; renumbered all subsequent specs (0015-0021). Spec count 20 → 21. |
| 2026-04-19 | Code-sync pass #1 against actual v0.1.18 repo. Confirmed findings: `src/dml/merge/` does not exist; MERGE currently routes through existing UPDATE+INSERT hooks via DuckDB's default decomposition against rowid-based `PlanUpdate`/`PlanDelete`. `src/tds/auth/` already has `AuthenticationStrategy` interface with `SqlAuthStrategy`/`FedAuthStrategy`/`ManualTokenStrategy` — integrated auth becomes a fourth sibling, not a new subsystem. CTAS config (`mssql_ctas_text_type`, `mssql_copy_tablock`, `mssql_ctas_use_bcp`, `is_new_table` auto-flag) already present with sensible defaults; new settings in this refactor are additive. Existing specs number up to 041. |
| 2026-04-19 | Code-sync pass #2, documentation corrections: (1) §4.7.6 fixed OVERWRITE conflict — existing `OVERWRITE true` means DROP+CREATE (schema replacement), so removed it as synonym for TRUNCATE. TRUNCATE is now a distinct third option (append | truncate | overwrite), enforced mutually exclusive. (2) §4.4 integrated auth rewritten to extend the existing `AuthenticationStrategy` interface with one new virtual method (`NextToken` for multi-round SSPI/GSSAPI exchange) and one new flag (`RequiresIntegratedAuth`) — existing three strategies get harmless defaults. New files live under `src/tds/auth/` alongside existing strategies. (3) §4.1.1 expanded with LOC counts and exact migration targets for all 9 source files that contain type-dependent logic (type_converter.cpp 515, bcp_row_encoder.cpp 758, value_serializer.cpp 466, filter_encoder.cpp 982, ctas_executor.cpp 567, plus helpers). (4) Spec list renumbered from 0001-0021 to 042-062 to continue existing `.specify/` workflow convention (last existing: 041-xml-type-support). |
| 2026-04-19 | Code-sync pass #3. (1) §4.10.4 fixed — replaced invented `IsInExplicitTransaction(ctx, catalog)` helper with actual API `ctx.transaction.IsAutoCommit()` (as used in `mssql_connection_provider.cpp:GetConnection` line 103). Reference path updated. (2) §4.6.2 TABLOCK section rewritten — acknowledged existing `mssql_copy_tablock` setting with `is_new_table` auto-detection (already shipped in spec 025/027). Proposed "new setting" removed; refactor just extends trigger conditions (heap detection, empty table detection, AG awareness). (3) Settings diff confirmed: all proposed new `mssql_*` settings are additive, no collisions with existing 30+ settings in `mssql_settings.cpp`. TABLOCK setting remains `mssql_copy_tablock` (not `mssql_bcp_tablock`). Packet-size negotiation: confirmed existing `negotiated_packet_size_` in `tds_connection.cpp` reads server-returned ENVCHANGE value; §4.6.1 change is purely in the requested size at LOGIN7 time (currently 4096 via `TDS_DEFAULT_PACKET_SIZE`). |
| 2026-04-19 | §5 consolidated from 21 specs to 6. Grouping principle: each spec is a coherent user-testable feature of ~1500-3500 LOC, not a micro-step in refactor. New layout: 042 foundation fixes (simdutf + password), 043 codec layer (one big-bang PR), 044 enterprise connectivity (VARIANT + integrated auth + named instance), 045 CTAS quality + TRUNCATE semantics, 046 BCP throughput (packet/pipeline/batch/TABLOCK), 047 DML performance (direct DML + MERGE + parallel BCP target). 042/043/044 can proceed in parallel; 045/046 both depend on 043; 047 depends on 043+046. Cross-references in §4.1.1 updated from old spec 050 to new spec 043. §5.4 scope guard rewritten — single deferrable item (parallel BCP within 047). |
| 2026-05-13 | §5 expanded from 6 to 7 specs. The previous spec 043 ("Codec layer consolidation, one big bang") was split into two: new 043 (Scan + BCP codec layer — hot encoding/decoding paths) and new 045 (SQL-generation codec migration — `FormatSqlLiteral` for filter pushdown and INSERT VALUES, `AppendDdlColumnType` for CTAS DDL). Rationale: derisk the codec refactor by validating the interface on hot paths with extensive existing test coverage before extending it to three more call-sites; ship UTF-heavy workload wins (simdutf) sooner; parallelize cleanly with external contributor work on auth (Oluies, discussion #97). Previous 045/046/047 renumbered to 046/047/048. Cross-references in §4.1.1, §5.4 scope guard, and inter-spec deps updated. |
| 2026-05-14 | §5 final renumbering: spec 042 reassigned to community-contributed integrated authentication (Oluies, discussion #97, already using `specs/042-integrated-authentication/`). Core refactor work now occupies 043-047 (six specs total, including 042). Previous 043 + 045 (split codec specs) merged back into single spec 044 "Codec layer" per user direction — utf_conversion is now framed as a codec-layer utility shared with LOGIN7 password encoding and other UTF-16 consumers, not a separate scope-limited prerequisite. Spec 043 "Foundation fixes" keeps simdutf + password bug fix as localized landings, with 044 consolidating the call-site migration. Cross-references in §4.1.1 collapsed back to single spec 044. Final spec count: 6 (one community, five core). |
| 2026-05-14 | Created two standalone design extracts for `.specify/` workflow input: `refactoring-foundation-043.md` (foundation fixes — simdutf + LOGIN7 password) and `refactoring-codec-044.md` (codec layer consolidation, VARIANT, type-mapping holes). Each is self-contained with preamble, scope, design, coordination notes, acceptance criteria — speckit can generate `.specify/` artifacts from a single file input. Main doc unchanged in structure; §5.2 updated to reference the extracts. |

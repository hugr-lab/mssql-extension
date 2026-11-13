# Spec 093: one decoder per type, for the TDS stream and for backups

Status: **draft**, 2026-11-13. Not started. The owner's decisions are recorded as DQ-42 – DQ-45 in
[085's decisions.md](../085-backup-reader-core/decisions.md) and quoted where they apply; geography's CRS label was amended
the same day (§ Why `OGC:CRS84`). Line references were taken at main `96ecf7e`.

This spec is the "types track". It runs in parallel with spec 085 PR 3 (off-row values). It takes
over the type half of spec 086 (D2, D6, D8a and tasks 8 – 10, 12, 15, 16), because a type is
decoded once for both read paths (086 D8a, owner 2026-11-11). 086 keeps the storage formats:
ROW / PAGE compression, sparse columns, MS-BINXML and the `json` spike.

Markers: **[spec]** a Microsoft open specification (MS-TDS, MS-SSCLRT, MS-BCP); **[doc]** Microsoft
Learn product documentation; **[measure]** to be measured on a throwaway container before the PR that
depends on it (§ Experiments); **[measured]** measured on one, recorded in `measured/<id>.md`;
**(derived)** a rule this spec derives from an owner decision rather than one the owner stated, open
to review.

## Problem

Today, by type:

| SQL Server | `TYPE mssql` read | raw `mssql_scan` | write | backup (085) |
|---|---|---|---|---|
| `sql_variant` | `VARCHAR`, through a `CASE` over `SQL_VARIANT_PROPERTY(…, 'BaseType')` with CONVERT styles (#542, `src/catalog/mssql_column_info.cpp:589-598`); the base type is lost, and the server spends 8.8× the CPU of a plain cast | **refused**: COLMETADATA type `0x62` is not parsed (`src/tds/tds_column_metadata.cpp:438`) and the describe refuses it at bind | statement path only, as a string literal, so the server stores an `nvarchar` base type; BCP refused | `BLOB`, its stored bytes |
| `vector(n)` (2025) | **unreadable** (#363): `sys.columns` says `varbinary`, so the catalog declares `BLOB`; the server sends `varchar(max)` JSON, and the stream fails its shape check | `VARCHAR`, the JSON text | none (`src/codec/type_family.cpp:127` throws for `ARRAY`) | `BLOB` labelled `converted`: the reader takes the system type from the xtype, `varbinary`, so its `vector` test never fires (finding of the recon, measured in V4) |
| `geometry` | `GEOMETRY`, no CRS, through `.STAsBinary()` (`mssql_column_info.cpp:561-563`): 2D only, Z and M dropped, server CPU per value | **refused** (UDT `0xF0` not parsed) | statement path: `geometry::STGeomFromWKB(0x…, 0)`, SRID fixed at 0 (`WrapSpatialLiteral`, `src/dml/insert/mssql_value_serializer.cpp:92-112`); COPY / INSERT BULK / staged DML refused (`BulkWireCanCarry`); CTAS creates `VARBINARY(MAX)` (`type_family.cpp:108-113`) | `BLOB` |
| `geography` | as geometry, indistinguishable from it | **refused** | as geometry, SRID fixed at 4326 | `BLOB` |
| `hierarchyid` | `VARCHAR` through `CAST(… AS NVARCHAR(MAX))` | **refused** | string literal (the server parses it); BCP refused | `BLOB` |
| user CLR types | `VARCHAR` through the same cast | **refused** | string literal | `BLOB` |
| `json` (2025) | `VARCHAR` (UTF-8 `varchar(max)` on the wire) | `VARCHAR` | literal; BCP as a string | `BLOB` (undocumented binary: 086 E9) |
| `datetime2(7)` | `TIMESTAMP_NS`; a value outside 1677 – 2262 is a **silent NULL** (`src/codec/datetime_codec.cpp:807-811, 922`; the range check `Datetime2WireToNativeUnit`, `:670-720`; #168). The common case is a temporal table's `ValidTo = 9999-12-31 23:59:59.9999999`: every current row reads NULL, and `= '9999-12-31'` finds nothing | same | refused outside the server's range (#447) | same kernel, same NULL |

Three consequences:
- Six types are read through server-side rewrites (`.STAsBinary()`, `CASE … CONVERT`, `CAST … NVARCHAR`).
  They cost server CPU, lose information, and do not exist on a backup, where there is no server.
- A raw `mssql_scan` cannot read a table holding any CLR type or a `sql_variant`.
- `datetime2(7)` loses the most common far-future value without a word.

## Scope

In:
- `sql_variant` → DuckDB `VARIANT`, read and write.
- `vector(n)` → `FLOAT[n]`, read and write (#363).
- `hierarchyid` → its `ToString()` text, decoded on the client.
- `geometry` / `geography` → `GEOMETRY` from the stored MS-SSCLRT bytes, Z / M kept, with a CRS for
  geography; bulk writes.
- `json` → DuckDB `JSON` in `TYPE mssql`.
- `datetime2(7)`: a warning for out-of-range values, and a setting to read the column as `TIMESTAMP`.
- The `0x62` / `0xF0` / `0xF5` COLMETADATA and row framing, so a raw `mssql_scan` reads every one of
  these types; a user CLR type reads as `BLOB`.
- The backup reader's use of the same kernels (085 D13's stored bytes give way to the real types).

Out:
- Spatial predicate pushdown (`ST_Intersects` …) and `VECTOR_DISTANCE` / `mssql_vector_search`
  pushdown. They are writer work in spec 079's line, filed under #515 / #363 as their own tasks.
- MS-BINXML (`xml` in a backup) and the `json` binary in a backup: these formats never cross the wire
  (the server sends text), so there is no shared kernel; they stay in 086 (PR 4, E9).
- Decoding user CLR types: opaque bytes by design (`BLOB`).
- Storage formats (ROW / PAGE, sparse, columnstore): 086 and 088. The owner's "no refusals after PR 3"
  covers types and off-row values, not storage formats (DQ-42).

## The owner's decisions

- **DQ-42: no refusals by type after 085 PR 3** (owner, 2026-11-13). Every type in a backup reads as
  its DuckDB type once its kernel lands, and as its stored bytes before (085 D13). ROW / PAGE
  compression, sparse columns and columnstore are storage formats and are not covered.
- **DQ-43: `sql_variant` is read as `VARIANT`** (owner, 2026-11-13; breaking accepted). The base value
  is kept in its own type. `time(7)` inside keeps its nanoseconds (`TIME_NANOS`). A write of a value
  that SQL Server's `sql_variant` cannot hold is refused by name.
- **DQ-44: `datetime2(7)`** (owner, 2026-11-13):
  - inside a `VARIANT`, per value: `TIMESTAMP_NANOS` inside 1677 – 2262, otherwise `TIMESTAMP_MICROS`
    (the 100 ns digit dropped); never NULL;
  - a plain column keeps `TIMESTAMP_NS` by default, and an out-of-range value logs one `WARNING` per
    column into `duckdb_logs`;
  - a setting `mssql_datetime2_7_as = 'timestamp_ns' | 'timestamp'`, with an ATTACH option of the
    same name, read at ATTACH. `'timestamp'` reads the column in microseconds over the whole
    0001 – 9999 range, as `time(7)` and `datetimeoffset(7)` are read already;
  - the backup scan follows the same setting (one kernel).
- **DQ-45: spatial** (owner, 2026-11-13):
  - `geography` → `GEOMETRY('OGC:CRS84')` for the whole column (first decided as `EPSG:4326`,
    amended the same day: § Why `OGC:CRS84`);
  - `geometry` → `GEOMETRY` with no CRS. A SQL Server column declares no SRID and the bind sees no
    values, so the type cannot follow them;
  - a CTAS from a `GEOMETRY` creates a `geometry` column;
  - a curve or `FullGlobe` value read into a `GEOMETRY` column is an error. It names the column and
    the escape hatch `geometry_as := 'blob'`, an ATTACH option and a `mssql_backup_scan` parameter
    that reads the whole column as its bytes. Curves are not linearised;
  - full support, both paths ("Если можем то и геометрию сделать", 2026-11-12).
- **One decoder per type for both paths** (086 D8a, owner 2026-11-11). Restated here because this
  spec carries it out.

The WKB the kernel emits is longitude, latitude, as `STAsBinary()` and GeoParquet write it.

### Why `OGC:CRS84`, not `EPSG:4326` (owner, 2026-11-13)

The owner first chose `EPSG:4326`. The review of this draft read the pinned DuckDB (no server
needed):
- the extension can build `LogicalType::GEOMETRY("EPSG:4326")`: the definition parses as an
  authority code (`duckdb/src/common/types.cpp:2057-2070`);
- a user cannot write that type in SQL without the spatial extension. `BindGeometryType` asks
  `CoordinateReferenceSystem::TryIdentify` (`duckdb/src/catalog/default/default_types.cpp:337-361`),
  the built-in definitions are only `OGC:CRS84` and `OGC:CRS83`
  (`default_coordinate_systems.cpp:36-37`), and an authority code is not complete on its own
  (`geometry_crs.hpp:62-69`). So `x::GEOMETRY('EPSG:4326')`, a `CREATE TABLE` naming the type, or
  a cast to the column's type fails with a binder error unless spatial is loaded or
  `ignore_unknown_crs` is set;
- `COPY … TO 'x.parquet'` writes GeoParquet V1 by default, which needs PROJJSON; a CRS it cannot
  convert raises an error (`duckdb/extension/parquet/parquet_geometry.cpp:180-198`). `OGC:CRS84` is
  GeoParquet's default and is written as no CRS (`:175`).

So a `geography` column labelled `EPSG:4326` could not be copied to Parquet without the spatial
extension. **The owner amended it to `OGC:CRS84`** (2026-11-13: "это одно и то же"), #515's first
proposal: the same WGS 84 datum, with the axis order longitude, latitude that the WKB holds; it is
built in, and Parquet writes it with no conversion. D1 still checks the DuckLake write.

## Design

### D1: the kernels live in `src/codec`, DuckDB-free up to the vector write

| piece | file | fed by |
|---|---|---|
| `sql_variant` envelopes: `ParseVariantWire` (MS-TDS 2.2.5.5.4: BASETYPE, PROPBYTES, a 5-byte TDS collation for strings) and `ParseVariantDisk` (base xtype, version 1, a u32 CollationId; datetime / smalldatetime / money halves in record order, `backup_sql_variant.hpp`) → one `(base type, properties, value bytes, code page)` form | `codec/variant/variant_envelope.{hpp,cpp}` | TDS row stager; backup record stager |
| base values → `VARIANT`, through DuckDB's batch builder (`BuildVariant` + `EmitPrimitiveNode` over a cursor on the staged bytes, `duckdb/.../variant/variant_builder.hpp:351`); no `Value` per row | `codec/variant/variant_codec.cpp`, `FinalizeKernel::Variant` | the envelope's output |
| `vector`: header check (`A9`, `01`, dims equal to the column's n, element type 0 / 1, reserved bytes zero), then a copy (float32) or a widening loop (float16) into the `ARRAY` child | `codec/vector_codec.cpp`, `FinalizeKernel::Vector` | the `0xF5` wire value; the stored bytes |
| `hierarchyid`: MS-SSCLRT 2.2.2 bit string → `/a/b.c/` text | `codec/clr/hierarchyid.cpp`, `FinalizeKernel::HierarchyId` | the UDT value; the stored bytes |
| MS-SSCLRT → little-endian ISO WKB: versions 1 and 2, the single-point and single-segment forms, Z / M (ISO +1000 / +2000 / +3000 codes), empty shapes, collections (depth 16, as DuckDB), geography's latitude-first points swapped; a curve or `FullGlobe` reported as a status, not written | `codec/clr/ssclrt_wkb.cpp`, `FinalizeKernel::Spatial` | the UDT value; the stored bytes |
| WKB → MS-SSCLRT for `geometry` writes, if G4 shows the server accepts client bytes | `codec/clr/wkb_ssclrt.cpp` | the write path |

Each kernel runs once per column per chunk (`FinalizeStagedColumn`, shared by `RowStager` and
`RecordStager`); the per-value work inside it is the envelope parse, which a variable format makes
inherent. No per-value path enters the row walk. `sql_variant` is the one documented exception to
"one type per column": the base type lives in each value, so the kernel switches on it per value.

`tds::ColumnMetadata` gains what a kernel needs: `udt_kind` (geometry, geography, hierarchyid,
other) from COLMETADATA's UDT_INFO type name or from the catalog; the variant envelope (wire or disk);
the vector's element type. `ResolveColumnOps` picks the kernel from them.

### D2: the `is_cast_required` flag is split before any type moves

The flag means two things today: "read through a server cast" and "the server compares or keys it
unlike DuckDB". It is read in the filter encoder (`filter_encoder.cpp:330, 667, 1436, 1495`), the
rowid key choice (`mssql_table_entry.cpp:477`), MERGE (`mssql_merge_rewrite.cpp:393, 510`), the 079
writer (`mssql_node_writer.cpp:42, 150`, `mssql_expression_vocabulary.cpp:191`), the INSERT plan
(`mssql_catalog_plan.cpp:203`) and `mssql_column_info.cpp` itself. Two neighbours answer the same
questions and move with it:
- `is_geometry`, spatial's own flag (12 uses), which the read form replaces;
- `IsKnownSQLServerType`, which also gates the bulk wire (`BulkWireCanCarry`,
  `target_resolver.cpp:1105-1114`, the #296 trap), the CTAS executor, primary key discovery
  (`mssql_primary_key.cpp:228`) and the writer. Once `sql_variant` or `hierarchyid` became "known",
  the bulk wire would accept them silently.

Once `sql_variant` is decoded natively the first meaning goes, and the second must not go with it: a
pushed filter on a `sql_variant` drops rows (#514).

T0 replaces it with two answers, without changing behaviour:
- the **read form**: bare, server cast, or CLR rewrite. Each later PR moves its type to bare;
- the **comparison fidelity**: a type-level predicate that the filter encoder, the rowid key choice,
  MERGE and the writer ask. `sql_variant`, `vector`, `hierarchyid`, `json` (the 2025 type has no
  comparison operators **[measure]** J1), spatial and CLR types stay out of pushed comparisons and keys
  whatever their read form;
- the **bulk wire**: a per-type answer, so a type joins COPY / INSERT BULK only in its own write PR.

### D3: `sql_variant` → `VARIANT`

**Read.** The catalog column is `VARIANT`. The read expression is the bare column: the `CASE` of
#542 goes, and so does its server CPU. The wire value is `0x62`, LONGLEN (0 = NULL). A raw
`mssql_scan` and `mssql_scan_params` type such a column `VARIANT` too, and `StreamCanProduce`,
`ColumnTypeFits` and `column_types` accept it.

| base type | `VARIANT` tag | note |
|---|---|---|
| bit | BOOL_TRUE / BOOL_FALSE | |
| tinyint, smallint, int, bigint | UINT8, INT16, INT32, INT64 | |
| real, float | FLOAT, DOUBLE | |
| decimal(p,s), numeric(p,s) | DECIMAL(p,s) | decimal and numeric not told apart |
| money, smallmoney | DECIMAL(19,4), DECIMAL(10,4) | as the column map |
| date | DATE | |
| time(0–6) | TIME_MICROS | |
| time(7) | **TIME_NANOS** | DQ-43 |
| datetime2(0), (1–3), (4–6) | TIMESTAMP_SEC, TIMESTAMP_MILIS, TIMESTAMP_MICROS | as the column map |
| datetime2(7) | **TIMESTAMP_NANOS** inside 1677 – 2262, else TIMESTAMP_MICROS | DQ-44 |
| datetime, smalldatetime | TIMESTAMP_MICROS | as the column map |
| datetimeoffset(0–6) | TIMESTAMP_MICROS_TZ | the UTC instant; the offset is lost, as the column map loses it |
| datetimeoffset(7) | TIMESTAMP_NANOS_TZ inside 1677 – 2262, else TIMESTAMP_MICROS_TZ | **(derived)** from DQ-44 |
| uniqueidentifier | UUID | |
| binary(n), varbinary(n) | BLOB | as stored, padding included |
| char(n), varchar(n) | VARCHAR | transcoded by the value's own collation (spec 091's kernels, any page, double-byte included); `char(n)` trimmed as the column codec trims it (#405) **(derived)**, **[measure]** S1. A collation with no table (newer than the extension) gives a BLOB inside the `VARIANT`, its bytes, and one `WARNING` per column **(derived)** |
| nchar(n), nvarchar(n) | VARCHAR | `nchar(n)` trimmed likewise **(derived)** |

What a `VARIANT` cannot say: decimal vs numeric, money vs decimal(19,4), datetime vs datetime2 vs
smalldatetime, varchar vs nvarchar, the collation, the declared length. So a `sql_variant` column
stays out of rowid keys, keyless DML and MERGE ON (D2's comparison fidelity), as #515 records.

**Write** (T2). A `VARIANT` value is written as `CAST(<typed literal> AS <base type>)`, the base
type chosen from its tag; the server converts it to `sql_variant`. The table is **(derived)**: the
narrowest base type that holds every value of the tag.

| `VARIANT` tag | base type written |
|---|---|
| BOOL | bit |
| INT8, INT16 | smallint |
| UINT8 | tinyint |
| INT32, UINT16 | int |
| INT64, UINT32 | bigint |
| UINT64, INT128, UINT128 | decimal(38,0) when it fits, else refused |
| FLOAT, DOUBLE | real, float |
| DECIMAL(w,s) | decimal(w,s) |
| DATE, TIME_MICROS, TIME_NANOS | date, time(6), time(7) |
| TIMESTAMP_SEC / _MILIS / _MICROS / _NANOS | datetime2(0) / (3) / (6) / (7); nanoseconds rounded to 100 ns, as today's TIMESTAMP_NS literal rounds (`ComputeDatetime2Components`); TIME_NANOS → time(7) likewise |
| TIMESTAMP_MICROS_TZ, TIMESTAMP_NANOS_TZ | datetimeoffset(6), datetimeoffset(7) at +00:00 |
| UUID | uniqueidentifier |
| BLOB | varbinary(n), n ≤ 8000 |
| VARCHAR | nvarchar(n), n ≤ 4000 UTF-16 code units; past that, varchar(n) when every character is ASCII and n ≤ 8000 |
| SQL NULL / VARIANT NULL | NULL |

Refused by name (DQ-43), the column and the value's tag in the message: OBJECT, ARRAY, BIGNUM,
BITSTRING, GEOMETRY, INTERVAL, TIME_MICROS_TZ, an integer past decimal(38,0), a string that fits
neither form above, a blob past 8000 bytes, a date or timestamp the server cannot hold (`ServerHoldsTemporal`). The
value-level literal goes through the existing `FormatSqlLiteral` of the base type's family.

COPY, INSERT BULK and `#stage`: an SSVARIANT bulk encoder (`0x62` in INSERT BULK's COLMETADATA) if
S2 shows the server takes it. Otherwise:
- an INSERT of `VARIANT` values stays on the statement path, and COPY of one is refused by name, as
  geometry is today;
- an UPDATE of a keyed table setting a `sql_variant` column stays on the VALUES path whatever
  `mssql_dml_stage_threshold` says;
- the paths that always stage refuse by name: keyless UPDATE / DELETE whose stage holds a
  `sql_variant` column, and a staged MERGE whose `#src` would. A CTAS from a `VARIANT` creates a `sql_variant`
column.

### D4: `vector(n)` → `FLOAT[n]`

- **Negotiation.** LOGIN7 FeatureExt `0x0E` VECTORSUPPORT, requesting version 2 (float32 and
  float16) **[spec]** (MS-TDS 2.2.6.4, 2.2.7.11). The ack parser already skips unknown ids by length,
  so the request is safe on every server.
  - A setting `mssql_vector_support` (default `true`) turns the request off, as
    `mssql_utf8_support` does **(derived)**.
  - It is read once per catalog at ATTACH, as `mssql_client_transcode` is **(derived)**, so the
    logins of one catalog's pool ask alike.
  - SQL Server 2025 17.0.5035.5 acknowledges a request for version 1, 2 or 3 with **version 1**. A
    login that requests version 0 is dropped with no error token **[measured]** V1.
  - The acknowledged version is kept per connection.
- **Wire.** The type is VECTORTYPE `0xF5`, and its TYPE_INFO is `<USHORT max length> <BYTE element
  type>` (0 = float32, 1 = float16) **[measured]** V1.
  - A value is a USHORT length and exactly the column's number of bytes **[measured]** V1. A NULL
    is `0xFFFF` **[spec]**, which the bulk path takes **[measured]** V6.
  - Those bytes are an 8-byte header and the little-endian elements: `A9 01`, the dims as a u16, the
    element type as a u8, three zero bytes. They are the same bytes a backup stores (V4).
- **Form per column.** COLMETADATA decides, column by column.
  - Under version 1, a float32 column streams `0xF5`, and a float16 column streams the JSON form
    below **[measured]** V1. The float16 `0xF5` read form was not seen: no version above 1 was
    acknowledged.
  - Every form carries UserType 255: a column, an alias, a `CAST(… AS vector(n))` and an `ISNULL`
    over it **[measured]** V1. A raw scan therefore recognises a vector from COLMETADATA alone.
  - n is `(max length − 8) / element size`.
- **Catalog type.** `ARRAY(FLOAT, n)`. The metadata queries name a column's type by
  `ISNULL(TYPE_NAME(c.system_type_id), …)` (`mssql_metadata_cache.cpp:205, 237`,
  `mssql_primary_key.cpp:61`), which says `varbinary` for a vector. They gain the user type's name
  when it is `vector` (`user_type_id` 255).
  - The element type is `sys.columns.scale` (0 or 1). n is `(max_length − 8) / (scale = 1 ? 2 :
    4)` **[measured]** V5.
  - 2025's `vector_dimensions` / `vector_base_type_desc` say the same, but the queries do not name
    them, because the query text must also compile on 2017–2022.
  - An alias type over `vector` cannot exist: Msg 42212 **[measured]** V5.
  - A float16 vector widens to `FLOAT` exactly.
- **Without the feature, and for float16 under version 1,** the column is still declared `FLOAT[n]`.
  - It arrives as `varchar(max)` PLP text under `Latin1_General_100_BIN2_UTF8`, as `[e1,…]` with each
    element printed `%.7e`. It is parsed on the client.
  - **float32 is inexact** **[measured]** V2. 8 significant digits do not identify every float32.
    Two neighbouring values print as the same text, and about 1.5 % of float32 values read back as a
    neighbour, one ulp (unit in the last place) away.
  - **float16 is exact** **(derived)**: 8 digits are more than float16 needs, and the values V2
    tried print exactly.
  - A one-line `WARNING` per catalog says so the first time a float32 vector is read this way.
- **The describe.** **[doc]** says `sp_describe_first_result_set` "doesn't correctly return the
  vector data type".
  - The procedure and `sys.dm_exec_describe_first_result_set` follow the session **[measured]** V3.
    On a session that acknowledged the feature, a float32 column is `vector(n)` (165 / 255, max
    length 20 for n = 3). A float16 column is `varchar(max)` on every session, as is any vector on
    a session without the feature.
  - In each case it names the form that session streams. With the setting fixed per catalog, one
    catalog's describe and stream agree. A stream that still differs is the existing shape-check
    error (for example, after a failover to another build).
- **Write.** The statement path sends `CAST(N'[…]' AS vector(n))` with 9 significant digits
  (`%.9g`), which is exact for float32 **(derived)** from V2.
  - COPY, INSERT BULK and `#stage` declare `vector(n)` / `vector(n, float16)` in the INSERT BULK
    statement. They send each value's bytes, the header and the elements, as `varbinary(length)`
    (`0xA5`) on every session **(derived)**. No JSON is needed.
  - The server takes `0xA5` with the feature and without it, float32 and float16 alike, and stores
    the bytes as sent **[measured]** V6.
  - `0xF5` would work only on a session that acknowledged the feature (4816 otherwise), so one
    form covers both.
  - A wrong length is the server's 7347, and a bad header its 42206 **[measured]** V6.
- **CTAS.** A CTAS or COPY creates `vector(n)` from `FLOAT[n]` (n ≤ 1998) on a server
  that has the type (2025, Azure SQL), and refuses by name elsewhere. `DOUBLE[n]` is refused with the
  fix named (`::FLOAT[n]`) **(derived)**: a narrowing the user should write.
- Filters on a vector are never pushed, and it is never a key (D2).

### D5: `hierarchyid`

The column is read bare (UDT `0xF0`) and decoded on the client into the `ToString()` text, so its
type stays `VARCHAR`. It is non-breaking, and the server's `CAST … NVARCHAR(MAX)` goes. The text does
not sort like the tree, so it stays out of ORDER BY pushdown, and filters stay in DuckDB (D2). Writes
are unchanged (a string literal the server parses).

### D6: `geometry` / `geography`

**Read.** The column is read bare: UDT `0xF0`, PLP, the MS-SSCLRT value **[spec]** (MS-TDS
2.2.5.5.2). `.STAsBinary()` goes, and so does its server CPU. The kernel writes ISO WKB: Z and M are
kept (a change for data that has them), the SRID of each value is read but not checked (DQ-45). The
types:
- `geography` → `GEOMETRY('OGC:CRS84')`, whatever SRID a value
  carries. A value in another SRID
  (4120 – 4999 are allowed) keeps the label; this is documented;
- `geometry` → `GEOMETRY`;
- with `geometry_as := 'blob'`: `BLOB`, the MS-SSCLRT bytes as `CAST(g AS varbinary(max))` returns
  them (they load back by `CAST(… AS geometry)`).

A curve (version 2 shape types 8 – 10, figure attributes 2 / 3) or a `FullGlobe` (shape type 11)
fails the query: `column "dbo"."t"."g" holds a CircularString, which GEOMETRY cannot hold; read the
column as bytes with ATTACH option geometry_as 'blob' (or mssql_backup_scan(…, geometry_as := 'blob'))`.
DuckDB's GEOMETRY holds types 1 – 7 only (`FromBinary`), so writing the bytes unchecked, as the
binary kernel does today with `STAsBinary()`'s output, could let an invalid value in.

A raw `mssql_scan` takes the type from UDT_INFO's type name (geometry, geography, hierarchyid, else
`BLOB`) and the catalog's `geometry_as`.

**Write** (T6).
- The SRID comes from the source's CRS: `EPSG:n` → n, `OGC:CRS84` → 4326, no CRS → 0 for a
  `geometry` target and 4326 for a `geography` one (today's fixed values).
- The statement path keeps `STGeomFromWKB(lit, srid)`; a Z / M value needs a form the server takes,
  **[measure]** G7 (WKT `POINT(x y z m)` is documented to carry them **[doc]**). If no form keeps
  them, a value with Z / M is refused by name on write, never written flat **(derived)**.
- COPY, INSERT BULK and staged DML: the WKB is staged as `varbinary(max)` in the session table spec
  080 D3 already fills, and `INSERT … SELECT geography::STGeomFromWKB(w, srid) FROM #stage` writes it.
  The server computes geography's "larger than a hemisphere" bit, so no geodesic code runs on the
  client. It also interprets ring orientation as it does for any WKB: a clockwise exterior ring is the
  complement of the area meant. That is the server's semantics, recorded by G4 and documented, not
  corrected. The stage costs the direct load's minimal logging, TABLOCK and parallel writers (each
  writer would need its own stage); the PR measures it. For `geometry`, client-built MS-SSCLRT sent
  directly as `varbinary` (MS-BCP 2.1.5 **[spec]**) replaces the stage if G4 shows the server takes
  it.
- A CTAS or COPY from `GEOMETRY`, any CRS, creates `geometry` (DQ-45); today it creates
  `VARBINARY(MAX)`.

### D7: `json` → `JSON`

`TYPE mssql` reports `LogicalType::JSON()` for a `json` column (the recon's T7, accepted with the PR
split). The wire form is unchanged
(`varchar(max)` UTF-8). JSONSUPPORT (`0x0D`) is not requested: it changes nothing but the describe.
Slightly breaking: a `DESCRIBE` says `JSON`.

### D8: `datetime2(7)`

- **The warning.** The kernel counts the values it cannot hold as `TIMESTAMP_NS`. The stream (TDS) or
  the scan (backup) logs one `WARNING` per column per statement, the first time it sees one: `mssql: column
  "ValidTo" holds datetime2(7) values outside 1677-2262 (e.g. 9999-12-31), read as NULL; set
  mssql_datetime2_7_as = 'timestamp' to read them`. The NULL itself is unchanged by default.
- **The setting.** `mssql_datetime2_7_as`, `'timestamp_ns'` (default) or `'timestamp'`, read once per
  catalog at ATTACH like `mssql_client_transcode`, with the ATTACH option `datetime2_7_as` winning.
  Under `'timestamp'` the column is `TIMESTAMP`, the 100 ns digit truncated; filters are rewritten
  over the range of stored values as `time(7)`'s are (`TruncatedReadComparison`), and the column is
  no longer a lossless key (`IsRoundTripExactForKey`: `NotRoundTrip`, as `time(7)`). `column_types`
  and the 079 vehicle follow the catalog's answer.
- **What the key rule costs.** A primary key holding a `datetime2(7)` column then falls through to
  another unique index; a table with no usable key and a `datetime2(7)` column (the default scale)
  cannot be keyed by all its columns either (`mssql_table_entry.cpp:476-486`), so its UPDATE / DELETE
  are refused by name. That is today's behaviour for `time(7)`, documented beside the setting.
- **Plumbing.** The key and filter checks see only the type name and scale today
  (`IsRoundTripExactForKey`, `IsLiteralMismatch`, `mssql_rowid_key_choice.hpp:110-131`;
  `IsTruncatedReadColumn`, `filter_encoder.cpp:329`; `IsGroupable`, `mssql_node_writer.cpp:149-157`;
  primary key discovery, `mssql_primary_key.cpp:228`, has no catalog). They take the column's read
  type instead (`TIMESTAMP` vs `TIMESTAMP_NS`), which the catalog's answer already fixes at ATTACH.
- **ORDER BY** is not pushed for the column, as today. A truncated read keeps the order of one key,
  but not of a key list: two values 100 ns apart tie in DuckDB, which then orders by the next key,
  where the server does not. (`time(7)` and `datetimeoffset(7)` are pushed as any key today,
  `mssql_column_info.cpp:359-361`: that is a bug in its own right, to be filed as its own issue.)
- **The backup scan** reads the setting at bind (it has no ATTACH yet; 085 task 23's ATTACH reads it
  there). That part lands with T8, so T9 itself touches no backup file before 085 PR 3.

### D9: the backup's use of the kernels

`TypeReading` and `MetadataOf` gain one call into a new `src/backup/backup_type_forms.cpp`, which
maps a catalog column to its kernel:
- the disk envelope for `sql_variant`;
- `vector`, detected by its user type (255) and `type_name`, not the xtype. In `syscolpars` it has
  xtype 165 and utype 255, `scale` is the element type and `length` is `8 + n × element size`. The
  stored bytes are the `0xF5` wire value's (V4);
- the CLR system types, by name;
- a user CLR type, which stays `BLOB`.

The stager keeps its `Bytes` arm, so no new staging arm is needed. A spatial or `sql_variant` value
that went off row is assembled by PR 3 before the kernel sees it. The state column of
`mssql_backup_columns` says `converted` for each type once its kernel is hooked.

Until 085 PR 3 merges, T1 – T7 touch no backup file. T8 hooks every kernel merged by then; after it,
each later type PR hooks its own kernel.

## What changes for a user

| change | PR | breaking? |
|---|---|---|
| `sql_variant` reads as `VARIANT`, base types kept; raw scans read it | T1 | **yes**: VARCHAR → VARIANT in DESCRIBE, CTAS, Parquet / DuckLake output |
| `VARIANT` values write into `sql_variant` in their own base types | T2 | no (was a string base type) |
| `vector(n)` tables are readable, as `FLOAT[n]`; writes and CTAS `vector(n)` | T3 | no (was unreadable) |
| `hierarchyid` decoded on the client, raw scans read it | T4 | no (same text) |
| `geography` is `GEOMETRY('OGC:CRS84')`; Z / M kept; curves / FullGlobe a named error with `geometry_as` | T5 | **yes**: the type of geography columns; the WKB of values with Z / M; a curve that read before (as whatever `STAsBinary()` emitted) now fails |
| spatial COPY / INSERT BULK / staged DML; SRID from the CRS; CTAS creates `geometry` | T6 | **yes**: CTAS from GEOMETRY creates geometry, not VARBINARY(MAX) |
| `json` is `JSON` | T7 | slightly: the type name |
| the backup reads the types above instead of their bytes | T8 | yes for backup users of PR 2's `BLOB`s; the backup reader is unreleased |
| `datetime2(7)`: a warning, and `mssql_datetime2_7_as` | T9 | no |

Each breaking row gets a CHANGELOG entry under "Changed", and `docs/type-mapping.md` is updated in the
PR that changes it.

## Experiments

Run on throwaway containers, 2022 and 2025, from the images pinned in `test/backup/gen/images.txt`.
The captured bytes are committed as test vectors (synthetic values only). Each experiment runs before
the PR it decides.

| # | for | what | decides |
|---|---|---|---|
| V1 | T3 | LOGIN7 with VECTORSUPPORT v0 – v3; `SELECT v` from `vector(3)` and `vector(3, float16)`, and expressions over it | the acknowledged version; the `0xF5` TYPE_INFO byte order; the float16 form; UserType |
| V2 | T3 | the same without the feature: the text of 1.0, 0.1, 3.4028235e38, denormals, −0.0 | whether the JSON fallback is exact |
| V3 | T3 | `sp_describe_first_result_set` on `SELECT v`, float32 and float16, with and without the feature | whether the describe and the stream agree |
| V4 | T8 | a backup of `vector(3)` and `vector(3, float16)`: syscolpars xtype / utype / length, the stored bytes | u32 vs u8 + reserved element type on disk; the backup's vector detection |
| V5 | T3 | `sys.columns` and `sys.types` for vector columns on 2025 | how the element type is known |
| V6 | T3 | INSERT BULK into a vector column with `0xF5`, `varbinary` and JSON text, with and without the feature | the bulk form |
| S1 | T1 | `sql_variant` over TDS: one row per base type, char padding, code-page / UTF-8 / SC collations, decimal(38,x), datetime2(7) 9999-12-31, time(7) with a 100 ns digit, datetimeoffset +14:00 | envelope test vectors; the char trim |
| S2 | T2 | INSERT BULK with `0x62` in COLMETADATA | the bulk encoder for `VARIANT` |
| G1 | T5 | every OGC type, EMPTY of each, holes, collections, Z, M, ZM, CircularString, CompoundCurve, CurvePolygon, `FullGlobe`, as `CAST(g AS varbinary(max))`, `STAsBinary()`, `AsBinaryZM()`, `STAsText()` | kernel output equal to `AsBinaryZM()` byte for byte (to `STAsBinary()` without Z / M); the curve and FullGlobe bytes |
| G2 | T0 | a raw `SELECT g, h` with `MSSQL_DEBUG=3` | UDT_INFO layout; MAX_BYTE_SIZE of geometry, geography, hierarchyid; PLP framing of a small UDT |
| G3 | T5 | `geography::Point(50, 10, 4326)` stored | latitude first on disk and on the wire |
| G4 | T6 | INSERT BULK of client-built MS-SSCLRT into geometry / geography columns declared `varbinary(max)`: valid, a wrong ring orientation, no H bit on a polygon larger than a hemisphere | whether `geometry` skips the stage |
| G5 | — | (dropped: the mixed-SRID rule, settled by DQ-45) | |
| G6 | T5 | bare UDT read vs `AsBinaryZM()`, 1M points: server CPU, wire bytes (`MSSQL_COUNTERS=1`), interleaved A/B | the read's gain, for the PR body |
| G7 | T6 | `STGeomFromWKB` with ISO Z / M codes, and `STGeomFromText` with `POINT(x y z m)` | how a Z / M value is written |
| H1 | T4 | `hierarchyid` from the root to depth 20, negative and fractional labels (`/-1/`, `/1.5/`, `/1.-2/`), every Li range boundary, against `ToString()` | the kernel's test vectors |
| J1 | T0 | comparisons, `GROUP BY` and `ORDER BY` on a 2025 `json` column | whether `json` is comparable on the server |
| D1 | T5 | DuckDB at the pin, no server: `GEOMETRY('OGC:CRS84')` cast to and from `GEOMETRY`, with and without the spatial extension; Parquet and DuckLake writes | the label works downstream |
| C1 | T8 | a user CLR type with `IsFixedLength = true`, `Format.Native`, in a backup | where the record holds it |

## Security and hardening

- Every envelope is checked against its value's length: `sql_variant`'s header per base type (S1's
  lengths), the vector header and `8 + dims × element size`, MS-SSCLRT's point, figure, shape and
  segment counts and offsets (a shape's parent before it, figure and point offsets within their
  arrays), hierarchyid's bit string within 892 bytes. A violation is a named error with the column,
  never a read past the value.
- The WKB output of one value is bounded by its input (16 bytes a point plus headers), so it needs no
  growth beyond what 085 D17 reserves.
- Fuzz harnesses, DuckDB-free, seeded from the experiments' vectors: `fuzz_variant_envelope` (both
  envelopes), `fuzz_vector`, `fuzz_hierarchyid`, `fuzz_ssclrt` (registered in `build.sh`
  KNOWN_TARGETS, the Makefile, `cflite-pr.yml` and the README, as the backup harnesses are).
- Writes: literals go through the existing formatters; nothing a value holds is spliced into SQL text
  unescaped.

## Tests and acceptance

- **Integration (CI, 2022)**: every base type in `sql_variant` read through the catalog and a raw
  scan; written back, its base type checked with `SQL_VARIANT_PROPERTY`; every spatial form of G1
  read and compared with `AsBinaryZM()` on the server; geography's CRS; the curve error and
  `geometry_as`; spatial COPY / INSERT / UPDATE / DELETE / MERGE round trips with SRIDs 0, 4326 and
  3857; `hierarchyid` against `ToString()`; `datetime2(7)` 9999-12-31 under both settings, with the
  warning in `duckdb_logs` and filters `=`, `<`, `>=` on the sentinel.
- **Integration (CI, 2025)**: a new job on the 2025 image pinned by digest in
  `test/backup/gen/images.txt` for the 2025 types, `vector` (float32, float16, 1 / 3 / 1998 dims, NULL, with and without the feature) and
  `json`. It runs only the `test/sql/types_2025/` files.
- **C++ tests**: each kernel against the committed vectors.
- **The backup oracle lane** (T8): each type's fixture on 2017 – 2025 (`vector`, `json` on 2025),
  compared with the restored database through `TYPE mssql`, which now uses the same kernels; the
  `geometry_as` and `datetime2_7_as` forms included.

Acceptance:
1. Every value of every fixture equals the server's own conversion (`SQL_VARIANT_PROPERTY` + the
   cast value, a `char` / `nchar` base value trimmed as the column codec trims it; `AsBinaryZM()`;
   `ToString()`; the float32 bits) on both paths.
2. A raw `mssql_scan` reads a table of every type in this spec.
3. No type in this spec is read through a server-side rewrite.
4. The fuzz harnesses replay clean under ASan and UBSan.
5. `mssql_backup_columns` reports `converted` for every type in this spec (T8 on).

## Tasks

All PRs from main, never stacked. T0 first; T9 at any time (its backup part with T8); then T1 → T2,
T3, T4, T5 → T6 and T7 in any order. T8 after 085 PR 3. T9 is outside the recon's T0 – T8 list:
it carries DQ-44, decided after the split.

| PR | content | days |
|---|---|---|
| **T0** | D2's split of `is_cast_required` (no behaviour change); TYPE_INFO and row framing for `0x62` and `0xF0`, the raw-scan refusal naming the type; experiment G2 (`measured/G2.md`). #557 | 2–3 |
| **T1** | `sql_variant` read as `VARIANT` (D3): both envelopes, the kernel, catalog / raw scan / describe / `column_types` / RETURNING; S1; `fuzz_variant_envelope` | 5–6 |
| **T2** | `sql_variant` write (D3): literals, refusals, the bulk encoder (S2: INSERT BULK takes `0x62` with the row envelope); CTAS | 3–4 |
| **T3** | `vector(n)` as `FLOAT[n]` (D4): the `0xF5` TYPE_INFO and row framing (moved from T0: the server sends it only under VECTORSUPPORT), VECTORSUPPORT, `mssql_vector_support`, the kernel, the JSON fallback (float16 under version 1, every vector without the feature), writes (the bytes as `varbinary` on the bulk path), CTAS / COPY; V1–V6 (measured); the 2025 CI job; `fuzz_vector` | 4.5–5.5 |
| **T4** | `hierarchyid` on the client (D5); H1; `fuzz_hierarchyid`; a user CLR type read as `BLOB` in a raw scan (moved from T0: testing how the describe reports one needs a CLR assembly, built in a .NET SDK container) | 2–3 |
| **T5** | spatial read (D6): the MS-SSCLRT kernel, bare read, `GEOMETRY('OGC:CRS84')`, the curve error, `geometry_as`; G1, G3, G6, D1; `fuzz_ssclrt` | 5–7 |
| **T6** | spatial write (D6): SRID from the CRS, the stage, client MS-SSCLRT for geometry if G4 passes, CTAS creates `geometry`; G4, G7 | 4–5 |
| **T7** | `json` as `JSON` (D7) | 0.5 |
| **T8** | the backup hookup (D9): `backup_type_forms.cpp`, the vector detection fixed, fixtures per type on 2017–2025; C1 | 2–3 |
| **T9** | `datetime2(7)` (D8): the warning, `mssql_datetime2_7_as` and its ATTACH option, the read type through the key / filter / writer checks, the rewritten filters | 3–4 |

**Total ≈ 31–41 days**, docs per PR included (README type table, `docs/type-mapping.md`,
`DATAMODEL.md`'s codec section, CLAUDE.md's settings table, CHANGELOG). Spec 086 drops tasks 8 – 10,
12, 15 and 16 (10.5 days) to this spec.

## Risks

- **The geography CRS downstream**: `OGC:CRS84` is built in and GeoParquet's default; whether
  DuckLake keeps it on a geometry column is D1.
- **Z / M values change** for anyone who reads them today (they were dropped). Documented as breaking.
- **The vector JSON fallback** is inexact for float32 (V2: about 1.5 % of values a neighbour away).
  A server without VECTORSUPPORT reads them so, and the warning says so. The bulk write sends the
  bytes and is exact either way (V6).
- **MS-SSCLRT version 2 details** (FullGlobe's bytes, segment forms) are known only from the spec
  until G1; the kernel refuses what G1 did not show.
- **`datetime2_7_as = 'timestamp'`** makes a datetime2(7) key lossy, as `time(7)` is: such a key
  falls through to another unique index, and a table with none loses UPDATE / DELETE (D8). The
  setting is opt-in for that reason.

## References

- The recon behind this spec (2026-11-13, not committed): its facts are carried into the sections
  above with their sources.
- Spec 086 D2, D6, D8a; spec 085 D13, D17; spec 091 / 092 (code pages); spec 079 (the writer); spec 080
  D3 (`#stage`).
- Issues #354, #363, #515 (Types), #168, #514; PR #542.
- [MS-TDS] 2.2.5.4.3, 2.2.5.5.2 (UDT), 2.2.5.5.4 (SSVARIANT), 2.2.5.5.6 (JSON), 2.2.5.5.7 (vector),
  2.2.5.6, 2.2.6.4, 2.2.7.11; [MS-SSCLRT] 2.1.1 – 2.1.7, 2.2.2, 3.1; [MS-BCP] 2.1.5.
- Microsoft Learn: the vector data type, `STAsBinary`, `AsBinaryZM`, the json data type.
- MIT drivers for cross-checks only: dotnet/SqlClient `SqlVector.cs`, microsoft/mssql-jdbc PRs #2868,
  #2853.

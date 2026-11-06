# Spec 086: compressed row formats and the remaining types

Status: **draft**, 2026-11-05. Not started. Depends on spec 085, and for the double-byte code pages
on spec 091's module `src/codec/code_page/` (D9). Series overview: `../README.md`.
Markers as in 085: **(2025)**, **(unverified)**. Sources cited as in 085 (R§, X§, W§, A§, E<n>§);
the owner's decisions as DQ-nn ([decisions.md](../085-backup-reader-core/decisions.md)).

## Problem

Spec 085 reads uncompressed rowstore and the common types, and refuses the rest by name. Real
databases use ROW and PAGE compression widely, and carry `sql_variant`, `xml`, spatial types,
`hierarchyid`, sparse columns, and on 2025 `vector` and the native `json` type. This spec makes them
readable, each one gated by an oracle test against the server's own conversion. The 2025 `json`
binary is undocumented; it is a research task with a gate, not a promise (R§2, X§4).

## Scope

In: ROW compression (the CD record format), PAGE compression (anchor and dictionary), sparse columns
and the column set, `sql_variant` → `VARIANT`, `xml` (MS-BINXML → text), `geometry` / `geography`
(MS-SSCLRT → WKB), `hierarchyid`, `vector`, the double-byte code pages (932, 936, 949, 950), and the
`json` research spike.

Out: user CLR types (opaque bytes, W§7: refused by name, as now), columnstore (088), the redo of CD
and page-dictionary records (090's follow-up).

## What is known

| item | state | source |
|---|---|---|
| CD record: header, column count, 4-bit CD array, short and long data regions | decoded for integers and strings, verified at every E1 step (2,000–2,199 rows, sign flips, NULL / empty / long, ghosts) **(2025)** | E1§4.9, `experiments/E1/pages_ext.py` `parse_cd` |
| CD integers: big-endian, minimal length, top bit flipped (`0x81` = 1, `0x68` = −24) | verified **(2025)** | E1§4.9, X§4 |
| CD codes 11 (bit = 1), 12 (page-dictionary symbol), clusters of 30 columns, decimals as vardecimal, Unicode compression (SCSU), key columns first | documented by DbReader (MIT, sourced in its PROVENANCE.md); not decoded by us | W§6 |
| PAGE: `typeFlagBits` 0x80, a compression-info record at offset 96 (flags 0x06, anchor record, a dictionary of 4 entries seen), CD records with prefix references | recognised, not decoded **(2025)** | X§4 |
| sparse vector: complex column `0x0005`: u16 kind, u16 n, n × u16 column id, n × u16 end offset, values | decoded **(2025)** | X§4, `tools/pages.py` `sparse_vector` |
| `sql_variant`: `[base type][version 1][properties][value]` | decoded for nvarchar, int, datetime, decimal **(2025)** | X§4, `pages.py` `sql_variant`, E1§6 (defaults: int and varchar only) |
| `vector(n)`: varbinary, utype 255: `a9 01`, u16 dims, u32 element type (0 = float32), dims × float32 | decoded (`[1.5, -2.25, 3.0]`) **(2025)** | X§4 |
| `xml`: MS-BINXML (signature `df ff 01 b0 04`), names in UTF-16 | names and text extracted, not rendered | X§4 |
| `geometry` / `geography`: MS-SSCLRT (SRID i32, version, properties byte, doubles) | SRID 0 / 4326 and points visible | X§4 |
| `json` (xtype 244): a binary tree; keys and strings UTF-8 with length prefixes `(len << 1) \| 1`; integers u32 with bit 31 set | **undocumented**; only E5's two-key objects were read | X§4, E5§6.2 |
| PAGE compressed pages on 2025 and later builds (ROW: E1's subset above) | not tested | X§10 |

## Decisions

- **D1 — clean room, as E5.** The CD and page-dictionary decoders are written from DbReader's
  documented facts (MIT; facts adapted with attribution, R§7) and our own measurements, recorded in a
  `PROVENANCE.md` next to the code as E5 §8 does. No GPL or AGPL decoder source (OrcaMDF, Internals
  Viewer, mdf-rs) is opened. MS-BINXML and MS-SSCLRT are Microsoft open specifications.

- **D2 — output types.** As 085's map (`MapSQLServerTypeToDuckDB`), except where the backup can return
  what the wire cannot:

  | SQL Server | `TYPE mssql` today | backup reader | reason |
  |---|---|---|---|
  | `sql_variant` | `VARCHAR` (server-side NVARCHAR cast, A§1) | **`VARIANT`** holding the base type's DuckDB value | the base type and its precision survive; DuckLake stores `VARIANT` (A§1, `ducklake_types.cpp:171`) |
  | `vector(n)` | `VARCHAR` (JSON text) | **`FLOAT[n]`** | the exact type, and DuckDB's array functions take it. DuckLake has no `ARRAY` (`ducklake_types.cpp`), so the importer casts to `FLOAT[]`, a core cast with no dependency |
  | `json` | `VARCHAR` | `JSON` (once the spike passes) | DuckLake accepts the `json` alias |
  | `xml` | `VARCHAR` | `VARCHAR` | the server's text form |
  | `geometry`, `geography` | `GEOMETRY` (from `.STAsBinary()`) | `GEOMETRY`, the same WKB | parity |
  | `hierarchyid` | `VARCHAR` (`ToString()` through the NVARCHAR cast) | `VARCHAR`, the same text | parity |

  The two divergences from `TYPE mssql` (`VARIANT`, `FLOAT[n]`) are accepted (DQ-19); the live
  catalog could follow later, outside this series.

- **D3 — every decoder ships behind an oracle test.** The expected value is the server's own
  conversion of the restored column, read through `mssql_scan`: `CAST(x AS nvarchar(max))` for xml,
  hierarchyid and json; `x.STAsBinary()` for spatial; `SQL_VARIANT_PROPERTY(v, 'BaseType' /
  'Precision' / 'Scale' / 'Collation')` plus the cast value for `sql_variant`; the plain column for the
  rest. A value the oracle does not confirm keeps the 085 refusal (by name, when projected).

- **D4 — CD values are batch-decoded like FixedVar ones.** The CD walk writes each value's bytes and
  its CD code into the codec staging; per-column kernels convert the CD encodings (big-endian
  sign-flipped integers, trimmed floats, vardecimal, SCSU, …) in one call per column (085 D7). The
  column's CD index is `−offset − 1` from its `sysrscols` row (every physical column of a compressed
  rowset has a negative offset, E1 `_decode_cd`) **(2025)**.

- **D5 — compression is per rowset.** `sysrowsets.cmprlevel` (0 none, 1 ROW, 2 PAGE, 3 COLUMNSTORE,
  4 COLUMNSTORE_ARCHIVE; E5§3.1) decides the decoder of each rowset. Partitions of one table may
  differ (085 `TableLayout`).

- **D6 — spatial is 2D WKB, as `STAsBinary()`.** The live catalog reads spatial columns through
  `.STAsBinary()` (`mssql_column_info.cpp:532-534`), which emits 2D OGC WKB; the reader emits the
  same bytes, so the two agree. Z and M values are dropped as `STAsBinary()` drops them, and that is
  documented. Curves (MS-SSCLRT version 2 segments: CircularString, CompoundCurve, CurvePolygon) are
  refused by name. The column type is `GEOMETRY` with no CRS, as the live catalog: SQL Server keeps the
  SRID per value, DuckDB's CRS is per column. A per-column `crs := MAP {'dbo.t.geo': 'EPSG:4326'}`
  option is offered, with values of another SRID refused by name. Z/M and CRS handling are accepted
  as stated (DQ-20).

- **D7 — the column set.** Sparse columns are decoded individually. A `COLUMN_SET` column (an `xml`
  computed over the sparse values, not stored) is generated in the server's text form only if the
  oracle confirms it byte for byte; otherwise it is refused when projected.

- **D8 — `json` is a research task with a gate.** A spike (E9, below) decodes controlled values and
  compares them with `CAST(j AS nvarchar(max))`. The decoder ships only when every value of the
  corpus (every JSON type, nesting to the server's limit, Unicode, escapes, numbers at every edge,
  large documents that go off-row) equals the server's text. Until then the column is refused by name.
  No raw-bytes escape hatch: nobody downstream could read an undocumented binary.

- **D9 — the double-byte code pages** (932, 936, 949, 950: 206 + 172 + 86 + 274 collations of 5,540)
  go through spec 091's vectorized kernel, one call per column per chunk: the ASCII fast path over
  the chunk buffer, then a table pass indexed by lead byte. The tables come from SQL Server itself:
  spec 091 (SQL Server code pages, generated tables and vectorized kernels) CASTs every lead / trail
  pair to `nvarchar` under a collation of each code page, commits the output and samples it in a CI
  oracle check, so an unmapped or invalid sequence comes out as the server's CAST makes it. Spec 091
  builds the tables and kernels; 086 reuses them and adds the backup-specific oracle fixtures: the
  restored column's `CAST(c AS nvarchar(max))` against the reader's text, per code page (task 11).

## Design

### The CD record (ROW compression)

Facts marked E1 are verified **(2025)**; the others come from DbReader and are verified by the
oracle before they ship.

- Header byte: bit 0 = CD format, bits 1–3 = record type (as FixedVar: 0 primary, 1 forwarded,
  2 stub, 5 / 6 / 7 ghosts), 0x20 = a long data region follows (E1).
- Column count: 1 byte; with its high bit set, 2 bytes (E1 refused `≥ 128` columns; the 2-byte form
  is DbReader's).
- CD array: 4 bits per column, low nibble first (E1). Codes: 0 NULL, 1 EMPTY (integers: 0), 2–9 = 1–8
  bytes in the short data region, 10 long (E1); 11 = bit 1, 12 = page-dictionary symbol (W§6).
- More than 30 columns: the CD array is cut in clusters of 30 with a cluster array (W§6). E1 did not
  meet one; fixtures with 31, 60 and 300 columns are in the matrix.
- Short data region: the 1–8-byte values back to back, in CD order (E1).
- Long data region: u8 flags, u16 count, count × u16 end offsets relative to its data start; bit 15
  of an end offset marks a complex column (row-overflow or LOB pointer, decoded by 085's `LobReader`);
  E1 refused the complex case, this spec decodes it.
- Value encodings by family (DbReader, to verify): integers big-endian sign-flipped and trimmed (E1);
  `bit` through codes 1 / 11; `decimal` / `numeric` in the vardecimal form; `money` / `smallmoney` as
  integers; `float` / `real` with trailing zero bytes trimmed; `date`, `time`, `datetime2`,
  `datetimeoffset`, `datetime`, `smalldatetime` in their trimmed forms; `uniqueidentifier` not
  compressed; `char` / `binary` trimmed of trailing spaces / zeros and padded back to the column
  length; `nchar` / `nvarchar` in SQL Server's Unicode compression (an SCSU implementation).
- Record forms to cover: forwarded and stub CD records in a ROW-compressed heap; a uniquifier in a CD
  record (E1 refused it); ghosts (E1§4.10: 21 CD ghosts in D7, skipped); columns added after the
  record (the record's column count is smaller: the `sysseobjvalues` default or NULL, as 085).

### PAGE compression

- A page with `typeFlagBits` 0x80 carries a compression-information record at offset 96 (X§4):
  flags (0x06 seen: anchor present, dictionary present), an **anchor record** (a CD record holding a
  prefix value per column), and a **dictionary** of entries. The records that follow are CD records
  whose values reference the anchor (a prefix length plus the suffix) and the dictionary (code 12, an
  index into it).
- The order of application (dictionary entry, then prefix, or the other way round) and the entry
  format come from DbReader and are fixed by fixtures before code is written: values with a common
  prefix, repeated values, NULLs, and all three on one page.
- A page without the 0x80 flag in a PAGE-compressed rowset is a ROW-compressed page (DbReader); a
  fixture produces one (a page under the compression threshold).

### Types

- **`sql_variant`** (X§4): byte 0 = base type (xtype), byte 1 = version (1), then the properties and
  the value. Properties: strings (167, 175, 231, 239) u16 maximum length + u32 collation id (header 8
  bytes); `decimal` / `numeric` u8 precision + u8 scale (4); `binary` / `varbinary` u16 maximum length
  (4); `time` / `datetime2` / `datetimeoffset` u8 scale (3); every other base type none (2). The value
  is the base type's on-disk form, decoded by the 085 kernel of that type and stored into the
  `VARIANT`. Fixtures: every base type a `sql_variant` accepts.
- **`xml`**: MS-BINXML tokens (element and attribute starts, text, CDATA, comments, processing
  instructions, namespace declarations, typed values of typed xml) rendered to the text the server
  returns for `CAST(x AS nvarchar(max))`: its attribute quoting, entity escaping, empty-element form
  and namespace placement. Typed xml (a schema collection) stores typed atomic values; those render
  through the type's canonical XSD text form. Large documents are LOBs (085).
- **`geometry` / `geography`** (MS-SSCLRT): SRID i32, version u8, a properties byte (Z, M, valid,
  single point, single line segment, larger than a hemisphere), the point count and points (x, y as
  doubles), the Z and M arrays, the figures (attribute, first point), the shapes (parent, first
  figure, OGC type), and in version 2 the segments. The single-point and single-segment forms carry
  no figures or shapes. Output: 2D WKB equal to `STAsBinary()` (D6), including empty shapes and
  geometry collections. Geography's coordinate order on disk (latitude first or longitude first) is
  **to verify** against `STAsBinary()`, which writes longitude, latitude.
- **`hierarchyid`** (MS-SSCLRT): the compressed bit string of ordinal labels, rendered as
  `ToString()` (`/1/2.5/`, `/` for the root).
- **`vector(n)`** (X§4): `0xA9`, version `0x01`, u16 dims, u32 element type (0 = float32), dims ×
  float32, into `FLOAT[n]`. Another element type (float16 is announced for 2025 previews) is refused
  by name.
- **Double-byte code pages**: spec 091's lead-byte-indexed tables for 932, 936, 949, 950, through
  091's kernel (D9).

### Sparse columns

The sparse vector is the trailing complex variable column whose first u16 is 5 (X§4): u16 kind, u16
n, n × u16 column id (`syscolpars.colid`), n × u16 end offset, then the values in their FixedVar
on-disk form. A sparse column absent from the vector is NULL. Sparse columns are never data-compressed
(W§6), so a sparse table is FixedVar.

### `json`: the research spike (E9)

1. On a 2025 container: a table with a `json` column filled with a controlled corpus (every JSON
   value type; objects and arrays nested to the server's limit; empty object, array, string; Unicode
   in keys and values, surrogate pairs, escapes; integers and decimals at every width and sign edge,
   exponents; documents that go off-row).
2. Capture the stored bytes (the backup, DBCC PAGE) and the server's text for each value.
3. Write the decoder clean-room from the bytes (E5's method and PROVENANCE discipline).
4. Gate: every value equal to the server's text. Report what was learnt in
   `experiments/E9-json.md`, as E1–E6 do.

Timebox: 2 weeks. If it fails, the refusal stays and the spec records why.

## Security and hardening

- The CD walk is bounded like 085's FixedVar walk: column count against the layout, CD array and
  cluster array within the record, short values within the short region, long end offsets monotonic
  and within the record.
- PAGE: the compression-info record within the page; anchor and dictionary counts against the page;
  a dictionary reference past the dictionary, or a prefix longer than the anchor value, is a named
  error.
- MS-BINXML: a token stream with a depth limit and a size limit; names and namespace tables bounded;
  no entity expansion (MS-BINXML has no DTD expansion, but the renderer still caps output at
  2^31 − 1 bytes).
- MS-SSCLRT: point, figure, shape and segment counts against the value length; offsets within their
  arrays; a shape's parent before it.
- `sql_variant`: header length per base type; the value length within the column.
- SCSU and the code-page decoders (091's): invalid sequences take the oracle's behaviour (D9), never
  a read past the value.
- Resources (085 D17): the CD walk, the anchor and the dictionary are bounded by their page; an `xml`
  or spatial value renders into a buffer reserved as it grows, capped at 2^31 − 1 bytes like a LOB
  value, waiting or refused by name by 085 D17's rule (085 § Resources).
- Fuzz harnesses, seeded from the fixtures: `fuzz_backup_cd` (CD record and value kernels),
  `fuzz_backup_page_ci` (compression-info record and reconstruction), `fuzz_backup_binxml`,
  `fuzz_backup_ssclrt`, `fuzz_backup_variant`, `fuzz_backup_hierarchyid`, `fuzz_backup_json` (when it
  exists). DuckDB-free (085 D15) except the kernels' final vector writes.

## Testing and acceptance

Oracle lane (085): each fixture is backed up (uncompressed and ZSTD), restored, and compared through
`TYPE mssql` for the parity types and through the D3 server-side conversions for the others.

Fixtures (`test/backup/gen/086_*.sql`):
- ROW and PAGE versions of one wide table with every type 085 decodes, plus NULL / empty / maximum /
  minimum / negative values, trailing spaces and zeros, Unicode strings with and without non-BMP
  characters; 31, 60, 129 and 300 columns; LOB and row-overflow columns in compressed rows; a
  ROW-compressed heap with forwarded rows; a non-unique clustered index (uniquifier); ghosts left by
  deletes in the backup's batch; metadata-only ADD COLUMN after the data; per-partition compression
  (the partition metadata is 085's, from E7; E7 decoded a ROW-compressed partition beside plain ones,
  PAGE per partition was not covered, E7§7).
- `sql_variant` of every base type; typed and untyped xml (elements, attributes, namespaces,
  comments, CDATA, processing instructions, entities, large documents); spatial of every OGC type,
  empty shapes, holes, collections, SRIDs 0 / 4326 / 3857, Z / M values (dropped), curves (refused);
  `hierarchyid` from the root to depth 20 with negative and fractional labels; `vector` of 1, 3 and
  1,998 dims; sparse tables with and without a column set; varchar columns under each double-byte
  code page.

Version matrix: 2017 / 2019 / 2022 / 2025 for everything but `vector` and `json` (2025). The CD
format dates from 2008, so a 2017 failure points at a layout difference, not a new format.

Acceptance:
1. Every fixture column of every version in the matrix equals the oracle, or is refused by name with
   the reason the oracle found.
2. ROW and PAGE versions of a table equal its uncompressed version, `_mssql_rid` included (085 D8's
   uncompressed key bytes).
3. A `sql_variant` column round-trips base type, precision, scale and collation.
4. Spatial WKB equals `STAsBinary()` byte for byte.
5. The fuzz harnesses replay their corpora clean under ASan and UBSan.
6. `json`: D8's gate passes, or the spike report records why not and the refusal stays.

## Tasks

Order is the PR order.

**PR 1 — ROW**
1. CD record parser: header, column counts, CD array, clusters, short and long regions, complex long
   columns — 3 d
2. CD value kernels per family, SCSU included — 5 d
3. CD forms: forwarded, stub, uniquifier, ghosts, added and dropped columns — 1.5 d
4. ROW fixtures, version matrix; `fuzz_backup_cd` — 2 d

**PR 2 — PAGE**

5. Compression-info record, anchor, dictionary, reconstruction; fixtures first — 4 d
6. `fuzz_backup_page_ci`; PAGE fixtures — 1 d

**PR 3 — sparse, `sql_variant`, `vector`, `hierarchyid`, double-byte code pages**

7. Sparse vector and the column set (D7) — 1.5 d
8. `sql_variant` → `VARIANT` — 2 d
9. `vector` → `FLOAT[n]` — 0.5 d
10. `hierarchyid` → text — 2 d
11. Double-byte code pages: spec 091's tables and kernel reused (D9), plus the backup-specific oracle
    fixtures — 0.5 d
12. Fuzz harnesses variant / hierarchyid; fixtures — 1 d

**PR 4 — `xml`**

13. MS-BINXML reader and text renderer, typed values — 5 d
14. `fuzz_backup_binxml`; fixtures — 1 d

**PR 5 — spatial**

15. MS-SSCLRT reader and WKB writer, curves refused, `crs` option if confirmed — 4 d
16. `fuzz_backup_ssclrt`; fixtures — 1 d

**PR 6 — `json` (only if the spike passes)**

17. E9 spike — 5–10 d (timeboxed, not in the total)
18. Decoder, `fuzz_backup_json`, fixtures — 3 d (only after the gate)

**Across PRs**

19. Docs per PR (README type table, `DATAMODEL.md` row-engine section, CLAUDE.md, CHANGELOG) — 1 d

**Total ≈ 36 days, 7–8 weeks** (37.5 before spec 091 took the double-byte tables out of task 11),
plus the `json` spike (1–2 weeks) and its decoder (~3 days) if the gate passes. R§8 phase 1 gave
5–6 weeks for ROW / PAGE, sparse, `sql_variant`, xml, spatial and vector together with the overlay,
locators and parallel scan (which moved to 085). The difference is `hierarchyid`, the double-byte
code pages and the per-type oracle tests and fuzz harnesses.

## Risks

- **ROW / PAGE are known to us only through DbReader's facts and E1's integer-and-string subset.**
  The page dictionary and SCSU in particular have never been decoded here. PAGE compression on
  2025 was not tested (X§10).
- **xml text exactness**: the server's serialisation choices (whitespace, entity escaping,
  namespace placement, typed-value formatting) must be matched exactly or the oracle rejects them.
- **Geography coordinate order** on disk versus `STAsBinary()`'s longitude-first output.
- **Z / M and CRS** are lost or flattened in parity with the live catalog (D6); a lake user who needs
  them has no path yet.
- **`json` may not be decodable** in the timebox; then 2025 tables with `json` columns import without
  that column only if the user projects it away.
- Decided ([decisions.md](../085-backup-reader-core/decisions.md)): the `VARIANT` / `FLOAT[n]`
  divergences (D2; DQ-19, accepted); 2D WKB without Z / M, with the optional per-column CRS (D6;
  DQ-20, accepted).

## References

- RESEARCH.md §1 (goal 4), §3 ("Rows", "Types vs the TDS wire"), §7 (prior art and licences).
- research-experiment.md §4 ("Other types" table), §10.
- research-web.md §6 (ROW, PAGE, sparse, LOB), §7 (types).
- research-architecture.md §1 (on disk ≠ wire; `sql_variant` → VARIANT; geometry has no parser).
- E1 §4.9 (ROW decoder), §4.10 (ghosts), §6 (`sql_variant` defaults); `experiments/E1/pages_ext.py`.
- E5 §3.3 (canonical values per type, incl. vector and json bytes), §6.2 (json observations), §8
  (the provenance discipline to copy).
- [MS-BINXML], [MS-SSCLRT] (Microsoft open specifications).

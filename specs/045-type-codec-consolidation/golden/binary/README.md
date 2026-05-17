# Binary family — golden fixtures (spec 045 / US3 sub-phase 5)

Captured from the implementation at branch HEAD `1c7a758` (post-Money
migration) before the Binary codec migration landed. The Binary dispatch
sites at that point were the legacy per-type helpers:
`TypeConverter::ConvertBinary`, `BCPRowEncoder::EncodeBinary` /
`EncodeBinaryPLP` / EncodeRow BLOB arm / EncodeValue BLOB arm,
`MSSQLValueSerializer::SerializeBlob`, the filter encoder's inline
"0x<hex>" snprintf loop, and the DDL translator's inline
`"VARBINARY(MAX)"` constants (in both `MapTypeToSQLServer` and
`MapLogicalTypeToCTAS`).

## Files

| File | What | Format |
|---|---|---|
| `decode_cases.txt` | TDS BIGBINARY/BIGVARBINARY → DuckDB BLOB | mode \| wire-LE-hex \| expected-hex |
| `encode_cases.txt` | DuckDB BLOB → BCP wire | mode \| input-hex \| expected wire bytes |
| `literal_cases.txt` | DuckDB BLOB → SQL literal | input-hex \| ctx \| expected text |
| `ddl_cases.txt` | BLOB → SQL Server type name | ctx \| expected text |
| `edge_cases.txt` | Empty BLOB, large BLOB, GEOMETRY routing | documented separately |

## Conventions

- **Modes**: `non-plp` for fixed-length BIGBINARY (column `max_length > 0`
  and ≤ 8000) and `plp` for VARBINARY(MAX) (column `max_length == -1`,
  detected via `BCPColumnMetadata::IsPLPType()`).
- Per FR-022 the Binary family produces byte-identical literal output for
  both `LiteralContext::Filter` and `LiteralContext::InsertValues` — the
  canonical form is `0x<UPPERHEX>` (uppercase hex, `0x` prefix, no
  separators, no quotes). Pre-spec-045 both dispatch sites already
  produced this exact text; this is a one-line consolidation.
- Per FR-027/FR-028 the Binary family produces byte-identical DDL output
  for both `DdlContext::CreateTable` and `DdlContext::CtasCreateTable` —
  always `VARBINARY(MAX)`. CTAS does not size-restrict because DuckDB
  BLOBs are inherently variable-length.
- **EstimateLiteralSize** is upper-bound: `2 + max_length * 2` (the
  `0x` prefix plus two hex chars per byte). For PLP types where
  `max_length == -1` we treat it as unknown and use a fixed estimate
  (`2 + 8192 * 2 = 16386`) matching pre-spec-045 behavior.

## GEOMETRY routing (spec 045 + this sub-phase extension)

DuckDB main branch ships first-class `LogicalType::GEOMETRY()` storing
WKB blobs. On the SQL Server side, `geometry` and `geography` are CLR
UDTs (TDS type `0xF0`) that serialize on the wire using Microsoft's
proprietary "Spatial Type Binary Format" — **not** standard OGC WKB.

To bridge this:

1. `MSSQLColumnInfo::MapSQLServerTypeToDuckDB` maps the `geometry` and
   `geography` `INFORMATION_SCHEMA` data_type names to
   `LogicalType::GEOMETRY()`.
2. `MSSQLColumnInfo` carries a new `is_geometry` flag.
3. `BuildColumnExpression` in `table_scan.cpp` projects
   `[col].STAsBinary() AS [col]` for geometry/geography columns. SQL
   Server's `STAsBinary()` returns `varbinary(max)` containing standard
   OGC WKB.
4. On the wire the column then arrives as TDS_TYPE_BIGVARBINARY with
   PLP-framed bytes. `codec::binary::DecodeFromTds` writes those bytes
   via `StringVector::AddStringOrBlob` into the destination vector,
   whose `LogicalType` is `GEOMETRY` — same `string_t` physical storage
   as `BLOB`, so the write is correct without any GEOMETRY-specific
   decoder branch.
5. `type_family.cpp` routes `LogicalTypeId::GEOMETRY` to
   `TypeFamily::Binary`, so the literal/DDL/encode paths reuse the
   Binary codec (renders as `0x<hex>` for filter literal and
   `VARBINARY(MAX)` for DDL preview).

This means **no Geometry codec file**; geometry is a configuration
on top of the Binary codec, not a separate family. The fixtures below
cover the routing edge cases.

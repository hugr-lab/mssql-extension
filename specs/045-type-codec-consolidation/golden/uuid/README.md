# Uuid family — golden fixtures (spec 045 / US3 sub-phase 7)

Captured during the Uuid codec migration — the **last family** to migrate
before Phase 7 (DDL final consolidation). The pre-migration dispatch
sites were:

- `TypeConverter::ConvertGuid` — internal helper called from
  `ConvertValue` on `TDS_TYPE_UNIQUEIDENTIFIER`. Delegated to
  `GuidEncoding::ConvertGuid` (the low-level middle-endian byte-order
  helper) and stored the resulting `hugeint_t` into the output vector.
- `BCPRowEncoder::EncodeGUID` — BCP wire-format encoder. Writes the
  1-byte length prefix (always 16) followed by the 16 bytes in TDS
  mixed-endian order (Data1 LE, Data2 LE, Data3 LE, Data4 BE).
- `MSSQLValueSerializer::SerializeUUID` — INSERT-path SQL literal form
  (the InsertValues context). Emits `'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx'`
  via `UUID::ToString`.
- `FilterEncoder::ValueToSQLLiteral` UUID arm — used `"'" + value.ToString() + "'"`
  (the Filter context). Already byte-identical to the InsertValues form
  in practice but bypassed the codec layer.
- `MSSQLDDLTranslator::MapTypeToSQLServer` / `MapLogicalTypeToCTAS` —
  both returned the literal string `"UNIQUEIDENTIFIER"` (already byte-
  identical between CreateTable and CtasCreateTable contexts, no
  parameters or length suffix).

## SQL Server coverage

The single non-UDT GUID type:

| SQL Server type | TDS type id | Wire size (bytes) | DuckDB type | Round-trip |
|---|---|---|---|---|
| `uniqueidentifier` | 0x24 | 16 | `UUID` (hugeint_t with high bit flipped) | full round-trip |

There is no length parameter, scale, or precision. The wire format is
fixed at 16 bytes in TDS mixed-endian order:

- bytes 0-3: `Data1` (little-endian uint32)
- bytes 4-5: `Data2` (little-endian uint16)
- bytes 6-7: `Data3` (little-endian uint16)
- bytes 8-15: `Data4` (big-endian, as-is)

The standard UUID textual form `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`
is big-endian for all four groups, so the codec reorders Data1-3 on
both decode and encode paths. The byte reordering helper
`GuidEncoding::ReorderGuidBytes` is reused — Uuid is the only family
where a pre-existing low-level helper survives the migration intact.

## DuckDB storage detail

DuckDB stores UUIDs as `hugeint_t` with the **high bit of the upper
64-bit half XOR-flipped** for sortability (see
`duckdb/src/common/types/uuid.cpp`). Both decode and encode paths apply
the same XOR mask `(uint64_t(1) << 63)` so the on-wire bytes round-trip
correctly to / from `UUID::ToString`.

## Issue #89 fallback

`RenderAsString` mirrors the legacy fallback path that already lived in
`WriteAsStringFallback`: `hugeint_t guid = GuidEncoding::ConvertGuid(value.data());`
followed by `UUID::ToString(guid)`. The fixture row in `edge_cases.txt`
covers the case where the catalog says `VARCHAR` but TDS returns
`UNIQUEIDENTIFIER` (e.g. a view that does `CAST(uuid_col AS varchar(36))`
on a column that was actually re-typed in the view definition).

## FR-022 / FR-027 / FR-028 invariants

- **FR-022 (literal byte-identity, Filter == InsertValues)**: trivially
  satisfied — both contexts emit the same `'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx'`
  form. Verified by `TestFormatSqlLiteralByteIdentity` in
  `test/cpp/codec/test_uuid_codec.cpp`.
- **FR-027 (DDL CreateTable byte-identity to legacy)**: emits
  `"UNIQUEIDENTIFIER"` — same string as pre-spec-045.
- **FR-028 (DDL CtasCreateTable byte-identity to CreateTable)**:
  trivially satisfied — same literal string.

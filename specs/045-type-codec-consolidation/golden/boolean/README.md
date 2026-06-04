# Boolean family — golden fixtures (spec 045 / US3 sub-phase 1 / T056)

Captured from the implementation at branch HEAD `c639c74` (post-Phase-5)
before the Boolean codec migration landed. The Boolean dispatch sites at
that point were still the legacy per-type helpers (`TypeConverter::ConvertBoolean`,
`BCPRowEncoder::EncodeBit`, `MSSQLValueSerializer::SerializeBoolean`,
filter-encoder's inline ternary, DDL translator's inline `"BIT"`).

These fixtures are the byte-identity reference that the post-migration
`codec::boolean::*` must match exactly.

## Files

| File | What | Format |
|---|---|---|
| `decode_cases.txt` | TDS BIT/BITN → DuckDB bool | hex wire bytes \| expected bool |
| `encode_cases.txt` | DuckDB bool → BCP wire bytes | bool \| expected hex bytes |
| `literal_cases.txt` | DuckDB bool → SQL literal | bool \| ctx \| expected text |
| `ddl_cases.txt` | BOOLEAN → SQL Server type name | type \| cfg \| ctx \| expected text |

## Conventions

- `ctx` is `Filter` or `InsertValues` (for literal cases) or `CreateTable` /
  `CtasCreateTable` (for DDL cases). Per FR-020(b) and FR-027/FR-028 the
  Boolean family produces identical output for both context values.
- `cfg.text_type` is irrelevant for Boolean DDL (Boolean has no text-type
  alternative) but the case row records it anyway for completeness.
- Hex bytes are big-endian as displayed (the actual wire is LE for
  multi-byte payloads, but Boolean is a single byte so endianness does
  not matter).

## Capture method

Read directly from `src/tds/encoding/type_converter.cpp:ConvertBoolean`
(decode), `src/tds/encoding/bcp_row_encoder.cpp:EncodeBit` (encode),
`src/dml/insert/mssql_value_serializer.cpp:SerializeBoolean` and
`src/table_scan/filter_encoder.cpp:ValueToSQLLiteral` (literal), and
`src/catalog/mssql_ddl_translator.cpp:MapTypeToSQLServer` /
`MapLogicalTypeToCTAS` (DDL) at SHA `c639c74`. Verified by inline
assertions in `test/cpp/codec/test_boolean_codec.cpp`.

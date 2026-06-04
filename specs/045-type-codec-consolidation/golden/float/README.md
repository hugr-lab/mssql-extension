# Float family — golden fixtures (spec 045 / US3 sub-phase 2 / T061)

Captured from the implementation at branch HEAD `df7c56c` (post-Boolean
migration) before the Float codec migration landed. The Float dispatch
sites at that point were still the legacy per-type helpers
(`TypeConverter::ConvertFloat`, `BCPRowEncoder::EncodeFloat` / `EncodeDouble`,
`MSSQLValueSerializer::SerializeFloat` / `SerializeDouble`, the
filter-encoder's `value.ToString()` inline call, and the DDL translator's
inline `"REAL"` / `"FLOAT"`).

## Files

| File | What | Format |
|---|---|---|
| `decode_cases.txt` | TDS REAL/FLOAT/FLOATN → DuckDB float/double | hex wire bytes (LE) \| expected value (hex bit-pattern) |
| `encode_cases.txt` | DuckDB float/double → BCP wire bytes | type \| value \| expected hex bytes (LE) |
| `literal_cases.txt` | DuckDB float/double → SQL literal | type \| value \| ctx \| expected text |
| `ddl_cases.txt` | FLOAT/DOUBLE → SQL Server type name | type \| cfg \| ctx \| expected text |
| `edge_cases.txt` | NaN / +Inf / -Inf / subnormal / denormal | documented separately |

## Conventions

- `ctx` is `Filter` or `InsertValues` (literal) or `CreateTable` /
  `CtasCreateTable` (DDL). Per FR-020 (b) and FR-027/FR-028 the Float
  family produces identical output for both context values.
- For literal cases the **post-migration behaviour** is documented here.
  The legacy `value.ToString()` path the Filter context took
  (pre-spec-045) produced slightly different rendering for some inputs
  (no `.0` suffix on integral values, locale-dependent precision); both
  forms parse identically server-side for the typical use cases but the
  byte-identity invariant of FR-020 (b) requires unification. The
  unified form follows the legacy InsertValues semantics (setprecision
  with the type's significant-digit count, `.0` suffix on integral
  values, client-side NaN/Inf rejection).
- The NaN/Inf rejection is a client-side hardening — pre-spec-045 the
  Filter context happily rendered `nan` / `inf` strings into the
  generated `WHERE` clause; SQL Server then rejected the query with an
  opaque parse error. Post-spec-045 the client throws
  `InvalidInputException` with a clear message before the bytes leave
  the codec. Same defensive pattern as spec 045 Phase 5's NVARCHAR
  length validation (FR-023, issue #91).
- Hex bit-pattern format for floats / doubles: most-significant byte
  first (network-order display), but the TDS wire is little-endian
  (least-significant byte first).

## Capture method

Read directly from `src/tds/encoding/type_converter.cpp:ConvertFloat`
(decode), `src/tds/encoding/bcp_row_encoder.cpp:EncodeFloat` /
`EncodeDouble` (encode), `src/dml/insert/mssql_value_serializer.cpp:SerializeFloat`
/ `SerializeDouble` (literal — post-migration form), and
`src/catalog/mssql_ddl_translator.cpp:MapTypeToSQLServer` /
`MapLogicalTypeToCTAS` (DDL) at SHA `df7c56c`. Verified by inline
assertions in `test/cpp/codec/test_float_codec.cpp`.

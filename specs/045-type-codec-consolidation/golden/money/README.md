# Money family — golden fixtures (spec 045 / US3 sub-phase 4)

Captured from the implementation at branch HEAD `29c1d21` (post-Decimal
migration) before the Money codec migration landed. The only legacy
dispatch site for Money is `TypeConverter::ConvertMoney` in
`src/tds/encoding/type_converter.cpp`. All other operations on values of
SQL Server MONEY/SMALLMONEY origin route through the Decimal family in
DuckDB-land (because `TypeConverter::GetDuckDBType` maps
`MONEY → DECIMAL(19,4)` and `SMALLMONEY → DECIMAL(10,4)`).

## Scope

Money is **scan-decode-only** per `data-model.md`:

| Operation | Status |
|---|---|
| `DecodeFromTds` | **Implemented** by `codec::money::DecodeFromTds` |
| `EncodeToBcp` | Declaration-only; **linker-fenced** (deliberate) |
| `FormatSqlLiteral` | Declaration-only; **linker-fenced** (deliberate) |
| `FormatDdlTypeName` | Declaration-only; **linker-fenced** (deliberate) |

This fence catches accidental routing of "money values" through a
distinct path: encode/literal/DDL of any value that originated as
MONEY must go through the **Decimal codec** because the value's
DuckDB-side `LogicalType` is `DECIMAL(19,4)` / `DECIMAL(10,4)`.

## Files

| File | What | Format |
|---|---|---|
| `decode_cases.txt` | TDS MONEY/SMALLMONEY/MONEYN → DuckDB DECIMAL scaled integer | `<wire-LE-hex>` \| `<storage-type>` \| `<expected-scaled-int>` |
| `edge_cases.txt` | Sign handling, INT32/INT64 boundary values, MONEYN dispatch | documented separately |

## Wire layout (T-SQL TDS spec)

### MONEY (8 bytes)
- Bytes 0..3: **high-order** int32 (little-endian)
- Bytes 4..7: **low-order** int32 (little-endian)
- Scaled value: `int64((int64_t)high << 32) | (uint32_t)low`, then × 10000
- Stored in DuckDB `DECIMAL(19,4)` → physical `INT128` (because p > 18)

### SMALLMONEY (4 bytes)
- Single int32 (little-endian)
- Scaled value: `int32`, then × 10000
- Stored in DuckDB `DECIMAL(10,4)` → physical `INT64`

### MONEYN (variant)
- Nullable wrapper; the value-bytes length determines whether to dispatch
  as MONEY (size==8) or SMALLMONEY (size==4). NULL has length==0 and is
  short-circuited by `TypeConverter::ConvertValue` *before* reaching the
  per-family decoder (`is_null` flag); `codec::money::DecodeFromTds` is
  never invoked with zero-byte input from production paths.

## Conventions

- All wire bytes are written in **little-endian** order, matching what TDS
  sends. `ConvertMoney` reads the 4-byte halves in declaration order
  (high then low), each as LE int32.
- Storage type column in `decode_cases.txt`: `hugeint` for the
  `DECIMAL(19,4)` path, `int64` for the `DECIMAL(10,4)` path.
- The "expected" column is the **scaled integer** (e.g. `1.5000 → 15000`)
  — exactly what DuckDB stores in `FlatVector::GetData<T>(vec)[row]`.

## Why this README sits next to short fixture files

Money has no encode/literal/DDL fixtures because those operations are
explicitly fenced. The README documents the scope so future readers
don't expect the "missing" files.

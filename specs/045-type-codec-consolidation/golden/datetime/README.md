# DateTime family — golden fixtures (spec 045 / US3 sub-phase 6)

Captured during the DateTime codec migration. The pre-migration dispatch
sites were:

- `TypeConverter::ConvertDate` / `ConvertTime` / `ConvertDateTime` /
  `ConvertDatetimeOffset` — internal switch on `column.type_id` for the
  TDS wire format, delegating to `DateTimeEncoding::Convert{Date,Time,
  Datetime,SmallDatetime,Datetime2,DatetimeOffset}`.
- `BCPRowEncoder::EncodeDate` / `EncodeTime` / `EncodeDatetime2` /
  `EncodeDatetimeOffset` — BCP wire-format encoders, scale-parameterised
  via `col.scale`.
- `MSSQLValueSerializer::SerializeDate` / `SerializeTime` /
  `SerializeTimestamp` / `SerializeTimestampTZ` — INSERT-path SQL literal
  forms (the InsertValues context).
- `FilterEncoder::ValueToSQLLiteral` — DATE / TIME / TIMESTAMP / TIMESTAMP_TZ
  arms used `Date::ToString` / `value.ToString()` / `Timestamp::ToString`,
  which produced a less explicit form than the INSERT path (Filter context).
- `MSSQLDDLTranslator::MapTypeToSQLServer` — CreateTable context. Mapped
  `TIMESTAMP → DATETIME2(6)` and `TIMESTAMP_TZ → DATETIMEOFFSET(7)`.
  No arms for TIMESTAMP_MS/NS/SEC (fell through to default-throw).
- `MSSQLDDLTranslator::MapLogicalTypeToCTAS` — CtasCreateTable context.
  Mapped `TIMESTAMP → DATETIME2(7)` (divergent from CreateTable) and
  `TIMESTAMP_TZ → DATETIMEOFFSET(7)`. No arms for TIMESTAMP_MS/NS/SEC.

## SQL Server coverage (every non-UDT temporal type)

This sub-phase aims for **complete SQL Server datetime coverage** per the
spec request. All wire formats are decoded; the BCP encode and DDL emit
paths cover every DuckDB temporal type:

| SQL Server type | TDS type id | Wire size (bytes) | DuckDB type | Round-trip |
|---|---|---|---|---|
| `date` | 0x28 | 3 | `DATE` | round-trip |
| `time(0..2)` | 0x29 | 3 | `TIME` (μs) | scale-aware decode; encode at column scale |
| `time(3..4)` | 0x29 | 4 | `TIME` (μs) | scale-aware decode; encode at column scale |
| `time(5..7)` | 0x29 | 5 | `TIME` (μs; 100ns precision lossy at scale 7) | round-trip at scales 0-6 |
| `datetime` | 0x3D | 8 | `TIMESTAMP` (1/300 s → μs) | decode only (CTAS emits DATETIME2(6)) |
| `smalldatetime` | 0x3A | 4 | `TIMESTAMP` (minute resolution) | decode only |
| `datetime2(0..2)` | 0x2A | 6 | `TIMESTAMP_*` | scale-aware |
| `datetime2(3..4)` | 0x2A | 7 | `TIMESTAMP_*` | scale-aware |
| `datetime2(5..7)` | 0x2A | 8 | `TIMESTAMP_*` | scale-aware (TIMESTAMP_NS loses 2 digits at scale 7) |
| `DATETIMN` (legacy NULLable wrap of `datetime` / `smalldatetime`) | 0x6F | 4 or 8 | `TIMESTAMP` | length-dispatched on the wire (4 → smalldatetime; 8 → datetime) |
| `datetimeoffset(0..2)` | 0x2B | 8 | `TIMESTAMP_TZ` (UTC) | UTC-only; outgoing offset is always 0 |
| `datetimeoffset(3..4)` | 0x2B | 9 | `TIMESTAMP_TZ` (UTC) | as above |
| `datetimeoffset(5..7)` | 0x2B | 10 | `TIMESTAMP_TZ` (UTC) | as above (scale 7 lossy 2 digits) |

## Files

| File | What | Format |
|---|---|---|
| `decode_cases.txt` | TDS wire → DuckDB | type \| scale \| wire-hex \| expected-{date/time/timestamp} |
| `encode_cases.txt` | DuckDB → BCP wire | type \| scale \| input \| expected wire bytes |
| `literal_cases.txt` | DuckDB → SQL literal | type \| input \| ctx \| expected text |
| `ddl_cases.txt` | DuckDB → SQL Server type name | type \| ctx \| expected text |
| `edge_cases.txt` | DATETIMEN length dispatch, scale 7 lossy, fallback rendering, NULL |

## Conventions

- **FR-022**: `FormatSqlLiteral` produces byte-identical output in
  `LiteralContext::Filter` and `LiteralContext::InsertValues`. Both use the
  explicit CAST forms:
    - `DATE` → `'YYYY-MM-DD'`
    - `TIME` → `'HH:MM:SS.fffffff'`
    - `TIMESTAMP*` → `CAST('YYYY-MM-DDTHH:MM:SS.fffffff' AS DATETIME2(7))`
    - `TIMESTAMP_TZ` → `CAST('YYYY-MM-DDTHH:MM:SS.fffffff+HH:MM' AS DATETIMEOFFSET(7))`

  This unifies the pre-spec-045 filter form (which used `Date::ToString` /
  `Timestamp::ToString` and could diverge on edge cases) onto the more
  explicit INSERT form. Filter literals against `datetime2(p < 7)` columns
  are still well-defined — SQL Server's implicit cast trims the trailing
  zeros.

- **FR-027/FR-028**: `FormatDdlTypeName` is byte-identical in both
  `DdlContext::CreateTable` and `DdlContext::CtasCreateTable`:
    - `DATE` → `DATE`
    - `TIME` → `TIME(7)`
    - `TIMESTAMP` → `DATETIME2(6)` (μs — exact match to DuckDB native)
    - `TIMESTAMP_MS` → `DATETIME2(3)` (NEW; was default-throw)
    - `TIMESTAMP_NS` → `DATETIME2(7)` (NEW; closest fit, lossy 2 digits)
    - `TIMESTAMP_SEC` → `DATETIME2(0)` (NEW; was default-throw)
    - `TIMESTAMP_TZ` → `DATETIMEOFFSET(7)`

  Pre-spec-045 the two DDL functions differed for `TIMESTAMP`
  (CreateTable=6 / CtasCreateTable=7); now unified on 6 (μs precision is
  exactly DuckDB's TIMESTAMP — `7` was wire space that the peer could
  never populate).

- **NULL** via NBC (per spec 040 — DATETIMEOFFSET-with-NBC test coverage):
  `TypeConverter::ConvertValue` short-circuits on `is_null` before calling
  the codec, so the DateTime codec never receives an empty `bytes` vector
  for NULL. The codec dispatcher (`codec::FormatSqlLiteral`) also
  short-circuits `Value::IsNull()` → `"NULL"` before delegating.

## RenderAsString (issue #89 fallback)

When a SQL Server view CAST(...) declares a column as VARCHAR but TDS
COLMETADATA reports a temporal wire type, `TypeConverter::ConvertValue`
falls back to `WriteAsStringFallback` which calls
`codec::datetime::RenderAsString(bytes, col)`. The render dispatches on
`col.type_id` + `bytes.size()` and produces the bare text form (no SQL
quoting, no CAST wrapper):

| Type | Rendered form |
|---|---|
| DATE | `YYYY-MM-DD` |
| TIME | `HH:MM:SS.fffffff` |
| DATETIME / SMALLDATETIME / DATETIME2 / DATETIMEN | `YYYY-MM-DD HH:MM:SS.fffffff` (space separator, not 'T') |
| DATETIMEOFFSET | `YYYY-MM-DD HH:MM:SS.fffffff+HH:MM` (UTC; offset always +00:00) |

Same encoders are used for both the SQL-literal path and the
RenderAsString fallback so the rendered text is deterministic and
round-trippable through `CAST(... AS DATETIME2(7))` / `CAST(... AS
DATETIMEOFFSET(7))`.

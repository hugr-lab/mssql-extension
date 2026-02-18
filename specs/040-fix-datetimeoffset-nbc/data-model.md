# Data Model: Datetime Type Wire Formats

**Phase 1 Output** | **Date**: 2026-02-18

## Scale-Dependent Datetime Types

All three types share the same `GetTimeByteLength(scale)` mapping:

| Scale | Time bytes | Total: TIME | Total: DATETIME2 | Total: DATETIMEOFFSET |
| ----- | :--------: | :---------: | :---------------: | :-------------------: |
| 0-2   | 3          | 3           | 6 (3+3)           | 8 (3+3+2)            |
| 3-4   | 4          | 4           | 7 (4+3)           | 9 (4+3+2)            |
| 5-7   | 5          | 5           | 8 (5+3)           | 10 (5+3+2)           |

## Wire Format: DATETIMEOFFSET

```
[1-byte length prefix] [time: 3-5 bytes] [date: 3 bytes] [offset: 2 bytes]
```

- **Length prefix**: Total data bytes (excluding the prefix itself). 0 = NULL in ROW mode.
- **Time**: Unsigned LE integer, units of 10^(-scale) seconds. Already in UTC.
- **Date**: 3-byte unsigned LE, days since 0001-01-01. Already in UTC.
- **Offset**: 2-byte signed LE, minutes from UTC. For display only — not needed for UTC conversion.

## DuckDB Type Mapping

| SQL Server Type   | DuckDB Type          | Precision loss     |
| ----------------- | -------------------- | ------------------ |
| TIME(0-7)         | TIME                 | Scale 7: truncated to microseconds |
| DATETIME2(0-7)    | TIMESTAMP            | Scale 7: truncated to microseconds |
| DATETIMEOFFSET(0-7) | TIMESTAMP_TZ       | Scale 7: truncated to microseconds; offset discarded (UTC stored) |
| DATETIME          | TIMESTAMP            | 1/300s ticks → microseconds (rounding) |
| SMALLDATETIME     | TIMESTAMP            | Minute precision |
| DATE              | DATE                 | None |

## NBC Row Format

In NBCROW (token 0xD2), NULL columns are indicated by a bitmap at the start:

```
[null bitmap: ceil(N/8) bytes] [non-null column 1 data] [non-null column 2 data] ...
```

For non-NULL scale-dependent datetime columns, the wire format is identical to standard ROW: 1-byte length prefix + data bytes. The only difference is that NULL columns have no data at all (no 0-byte length prefix).

## Test Table: NullableDatetimeScales

New table for docker/init/init.sql to force NBCROW encoding:

```sql
CREATE TABLE dbo.NullableDatetimeScales (
    id INT NOT NULL PRIMARY KEY,
    -- TIME at different scales
    col_time_s0       TIME(0) NULL,
    col_time_s3       TIME(3) NULL,
    col_time_s7       TIME(7) NULL,
    -- DATETIME2 at different scales
    col_datetime2_s0  DATETIME2(0) NULL,
    col_datetime2_s3  DATETIME2(3) NULL,
    col_datetime2_s7  DATETIME2(7) NULL,
    -- DATETIMEOFFSET at different scales
    col_dto_s0        DATETIMEOFFSET(0) NULL,
    col_dto_s3        DATETIMEOFFSET(3) NULL,
    col_dto_s7        DATETIMEOFFSET(7) NULL,
    -- Padding nullable columns to ensure NBCROW encoding
    pad_01 INT NULL, pad_02 INT NULL, pad_03 INT NULL,
    pad_04 INT NULL, pad_05 INT NULL, pad_06 INT NULL,
    pad_07 INT NULL, pad_08 INT NULL, pad_09 INT NULL,
    pad_10 INT NULL, pad_11 INT NULL, pad_12 INT NULL
);
```

Test data rows:
1. All non-NULL (values at each scale)
2. All datetime columns NULL (tests null bitmap for datetime types)
3. Mixed NULL/non-NULL (alternating pattern)
4. Only DATETIMEOFFSET non-NULL (others NULL)
5. All non-NULL with different offset values (+05:30, -08:00, +00:00)

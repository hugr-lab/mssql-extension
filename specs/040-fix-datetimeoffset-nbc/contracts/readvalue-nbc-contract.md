# Contract: ReadValueNBC Type Dispatch

**Phase 1 Output** | **Date**: 2026-02-18

## Interface

```
size_t ReadValueNBC(const uint8_t *data, size_t length, size_t col_idx,
                    std::vector<uint8_t> &value, bool &is_null)
```

## Preconditions

- Column at `col_idx` is NOT NULL per the NBC bitmap (caller already checked)
- `data` points to the start of this column's wire data (after bitmap, after preceding columns)
- `is_null` is always set to `false` (NBC bitmap handles nullability)

## Contract for scale-dependent datetime types

All three types (TIME, DATETIME2, DATETIMEOFFSET) use the same NBC wire pattern:

```
Input:  [1-byte length] [data_length bytes of payload]
Output: value = payload bytes (without length prefix)
Return: 1 + data_length (total bytes consumed)
```

The `data_length` byte encodes the total payload size, which varies by type and scale:
- TIME: 3-5 bytes
- DATETIME2: 6-8 bytes (time + 3 date bytes)
- DATETIMEOFFSET: 8-10 bytes (time + 3 date + 2 offset bytes)

## Required change

Add `case TDS_TYPE_DATETIMEOFFSET` between `TDS_TYPE_DATETIME2` and `TDS_TYPE_UNIQUEIDENTIFIER` in the `ReadValueNBC()` switch statement, using the identical pattern already used by the other types.

## Postconditions

- `value` contains the raw payload bytes (time + date + offset)
- `TypeConverter::ConvertDatetimeOffset()` receives these bytes and uses `DateTimeEncoding::ConvertDatetimeOffset(data, scale)` to produce a UTC `timestamp_t`
- Return value equals total bytes consumed (1 + data_length)

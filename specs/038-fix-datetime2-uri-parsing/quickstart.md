# Quickstart: Fix datetime2(0) Truncation and URI Password Parsing

## Fix 1: datetime2 and TIME Scale-Aware Decoding

### What to Change

**File**: `src/tds/encoding/datetime_encoding.cpp`

**`ConvertDatetime2`** (line 80): Replace `int64_t microseconds = time_ticks / 10;` with the scale-aware conversion already used in `ConvertDatetimeOffset` (lines 129–143).

**`ConvertTime`** (line 40): Same fix — replace `int64_t microseconds = ticks / 10;` with scale-aware conversion.

### Conversion Logic

Extract a shared helper or inline the pattern:

```
if (scale <= 6):
    multiplier = 10^(6 - scale)
    microseconds = ticks * multiplier
else:
    microseconds = ticks / 10
```

Scale mapping:
| Scale | Units | Multiplier to µs |
|-------|-------|-------------------|
| 0 | seconds | × 1,000,000 |
| 1 | 0.1 seconds | × 100,000 |
| 2 | 0.01 seconds | × 10,000 |
| 3 | milliseconds | × 1,000 |
| 4 | 0.1 ms | × 100 |
| 5 | 0.01 ms | × 10 |
| 6 | microseconds | × 1 |
| 7 | 100 nanoseconds | ÷ 10 |

### Tests

Add unit tests in `test/cpp/test_datetime_encoding.cpp`:
- Test `ConvertDatetime2` with scale 0 (the reported bug case)
- Test `ConvertDatetime2` with scales 1–6
- Test `ConvertDatetime2` with scale 7 (regression check)
- Test `ConvertTime` with scale 0 and scale 7
- Test edge cases: midnight (0 ticks), max time (23:59:59)

## Fix 2: URI Parser `@` Handling

### What to Change

**File**: `src/mssql_storage.cpp`

**Line 196**: Change `rest.find('@')` to `rest.rfind('@')`.

One line change. That's it.

### Tests

Add unit tests in `test/cpp/test_uri_parsing.cpp`:
- Password with single `@` (unencoded)
- Password with multiple `@` (unencoded)
- Password with URL-encoded `@` (`%40`)
- Password with `:`, `/`, `?` (URL-encoded)
- URI with no credentials (no `@`)
- Standard URI without special characters (regression)

## Build & Test

```bash
GEN=ninja make        # Build
GEN=ninja make test   # Run unit tests (no SQL Server needed)
```

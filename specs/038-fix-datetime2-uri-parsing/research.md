# Research: Fix datetime2(0) Truncation and URI Password Parsing

**Date**: 2026-02-16
**Branch**: `038-fix-datetime2-uri-parsing`

## Research 1: TDS datetime2 Time Encoding by Scale

### Decision

Time ticks in datetime2 (and TIME) are stored in units of 10^(-scale) seconds, not always 100-nanosecond units. The conversion to microseconds must multiply by 10^(6-scale) for scales 0–6, and divide by 10 for scale 7.

### Rationale

The MS-TDS specification defines the time portion as an unsigned integer representing "the number of 10^(-n) second increments since 12 AM within the day", where n is the scale. The existing `ConvertDatetimeOffset` function already implements this correctly (lines 129–143 of `datetime_encoding.cpp`). The same logic must be applied to `ConvertDatetime2` and `ConvertTime`.

### Evidence from the Codebase

**Correct implementation** (`ConvertDatetimeOffset`, lines 129–143):
```cpp
if (scale <= 6) {
    int64_t multiplier = 1;
    for (int i = 0; i < 6 - scale; i++) {
        multiplier *= 10;
    }
    microseconds = time_ticks * multiplier;
} else {
    microseconds = time_ticks / 10;
}
```

**Incorrect implementations** (both use `ticks / 10` regardless of scale):
- `ConvertDatetime2` line 80: `int64_t microseconds = time_ticks / 10;`
- `ConvertTime` line 40: `int64_t microseconds = ticks / 10;`

**BCP encoder** (`TimestampToDatetime2Components`, lines 721–754 of `bcp_row_encoder.cpp`) already handles scale correctly in the write path.

### Bug Reproduction (from Issue #73)

For `datetime2(0)` value `2020-04-04 12:12:48`:
- SQL Server stores time as `43968` (seconds since midnight: 12×3600 + 12×60 + 48)
- Current code: `43968 / 10 = 4396` microseconds → `00:00:00.004396`
- Correct code: `43968 × 1,000,000 = 43,968,000,000` microseconds → `12:12:48`

### Alternatives Considered

None — the fix is unambiguous. The correct conversion is already implemented in `ConvertDatetimeOffset`.

---

## Research 2: URI `@` Delimiter for Passwords

### Decision

Change `rest.find('@')` to `rest.rfind('@')` in the `ParseUri` function to use the last `@` as the credentials/host delimiter.

### Rationale

In the standard URI syntax `scheme://userinfo@host`, the `@` delimiter is the last one before the host component. RFC 3986 recommends URL-encoding `@` in the userinfo component, but in practice many users don't. Using `rfind` handles both cases: URL-encoded passwords work because `%40` doesn't contain a literal `@`, and unencoded passwords work because the host portion cannot contain `@`.

### Evidence from the Codebase

**Current code** (`mssql_storage.cpp`, line 196):
```cpp
auto at_pos = rest.find('@');
```

For `mssql://sa:MyPass@Word@127.0.0.1:1433/master`:
- `find('@')` returns position of `@` after `MyPass` → user=`sa`, password=`MyPass`, host=`Word@127.0.0.1`
- `rfind('@')` returns position of `@` before `127.0.0.1` → user=`sa`, password=`MyPass@Word`, host=`127.0.0.1`

### Alternatives Considered

1. **Require URL-encoding**: Only support `%40` for `@` in passwords. Rejected — this is user-hostile and the common URI convention is to use the last `@`.
2. **Custom escape syntax**: Use `\@` or similar. Rejected — non-standard, confusing.
3. **Use `rfind`**: Simple, standard, handles both encoded and unencoded `@`. Chosen.

---

## Research 3: TIME Type Impact

### Decision

The `ConvertTime` function shares the same bug and must be fixed with the same scale-aware conversion.

### Rationale

`ConvertTime` (line 29–43) uses the same `ticks / 10` hardcoded conversion. The TIME type in SQL Server uses identical encoding (scale 0–7, same byte lengths). Users storing TIME(0) through TIME(6) columns will experience the same data corruption.

### Alternatives Considered

None — the fix is the same as for datetime2.

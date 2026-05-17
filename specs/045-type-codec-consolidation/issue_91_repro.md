# Issue #91 — BCP NVARCHAR length validation: reproduction evidence

**Spec**: 045-type-codec-consolidation
**Phase**: 5 — US5 String migration + issue #91 fix
**Tracking issue**: [#91 — BCP nvarchar character-vs-byte length mismatch](https://github.com/hugr-lab/mssql-extension/issues/91)
**Baseline SHA**: `14fdc634` (see `golden/baseline_sha.txt`)
**Test file**: `test/sql/copy/copy_nvarchar_length_validation.test`
**Date captured**: 2026-05-15 (this branch HEAD before Phase-5 implementation)

## Behavioral summary

| Scenario | Input | Target | UCS-2 code units | UTF-16 bytes | Capacity bytes | Baseline (14fdc634) | Phase-5 fix |
|---|---|---|---|---|---|---|---|
| (a) | 250 × 'A' + 250 × 'Б' | `NVARCHAR(500)` | 500 | 1000 | 1000 | ✅ succeed + round-trip | ✅ succeed + round-trip |
| (b) | 8 × ASCII + 4 × 😀😁😂😃 | `NVARCHAR(20)` | 16 | 32 | 40 | ✅ succeed + round-trip | ✅ succeed + round-trip |
| (c) | 8 × ASCII + 8 × emoji | `NVARCHAR(20)` | 24 | 48 | 40 | ❌ **opaque server error** | ❌ **client-side clear error** |

Note that the **failure outcome is the same** for scenario (c) — neither side accepts data that the server cannot store. What changes is the **failure message clarity**: pre-fix the operator only learns `colid 2` (an internal index) and a generic "invalid column length"; post-fix they see the column name and the exact UCS-2 code-unit overage.

## Baseline failure mode — pre-fix on SHA 14fdc634

Build and run:

```bash
git worktree add /tmp/spec045-baseline 14fdc634
ln -sf $PWD/vcpkg /tmp/spec045-baseline/vcpkg   # share the manifest dir
cd /tmp/spec045-baseline && git submodule update --init --recursive
GEN=ninja make
cp ../mssql-extension/test/sql/copy/copy_nvarchar_length_validation.test test/sql/copy/
MSSQL_TESTDB_DSN='Server=localhost,1433;Database=TestDB;User Id=sa;Password=TPassw0rd!' \
  build/release/test/unittest 'test/sql/copy/copy_nvarchar_length_validation.test'
```

Output (verbatim, captured 2026-05-15 on `045-type-codec-consolidation` HEAD `93e821d`):

```
[0/1] (0%): test/sql/copy/copy_nvarchar_length_validation.test
1. test/sql/copy/copy_nvarchar_length_validation.test:110
================================================================================
Query failed, but error message did not match expected error message: payload (test/sql/copy/copy_nvarchar_length_validation.test:110)!
================================================================================
COPY local_c TO 'mssql://db/dbo/copy_nv_91_c' (FORMAT 'bcp', CREATE_TABLE false);
================================================================================
Actual result:
================================================================================
IO Error: MSSQL COPY: Failed to finalize BCP stream: {"exception_type":"Invalid Input","exception_message":"MSSQL: BCP failed: Received an invalid column length from the bcp client for colid 2."}

[1/1] (100%): test/sql/copy/copy_nvarchar_length_validation.test
===============================================================================
test cases:  1 |  0 passed | 1 failed
assertions: 22 | 21 passed | 1 failed
```

### What the baseline output proves

- Scenarios (a) and (b) succeed — 21 of 22 assertions pass.
- Scenario (c) is the failing assertion: the test expects a client-side error that contains the substring `payload`; the baseline raises an opaque server-side error that mentions only `colid 2`.
- The error message does **not** identify the column by name and does **not** report the over-length amount in code units (the unit that nvarchar(N) actually counts in).
- The error is raised *after* the BCP stream is finalized server-side, i.e. after the bytes have already been transmitted to SQL Server.

## Wire-level analysis — why the baseline fails

`src/tds/encoding/bcp_row_encoder.cpp:353 EncodeNVarchar` (on baseline SHA 14fdc634):

```cpp
void BCPRowEncoder::EncodeNVarchar(vector<uint8_t> &buffer, const string_t &value) {
    size_t input_len = value.GetSize();        // UTF-8 byte count
    const char *input = value.GetData();
    size_t start_pos = buffer.size();
    buffer.resize(start_pos + 2 + input_len * 2);  // reserve 2-byte length prefix + UTF-16 bytes
    size_t utf16_len = Utf16LEEncodeDirect(input, input_len, buffer.data() + start_pos + 2);
    buffer[start_pos]     = static_cast<uint8_t>(utf16_len & 0xFF);
    buffer[start_pos + 1] = static_cast<uint8_t>((utf16_len >> 8) & 0xFF);
    buffer.resize(start_pos + 2 + utf16_len);
}
```

There is **no validation against `col.max_length`** anywhere in this function — the encoder simply writes the actual encoded UTF-16 length into the 2-byte prefix. When that length exceeds the target column's declared `max_length`, SQL Server's BCP receiver rejects the row at stream-finalize time with the generic "Received an invalid column length" message.

The 2-byte length prefix written by the baseline for scenario (c)'s payload is `0x0030` (48 bytes), exceeding the column's `max_length=0x0028` (40 bytes for `NVARCHAR(20)`). The server rejects on that length-prefix mismatch.

## Post-fix mode — Phase 5 HEAD (captured 2026-05-15)

After T041-T051 landed, scenario (c) raises a client-side
`InvalidInputException` BEFORE any wire bytes are sent to SQL Server.
The error message names the column and reports the over-length amount in
UCS-2 code units (the unit that `nvarchar(N)` actually counts in).

Reproduction (verbatim from a `build/release/duckdb` REPL on Phase-5 HEAD):

```text
$ MSSQL_TESTDB_DSN='Server=localhost,1433;Database=TestDB;User Id=sa;Password=...' \
  build/release/duckdb -unsigned -c "
    LOAD 'build/release/extension/mssql/mssql.duckdb_extension';
    ATTACH '\$MSSQL_TESTDB_DSN' AS db (TYPE mssql);
    -- target table pre-created via mssql_exec:
    --   CREATE TABLE dbo.copy_nv_91_c (id INT NOT NULL, payload NVARCHAR(20) NOT NULL)
    CREATE TABLE local_c AS SELECT 1 AS id,
      ('ABCDEFGH' || '😀😁😂😃😄😅😆😇')::VARCHAR AS payload;
    COPY local_c TO 'mssql://db/dbo/copy_nv_91_c' (FORMAT 'bcp', CREATE_TABLE false);
  "

Invalid Input Error: MSSQL: NVARCHAR column 'payload' overflow:
value is 24 UCS-2 code units (48 UTF-16LE bytes) but column max is
20 code units (40 bytes)
```

And the SQL regression test passes:

```text
$ build/release/test/unittest 'test/sql/copy/copy_nvarchar_length_validation.test'
[0/1] (0%): test/sql/copy/copy_nvarchar_length_validation.test
[1/1] (100%): test/sql/copy/copy_nvarchar_length_validation.test
===============================================================================
All tests passed (22 assertions in 1 test case)
```

### The fix on the encoder side

`src/codec/string_codec.cpp` adds the FR-023 client-side length check
inside `ValidateNVarcharLength`:

```cpp
void ValidateNVarcharLength(const char *utf8_data, size_t utf8_len,
                            const mssql::BCPColumnMetadata &col) {
    if (col.IsPLPType()) {
        return;  // nvarchar(max) — no client-side cap
    }
    std::string tmp(utf8_data, utf8_len);
    size_t utf16_byte_len = tds::encoding::Utf16LEByteLength(tmp);
    if (utf16_byte_len > col.max_length) {
        throw InvalidInputException(
            "MSSQL: NVARCHAR column '%s' overflow: value is %zu UCS-2 code "
            "units (%zu UTF-16LE bytes) but column max is %u code units (%u bytes)",
            col.name, utf16_byte_len / 2, utf16_byte_len,
            col.max_length / 2, col.max_length);
    }
}
```

Called from `EncodeNVarcharFromUtf8` (the shared inner helper of both
`codec::string::EncodeToBcp` overloads) — before any wire bytes are
appended to the per-row BCP buffer. The PLP path
(`col.IsPLPType() == true`, i.e. `max_length == 0xFFFF`) is unchanged
because `nvarchar(max)` has no fixed column cap.

### Comparison

| Aspect | Pre-fix (baseline 14fdc634) | Post-fix (Phase 5 HEAD) |
|---|---|---|
| When error fires | After all bytes flushed to SQL Server; on `BCP_DONE` finalize | Client-side, before any bytes leave the encoder |
| Message identifies the column? | No — only "colid 2" (internal index) | Yes — `'payload'` (column name) |
| Reports over-length in UCS-2 code units? | No — generic "invalid column length" | Yes — `24 UCS-2 code units` |
| Reports byte count? | No | Yes — `48 UTF-16LE bytes` vs allowed `40 bytes` |
| Network round-trip cost on failure | Full BCP stream of the failing batch + server rejection token | None (catches before bytes are sent) |
| Connection state after failure | Clean — server cleanly closed BCP frame with rejection | **Dirty — server still expects more BCP data; ATTENTION-token recovery is a pre-existing limitation tracked outside this spec** |

The connection-state limitation in the last row is documented inside
`test/sql/copy/copy_nvarchar_length_validation.test` — the test skips
post-(c)-cleanup and relies on the pre-test `IF OBJECT_ID(...)` DROP for
subsequent runs.

## Provenance

- Repo: `/Users/vgribanov/projects/hugr-lab/mssql-extension`
- Baseline worktree: `/tmp/spec045-baseline` @ `14fdc634`
- Server: SQL Server 2022 in Docker container `mssql-dev`, port 1433, database `TestDB`
- Run from `045-type-codec-consolidation` HEAD `93e821d`

# Phase 1 Data Model: LOGIN7 Non-ASCII Fix + simdutf Foundation

**Feature**: 043-refactoring-foundation
**Date**: 2026-05-14

This is a protocol-encoding feature, not a data-modeling feature. The
"entities" below are in-memory structures touched by the change.
Persistence is N/A.

## E1. LOGIN7 Packet (in-memory)

The TDS LOGIN7 packet (MS-TDS §2.2.6.4) is a contiguous byte
sequence built by `TdsProtocol::BuildLogin7*` in
`src/tds/tds_protocol.cpp`.

### Layout (TDS 7.4)

| Offset | Field | Size (bytes) | Notes |
|--------|-------|--------------|-------|
| 0 | Length | 4 | Total LOGIN7 length, LE |
| 4 | TDSVersion | 4 | `0x74000004` (TDS 7.4) |
| 8 | PacketSize | 4 | Negotiated packet size, LE |
| 12 | ClientProgVer | 4 | Client program version |
| 16 | ClientPID | 4 | **Run-varying** — excluded from regression bytecmp |
| 20 | ConnectionID | 4 | Zero for new connections |
| 24 | OptionFlags1 | 1 | `USE_DB | SET_LANG = 0xA0` |
| 25 | OptionFlags2 | 1 | `fODBC = 0x02` |
| 26 | TypeFlags | 1 | DFLT / read-write |
| 27 | OptionFlags3 | 1 | TDS 7.2+ options |
| 28 | ClientTimeZone | 4 | Minutes from UTC |
| 32 | ClientLCID | 4 | `0x0409` en-US |
| 36 | ibHostName / cchHostName | 4 | **In-scope**: variable-field offset/length pair |
| 40 | ibUserName / cchUserName | 4 | **In-scope** |
| 44 | ibPassword / cchPassword | 4 | **In-scope** — the headline bug |
| 48 | ibAppName / cchAppName | 4 | **In-scope** |
| 52 | ibServerName / cchServerName | 4 | **In-scope** |
| 56 | (unused / extension offset) | 4 | Not touched |
| 60 | ibCltIntName / cchCltIntName | 4 | Length 0 |
| 64 | ibLanguage / cchLanguage | 4 | Length 0 |
| 68 | ibDatabase / cchDatabase | 4 | **In-scope** |
| 72 | ClientID (MAC) | 6 | **Run-varying** — excluded from regression bytecmp |
| 78 | ibSSPI / cbSSPI | 4 | Length 0 (no SSPI in SQL auth) |
| 82 | ibAtchDBFile / cchAtchDBFile | 4 | Length 0 |
| 86 | ibChangePassword / cchChangePassword | 4 | Length 0 |
| 90 | cbSSPILong | 4 | Zero |
| 94 onward | Variable data | … | Concatenated UTF-16LE field payloads in declared order |

### Invariants this spec enforces

1. **Length semantics**: For every variable field at offsets 36, 40,
   44, 48, 52, 68, the `cch*` (low 16 bits of each pair) is the
   **count of UTF-16 code units**, not UTF-8 byte length.
   (FR-001.)
2. **Offset semantics**: For every variable field, the `ib*` (high
   16 bits of each pair) equals the prior `ib*` plus the UTF-16LE
   byte length of the prior field. The first variable field starts
   at offset 94. (FR-002.)
3. **Field cap**: For every variable field, `cch* <= 128`. Overflow
   throws `IOException` with field name and observed length.
   (FR-008.)
4. **Password obfuscation**: For offset 44 only, the UTF-16LE bytes
   in the variable region are obfuscated by `TdsProtocol::EncodePassword`
   (nibble swap + XOR 0xA5) per MS-TDS §2.2.6.4. Other fields are
   plain UTF-16LE. (FR-003.)
5. **ASCII regression**: For ASCII-only input, all of region [36..72)
   plus region [94..) is bitwise identical to v0.1.18. (FR-007,
   SC-003, Clarification Q3.)

## E2. Login7VarField helper (proposed, internal)

New file-static helper in `src/tds/tds_protocol.cpp` consumed by
all three `BuildLogin7*` functions.

```cpp
struct Login7VarFieldResult {
    std::vector<uint8_t> utf16le_bytes;  // payload bytes (obfuscated if password)
    uint16_t cch;                        // UTF-16 code-unit count
    uint16_t ib;                         // offset of this field in the packet
};

static Login7VarFieldResult EncodeLogin7VarField(
    const char *field_name,             // for error message (e.g. "Password")
    const std::string &utf8_text,       // user-supplied UTF-8
    uint16_t &cumulative_ib_offset,     // in/out
    bool obfuscate_password = false);
```

### Contract

- **Encoding**: `utf8_text` is encoded to UTF-16LE via
  `SimdutfUtf16LEEncode`. Empty input → empty output, `cch = 0`.
- **Cap check**: If `result.size() / 2 > 128`, throws
  `IOException("LOGIN7 field <field_name> exceeds the TDS limit of
  128 UTF-16 code units (got <N>)")`. (FR-008.)
- **Obfuscation**: If `obfuscate_password == true`, applies
  `TdsProtocol::EncodePassword` semantics to the encoded bytes.
- **Offset bookkeeping**: Returns `ib = cumulative_ib_offset` as
  the offset assigned to this field; advances
  `cumulative_ib_offset` by `result.utf16le_bytes.size()`.

### State transitions

None — the helper is pure.

## E3. simdutf wrapper module (new public surface)

New header `src/include/tds/encoding/simdutf_wrappers.hpp` and
matching `.cpp` in `src/tds/encoding/`. Symbols live in namespace
`duckdb::tds::encoding`.

### Public functions

See `contracts/simdutf_wrappers.hpp` for the exact header.

### Invariants

- All functions are free, stateless, thread-safe.
- For valid UTF-8 / UTF-16LE input, output is bitwise identical
  to the legacy `Utf16LEEncode` / `Utf16LEDecode` / etc.
  (FR-025, SC-007.)
- For invalid input, the wrapper falls back to the legacy
  converter. (FR-034, Clarification Q1.)
- No exceptions thrown on invalid input. (FR-034.)

### Lifecycle

Static C++ free functions. No object lifecycle.

## E4. Connection-string parsed map (no schema change)

`case_insensitive_map_t<string>` produced by `ParseUri` /
`ParseConnectionString`. Schema unchanged; semantics tightened by
the audit:

| Key | Semantic change in this spec |
|-----|------------------------------|
| `user` | Now strictly UTF-8 bytes from a stricter `UrlDecode` (R5). |
| `password` | Same. ADO.NET `{...}` quoting honored (R6). |
| `database` | Same. |
| `server` / `host:port` | Same. |
| Other keys | Audit-only, no semantic change. |

## E5. LOGIN7 round-trip test fixture

A test-side data structure shared by `test_login7_encoding.cpp`.

```cpp
struct Login7Fixture {
    std::string host;
    std::string user;
    std::string password;
    std::string app_name;
    std::string server;
    std::string database;
};

struct Login7Parsed {
    uint16_t ib_hostname, cch_hostname;
    uint16_t ib_username, cch_username;
    uint16_t ib_password, cch_password;
    uint16_t ib_appname,  cch_appname;
    uint16_t ib_servername, cch_servername;
    uint16_t ib_database, cch_database;
    std::vector<uint8_t> hostname_bytes;
    std::vector<uint8_t> username_bytes;
    std::vector<uint8_t> password_bytes;  // still obfuscated; test de-obfuscates before compare
    std::vector<uint8_t> appname_bytes;
    std::vector<uint8_t> servername_bytes;
    std::vector<uint8_t> database_bytes;
};

Login7Parsed ParseLogin7Packet(const std::vector<uint8_t> &packet);
```

Used to verify (FR-020 / SC-002) that the packet round-trips to
the same UTF-8 input for arbitrary fixtures.

## Entity reference summary

| Entity | Location | Lifecycle | New / Modified |
|--------|----------|-----------|----------------|
| LOGIN7 packet | `tds_protocol.cpp` | transient (per connection) | semantics modified for non-ASCII |
| `Login7VarField` helper | `tds_protocol.cpp` (file-static) | per-call | NEW |
| simdutf wrapper free functions | `src/tds/encoding/simdutf_wrappers.{hpp,cpp}` | static | NEW |
| Parsed connection-string map | `src/mssql_storage.cpp` | transient (per attach) | semantics tightened |
| `Login7Fixture` / `Login7Parsed` | `test/cpp/test_login7_encoding.cpp` | test only | NEW |

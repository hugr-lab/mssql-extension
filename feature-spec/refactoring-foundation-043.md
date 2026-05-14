# Foundation Fixes — Design Document

**Spec ID**: 043
**Target release**: hugr-lab/mssql-extension v0.2.0
**Scope**: §4.2 (SIMD UTF Conversion via simdutf), §4.8 (Non-ASCII Password Fix)
**Source**: extracted from `refactoring-v0.2.md` for `.specify/` workflow input
**Dependencies**: none (parallel-mergeable with spec 042 Integrated Authentication)
**Consumers**: spec 044 (Codec Layer) uses the simdutf wrapper introduced here

---

## Context

The `hugr-lab/mssql-extension` is a DuckDB community extension providing
native TDS-protocol access to Microsoft SQL Server, Azure SQL, Azure
Synapse Dedicated/Serverless, and Microsoft Fabric Warehouse. As of
v0.1.18 it supports SQL authentication, Azure Entra ID
(device-code/fedauth/manual-token), table scan with predicate
pushdown, INSERT/UPDATE/DELETE/CTAS DML, BCP-backed bulk load, and a
partial MERGE implementation. The v0.2.0 release consolidates a
broader refactor; this spec is the smallest standalone unit and
gates the foundation that the codec layer (spec 044) builds on.

This document is one of two standalone design extracts feeding the
`.specify/` workflow:

- **This document** (`refactoring-foundation-043.md`): foundation
  fixes — simdutf migration + LOGIN7 password encoding fix.
- **Companion** (`refactoring-codec-044.md`): full codec layer
  consolidation including the consumer side of the simdutf
  wrapper.

The full v0.2.0 refactor context lives in `refactoring-v0.2.md`;
this file is self-contained for the purpose of generating the
spec 043 `.specify/` artifacts (spec.md, plan.md, tasks.md,
research.md, quickstart.md, checklists/requirements.md) following
the existing template established by specs 001-041 in the repo.

---

## Spec 043 — Foundation fixes

**`specs/043-foundation-fixes.md`** covers §4.2 (simdutf UTF
conversion) and §4.8 (LOGIN7 non-ASCII password fix).

Why together: both are small standalone fixes touching low-level
encoding. Both independent of everything else. Splitting them
into two specs doubles `.specify/` overhead for ~700 LOC of
combined change.

Scope:
- Add simdutf as a vcpkg dependency, build wrapper at
  `src/tds/encoding/utf16.cpp` exposing `Utf8ToUtf16LE` /
  `Utf16LEToUtf8` with the same signatures as the current hand-
  rolled converter. Old converter stays in place for now —
  switching call sites is the codec spec's job (044).
- Fix LOGIN7 password encoding: investigate the non-ASCII
  password bug (likely UTF-16LE char-count vs byte-count
  mismatch or XOR/nibble-order issue on the obfuscated bytes),
  fix it as a localized patch in `src/tds/auth/sql_auth_strategy.cpp`.
  No interface changes.

Out of scope for 043:
- Migrating call sites to use simdutf. That's part of the codec
  layer migration in 044.
- Refactoring auth strategies. The password fix is a one-liner-
  scale patch; structural changes wait for spec 042 (Oluies)
  which is already restructuring this area.

Testable:
- simdutf: unit tests for round-trip UTF-8 ↔ UTF-16LE on edge
  cases (surrogates, empty strings, non-BMP characters, invalid
  sequences). Both old and new converters tested against the
  same fixtures during the transition.
- Password: integration test against Docker SQL Server with
  passwords containing non-ASCII bytes (cyrillic, accented Latin,
  CJK). Current 0.1.18 fails on these; after fix they succeed.


---

## §4.2 SIMD UTF Conversion via simdutf

#### 4.2.1 Rationale

`simdutf` is already linked by DuckDB (`third_party/simdutf/`).
The library uses AVX2/AVX-512/NEON kernels auto-selected at runtime.
For ASCII-only strings it degenerates to `memcpy`; for mixed
content it is 3–10× faster than scalar conversion.

#### 4.2.2 Wrapper interface

```cpp
// src/encoding/utf_conversion.hpp

class Utf16ToUtf8Converter {
public:
    // Per-row: decode a single UTF-16LE byte sequence into a target
    // DuckDB string_t allocated in the target vector's string heap.
    static inline string_t DecodeInto(
        Vector& target_vec,
        const uint8_t* utf16le_bytes,
        size_t byte_count);

    // Batch: decode a column of UTF-16LE strings described by
    // (blob, offsets[count+1]) into a flat string vector.
    static void DecodeBatchContiguous(
        Vector& target_vec,
        const uint8_t* utf16le_blob,
        const uint32_t* offsets_bytes,
        idx_t count);
};

class Utf8ToUtf16Converter {
public:
    // Encode one DuckDB string_t as length-prefixed UTF-16LE into
    // the TDS writer. `u_short_len` chooses between 2-byte length
    // prefix (NVARCHAR <= 4000) and PLP chunked encoding (MAX).
    static inline void EncodeLengthPrefixed(
        TdsWriter& writer,
        const string_t& src,
        bool u_short_len);

    // Batch variant: pre-compute total UTF-16 output size, allocate
    // contiguous buffer, convert row by row, write in one tds
    // send. Used by BCP path for NVARCHAR columns.
    static void EncodeBatchContiguous(
        const Vector& in,
        idx_t count,
        TdsWriter& writer);
};
```

#### 4.2.3 Integration points

- `StringCodec::DecodeBatch` uses `DecodeBatchContiguous` after
  gathering UTF-16LE row bytes into a single buffer via TDS token
  parsing.
- `StringCodec::EncodeBcpBatch` uses `EncodeBatchContiguous` for
  NVARCHAR columns.
- PLP (`NVARCHAR(MAX)`) decode: chunk-wise append into a per-row
  staging buffer, then per-row `DecodeInto` on finalize. Cannot
  fully batch because chunk boundaries may split UTF-16 code units
  (surrogate pairs), requiring careful accumulation.
- Pushdown literal `FormatSqlLiteral` for VARCHAR/NVARCHAR literals
  must also emit correctly-escaped UTF-16 — reuses the same
  conversion path.

#### 4.2.4 CMake wiring

`simdutf` headers are inside the DuckDB submodule. Extension
CMakeLists adds:

```cmake
target_include_directories(${TARGET_NAME}_loadable_extension
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/simdutf)
target_include_directories(${TARGET_NAME}_static_extension
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/simdutf)
```

The library itself is already linked via `duckdb_static`; no new
link step needed. Validate by compiling a trivial test that calls
`simdutf::convert_utf8_to_utf16le`.

If symbol conflicts arise (unlikely — simdutf is namespaced),
consider namespacing via `-Dsimdutf=duckdb_mssql_simdutf` or
vendoring a separate copy.

#### 4.2.5 Expected performance impact

| Scenario | Speedup (estimated) |
|---|---|
| ASCII-heavy NVARCHAR columns (row decode) | 3–5× |
| Cyrillic/CJK NVARCHAR columns (row decode) | 5–10× |
| BCP NVARCHAR encode (UTF-8 → UTF-16) | 5–8× |
| VARCHAR columns (no UTF-16 involved) | unchanged |

Validated via `test/benchmark/utf_conversion.cpp` with synthetic
datasets. Acceptance: no regression on any workload; ≥ 2× on
mixed-content NVARCHAR.

---


---

## §4.8 Non-ASCII Password Fix

#### 4.8.1 Likely root causes

LOGIN7 password encoding specification (MS-TDS §2.2.6.4):

1. Convert password text to UCS-2/UTF-16LE.
2. Byte-swap each nibble: `((b & 0x0F) << 4) | ((b >> 4) & 0x0F)`.
3. XOR each byte with `0xA5`.
4. `ibPassword` = byte offset of password in variable data block.
5. `cchPassword` = **number of 16-bit characters** (NOT bytes).

Common bugs at any of these steps:

- **(a)** UTF-8 → UTF-16 conversion uses locale-dependent `mbstowcs`
  which depends on `LC_ALL`/`LC_CTYPE` — fails for bytes outside
  current locale.
- **(b)** XOR applied to UTF-8 bytes before conversion.
- **(c)** `cchPassword` set to UTF-8 byte length instead of UTF-16
  code unit count.
- **(d)** `cchPassword` set to UTF-16 byte count (double the correct
  value).
- **(e)** Surrogate pair handling broken (characters outside BMP,
  e.g. `𝄞`, take 2 UTF-16 code units each — character count ≠
  length of input string even in UTF-16 terms).

#### 4.8.2 Diagnosis plan

1. Add targeted test: connect with password = `"Тест123!"` (Cyrillic),
   `"Ünlaut"` (umlaut), `"🔒secure"` (emoji with surrogate pair).
2. Enable `MSSQL_DEBUG=1` to dump LOGIN7 packet.
3. Decode the password field manually (reverse XOR 0xA5, reverse
   nibble swap, interpret as UTF-16LE) — compare to expected.
4. Fix the step that differs.

#### 4.8.3 Fix

Once identified, the fix likely belongs in
`TdsConnection::BuildLogin7Packet()`. Replace whatever string
conversion is there with:

```cpp
// UTF-8 → UTF-16LE via simdutf (same library used elsewhere)
size_t password_utf16_units = simdutf::utf16_length_from_utf8(
    password.data(), password.size());

std::vector<char16_t> password_utf16(password_utf16_units);
simdutf::convert_utf8_to_utf16le(
    password.data(), password.size(), password_utf16.data());

// Apply TDS XOR/nibble-swap per MS-TDS §2.2.6.4
for (auto& unit : password_utf16) {
    uint8_t lo = unit & 0xFF;
    uint8_t hi = (unit >> 8) & 0xFF;
    lo = ((lo & 0x0F) << 4) | ((lo & 0xF0) >> 4);
    hi = ((hi & 0x0F) << 4) | ((hi & 0xF0) >> 4);
    unit = (hi << 8) | lo;
    unit ^= 0xA5A5;
}

// cchPassword = UTF-16 code unit count, NOT bytes
login7.cchPassword = password_utf16_units;
login7.ibPassword = /* offset in variable block */;
// Write password_utf16 data as bytes.
```

#### 4.8.4 Related: connection string parsing for non-ASCII

Also verify:

- `ConnectionString::Parse` preserves UTF-8 bytes through splitting
  on `;` and `=`. No truncation at high bytes.
- URI format (`mssql://...`) URL-decodes `%XX` escapes correctly
  before UTF-8 interpretation.
- ADO.NET format respects `{...}` quoting for passwords containing
  special characters.

Add tests:

```
mssql://user:%D0%9F%D0%B0%D1%80%D0%BE%D0%BB%D1%8C@host/db  # "Пароль"
Server=host;User Id=u;Password={p@ss;word with semicolon}
```

---


---

## Coordination notes

- **Spec 042 (Oluies, integrated authentication)** is refactoring
  `src/tds/auth/` and adding new strategies (`Krb5Authenticator`,
  `WinSspiAuthenticator`). The password fix in this spec is a
  one-line-scale localized patch in `sql_auth_strategy.cpp` —
  coordinate merge order so that whichever lands second rebases
  cleanly on the other.
- **Spec 044 (Codec Layer)** consumes the simdutf wrapper
  introduced here. Until 044 lands, the legacy `utf16.cpp`
  call sites continue using the hand-rolled converter; 044
  switches all of them (string codec, password encoding,
  PRELOGIN server-name encoding, etc.) over to the shared
  utility. This means 043 introduces the wrapper but does not
  yet migrate any call site.

## Acceptance criteria (high level)

1. `simdutf` is added as a vcpkg dependency, builds clean on
   Linux/macOS/Windows CI runners.
2. `src/tds/encoding/utf_conversion.hpp` (or equivalent agreed
   location) exposes `Utf8ToUtf16LE` and `Utf16LEToUtf8` with
   signatures compatible with the existing hand-rolled converter.
3. Round-trip unit tests for UTF-8 ↔ UTF-16LE cover: ASCII,
   BMP-only multi-byte UTF-8, non-BMP (4-byte UTF-8 / surrogate
   pair UTF-16), invalid sequences, empty input. Both the new
   simdutf path and the legacy path run the same fixture set
   and produce identical output.
4. Non-ASCII LOGIN7 passwords (cyrillic, accented Latin, CJK)
   succeed against a Docker SQL Server instance. The integration
   test reproducing the v0.1.18 failure now passes.
5. No regression in existing scan/BCP/DML tests.

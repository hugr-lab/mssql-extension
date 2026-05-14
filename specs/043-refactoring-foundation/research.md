# Phase 0 Research: LOGIN7 Non-ASCII Fix + simdutf Foundation

**Feature**: 043-refactoring-foundation
**Date**: 2026-05-14

This document resolves the technical unknowns flagged in `plan.md`
and records the decisions, rationale, and rejected alternatives.

## R1. simdutf vcpkg port availability at the current baseline

**Decision**: Use the simdutf port from vcpkg. If the project's
current pinned baseline
(`5ee5eee0d3e9c6098b24d263e9099edcdcef6631`) does not publish
`simdutf` across all four required triplets (`x64-linux-static`,
`x64-osx-static`, `x64-windows-static-release`,
`x64-mingw-static`), bump the baseline forward to the smallest
newer commit that does. The bump is a single-line edit to
`vcpkg.json`'s `builtin-baseline` field and a smoke build on each
platform.

**Rationale**:
- `simdutf` has been an official vcpkg port since 2021 and is
  widely available across triplets.
- Bumping the baseline forward is the project's standard practice
  for picking up new ports; it does not invalidate the existing
  OpenSSL override.
- Vendoring simdutf source into the repo (as the original
  `feature-spec/refactoring-foundation-043.md` document
  hypothetically suggested via DuckDB's submodule) is rejected:
  DuckDB does not expose simdutf headers to extensions, and
  carrying our own copy duplicates upstream maintenance.

**Alternatives considered**:
- Vendor simdutf source as a git submodule. Rejected: doubles
  third-party maintenance surface and breaks consistency with how
  all other C++ deps (OpenSSL, cpp-httplib) are handled.
- Depend on DuckDB's bundled simdutf (if any). Rejected: not
  exposed to extension code at the header level; relying on
  internal DuckDB symbols is fragile across DuckDB versions.

**Implementation note**: The verification at implementation time
is a `vcpkg install simdutf` on each triplet plus a CMake
`find_package(simdutf CONFIG REQUIRED)` smoke test before any
production code touches the dependency.

## R2. simdutf API entry points and invalid-input contract

**Decision**: The wrapper layer uses three simdutf entry points
per direction:

| Direction | Validate | Convert (fast) | Length |
|-----------|----------|----------------|--------|
| UTF-8 → UTF-16LE | `simdutf::validate_utf8(data, len)` | `simdutf::convert_valid_utf8_to_utf16le(in, len, out)` | `simdutf::utf16_length_from_utf8(in, len)` |
| UTF-16LE → UTF-8 | `simdutf::validate_utf16le(data, len)` | `simdutf::convert_valid_utf16le_to_utf8(in, len, out)` | `simdutf::utf8_length_from_utf16le(in, len)` |

Behavior on validation failure (per Clarification Q1, spec §
Clarifications, Session 2026-05-14): fall back to the legacy
hand-rolled converter (`duckdb::tds::encoding::Utf16LEEncode`
etc.) for that single input. The wrapper is a single boundary
point.

**Rationale**:
- `convert_valid_*` is the SIMD fast path. Skipping pre-validation
  and using `convert_*` (non-`valid`) requires simdutf to also
  validate — slower and less clear about error semantics.
- Pre-validate + dispatch is a 5-line function. Cheap.
- The fallback path is exercised only on input that fails strict
  UTF-8 validation; in practice, every caller already has UTF-8.
  The legacy converter retains its current observed semantics
  (skip invalid bytes, continue) for that edge case.

**Alternatives considered**:
- `convert_utf8_to_utf16le_with_errors`: returns error position.
  Rejected per Q1: produces truncation, not the legacy "skip and
  continue" behavior.
- `convert_utf8_to_utf16le` (non-`valid`): does internal
  validation, returns 0 on invalid. Rejected: no fallback to
  legacy, so spec 044's mass migration would silently change
  semantics for any consumer.

**Header surface** (see `contracts/simdutf_wrappers.hpp`):

```cpp
namespace duckdb::tds::encoding {

// Drop-in replacements for the legacy free functions in utf16.hpp.
// Symbols are prefixed Simdutf* to coexist with the legacy versions
// during spec 044's migration.

std::vector<uint8_t> SimdutfUtf16LEEncode(const std::string &input);
std::string SimdutfUtf16LEDecode(const uint8_t *data, size_t byte_length);
std::string SimdutfUtf16LEDecode(const std::vector<uint8_t> &data);
size_t SimdutfUtf16LEByteLength(const std::string &input);
size_t SimdutfUtf16LEEncodeDirect(const char *input, size_t input_len, uint8_t *output);

}  // namespace duckdb::tds::encoding
```

## R3. simdutf C++11/ODR compatibility verification

**Decision**: simdutf's public headers compile in C++11 mode. The
wrapper translation unit (`simdutf_wrappers.cpp`) will be compiled
with whatever standard CMake selects for the extension (currently
DuckDB's default — C++11 on Linux for ODR safety per CLAUDE.md).
The wrapper does **not** add `target_compile_features(cxx_std_*)`.

**Rationale**:
- simdutf upstream advertises C++11 as the minimum. CI smoke
  tests during implementation will confirm by compiling
  `simdutf_wrappers.cpp` in `-std=c++11` mode.
- simdutf is a static library compiled with its own internal C++
  standard inside vcpkg's port. ODR concerns are limited to
  symbols exposed via the public headers — simdutf exposes
  inline functions and constexpr in a separate namespace
  (`simdutf::`), which does not collide with DuckDB's
  `LogicalType::BIGINT`-style ODR landmines.
- If a future simdutf version drops C++11 support, the project
  can either pin the last C++11-compatible vcpkg port version or
  raise the project's own minimum C++ standard at that time. Not
  a 043 concern.

**Verification at implementation time**: As soon as
`simdutf_wrappers.cpp` is added, build with
`GEN=ninja make debug` on Linux and confirm:
1. No "multiple definition of `LogicalType::BIGINT`"-style
   errors.
2. No "this feature requires C++14" / `constexpr if` errors.
3. The loadable extension links cleanly.

**Alternatives considered**:
- Force the extension to C++17 to take advantage of simdutf's
  newer features. Rejected: violates the ODR constraint
  documented in CLAUDE.md ("DO NOT USE: causes ODR errors on
  Linux when DuckDB is C++11").

## R4. Static linking simdutf on all four target platforms

**Decision**: simdutf links statically on all four CI platforms
without explicit flags. The project's existing vcpkg static
triplets produce static archives (`.a` / `.lib`) for every
dependency by default; `find_package(simdutf CONFIG REQUIRED)`
locates the static library through vcpkg's exported config and
`target_link_libraries(... simdutf::simdutf)` consumes it. No
shared-library `.so`/`.dylib`/`.dll` enters the loadable
extension.

**Rationale**:
- Same path the project already takes for OpenSSL.
- Verified post-link via `ldd build/release/extension/.../mssql_loadable_extension.duckdb_extension`
  (Linux), `otool -L` (macOS), Dependency Walker (Windows). Any
  unexpected dynamic dependency is a CI failure.

**Alternatives considered**:
- Dynamic link (using `x64-linux-dynamic` etc.). Rejected:
  contradicts the project's static-distribution model and would
  force end-users to install simdutf on their system.

## R5. Existing `UrlDecode` malformed-escape semantics

**Findings** (from reading `src/mssql_storage.cpp:163-178`):

The current implementation uses `sscanf("%x", &hex_val)` on the
two characters following `%`. Observed behavior:

| Input | Current behavior | Per spec FR-011 we want |
|-------|------------------|--------------------------|
| `%41` (= `A`) | byte 0x41 ("A") | byte 0x41 — unchanged |
| `%D0` (= `Р` first byte) | byte 0xD0 | byte 0xD0 — unchanged |
| `%GG` (no hex chars) | literal `%`, then `G`, then `G` (sscanf returns 0, fallthrough) | literal pass-through — already correct |
| `%aG` (one hex char then non-hex) | byte 0x0a, `G` silently dropped | **BUG**: sscanf reads `a` (0x0a), returns 1; code advances `i+=2`, consuming `G` |
| `%` at end of string | literal `%` (fails `i+2 < size`) | literal pass-through — already correct |
| `%G` at end (one char after) | literal `%`, `G` (fails `i+2 < size`) | literal pass-through — already correct |
| `%2 ` (hex digit + space) | byte 0x02, space dropped | **BUG**: same class as `%aG` |

**Decision**: Per Clarification (FR-011 chose deterministic
literal pass-through), `UrlDecode` is replaced with a stricter
implementation that **requires both characters after `%` to be
hex digits**. The replacement uses a manual `IsHex(c)` check, not
`sscanf`. Behavior contract documented in a code comment on the
function.

Pseudocode:

```cpp
static string UrlDecode(const string &str) {
    string result;
    result.reserve(str.size());
    for (size_t i = 0; i < str.size(); ) {
        if (str[i] == '%' && i + 2 < str.size()
                && IsHex(str[i+1]) && IsHex(str[i+2])) {
            result += static_cast<char>((HexVal(str[i+1]) << 4) | HexVal(str[i+2]));
            i += 3;
        } else {
            result += str[i];
            i += 1;
        }
    }
    return result;
}
```

**Rationale**: Eliminates the `%aG` silent-drop class of bugs.
Pure-ASCII URIs are unaffected because their `%XX` sequences are
always two hex digits. Cyrillic/CJK URIs with proper
percent-encoding (the only correct way to express non-ASCII in a
URI) are unaffected.

**Tests** (FR-021): cover `%41`, `%D0%9F`, `%GG`, `%aG`, `%`,
`%2`, `%20`, `%2B`, `%%`, mixed-case `%dA`.

## R6. `ParseConnectionString` `{...}` quoting

**Findings** (from reading `src/mssql_storage.cpp:258-306`): no
`{...}` quoting today. `auto parts = StringUtil::Split(str, ';')`
splits unconditionally. A password like `{p@ss;word}` is split
into `[..., "Password={p@ss", "word}", ...]`.

**Decision**: Introduce minimal `{...}` quoting per FR-012. The
parse loop walks the string char-by-char tracking a
"`inside_braces`" flag:

- On `{` immediately after `=` (start of value), set
  `inside_braces = true`. Strip the leading `{`.
- Inside braces:
  - `}}` (two consecutive `}`) → emit a literal `}` and continue
    inside braces.
  - Lone `}` → end of value, `inside_braces = false`. Strip the
    trailing `}`.
  - Any other char (including `;`, `=`, raw `{`) → literal.
- Outside braces: `;` separates pairs (current behavior).

`"..."` and `'...'` quoting are **out of scope** for spec 043;
documented in FR-012 and Out-of-Scope. They can be added in a
follow-up if user feedback requires it.

**Rationale**:
- `{...}` is the canonical SQL Server ODBC/OLE DB / DuckDB
  extension quoting style; it's what users coming from
  on-prem SQL Server expect.
- `"..."` and `'...'` are .NET-flavored; lower demand here, and
  adding them later is non-breaking.

**Tests** (FR-022): `Password={a;b}` → value `a;b`;
`Password={a}}b}` → value `a}b`; non-ASCII `Password={Тест;123}`
→ value `Тест;123`; balanced empty braces `Password={}` → empty
value.

## R7. No locale-dependent narrowing in the parser path

**Findings** (audit of `src/mssql_storage.cpp` lines 1-400):
- No `mbstowcs`, `wcrtomb`, `wcstombs`, `_mbstowcs_s`,
  `std::wstring_convert`, or `std::setlocale` calls in the
  parse path.
- `StringUtil::Lower` and `StringUtil::Split` are DuckDB
  utilities operating on `std::string` bytes — locale-
  independent.
- `sscanf("%x")` is locale-dependent in theory; in practice on
  every glibc / macOS libc / MSVC CRT we ship against, hex
  parsing is invariant. Still, the replacement implementation
  in R5 removes the dependency entirely.

**Decision**: The audit (FR-013) is **satisfied by code review
only** — no code changes required beyond R5's `UrlDecode`
replacement. A unit test sets `LC_ALL=C` and a non-`C` locale
(if available in CI) and asserts identical parse output
(FR-022).

## R8. T-SQL recipe for non-ASCII login creation

**Decision**: Integration tests create the non-ASCII-password
login via T-SQL at the start of the test run using the existing
`sa` connection. Per SQL Server documentation, `CREATE LOGIN`
supports Unicode passwords via the `N'...'` prefix:

```sql
CREATE LOGIN [user_ru]
    WITH PASSWORD = N'Тест123!',
    CHECK_POLICY = OFF,
    CHECK_EXPIRATION = OFF;

GRANT CONNECT SQL TO [user_ru];
ALTER SERVER ROLE db_datareader ADD MEMBER [user_ru];  -- or USE [TestDB]; CREATE USER ...
```

Policy and expiration are disabled to keep the Docker SQL Server
test image's default password policy from rejecting short test
passwords.

The test file uses a SQLLogicTest preamble that opens an `sa`
attach, runs the setup, then opens the attach under the new
login.

**Rationale**: Matches the existing integration-test convention
in `test/sql/`. The setup is idempotent (`IF NOT EXISTS`-style
gating around `CREATE LOGIN`) so re-runs succeed.

**Cleanup**: A `teardown` SQL block drops the login at the end of
the test, but is safe to omit on a fresh Docker container which
the CI tears down anyway.

## R9. The LOGIN7 `Login7VarField` helper shape

**Decision**: New helper, file-scope static (or anonymous
namespace) inside `tds_protocol.cpp`. Signature:

```cpp
// Encode one variable LOGIN7 string field. Returns nothing because
// the caller already knows where it placed the field; the helper
// mutates the output buffer in place.
//
// `cch_out` receives the UTF-16 code-unit count (for the LOGIN7
// fixed-header cch* field).
// `ib_offset` is updated to point just past the encoded field.
//
// On overflow (> 128 UTF-16 code units) throws IOException with
// the field name and observed length.
struct Login7VarFieldResult {
    std::vector<uint8_t> utf16le_bytes;
    uint16_t cch;
    uint16_t ib;  // offset assigned to this field
};

static Login7VarFieldResult EncodeLogin7VarField(
    const char *field_name,            // for error message
    const std::string &utf8_text,
    uint16_t &cumulative_ib_offset,    // in/out
    bool obfuscate_password = false);  // true only for the password field
```

The helper:
1. Calls `SimdutfUtf16LEEncode(utf8_text)` (the new wrapper).
2. Asserts byte length / 2 ≤ 128 — else throws `IOException`
   matching the exact wording in FR-008.
3. If `obfuscate_password`, applies the existing
   `TdsProtocol::EncodePassword` obfuscation (nibble swap + XOR
   `0xA5`) to the encoded bytes.
4. Returns the encoded bytes plus the `cch` and the assigned
   `ib`. Advances `cumulative_ib_offset` by the encoded byte
   length.

The three `BuildLogin7*` functions each call this helper once
per variable field, then emit the resulting `ib`/`cch` pairs
into the fixed header and the bytes into the variable region.

**Rationale**: Single source of truth for "encode + measure +
validate + obfuscate (for password) + advance offset" — exactly
the contract the spec's Key Entities section requires.

## R10. ASCII regression-test comparison strategy

**Decision** (per Clarification Q3): The test in
`test_login7_encoding.cpp` constructs the LOGIN7 packet for a
fixed ASCII fixture and compares only the **variable-data
region** — bytes from offset 94 onward, plus the variable-field
`ib*`/`cch*` pairs in the fixed header (offsets 36–80 by MS-TDS
§2.2.6.4 layout). ClientPID, ClientID, ClientLCID, ClientProgVer,
ClientTimeZone, and TDS-version constants in the fixed header are
explicitly excluded from the comparison.

**Implementation sketch**:

```cpp
struct Login7Region {
    const uint8_t *bytes;
    size_t length;
};

// Region 1: variable-field offset/length pairs in the fixed header.
//   Offsets 36..86 (HostName..ChangePassword), each 4 bytes.
// Region 2: variable-data region.
//   Offset 94 onward.

void AssertLogin7VariableRegionsEqual(
    const std::vector<uint8_t> &actual,
    const std::vector<uint8_t> &expected);
```

The expected bytes are produced once from a hardcoded ASCII
fixture and committed alongside the test as a hex-string
constant.

## R11. Spec 042 (collaborator) coordination

**Decision**: Land spec 043's changes only in
`src/tds/tds_protocol.cpp` (the LOGIN7 builders) and
`src/mssql_storage.cpp` (the connection-string parser). Do
**not** touch `src/tds/auth/*` — that is spec 042's territory.
The PR description for spec 043 explicitly calls out the
coordination: whichever spec lands second performs a small
rebase of the LOGIN7 builder call sites if 042 changed the
LOGIN7-options struct shape.

**Rationale**: Minimizes merge conflict surface. Both specs
ultimately produce LOGIN7 packets but through different code
paths (042 changes which builder is called; 043 fixes how each
builder encodes variable fields). Conflicts will be in
call-arguments, not in the fix logic.

## Summary of resolved unknowns

| # | Topic | Decision summary |
|---|-------|------------------|
| R1 | vcpkg port | Use vcpkg `simdutf`; bump baseline forward if needed. |
| R2 | simdutf API | `validate_utf8` + `convert_valid_*`; fall back to legacy on invalid. |
| R3 | C++11/ODR | simdutf headers are C++11-compatible. Do not add `cxx_std_*` to extension target. |
| R4 | Static link | Existing vcpkg static triplets do this automatically. |
| R5 | UrlDecode | Replace `sscanf` with manual hex check; require both chars hex. |
| R6 | ADO.NET `{...}` | Add minimal `{...}` quoting with `}}` escape. No `"..."` / `'...'`. |
| R7 | Locale safety | Audit confirms no narrowing APIs. R5 removes the only locale-touching call. |
| R8 | Test login | `CREATE LOGIN ... WITH PASSWORD = N'...'` against Docker `sa`. |
| R9 | Helper shape | `EncodeLogin7VarField` returning `{bytes, cch, ib}`; one helper, three callers. |
| R10 | Regression test | Compare variable-data region + variable `ib*`/`cch*` pairs only. |
| R11 | Spec 042 | Coordinate via PR description; whichever lands second rebases. |

All NEEDS CLARIFICATION items from the spec are resolved. Plan
proceeds to Phase 1.

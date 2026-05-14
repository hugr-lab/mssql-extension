# Feature Specification: LOGIN7 Non-ASCII Fix + simdutf Foundation

**Feature Branch**: `043-refactoring-foundation`

**Created**: 2026-05-14

**Status**: Implemented (pending PR)

**Input**: User description: "Take the foundation refactoring batch from `feature-spec/refactoring-foundation-043.md` as spec 043 (042 is reserved by a collaborator). Treat the source document critically — the structure is right but content must be re-grounded in the actual codebase. Install simdutf statically as a foundation dependency, and start using it from the LOGIN7 fix as the first consumer; the broader migration of NVARCHAR / BCP / PLP hot paths to simdutf stays in spec 044 (Codec Layer). Also audit non-ASCII handling in the connection-string parser. All comments and documentation in English only."

## Overview

This spec is the smallest landable foundation slice of the v0.2.0
refactor (full context: `feature-spec/refactoring-v0.2.md`). It does
three things:

1. **Fix the LOGIN7 length-counting bug** — every variable-length
   string field in the LOGIN7 fixed header (`HostName`, `UserName`,
   `Password`, `AppName`, `ServerName`, `Database`) currently records
   the UTF-8 byte length of the input as if it were the UTF-16
   code-unit count, and computes byte offsets as `utf8_size * 2`.
   For ASCII inputs these two numbers coincide; for any non-ASCII
   input the `cch*` value is wrong and every subsequent field's
   `ib*` offset is misaligned, surfacing as `Login failed for user`.
2. **Audit non-ASCII handling in the connection-string parser** —
   verify that all three accepted credential formats (URI, ADO.NET
   key/value, DuckDB secret) carry non-ASCII bytes intact from user
   input to the LOGIN7 builder, with correct percent-decoding,
   `{...}` quoting, and no locale narrowing.
3. **Add simdutf as a foundation dependency** — install simdutf via
   vcpkg, link it statically across all platforms, and expose a
   thin in-project wrapper. The LOGIN7 fix is the **first and only
   consumer** in this spec. The broader migration of UTF-16 ↔ UTF-8
   call sites (catalog scan, NVARCHAR decode/encode, BCP, PLP) is
   explicitly deferred to **spec 044 (Codec Layer)**.

The split keeps spec 043 small but lands a complete dependency story
end-to-end: the new library is installed, statically linked, and
actually called from production code, not just sitting in CMake
unused. That gives spec 044 a safe foundation to consume in bulk
without re-litigating the build setup.

## Clarifications

### Session 2026-05-14

- Q: simdutf wrapper behavior on invalid UTF-8 input → A: Pre-validate
  with `simdutf::validate_utf8`. Valid input uses the fast
  `convert_valid_utf8_to_utf16le` path. Invalid input falls back to
  the legacy hand-rolled converter for that string, preserving v0.1.18
  bug-for-bug behavior (skip invalid bytes, continue). This locks the
  invalid-input contract so spec 044's mass migration cannot silently
  change semantics for any existing consumer.
- Q: Where do we enforce the TDS 128-UTF-16-code-unit cap on LOGIN7
  variable fields, and what does the user see? → A: Inside the LOGIN7
  builder helper (`Login7VarField`), at the point where UTF-16
  conversion happens. On overflow, throw an `IOException` of the form
  `"LOGIN7 field <name> exceeds the TDS limit of 128 UTF-16 code
  units (got <N>)"` which surfaces to the user at `ATTACH`/connect
  time. Applies to all six variable fields. The connection-string
  parser does not perform this check.
- Q: What does "bitwise identical to v0.1.18" mean for the ASCII
  regression test, given that ClientPID and similar environmental
  fields vary between runs? → A: The comparison covers only the
  **variable-data region** of the LOGIN7 packet — bytes from offset
  94 onward, plus the variable-field offset/length pairs (`ib*` /
  `cch*`) in the fixed header. The environmental fields (ClientPID,
  ClientID, TDS-version constants) are excluded by construction.
  FR-007 and SC-003 are updated accordingly.

## User Scenarios & Testing *(mandatory)*

### User Story 1 — Connect to SQL Server with a non-ASCII password (Priority: P1)

A user stores SQL Server credentials in a DuckDB secret, an ADO.NET
key/value connection string, or an `mssql://` URI. Their password
contains non-ASCII characters (Cyrillic, accented Latin, CJK, or an
emoji that needs a UTF-16 surrogate pair). Today, against v0.1.18,
the connection fails with `Login failed for user '<name>'` from SQL
Server. After this spec lands, the connection succeeds and queries
run identically to an ASCII-password connection.

**Why this priority**: This is the only user-visible defect in the
batch. Without it, any organization with a password policy that
mixes alphabets — common in Cyrillic, Western European, and CJK
locales — cannot use the extension at all.

**Independent Test**: Integration test
`test/sql/integration/non_ascii_password.test` against the project's
Docker SQL Server container. Setup creates a login with a non-ASCII
password via the existing `sa` connection; the test then attaches as
that user and runs `SELECT 1`. Against the pre-fix binary the test
fails at ATTACH; against the fixed binary it passes.

**Acceptance Scenarios**:

1. **Given** a SQL Server login `user_ru` with password `"Тест123!"`
   (Cyrillic, 11 UTF-8 bytes / 8 UTF-16 code units), **When** the
   extension authenticates via TDS LOGIN7, **Then** authentication
   succeeds and `SELECT 1` returns `1`.
2. **Given** a SQL Server login with password `"Ünlaut$2024"`
   (accented Latin), **When** the extension authenticates,
   **Then** authentication succeeds.
3. **Given** a SQL Server login with password `"🔒secure!"` (emoji
   surrogate pair), **When** the extension authenticates, **Then**
   authentication succeeds.
4. **Given** an ASCII-only password `"TestPassword1"`, **When** the
   extension authenticates, **Then** the LOGIN7 packet's
   variable-data region (offset 94 onward) and variable-field
   `ib*`/`cch*` pairs in the fixed header are bitwise identical to
   the pre-fix output, and authentication succeeds. (Environmental
   fixed-header fields — ClientPID, ClientID, TDS-version constants
   — are excluded from the comparison, per FR-007 and Clarification
   Q3.)

---

### User Story 2 — Authenticate with non-ASCII database, username, or app name (Priority: P1)

Beyond passwords, the same length-counting bug corrupts every LOGIN7
variable field. Users connecting to a database whose name contains
non-ASCII characters, or logging in with a non-ASCII username
(Azure SQL allows email-style usernames; some on-prem deployments
use localized identifiers), should authenticate cleanly.

**Why this priority**: Same root cause as Story 1, mechanically
fixed in the same patch. Splitting the fix to "password only" would
leave a fragile builder that still mis-encodes other fields. P1
because the cost of fixing all six fields together is essentially
zero on top of fixing password.

**Independent Test**: C++ unit test
`test/cpp/test_login7_encoding.cpp` builds a LOGIN7 packet for each
variable field set to a representative non-ASCII string, then parses
the bytes back: verify `cch*` equals the expected UTF-16 code-unit
count, `ib*` offsets advance correctly, and the decoded payload
matches the input.

**Acceptance Scenarios**:

1. **Given** a database name `"База"` (Cyrillic, 8 UTF-8 bytes / 4
   UTF-16 code units), **When** the extension issues LOGIN7,
   **Then** `cchDatabase = 4` and `ibDatabase` points to a 8-byte
   UTF-16LE-encoded `"База"`.
2. **Given** a username `"jürgen"`, **When** the extension issues
   LOGIN7, **Then** authentication succeeds against a SQL Server
   login created with that exact name.
3. **Given** every variable LOGIN7 field set to ASCII, **When**
   the packet is built, **Then** the variable-data region and the
   variable-field `ib*`/`cch*` pairs are bitwise identical to
   v0.1.18 — zero regression bytes in the region the fix touches.
   (Environmental fixed-header fields excluded by construction,
   per FR-007 and Clarification Q3.)

---

### User Story 3 — Round-trip non-ASCII credentials through every connection-string format (Priority: P2)

The extension accepts credentials in three formats: `mssql://` URI
(with percent-encoding), ADO.NET-style key/value
(`Server=...;User Id=...;Password=...`), and DuckDB secret API.
A user should be able to express the same non-ASCII password in any
of the three and get an identical, correct LOGIN7 packet.

**Why this priority**: Without this audit the LOGIN7 fix is half a
fix. If `ParseUri`'s `UrlDecode` truncates or mis-handles `%XX`
escapes for high bytes, the user's password never reaches the
LOGIN7 builder unchanged. If `ParseConnectionString` does not honor
`{...}` quoting, a password containing `;` (whether ASCII or not)
is silently split and the wrong bytes go to LOGIN7. P2 because the
most common form (DuckDB secret) already passes UTF-8 through
unchanged; the URI and ADO.NET paths need explicit verification.

**Independent Test**: SQL test
`test/sql/integration/non_ascii_connection_formats.test` plus C++
unit tests on `ParseUri` / `ParseConnectionString` covering:

- URI percent-encoded Cyrillic:
  `mssql://user:%D0%9F%D0%B0%D1%80%D0%BE%D0%BB%D1%8C@host/db`
  yields `password = "Пароль"`.
- ADO.NET with `{...}`-quoted value containing `;`:
  `Server=host;User Id=u;Password={p@ss;word with semicolon}`
  yields `password = "p@ss;word with semicolon"`,
  not `"{p@ss"`.
- ADO.NET with non-ASCII value:
  `Server=host;User Id=u;Password=Тест123!` yields exactly the
  UTF-8 bytes for `"Тест123!"`.
- DuckDB secret with non-ASCII password set via
  `CREATE SECRET ... (TYPE mssql, password 'Тест123!', ...)` yields
  the same UTF-8 bytes.

**Acceptance Scenarios**:

1. **Given** a non-ASCII password expressed in each of the three
   formats, **When** the extension reaches the LOGIN7 builder,
   **Then** the password byte sequence handed to the builder is
   identical (UTF-8) for all three formats.
2. **Given** an ADO.NET value `Password={Тест;123}` (braces around
   a value containing `;` and non-ASCII), **When** parsed,
   **Then** the password equals `"Тест;123"` and the next
   `Server=...` segment is parsed correctly.
3. **Given** a URI with a malformed `%` escape (`%G1`), **When**
   parsed, **Then** the parser produces deterministic behavior:
   either a clear error or literal pass-through (chosen behavior
   documented in code and asserted by test).
4. **Given** any system locale (`LC_ALL=C`, `LC_ALL=en_US.UTF-8`,
   `LC_ALL=ru_RU.UTF-8`), **When** the extension parses a
   connection string with non-ASCII bytes, **Then** the parsed
   bytes are identical across locales.

---

### User Story 4 — simdutf foundation installed and exercised in production (Priority: P2)

A developer working on spec 044 (Codec Layer) needs simdutf already
present in the build, statically linked on every platform, and
called from at least one production code path so that the
integration is proven end-to-end before bulk migration begins. The
LOGIN7 length fix uses simdutf as its UTF-8 → UTF-16LE conversion
primitive — that's the foothold. The legacy hand-rolled converter
in `src/tds/encoding/utf16.cpp` stays in place and continues to
serve every other call site until spec 044 migrates them.

**Why this priority**: Pure scaffolding with one production
consumer. The end-user benefit (3–10× UTF-16 throughput) lands in
spec 044 when NVARCHAR scan / BCP encode / PLP streaming move to
simdutf. P2 because spec 043's user-visible win is the LOGIN7 fix,
not this scaffolding — but landing the dependency in 043 (instead
of 044) lets the larger 044 PR focus exclusively on call-site
migration without re-litigating vcpkg / CMake / static-linking.

**Independent Test**: Build the extension on all CI platforms with
the new vcpkg dependency, run the existing test suite, then run a
new C++ unit test that asserts the LOGIN7 builder's UTF-16 output
matches both simdutf's output and the legacy converter's output for
a shared fixture set.

**Acceptance Scenarios**:

1. **Given** `vcpkg.json` lists `"simdutf"`, **When** the project
   is built on Linux (GCC), macOS (Clang/AppleClang), and Windows
   (MSVC, MinGW-Rtools), **Then** every platform builds cleanly
   with no platform-specific source forks, and simdutf is linked
   statically (no new runtime `.so`/`.dylib`/`.dll` dependency on
   the final binary).
3. **Given** the build is complete, **When** `mssql_version()` is
   invoked and the LOGIN7 builder is exercised by an attach with
   a non-ASCII password, **Then** the conversion is performed by
   simdutf (verified by symbol presence and by unit-test
   instrumentation that checks the active conversion path).
4. **Given** the same UTF-8 input across the simdutf wrapper and
   the legacy `Utf16LEEncode` converter, **When** both are
   invoked, **Then** for all valid-UTF-8 inputs the output byte
   sequences are bitwise identical. (Cross-check that simdutf
   produces the same bytes the existing code already produces, so
   nothing downstream breaks when 044 migrates the rest.)

---

### Edge Cases

- **Empty password**: `cchPassword = 0`, no bytes written. Existing
  code handles this; fix must not regress.
- **Surrogate-pair characters**: 4 UTF-8 bytes → 2 UTF-16 code units
  → `cch += 2`, `ib += 4`. Confirms the fix is "convert first, then
  measure" not "double UTF-8 size".
- **LOGIN7 field at TDS upper bound**: MS-TDS caps each variable
  string field at 128 UTF-16 code units (256 bytes). When the user
  supplies a longer non-ASCII string that fits in UTF-8 but
  exceeds 128 UTF-16 code units after conversion, the LOGIN7
  builder helper rejects with an `IOException` naming the field
  and the observed length. Surfaces to the user at
  `ATTACH`/connect time, not at parse time.
- **ASCII fast path**: for ASCII-only inputs the fix must produce a
  bitwise-identical LOGIN7 packet. Both simdutf and the legacy
  converter must agree on ASCII output (they do — both emit
  byte-then-zero pairs in the BMP).
- **Connection string with a `;` inside a `{...}`-quoted value**:
  current `ParseConnectionString` (`src/mssql_storage.cpp:258`)
  splits on raw `;` and drops the rest of the value. After audit,
  brace-quoted values pass through unsplit.
- **URI percent-decode with high bytes** (`%D0%9F`): must produce
  the raw bytes 0xD0 0x9F (UTF-8 prefix for `П`), not an error or a
  Latin-1 transcoded character.
- **URI percent-decode with malformed escape** (`%G1`, `%`, `%2`):
  current `UrlDecode` (`src/mssql_storage.cpp:163`) behavior must
  be reviewed and made deterministic — either pass through
  literally or return an error; pick one and document it.
- **Locale-dependent narrowing**: any use of `mbstowcs`, `wcrtomb`,
  `wcstombs`, `setlocale`, `std::wstring_convert`, or `_mbstowcs_s`
  in the parse path is a defect; non-ASCII must travel as raw UTF-8
  bytes regardless of `LC_*`.
- **TLS-encrypted LOGIN7**: with `encrypt=true` (default against
  modern SQL Server), LOGIN7 is wrapped in TLS. The fix applies to
  the plaintext packet before TLS framing, so this is transparent.
  Integration tests must cover both encrypted and unencrypted
  paths.
- **simdutf vcpkg port unavailable on a CI triplet**: if a triplet
  is missing the port at the project's pinned vcpkg baseline, the
  build MUST fail loudly at CMake configure time (via
  `find_package(simdutf CONFIG REQUIRED)`), not silently fall back
  to the legacy converter. The fix: pin a newer vcpkg baseline as
  part of this spec.
- **simdutf invalid-input behavior**: simdutf has well-defined
  validation APIs (`validate_utf8`, `validate_utf16le`) and
  `convert_*` variants that either fail-fast or replace invalid
  sequences with `U+FFFD`. The LOGIN7 fix path may assume the
  connection-string parser has already produced valid UTF-8;
  even so, the wrapper MUST never throw on bad input.

## Requirements *(mandatory)*

### Functional Requirements

**LOGIN7 length-counting fix**

- **FR-001**: For every variable-length LOGIN7 string field in
  `src/tds/tds_protocol.cpp` (`HostName`, `UserName`, `Password`,
  `AppName`, `ServerName`, `Database`), the `cch*` value written
  into the LOGIN7 fixed header MUST equal the **number of UTF-16
  code units** in the UTF-16LE encoding of the input. It MUST NOT
  be the UTF-8 byte count of the input.
- **FR-002**: For every variable-length LOGIN7 string field, the
  cumulative `ib*` offset advance MUST be the **byte length of
  the UTF-16LE encoded form** of the previous field. It MUST NOT
  be `utf8.size() * 2`.
- **FR-003**: The TDS password obfuscation (nibble-swap then XOR
  with `0xA5`, per MS-TDS §2.2.6.4, implemented in
  `TdsProtocol::EncodePassword`) MUST continue to apply to the
  encoded UTF-16LE bytes after the length-counting fix. The
  obfuscation routine MUST NOT be changed beyond what the length
  fix requires.
- **FR-004**: The fix MUST apply to every LOGIN7 builder in
  `src/tds/tds_protocol.cpp`: `BuildLogin7`,
  `BuildLogin7WithFedAuth`, `BuildLogin7WithADAL`. Any LOGIN7
  helper added by spec 042 (collaborator) must inherit the same
  correct behavior; coordinate via rebase.
- **FR-005**: The fix MUST be implemented as a localized patch.
  Acceptable shape: a small helper `Login7VarField` (or similar)
  that takes UTF-8 text plus a reference to the running
  `ib_offset`, performs UTF-16LE encoding once (via the simdutf
  wrapper from FR-030+), returns `{utf16_bytes, cch, ib}`, and is
  invoked once per variable field per builder. No auth-strategy
  refactor (that is spec 042's territory).
- **FR-006**: The fix MUST preserve the existing TDS_VERSION_7_4
  fixed-header layout: same 94-byte header, same offset/length
  pair ordering, same trailing fields. Only the encoded payload
  and the `cch*` / `ib*` numbers change for non-ASCII inputs.
- **FR-007**: For ASCII-only inputs the produced LOGIN7 packet
  MUST be bitwise identical to v0.1.18's output **in the
  variable-data region** — bytes from offset 94 onward, plus the
  variable-field offset/length pairs (`ib*` / `cch*`) inside the
  fixed header. Environmental fixed-header fields (ClientPID at
  offset 16, ClientID/MAC at offset 36+24, TDS-version constants,
  ClientLCID, ClientProgVer, ClientTimeZone) are explicitly
  excluded from the comparison. Asserted by a fixed-fixture
  comparison test.
- **FR-008**: The LOGIN7 builder helper MUST validate that every
  variable string field's encoded length does not exceed 128
  UTF-16 code units (the TDS protocol cap). If any of the six
  variable fields exceeds this cap, the helper MUST throw an
  `IOException` whose message identifies the offending field by
  name and the observed UTF-16 code-unit count, e.g.
  `"LOGIN7 field Password exceeds the TDS limit of 128 UTF-16
  code units (got 142)"`. The connection-string parser MUST NOT
  perform this check (defense-in-depth duplication is rejected
  to keep the parser TDS-agnostic).

**Connection-string non-ASCII audit**

- **FR-010**: `ParseUri` (`src/mssql_storage.cpp:181`) MUST
  percent-decode `%XX` sequences as raw bytes and return UTF-8
  byte sequences for all extracted fields (user, password,
  database, query parameters). High bytes (`%80`–`%FF`) MUST
  pass through unchanged.
- **FR-011**: `UrlDecode` (`src/mssql_storage.cpp:163`) MUST
  handle malformed escapes (`%G1`, trailing `%`, `%X`)
  deterministically. Either return a clear error to the caller
  or pass the malformed sequence through literally — the chosen
  behavior MUST be documented in a code comment and asserted by
  a unit test.
- **FR-012**: `ParseConnectionString` (`src/mssql_storage.cpp:258`)
  MUST honor `{...}` quoting around values, so that a `;` inside
  a quoted value does not split the field. Inside `{...}`, a
  literal `}` is escaped as `}}` per ADO.NET convention.
- **FR-013**: Neither `ParseUri` nor `ParseConnectionString` MUST
  call any locale-dependent narrowing API (`mbstowcs`, `wcrtomb`,
  `wcstombs`, `_mbstowcs_s`, `std::wstring_convert`,
  `std::setlocale`). Non-ASCII bytes travel as raw `std::string`
  (UTF-8). Compliance verified by code review documented in the
  research artifact produced by `/speckit-plan`.
- **FR-014**: The DuckDB secret reader path MUST be verified to
  preserve UTF-8 bytes end-to-end. No code change expected; if a
  defect surfaces it falls in scope.

**simdutf foundation**

- **FR-030**: `vcpkg.json` MUST declare `"simdutf"` as a
  dependency. The vcpkg baseline (currently
  `5ee5eee0d3e9c6098b24d263e9099edcdcef6631`) MUST be bumped only
  if the current baseline does not provide the port.
- **FR-031**: `CMakeLists.txt` MUST locate simdutf via
  `find_package(simdutf CONFIG REQUIRED)` and link it into both
  `${EXTENSION_NAME}` and `${LOADABLE_EXTENSION_NAME}` via
  `target_link_libraries(... simdutf::simdutf)`. The link MUST
  be static on every platform (consistent with the project's
  existing static OpenSSL / cpp-httplib linkage via static
  vcpkg triplets).
- **FR-032**: A thin wrapper header in
  `src/include/tds/encoding/` (filename to be finalized in
  `/speckit-plan`, e.g. `simdutf_utf16.hpp`) MUST expose
  free-function primitives for UTF-8 ↔ UTF-16LE conversion in
  the `duckdb::tds::encoding` namespace, with signatures
  compatible with the legacy functions in `utf16.hpp` so that
  spec 044's call-site migration is a single-include change per
  file. Minimum exposed API:
  - `Utf16LEEncode(const std::string&) -> std::vector<uint8_t>`
  - `Utf16LEDecode(const uint8_t*, size_t) -> std::string`
  - `Utf16LEByteLength(const std::string&) -> size_t`
  - `Utf16LEEncodeDirect(const char*, size_t, uint8_t*) -> size_t`
  Distinct symbol names from the legacy converter (e.g. under
  a `simdutf_backed` sub-namespace, or with a `Simdutf` prefix)
  so the two coexist in this spec.
- **FR-033**: The LOGIN7 builder fix (FR-005's helper) MUST use
  the simdutf wrapper for its UTF-8 → UTF-16LE conversion. This
  is the only production consumer in spec 043; all other call
  sites of the legacy converter remain unchanged.
- **FR-034**: The simdutf wrapper MUST NOT throw on invalid
  UTF-8 or invalid UTF-16LE input. Invalid-input contract: the
  wrapper MUST first call `simdutf::validate_utf8` (or
  `validate_utf16le` for the decode direction) on the input. If
  validation passes, the wrapper MUST use the fast
  `convert_valid_utf8_to_utf16le` /
  `convert_valid_utf16le_to_utf8` path. If validation fails, the
  wrapper MUST fall back to the legacy hand-rolled converter
  (`encoding::Utf16LEEncode` / `encoding::Utf16LEDecode` /
  `encoding::Utf16LEEncodeDirect` / `encoding::Utf16LEByteLength`
  in `src/tds/encoding/utf16.cpp`) for that single input. This
  preserves the legacy converter's invalid-byte handling (skip
  and continue) bit-for-bit at every consumer call site that
  spec 044 will migrate.
- **FR-035**: The simdutf wrapper MUST build cleanly on Linux
  (GCC), macOS (Clang/AppleClang), Windows (MSVC), and Windows
  (MinGW / Rtools 4.2). No platform-specific source forks.
- **FR-036**: The legacy hand-rolled converter in
  `src/tds/encoding/utf16.cpp` MUST remain in place after this
  spec. Migration of remaining call sites is spec 044's job.

**Testing requirements**

- **FR-020**: A C++ unit test
  `test/cpp/test_login7_encoding.cpp` MUST build a LOGIN7 packet
  for each of the three LOGIN7 builders with at least the
  following input matrix for each variable field: (a) ASCII,
  (b) BMP multi-byte (Cyrillic / accented Latin / CJK),
  (c) non-BMP surrogate-pair (emoji), (d) empty. The test MUST
  parse the packet bytes back and assert `cch*` = expected
  UTF-16 code unit count, `ib*` advances correctly, and the
  de-obfuscated decoded payload equals the original UTF-8 input.
- **FR-021**: A C++ unit test on `ParseUri` MUST cover
  percent-decoded Cyrillic / CJK / emoji passwords, malformed
  escapes (per FR-011's chosen behavior), and `%2B`/`%20` for
  literal `+`/space. Existing tests for ASCII URIs MUST continue
  to pass unchanged.
- **FR-022**: A C++ unit test on `ParseConnectionString` MUST
  cover `{...}` quoting (with internal `;` and `=`), non-ASCII
  values, and locale-independence (test sets `LC_ALL=C` and
  `LC_ALL=ru_RU.UTF-8` if available and asserts identical
  output).
- **FR-023**: An integration test
  `test/sql/integration/non_ascii_password.test` MUST run
  against the project's Docker SQL Server container. Setup
  creates a login with a non-ASCII password using the existing
  `sa` connection; the test then attaches as that user and
  runs `SELECT 1`. Test runs against both TLS-encrypted and
  plaintext LOGIN7 paths.
- **FR-024**: An integration test
  `test/sql/integration/non_ascii_connection_formats.test` MUST
  exercise the URI, ADO.NET, and DuckDB-secret credential
  paths each with a non-ASCII password, asserting all three
  connect successfully.
- **FR-025**: A C++ unit test on the simdutf wrapper MUST run a
  shared fixture set through both the simdutf wrapper and the
  legacy `Utf16LEEncode`/`Utf16LEDecode`/`Utf16LEByteLength`/
  `Utf16LEEncodeDirect` functions, asserting bitwise-identical
  output on valid input across at least 30 fixtures spanning
  ASCII, BMP multi-byte, non-BMP surrogate pairs, and edge
  cases (empty, single-character, max-length-for-test-purposes).
- **FR-026**: All existing scan, BCP, DML, transaction, catalog,
  TLS, and Azure auth test suites MUST continue to pass with no
  regressions. CI MUST remain green on every currently-tested
  platform.

### Key Entities

- **LOGIN7 packet builder**: Three existing functions in
  `src/tds/tds_protocol.cpp` — `BuildLogin7`,
  `BuildLogin7WithFedAuth`, `BuildLogin7WithADAL`. After this
  spec, every variable-field length and offset pair is derived
  from a single small helper whose contract is "given UTF-8
  text, produce the correct UTF-16LE bytes and the matching
  `cch` value, and advance `ib`."
- **Connection-string parser**: The free functions `ParseUri`,
  `UrlDecode`, and `ParseConnectionString` in
  `src/mssql_storage.cpp`. Touched only where the audit reveals
  a defect (locale narrowing, missing `{...}` quoting, ambiguous
  malformed-escape behavior).
- **simdutf wrapper module**: A new header (and possibly a
  small .cpp) under `src/include/tds/encoding/` and
  `src/tds/encoding/`, in namespace `duckdb::tds::encoding`,
  exposing UTF-8 ↔ UTF-16LE primitives backed by simdutf with
  free-function signatures compatible with the legacy
  converter. No DuckDB `Vector` / `string_t` coupling; that
  surface is spec 044's responsibility.
- **LOGIN7 round-trip fixture**: A test harness that builds a
  LOGIN7 packet in memory and reparses it field-by-field,
  shared between the three builder tests in FR-020.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 100% of integration test runs that pass a
  non-ASCII password (Cyrillic, accented Latin, CJK, emoji) to
  the extension authenticate successfully against the
  project's Docker SQL Server container.
- **SC-002**: 100% of LOGIN7 variable fields tested with a
  non-ASCII payload produce the correct `cch*` (UTF-16 code
  units) and `ib*` (cumulative UTF-16LE bytes) values when the
  packet is round-tripped by the unit test harness.
- **SC-003**: For ASCII-only credentials, the LOGIN7 packet's
  **variable-data region** (offset 94 onward, plus the variable-
  field offset/length pairs in the fixed header) is bitwise
  identical to v0.1.18 — zero regression bytes in the region the
  fix touches. Environmental fixed-header fields (ClientPID,
  ClientID, TDS-version constants) are excluded by construction.
- **SC-004**: A non-ASCII password expressed in any of the
  three accepted connection-string formats (URI, ADO.NET,
  DuckDB secret) is delivered to the LOGIN7 builder as
  identical UTF-8 byte sequences. Cross-format consistency:
  100%.
- **SC-005**: The connection-string parser produces identical
  output under at least two different locale settings
  (`LC_ALL=C` and `LC_ALL=en_US.UTF-8`) for the same non-ASCII
  input. Locale-independence: 100%.
- **SC-006**: The failing v0.1.18 reproducer — connecting with
  password `"Тест123!"` and receiving "Login failed for user" —
  returns success when run against the spec-043 binary against
  the same SQL Server instance.
- **SC-007**: The simdutf wrapper and the legacy converter
  produce bitwise-identical UTF-16LE output for 100% of at
  least 30 shared fixtures spanning ASCII, BMP, and non-BMP
  inputs.
- **SC-008**: simdutf is linked statically on every release
  platform — no new runtime `.so`/`.dylib`/`.dll` dependency
  in the final binary (verified via `ldd` / `otool -L` /
  Dependency Walker against the artifacts produced by CI).
- **SC-009**: CI builds the extension cleanly on Linux (GCC),
  macOS (Clang/AppleClang), Windows (MSVC), and Windows
  (MinGW/Rtools) with simdutf added. Build wall-clock
  regression vs. main is under 10% on every platform; final-
  binary size growth is under 500 KB per platform (sanity
  bound, not a tight target).
- **SC-010**: No regression in existing test suites. All
  previously green CI jobs remain green on the spec-043
  branch.

## Assumptions

- The TDS password obfuscation routine
  `TdsProtocol::EncodePassword` (nibble-swap then XOR `0xA5`)
  is correct as of v0.1.18. Static review of
  `src/tds/tds_protocol.cpp:139` supports this. The only
  defect in the LOGIN7 path is the UTF-8-byte vs
  UTF-16-code-unit length confusion in the three builders.
  If a second defect is uncovered during implementation
  (XOR order, nibble direction), it falls in scope of spec
  043 but the spec's framing may need adjustment.
- The Docker SQL Server image used in CI (`make docker-up`)
  accepts `CREATE LOGIN <name> WITH PASSWORD = N'...'` for
  passwords containing arbitrary Unicode. SQL Server has
  supported this since at least SQL Server 2008. The test
  setup creates the non-ASCII-password login at the start of
  the integration test run via the existing `sa` connection.
- The DuckDB secret API and DuckDB's connection-string
  plumbing preserve UTF-8 bytes end-to-end. Confirmed for
  current DuckDB main; the audit (FR-014) verifies and adds
  a regression test, no code change expected.
- simdutf is available in vcpkg at the project's pinned
  baseline (or the next reasonable baseline). Static
  linking is the project's default — vcpkg static triplets
  (`x64-linux-static`, `x64-osx-static`,
  `x64-windows-static-release`, `x64-mingw-static`) already
  produce static `.a`/`.lib` archives, and simdutf's vcpkg
  port supports them. No platform-specific source vendoring
  required.
- Spec 044 (Codec Layer) will perform the bulk migration of
  legacy `utf16.cpp` call sites to the simdutf wrapper. Spec
  043 deliberately leaves all those call sites unchanged so
  that the spec stays narrow and the regression surface stays
  small. The simdutf wrapper introduced in 043 has a stable,
  drop-in-compatible API so that spec 044's migration is
  mechanical.
- Spec 042 (collaborator, integrated authentication) owns
  `src/tds/auth/`. The LOGIN7 length fix lives in
  `src/tds/tds_protocol.cpp` (the packet builder), not the
  auth strategies. No structural overlap; whichever spec
  lands second rebases the LOGIN7 builders.
- Performance gains from simdutf (3–10× on non-ASCII NVARCHAR
  workloads) are *expected* based on simdutf's published
  benchmarks but are NOT acceptance criteria for spec 043.
  No NVARCHAR hot path is migrated in 043. Performance
  validation belongs to spec 044.
- The connection-string audit (FR-010 through FR-014) is a
  defensive sweep, not a known-bug hunt. Only one defect is
  pre-identified by inspection: missing `{...}` quoting in
  `ParseConnectionString`. Other items may surface during
  implementation; the audit budget is capped at one
  development day plus tests.
- All source comments, documentation files, commit messages,
  test fixture names, and PR descriptions for spec 043 are
  written in English.

## Out of Scope

- **Migration of existing `utf16.cpp` call sites to
  simdutf** (catalog scan, NVARCHAR decode/encode, BCP
  encode, PLP streaming, FormatSqlLiteral, etc.). Spec 044.
- **Performance benchmarking** and benchmark fixtures for
  NVARCHAR throughput. Spec 044.
- **Removal of the legacy `utf16.cpp` converter**. Spec 044
  (or later), once all call sites are migrated and the new
  path has soaked.
- **Refactor of `src/tds/auth/`** or addition of new
  authentication methods (Kerberos, WinSSPI, integrated
  auth). Spec 042 (collaborator).
- **Changes to TDS version negotiation, PRELOGIN, or the
  LOGIN7 fixed-header layout** beyond what the length fix
  mechanically requires.
- **Adding a new connection-string format**. The audit
  reviews the three existing formats; it does not add a
  fourth.
- **`Vector` / `string_t` / `TdsWriter`-coupled batch APIs**
  on top of the simdutf wrapper. Spec 044.

## Dependencies and Coordination

- **Parallel with spec 042** (collaborator, integrated
  authentication). Spec 042 restructures
  `src/tds/auth/sql_auth_strategy.cpp` and adds new
  authentication strategies. Spec 043 touches
  `src/tds/tds_protocol.cpp` (the LOGIN7 packet builder) and
  possibly `src/mssql_storage.cpp` (the connection-string
  parser), plus introduces the simdutf dependency. The two
  specs do not overlap on source files but both ultimately
  produce LOGIN7 packets; whichever lands second performs a
  small rebase. Coordinate via PR description.
- **Unblocks spec 044 (Codec Layer)**. Spec 044 consumes the
  simdutf wrapper introduced here for the bulk migration of
  UTF-16 call sites. Spec 044 cannot start until 043 has
  landed the wrapper module and the vcpkg dependency.
- **vcpkg baseline**: the project pins
  `5ee5eee0d3e9c6098b24d263e9099edcdcef6631`. If the simdutf
  port is unavailable at that baseline, spec 043 bumps the
  baseline to the smallest newer commit that publishes the
  port across all required triplets. The bump is recorded in
  the spec's research artifact produced by `/speckit-plan`.

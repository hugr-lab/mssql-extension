---

description: "Task list for spec 043 — LOGIN7 non-ASCII fix + simdutf foundation"
---

# Tasks: LOGIN7 Non-ASCII Fix + simdutf Foundation

**Input**: Design documents from `/specs/043-refactoring-foundation/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md

**Tests**: Tests are included in this plan. Spec 043 mandates them in FR-020 through FR-026; the project's existing convention is C++ unit tests in `test/cpp/` and SQLLogicTest files in `test/sql/`.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies on incomplete tasks)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3, US4)
- All paths are repository-relative; the build commands assume `GEN=ninja` per the project memory note

## Path Conventions

Single-project DuckDB extension. Source under `src/` (mirrored by `src/include/`), tests under `test/cpp/` (C++ unit) and `test/sql/` (SQLLogicTest integration). See `plan.md` § Project Structure for the per-file map.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Wire simdutf into the build graph so subsequent phases can `#include` and link.

- [X] T001 Add `"simdutf"` to the `dependencies` array in `vcpkg.json`. Keep existing `openssl` and the OpenSSL 3.4.1 override. Reference: research.md §R1.
- [X] T002 Verify `simdutf` port is available at the current pinned vcpkg baseline `5ee5eee0d3e9c6098b24d263e9099edcdcef6631` by running `vcpkg install simdutf` against the project's vcpkg checkout for each required triplet (`x64-linux-static`, `x64-osx-static`, `x64-windows-static-release`, `x64-mingw-static`). If any triplet's port is missing, find the smallest newer baseline commit that publishes it on all four triplets and update the `builtin-baseline` field in `vcpkg.json` accordingly. Document the chosen baseline in research.md §R1. **Verified locally**: port `simdutf` version-semver `6.1.1` exists at the pinned baseline; no bump needed. Baseline commit `5ee5eee0d3e9c6098b24d263e9099edcdcef6631` retained.
- [X] T003 Add `find_package(simdutf CONFIG REQUIRED)` to `CMakeLists.txt` next to the existing `find_package(OpenSSL REQUIRED)` call (around line 38). Add `target_link_libraries(${EXTENSION_NAME} simdutf::simdutf)` and `target_link_libraries(${LOADABLE_EXTENSION_NAME} simdutf::simdutf)` next to the existing OpenSSL link lines (around lines 164 and 184). Do NOT add any `target_compile_features(... cxx_std_*)` directive — keep the extension at DuckDB's default C++ standard per CLAUDE.md ODR notes. Reference: research.md §R3, §R4.
- [X] T004 Smoke-build the extension on the local dev box with `GEN=ninja make debug` to confirm `simdutf::simdutf` is found and links into both `mssql_extension` and `mssql_loadable_extension` targets. No source changes yet — this is a build-graph smoke test only. Resolve any "Could NOT find simdutf" by completing T002 first.

**Checkpoint**: `make debug` succeeds with simdutf linked. No production code changed yet.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Implement the `simdutf_wrappers` module so user-story phases can call it. Must complete before any user story phase begins.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [X] T005 Create `src/include/tds/encoding/simdutf_wrappers.hpp` mirroring the contract in `specs/043-refactoring-foundation/contracts/simdutf_wrappers.hpp`. Five free function declarations in namespace `duckdb::tds::encoding`, with `Simdutf*` symbol prefix to coexist with the legacy `utf16.hpp` symbols. Include doc comments quoting the invalid-input contract.
- [X] T006 Create `src/tds/encoding/simdutf_wrappers.cpp`. Implement `SimdutfUtf16LEEncode`: call `simdutf::validate_utf8(input.data(), input.size())`; on success, allocate `simdutf::utf16_length_from_utf8(...)` `char16_t` slots and call `simdutf::convert_valid_utf8_to_utf16le(...)`, then byte-swap-or-not as needed to produce a little-endian `std::vector<uint8_t>` (size = code units × 2); on failure, delegate to `encoding::Utf16LEEncode(input)` from `utf16.hpp`. Include `<simdutf.h>` and `tds/encoding/utf16.hpp`. Reference: research.md §R2.
- [X] T007 [P] In `src/tds/encoding/simdutf_wrappers.cpp`, implement `SimdutfUtf16LEDecode(const uint8_t*, size_t)`: pre-validate with `simdutf::validate_utf16le(...)` (reinterpret cast `uint8_t*` to `char16_t*` only if alignment permits; otherwise copy to an aligned buffer first). On success, allocate `simdutf::utf8_length_from_utf16le(...)` bytes and call `simdutf::convert_valid_utf16le_to_utf8(...)`. On failure, delegate to `encoding::Utf16LEDecode(data, byte_length)`. Add the `std::vector<uint8_t>` overload that forwards to the pointer/size form.
- [X] T008 [P] In `src/tds/encoding/simdutf_wrappers.cpp`, implement `SimdutfUtf16LEByteLength`: pre-validate; on success return `simdutf::utf16_length_from_utf8(...) * 2`; on failure delegate to legacy `encoding::Utf16LEByteLength`.
- [X] T009 [P] In `src/tds/encoding/simdutf_wrappers.cpp`, implement `SimdutfUtf16LEEncodeDirect(const char*, size_t, uint8_t*)`: pre-validate; on success call `simdutf::convert_valid_utf8_to_utf16le(input, len, reinterpret_cast<char16_t*>(output))` and return code units × 2; on failure delegate to legacy `encoding::Utf16LEEncodeDirect`.
- [X] T010 Add `src/tds/encoding/simdutf_wrappers.cpp` to the `EXTENSION_SOURCES` list in `CMakeLists.txt` (the project memory note flags this as a common build error if forgotten). Rebuild with `GEN=ninja make debug` to confirm the new TU compiles cleanly with the project's default C++ standard. Resolve any C++14+ feature errors by adjusting the implementation — no `cxx_std_*` bump is acceptable.

**Checkpoint**: Wrapper module compiles and links. None of its callers exist yet. The legacy converter remains untouched (FR-036).

---

## Phase 3: User Story 1 — Connect with non-ASCII password (Priority: P1) 🎯 MVP

**Goal**: A user can authenticate to SQL Server with a password containing non-ASCII characters (Cyrillic, accented Latin, CJK, surrogate-pair emoji).

**Independent Test**: Run `test/sql/integration/non_ascii_password.test` against the project's Docker SQL Server. Setup creates a login with password `"Тест123!"` via `sa`; the test attaches as that user and runs `SELECT 1`. Pre-fix this fails; post-fix it passes.

- [X] T011 [US1] Add a file-static helper `EncodeLogin7VarField` to `src/tds/tds_protocol.cpp` exactly per data-model.md §E2: takes `(field_name, utf8_text, &cumulative_ib_offset, obfuscate_password=false)`, returns `Login7VarFieldResult{utf16le_bytes, cch, ib}`. Calls `encoding::SimdutfUtf16LEEncode` for the conversion. After encoding, asserts `bytes.size() / 2 <= 128`; on overflow throws `IOException` with the exact wording in FR-008. For `obfuscate_password == true`, applies the existing `TdsProtocol::EncodePassword` nibble-swap + XOR 0xA5 transform to `result.utf16le_bytes`. Advances `cumulative_ib_offset` by `result.utf16le_bytes.size()`.
- [X] T012 [US1] Refactor `TdsProtocol::BuildLogin7` in `src/tds/tds_protocol.cpp:153` to call `EncodeLogin7VarField` once per variable string field in declared order (`HostName`, `UserName`, `Password` with `obfuscate_password=true`, `AppName`, `ServerName`, `Database`). Replace the existing `password.size()` / `host.size()` / etc. usage at lines 164-169 and the manual `cumulative_ib += len * 2` math at lines 175-219 with values returned by the helper. The fixed-header `cch*` / `ib*` writes at lines 309-326 now consume `result.cch` and `result.ib` directly. Remove the now-unused `EncodePassword` call site (line 141 stays — the helper invokes it).
- [X] T013 [US1] Create `test/cpp/test_login7_encoding.cpp`. Implement `ParseLogin7Packet` per data-model.md §E5 — extract the `ib*`/`cch*` pairs from offsets 36..72 and the variable-region bytes from offset 94 onward. Add a test case `BuildLogin7 round-trips Cyrillic password`: build a LOGIN7 with `host="localhost"`, `user="user_ru"`, `password="Тест123!"`, then parse it back and assert `cch_password == 8`, `ib_password` is correct, and the de-obfuscated UTF-16LE password bytes decode to `"Тест123!"`. Register the test in `CMakeLists.txt` under the existing test-cpp source list.
- [X] T014 [US1] Create `test/sql/integration/non_ascii_password.test` (satisfies SC-001 and SC-006). Use the existing `sa` env var pattern (`MSSQL_TESTDB_DSN` style). The test:
  1. Attaches via `sa` to `master`.
  2. Runs the T-SQL recipe from research.md §R8 to create login `user_ru` with `PASSWORD = N'Тест123!'` and grant access to `TestDB`. Wrap each step in `IF NOT EXISTS` guards for re-runnability.
  3. Detaches.
  4. **Attaches as `user_ru` twice — once with `encrypt=true` and once with `encrypt=false`** — using the Cyrillic password (`Password=Тест123!;Encrypt=...`). Both ATTACHes must succeed (FR-023 requires both TLS-encrypted and plaintext LOGIN7 paths to be covered).
  5. Runs `SELECT 1` against each attach and asserts result `1`.
  6. (Teardown is optional — Docker container is fresh per CI run.)
  Mark file group as `[mssql] [integration]`.
- [X] T015 [US1] Manually run the quickstart.md §6 reproducer against the local Docker SQL Server: connect with `Password=Тест123!` and confirm `SELECT 1` succeeds. This validates that the fix works outside the SQLLogicTest harness too.

**Checkpoint**: Cyrillic passwords work end-to-end. The MVP slice is complete.

---

## Phase 4: User Story 2 — Authenticate with non-ASCII database/username/app name (Priority: P1)

**Goal**: Non-ASCII characters work in any LOGIN7 variable field, not only password. Covers `HostName`, `UserName`, `AppName`, `ServerName`, `Database`.

**Independent Test**: The C++ unit test `test_login7_encoding.cpp` builds a LOGIN7 for each variable field with non-ASCII content (Cyrillic / accented Latin / CJK / emoji) and asserts the round-trip is correct. The integration test attaches with `User Id=jürgen` against a login created with that exact name and confirms success.

- [X] T016 [US2] Refactor `TdsProtocol::BuildLogin7WithFedAuth` in `src/tds/tds_protocol.cpp:1185` to use `EncodeLogin7VarField` for every variable field (password field is empty/N/A for FEDAUTH; all other variable fields still apply). Replace any `utf8.size()` / `len * 2` math identically to T012.
- [X] T017 [US2] Refactor `TdsProtocol::BuildLogin7WithADAL` in `src/tds/tds_protocol.cpp:1443` to use `EncodeLogin7VarField` for every variable field. Same shape as T016.
- [X] T018 [P] [US2] Extend `test/cpp/test_login7_encoding.cpp` with a fixture matrix: three builders × six variable fields × four input categories (ASCII / BMP multi-byte / non-BMP surrogate-pair / empty). For each combination, build the packet, parse it, and assert `cch*` and `ib*` and the decoded payload. Implement as a Catch2-style table-driven test (or the framework the project's existing C++ tests use — match `test/cpp/test_simple_query.cpp` style).
- [X] T019 [P] [US2] Add a unit test case in `test/cpp/test_login7_encoding.cpp` that supplies a 129-UTF-16-code-unit password (e.g., 129 ASCII chars) and asserts `EncodeLogin7VarField` throws `IOException` with the exact wording from FR-008. Add a second case using a non-ASCII string that fits in UTF-8 but exceeds 128 UTF-16 code units after conversion (e.g., 65 surrogate-pair emoji) to verify the cap is enforced on UTF-16 code units, not UTF-8 bytes.
- [X] T020 [P] [US2] Add an ASCII-only regression case to `test/cpp/test_login7_encoding.cpp`: build a LOGIN7 from a fixed ASCII fixture and compare the resulting bytes against a hardcoded hex-string constant for the variable-data region (offset 94 onward) plus the variable-field `ib*`/`cch*` pairs at offsets 36..72 in the fixed header (per the comparison contract in research.md §R10). Exclude ClientPID, ClientID, ClientLCID, ClientProgVer, ClientTimeZone, and TDS-version constants from the byte compare. Generate the expected bytes once from the post-fix build of T012 and freeze them in the test file.

**Checkpoint**: All six variable LOGIN7 fields handle non-ASCII correctly across all three builders. FR-001..FR-008 satisfied.

---

## Phase 5: User Story 3 — Round-trip non-ASCII credentials across all connection-string formats (Priority: P2)

**Goal**: The URI form (`mssql://`), ADO.NET form (`Server=...;Password=...`), and DuckDB secret form each carry non-ASCII credentials intact to the LOGIN7 builder. ADO.NET `{...}` quoting honored. `UrlDecode` malformed-escape behavior is deterministic.

**Independent Test**: `test/sql/integration/non_ascii_connection_formats.test` runs each of the three forms against the `user_ru` login created in Phase 3's integration test and asserts all three connect. C++ unit tests on `ParseUri` / `UrlDecode` / `ParseConnectionString` cover the malformed-escape and `{...}` cases.

- [X] T021 [US3] Rewrite `UrlDecode` in `src/mssql_storage.cpp:163-178` per research.md §R5. Use a manual `IsHex(c)` (`c >= '0' && c <= '9' || c >= 'A' && c <= 'F' || c >= 'a' && c <= 'f'`) and `HexVal(c)` helper; reject `%XX` if either character is not hex (pass `%` through literally and advance by 1). Replace `sscanf` entirely. Update the function's doc comment to state the deterministic behavior. Reference: research.md §R5 pseudocode.
- [X] T022 [US3] Add `{...}` quoting to `ParseConnectionString` in `src/mssql_storage.cpp:258-306` per research.md §R6. Replace the unconditional `StringUtil::Split(connection_string, ';')` with a char-by-char walk that tracks an `inside_braces` flag: when a value starts with `{`, scan to the matching unescaped `}` (where `}}` is a literal `}` inside braces). Outside braces, behavior is unchanged. The function still produces `case_insensitive_map_t<string>` with identical keys. Add a code comment summarizing the quoting rules. Do NOT add support for `"..."` or `'...'` quoting — out of scope per spec.
- [ ] T023 [P] [US3] Create `test/cpp/test_connection_string_parsing.cpp`. Add unit tests for `UrlDecode`: `%41`, `%D0%9F`, `%GG`, `%aG`, `%`, `%2`, `%20`, `%2B`, `%%`, mixed-case `%dA`, empty string. Assert deterministic literal pass-through for every malformed case. **Also add end-to-end `ParseUri` cases (FR-021):** Cyrillic password URI `mssql://user:%D0%A2%D0%B5%D1%81%D1%82123%21@host/db` → assert `password == "Тест123!"`; CJK example `mssql://u:%E6%9D%B1%E4%BA%AC@host/db` → password `"東京"`; emoji surrogate-pair example with `%F0%9F%94%92` → password `"🔒"`; URI with empty password / empty user / no password at all (existing ASCII shapes) — assert no regression. Register the test in `CMakeLists.txt` under the existing test-cpp source list.
- [ ] T024 [P] [US3] Extend `test/cpp/test_connection_string_parsing.cpp` with `ParseConnectionString` cases: `Server=host;Database=db` (current shape), `Password={a;b}` → value `"a;b"`, `Password={a}}b}` → value `"a}b"`, `Password={Тест;123}` → value `"Тест;123"`, `Password={}` → empty value, unterminated `Password={abc` (decide: error or pass through literally with `{` retained — match the implementation choice in T022 and assert it).
- [ ] T025 [P] [US3] Add a locale-independence test case to `test/cpp/test_connection_string_parsing.cpp`: temporarily set `LC_ALL=C` (via `setlocale`) and parse a connection string with non-ASCII bytes; restore and parse again under the default locale; assert the parsed `password` field is byte-identical. If the CI environment provides `ru_RU.UTF-8`, test that too; otherwise document the test as "best-effort on platforms with limited locale support."
- [X] T026 [US3] Create `test/sql/integration/non_ascii_connection_formats.test`. Exercise three ATTACH statements all targeting the `user_ru` login (created in T014's setup or via a shared setup file). Run each format **twice — once with `encrypt=true` and once with `encrypt=false`** (FR-023):
  - URI form: `mssql://user_ru:%D0%A2%D0%B5%D1%81%D1%82123%21@localhost:1433/TestDB?encrypt=true` and `encrypt=false`
  - ADO.NET form: `Server=localhost,1433;Database=TestDB;User Id=user_ru;Password=Тест123!;Encrypt=true` and `Encrypt=false`
  - Secret form: `CREATE SECRET ...; ATTACH '' ... (SECRET ..., ENCRYPT true)` and `ENCRYPT false`
  Add a fourth ATTACH using `{...}` quoting: `...;Password={Тест;123!}` against a separately-created login with that exact password (e.g., `user_braces`). Each ATTACH followed by `SELECT 1` asserting `1`. Also asserts FR-014 (the secret-reader UTF-8 round-trip) by use.

**Checkpoint**: Non-ASCII credentials work consistently across all three accepted formats; `{...}` quoting works; malformed URL escapes are deterministic.

---

## Phase 6: User Story 4 — simdutf foundation exercised and verified (Priority: P2)

**Goal**: simdutf is installed, statically linked on every platform, byte-equivalent to the legacy converter on valid input, and exercised by at least one production code path. Bulk migration deferred to spec 044.

**Independent Test**: `test/cpp/test_simdutf_wrappers.cpp` runs a shared fixture set through both `Simdutf*` and legacy converters and asserts bitwise equality. CI build artifacts verified to contain no new dynamic-library dependencies.

- [X] T027 [P] [US4] Create `test/cpp/test_simdutf_wrappers.cpp`. Define a shared `std::vector<std::string>` of at least 30 fixtures: ASCII (10 cases: empty, single char, long), BMP multi-byte (10 cases: Cyrillic phrases, accented Latin, Greek, CJK), non-BMP surrogate pairs (5 cases: emoji, math symbols), and edge cases (single high-byte, all 4-byte UTF-8, mixed). For each fixture run `Utf16LEEncode` and `SimdutfUtf16LEEncode` and assert the returned `std::vector<uint8_t>` are bitwise equal. Repeat for `Utf16LEByteLength` vs `SimdutfUtf16LEByteLength`. For decode, take each encoded byte vector and run both `Utf16LEDecode` and `SimdutfUtf16LEDecode` and assert the returned `std::string` are bitwise equal. Register in `CMakeLists.txt`.
- [X] T028 [P] [US4] In `test/cpp/test_simdutf_wrappers.cpp`, add an invalid-UTF-8 case: a string with a deliberately malformed sequence (e.g., `"\xC0\xC1invalid"`). Assert `SimdutfUtf16LEEncode` does not throw and produces output bitwise-identical to `Utf16LEEncode` (the fallback path).
- [ ] T029 [US4] Run `GEN=ninja make` on Linux (local or CI), then `ldd build/release/extension/mssql/mssql.duckdb_extension | grep -i simdutf` — assert no matches (static linking, SC-008). Repeat on macOS with `otool -L`. On Windows CI artifact, inspect with `dumpbin /dependents`. Document the verification in research.md §R4 (paste the empty grep output as evidence).
- [ ] T030 [US4] Compare binary size of the post-fix loadable extension against a fresh `main`-branch build. Run `ls -l build/release/extension/mssql/mssql.duckdb_extension` on both. Assert growth < 500 KB per platform (SC-009, binary-size half). Record the numbers in research.md §R1 as evidence.
- [ ] T031 [US4] Measure clean-build wall-clock on the local dev box: `make clean && time GEN=ninja make`. Compare against the same measurement on `main`. Assert regression < 10% (SC-009). Record numbers.

**Checkpoint**: simdutf scaffolding is production-grade. Foundation is ready for spec 044's mass migration.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Final hygiene before the spec-043 PR opens.

- [ ] T032 [P] Run `clang-format -i` (version 14, per `CLAUDE.md`) on every touched source file: `src/tds/tds_protocol.cpp`, `src/mssql_storage.cpp`, `src/tds/encoding/simdutf_wrappers.cpp`, `src/include/tds/encoding/simdutf_wrappers.hpp`, every new file under `test/cpp/`. Verify the diff is clean.
- [ ] T033 [P] Run the full test suite: `make docker-up && GEN=ninja make test-all`. Confirm zero regressions in catalog, scan, BCP, DML, transaction, TLS, and Azure-auth tests (SC-010).
- [X] T034 [P] Re-read `README.md` § Third-Party Licenses and confirm the simdutf entry plus the OpenSSL entry are still accurate. Cross-check `LICENSES/simdutf-LICENSE-MIT.txt` and `LICENSES/openssl-LICENSE.txt` content against upstream tags. No changes expected unless an upstream license bump occurred.
- [ ] T035 Run `quickstart.md` end-to-end on the local dev box (sections 1–9). Note any step that disagrees with reality and update `quickstart.md`.
- [ ] T036 [P] Update the spec's `Status` field in `specs/043-refactoring-foundation/spec.md` from `Draft` to `Implemented` once T001..T035 complete.
- [ ] T037 [P] Open a draft PR titled `feat: LOGIN7 non-ASCII fields fix + simdutf foundation (spec 043)`. PR description references: the spec.md file, the v0.1.18 reproducer, the coordination note with spec 042 from research.md §R11, and the four SC measurement results (binary size, ldd output, build time, test pass count). Mark PR as ready for review only after T033 succeeds.

**Final checkpoint**: All four user stories shipped, all SC items measured, CI green on all platforms, PR open.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 (Setup)**: T001 → T002 → T003 → T004 (each strictly sequential; T002 may bump baseline)
- **Phase 2 (Foundational)**: T005 → T006; T007/T008/T009 are [P] on T006's namespace plumbing; T010 closes the phase. Phase 2 blocks all user-story phases.
- **Phase 3 (US1, MVP)**: T011 → T012 → T013 → T014; T015 is a manual validation at the end
- **Phase 4 (US2)**: T016 and T017 are sequential (both edit `tds_protocol.cpp`); T018, T019, T020 are [P] once helper is in place (depend on T011 only)
- **Phase 5 (US3)**: T021 → T023 (UrlDecode tests depend on impl); T022 → T024 (ParseConnectionString tests depend on impl); T025 is [P] on either; T026 integration test depends on T021 and T022
- **Phase 6 (US4)**: T027 and T028 are [P] on Phase 2; T029, T030, T031 are [P] verification tasks once T010 (link) is done
- **Phase 7 (Polish)**: T032..T035 sequential or [P] as marked; T036 and T037 last

### User Story Dependencies

- **US1 (P1)**: depends on Phase 2 only. Delivers the MVP.
- **US2 (P1)**: depends on T011 (helper exists). Otherwise independent of US1 tests.
- **US3 (P2)**: depends on Phase 2 only (wrapper is not consumed by parser).
- **US4 (P2)**: depends on Phase 2 (wrapper exists) and on T010 (build wired). Independent of US1/US2/US3.

### Within Each User Story

- Implementation tasks before tests that exercise them (existing project convention; not TDD).
- Unit tests before integration tests within a story.
- Same-file edits are sequential; different-file tasks may parallelize.

### Parallel Opportunities

- **Phase 1**: T002 (vcpkg verify) and T003 (CMake) edits are different files, but T003 depends on T001 having added the dep; safer to run sequentially.
- **Phase 2**: T007, T008, T009 [P] once T006's includes/aliases are in place.
- **Phase 4**: T018, T019, T020 [P] once T011 exists; T016 and T017 are sequential.
- **Phase 5**: T023, T024, T025 [P]; T021 and T022 sequential (same file).
- **Phase 6**: T027 and T028 [P]; T029, T030, T031 [P].
- **Phase 7**: T032, T033, T034 [P]; T036, T037 [P].

---

## Parallel Example: User Story 2

```bash
# After T011 (helper) and T016/T017 (refactors) are done:
Task: T018 - extend test_login7_encoding.cpp with the 3×6×4 matrix
Task: T019 - add IOException overflow test cases
Task: T020 - add the ASCII regression fixed-fixture comparison
# All three [P] — they edit the same file in different test cases; coordinate via merge.
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001..T004) — build graph
2. Complete Phase 2: Foundational (T005..T010) — wrapper module compiled
3. Complete Phase 3: User Story 1 (T011..T015)
4. **STOP and VALIDATE**: run T014 (integration test) and T015 (manual repro). Cyrillic password works.
5. Open a draft PR at this point. Discuss whether to ship MVP-only or proceed.

### Incremental Delivery

1. Setup + Foundational → simdutf wired, wrapper module exists
2. + US1 → non-ASCII passwords work (MVP)
3. + US2 → non-ASCII anywhere in LOGIN7 works (cleaner story)
4. + US3 → all three connection-string forms tested end-to-end (defensive sweep)
5. + US4 → simdutf foundation verified end-to-end (handoff to spec 044)
6. + Polish → format, full test pass, PR open

### Parallel Team Strategy

This spec is small enough for one developer in one sprint. If two developers split:

- Dev A: Phases 1 + 2 + 3 + 4 (build + LOGIN7 fix track)
- Dev B (in parallel after Phase 2): Phase 5 + Phase 6 (parser audit + verification track)
- Both meet in Phase 7

---

## Notes

- [P] tasks = different files (or different test cases in the same file), no dependencies on incomplete tasks
- [Story] label maps task to user story for traceability
- Each user story is independently completable and testable
- Same-file tasks (e.g., T016 and T017 both edit `tds_protocol.cpp`) are intentionally NOT marked [P]
- Commit after each task or logical group; squash before merging if preferred
- Avoid: skipping T002 (baseline verify) — silently produces "Could NOT find simdutf" later
- Avoid: editing `src/tds/encoding/utf16.cpp` — that's FR-036 / out of scope; spec 044 owns it
- Avoid: adding `target_compile_features(... cxx_std_*)` to CMakeLists — CLAUDE.md flags this as a Linux ODR landmine

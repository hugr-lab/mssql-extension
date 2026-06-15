---

description: "Task list for spec 053 — lazy GSSAPI/Kerberos linking on Linux"
---

# Tasks: Lazy GSSAPI/Kerberos Linking on Linux

**Input**: Design documents from `specs/053-lazy-gssapi-linking/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/gssapi_runtime.md

**Tests**: Included where the spec requires verification (SC-003 via the existing
`test/kerberos/` stack; SC-004 via a focused C++ unit test). No full TDD was requested.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: US1 / US2 / US3 (maps to spec.md user stories)
- All paths are repo-relative from the repository root.

**Note on coupling**: this is a single refactor with three observable outcomes. The
shim (Phase 2) must exist before any call site can be rewired, and **all** `gss_*` /
`krb5_*` call sites must be rewired before the Linux link can be dropped (a single
remaining direct reference re-introduces the `DT_NEEDED`). US1 is therefore the
large MVP phase; US2/US3 are verification + UX refinement on top of it.

---

## Phase 1: Setup

**Purpose**: Confirm the build environment and locate the exact edit sites.

- [ ] T001 Verify the Kerberos dev headers are present for the build (Debian/Ubuntu `libkrb5-dev`, RHEL/Fedora `krb5-devel`); confirm `pkg_check_modules(MSSQL_GSSAPI krb5-gssapi)` / `MSSQL_KRB5_BASE krb5` succeed in a configure run so include dirs still resolve after the link is dropped.
- [ ] T002 Re-confirm the full `gss_*` / `krb5_*` call-site inventory in `src/tds/auth/krb5_authenticator.cpp` and `src/tds/auth/krb5_test_function.cpp` (8 `gss_*`, 13 `krb5_*` + 4 test-only `krb5_*` per data-model.md) so no call site is missed during rewiring.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Build the runtime-loader shim. No call site can be rewired and the link
cannot be dropped until this exists.

**⚠️ CRITICAL**: All user-story work depends on this phase.

- [ ] T003 Create `src/include/tds/auth/gssapi_runtime.hpp` (guarded by `MSSQL_ENABLE_KRB5`, **no DuckDB headers**): declare `struct GssApiFns` (8 function pointers) and `struct Krb5Fns` (13 + 4 test pointers) with signatures taken verbatim from `<gssapi/gssapi.h>`/`<krb5.h>`; declare `class Krb5RuntimeUnavailable : public std::runtime_error`; declare accessors `const GssApiFns &GetGssApi();` and `const Krb5Fns &GetKrb5();`. Namespace `duckdb::tds`. C++11-only constructs (per research R7).
- [ ] T004 Implement `src/tds/auth/gssapi_runtime.cpp` — Linux path: `dlopen` (`RTLD_NOW | RTLD_LOCAL`) trying SONAMEs `libgssapi_krb5.so.2`→`libgssapi_krb5.so` (gss) and `libkrb5.so.3`→`libkrb5.so` (krb5); `dlsym` each symbol; `std::call_once`-guarded one-time fill of the cached tables; on any failure throw `Krb5RuntimeUnavailable` with message naming the missing object + `dlerror()` + install package (`libgssapi-krb5-2` / `krb5-libs`) per contracts/gssapi_runtime.md (FR-005, FR-009, R3/R4/R6).
- [ ] T005 In the same `gssapi_runtime.cpp`, add the macOS (`__APPLE__`) path: populate `GssApiFns` with direct `&gss_*` addresses (framework still linked), no `dlopen`; `GetKrb5()` is unused on macOS (R5).
- [ ] T006 CMakeLists.txt: add `src/tds/auth/gssapi_runtime.cpp` to `EXTENSION_SOURCES` inside the existing `if(MSSQL_KRB5_AVAILABLE)` block (next to `krb5_authenticator.cpp`, ~line 217).

**Checkpoint**: Shim compiles and links (still alongside the existing direct calls). No behavior change yet.

---

## Phase 3: User Story 1 — Load without Kerberos libraries (Priority: P1) 🎯 MVP

**Goal**: The Linux binary loads on a clean image with no Kerberos runtime; non-Kerberos connections work; `DT_NEEDED` is free of GSSAPI/krb5.

**Independent Test**: On a clean Linux image (no `libgssapi-krb5-2`), `INSTALL mssql FROM community; LOAD mssql;` succeeds and `ldd` shows no `gssapi`/`krb5` entries (SC-001, SC-002, SC-005).

### Implementation for User Story 1

- [ ] T007 [US1] Rewire all `gss_*` call sites in `src/tds/auth/krb5_authenticator.cpp` through the shim: at the top of `AcquireCredentials()` and `DoSecContextStep()` grab `const auto &gss = GetGssApi();` and replace `gss_xxx(` → `gss.xxx(`; route `ThrowGssError` through `gss.display_status`. Include `tds/auth/gssapi_runtime.hpp`. Constructor still performs **no** GSSAPI calls (lazy trigger stays on first `InitialBytes()`).
- [ ] T008 [US1] Rewire all `krb5_*` call sites (raw-mode credential path) in `src/tds/auth/krb5_authenticator.cpp` through `const auto &k = GetKrb5();` (`k.xxx(`), behind the existing `MSSQL_KRB5_HAS_MIT_EXTENSIONS` guard.
- [ ] T009 [US1] Rewire all `gss_*` / `krb5_*` call sites in `src/tds/auth/krb5_test_function.cpp` through `GetGssApi()` / `GetKrb5()` so it carries no direct symbol references.
- [ ] T010 [US1] Confirm `src/include/tds/auth/krb5_authenticator.hpp` still includes the GSSAPI headers (types/handles needed) but references **no** GSSAPI function symbol — header stays link-clean and DuckDB-free.
- [ ] T011 [US1] CMakeLists.txt non-Apple branch (static ~lines 251–259 **and** loadable ~lines 293–297): **keep** `target_include_directories(... ${MSSQL_GSSAPI_INCLUDE_DIRS} ${MSSQL_KRB5_BASE_INCLUDE_DIRS})`; **remove** `target_link_libraries(... ${MSSQL_GSSAPI_LIBRARIES} ${MSSQL_KRB5_BASE_LIBRARIES})`; add `${CMAKE_DL_LIBS}` to the link line. Update the adjacent comment to note link-time deps are intentionally dropped on Linux (lazy `dlopen`). Apple branch unchanged.
- [ ] T012 [US1] Build on Linux (`make`) and assert no GSSAPI/krb5 in `DT_NEEDED`: `ldd build/release/extension/mssql/mssql.duckdb_extension | grep -iE 'gssapi|krb5'` returns empty (SC-002).
- [ ] T013 [US1] Clean-image load test reproducing issue #161 (no Kerberos package): build/use a minimal image, `INSTALL mssql FROM local_build; LOAD mssql;` succeeds and a SQL-auth `ATTACH` works (SC-001, SC-005). Capture as a runnable script (e.g. under `test/` or documented in quickstart.md).

**Checkpoint**: #161 is fixed — extension loads and works without the Kerberos runtime; no hard dependency remains.

---

## Phase 4: User Story 2 — Kerberos still works when libraries present (Priority: P1)

**Goal**: No regression to spec 042 Kerberos auth when the runtime is installed.

**Independent Test**: `test/kerberos/` docker stack passes all auth tests (SC-003).

### Implementation / Verification for User Story 2

- [ ] T014 [US2] Run the `test/kerberos/` stack end-to-end (`docker compose up -d --build` → `docker compose exec test-client /run-tests.sh`); confirm CredCache mode authenticates via the dlopened runtime with zero regression (FR-004, SC-003).
- [ ] T015 [US2] Confirm keytab and raw modes (Linux, MIT extensions) still authenticate through the shim's `Krb5Fns` table; confirm `mssql_kerberos_auth_test(host)` returns the same SPN / principal / token-size output as before.
- [ ] T016 [P] [US2] Confirm the macOS build still compiles and links against `-framework GSS` with the direct-address table, and that keytab/raw modes remain rejected at construction (behavior unchanged, FR-007).

**Checkpoint**: Kerberos fully functional where the runtime is present, on Linux and macOS.

---

## Phase 5: User Story 3 — Clear actionable error when unavailable (Priority: P2)

**Goal**: Requesting Kerberos without the runtime yields a self-explanatory error, not a loader crash or generic failure.

**Independent Test**: On a clean image, `authenticator=krb5` connection / `mssql_kerberos_auth_test` returns a message naming `libgssapi_krb5.so.2` and the install package (SC-004).

### Implementation for User Story 3

- [ ] T017 [US3] Finalize the `Krb5RuntimeUnavailable` message text in `gssapi_runtime.cpp` to include all three parts (missing object, `dlerror()`, package recommendation) per the error contract (FR-005).
- [ ] T018 [US3] In `src/tds/auth/krb5_test_function.cpp`, wrap the body so a thrown `Krb5RuntimeUnavailable` is caught and returned as the function's result string (functions stay registered and crash-free when the runtime is absent) (FR-006).
- [ ] T019 [P] [US3] Add a C++ unit test in `test/cpp/test_gssapi_runtime.cpp` asserting the `Krb5RuntimeUnavailable::what()` text contains the missing-library name and the install package (SC-004); register it in the test CMake. Verify a non-Kerberos connection path never constructs the shim (FR-010).

**Checkpoint**: All three observable outcomes verified.

---

## Phase 6: Polish & Cross-Cutting Concerns

- [ ] T020 [P] Update docs: `Kerberos.md` (note the runtime is now lazily loaded; name the install packages per distro) and the CLAUDE.md Integrated Authentication section / implementation-files list to add `gssapi_runtime.{hpp,cpp}`.
- [ ] T021 [P] Run `clang-format` (v14) over the new/changed files in `src/tds/auth/` and `src/include/tds/auth/`.
- [ ] T022 Run `make test` (unit tests, no SQL Server) and the full quickstart.md verification checklist; confirm SC-001…SC-005 all pass.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: no dependencies.
- **Foundational (Phase 2)**: depends on Setup; **blocks everything** (the shim must exist).
- **US1 (Phase 3)**: depends on Phase 2. The link-drop (T011) requires **all** rewiring (T007–T009) complete, or `DT_NEEDED` returns.
- **US2 (Phase 4)**: depends on US1 (rewiring + build) — it verifies the rewired paths still authenticate.
- **US3 (Phase 5)**: depends on Phase 2 (error type) and benefits from US1; T018/T019 can proceed once the shim + test fn rewiring (T009) are done.
- **Polish (Phase 6)**: after US1–US3.

### Within US1

- T007, T008, T009 edit two files (authenticator, test fn): T007+T008 same file (sequential); T009 is a different file ([P] vs T007/T008).
- T011 (CMake link-drop) only after T007–T010.
- T012 (ldd) after T011 build; T013 (clean-image) after T012.

### Parallel Opportunities

- T009 [P] can proceed alongside T007/T008 (different file).
- T016 [P] (macOS verify) independent of Linux verification.
- T019 [P] (unit test) and T020/T021 [P] (docs/format) are independent.

---

## Implementation Strategy

### MVP (User Story 1)

1. Phase 1 Setup → 2. Phase 2 Foundational (shim) → 3. Phase 3 US1 (rewire + drop link + verify clean load).
4. **STOP and VALIDATE**: clean-image load + `ldd` clean. This alone closes issue #161.

### Incremental Delivery

- Foundation + US1 → #161 fixed (MVP).
- + US2 → proven no Kerberos regression (gate before merge).
- + US3 → polished failure UX.
- + Polish → docs/format/full verification.

---

## Notes

- [P] = different files, no dependency.
- The single hardest constraint: **no direct `gss_*`/`krb5_*` reference may remain** before T011, or the linker re-adds `DT_NEEDED` and SC-002 fails. T002's inventory exists to prevent a missed call site.
- Build environment still needs the Kerberos **dev headers**; only the **runtime** dependency is removed.
- Commit after each phase checkpoint.

# Tasks: Replace libcurl with DuckDB Built-in httplib

**Input**: Design documents from `/specs/037-replace-libcurl-httplib/`
**Prerequisites**: plan.md (required), spec.md (required), research.md

**Tests**: Not explicitly requested — no test tasks generated. Existing `mssql_azure_auth_test()` validates auth flows.

**Organization**: Tasks grouped by user story. US1 (Windows build) and US2 (service principal auth) are both P1 and tightly coupled — implemented together. US3 (device code) and US4 (reduced deps) follow.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Phase 1: Setup

**Purpose**: Create the HTTP wrapper that replaces libcurl

- [X] T001 Create HTTP response struct and function declarations in src/include/azure/azure_http.hpp
- [X] T002 Implement httplib-based HTTP wrapper with CPPHTTPLIB_OPENSSL_SUPPORT in src/azure/azure_http.cpp

**Checkpoint**: HTTP wrapper compiles and is ready for use by azure_token.cpp and azure_device_code.cpp

---

## Phase 2: Foundational (Build Configuration)

**Purpose**: Update build system to remove curl and add httplib — MUST complete before migrating source files

**CRITICAL**: No user story work can begin until this phase is complete

- [X] T003 Remove "curl" from dependencies array in vcpkg.json
- [X] T004 Update CMakeLists.txt: remove find_package(CURL), remove CURL::libcurl from target_link_libraries, remove CURL_STATICLIB define, remove wldap32/normaliz from MSVC libs, add duckdb/third_party/httplib to include_directories, add CPPHTTPLIB_OPENSSL_SUPPORT to compile_definitions, add src/azure/azure_http.cpp to EXTENSION_SOURCES, remove src/tds/tds_win32_compat.cpp from EXTENSION_SOURCES

**Checkpoint**: Build config updated — curl removed, httplib configured. Build will fail until source migration is complete.

---

## Phase 3: User Story 1+2 — Windows Build + Service Principal Auth (Priority: P1)

**Goal**: Replace curl in azure_token.cpp so service principal auth works via httplib, enabling clean Windows MSVC builds

**Independent Test**: `make` succeeds on macOS/Linux. `mssql_azure_auth_test()` returns valid token for service_principal secret.

### Implementation

- [X] T005 [US1] [US2] Migrate AcquireTokenForServicePrincipal() in src/azure/azure_token.cpp: replace curl_easy_init/curl_easy_setopt/curl_easy_perform with azure::HttpPost(), remove WriteCallback and UrlEncode(CURL*,...) helpers, remove #include <curl/curl.h>
- [X] T006 [US1] [US2] Verify local build succeeds on macOS/Linux with `make`

**Checkpoint**: Service principal authentication works. Extension builds without libcurl on macOS/Linux.

---

## Phase 4: User Story 3 — Device Code Flow (Priority: P2)

**Goal**: Replace curl in azure_device_code.cpp so interactive authentication works via httplib

**Independent Test**: Device code flow displays user code and verification URL. Poll loop works.

### Implementation

- [X] T007 [US3] Migrate HttpPost(), RequestDeviceCode(), and PollForToken() in src/azure/azure_device_code.cpp: replace all curl calls with azure::HttpPost(), remove local WriteCallback and UrlEncode helpers, remove #include <curl/curl.h>
- [X] T008 [US3] Verify no curl references remain: search src/ for curl_easy, curl/curl.h, CURL, libcurl

**Checkpoint**: All Azure auth flows work without libcurl. No curl references in source code.

---

## Phase 5: User Story 4 — Clean Up and Windows Verification (Priority: P2)

**Goal**: Remove POSIX compat wrappers and verify Windows MSVC build passes

**Independent Test**: CI build passes on all platforms including Windows MSVC with community-extensions triplet.

### Implementation

- [X] T009 [US4] Delete src/tds/tds_win32_compat.cpp
- [ ] T010 [US4] Push to branch and trigger CI build with Windows MSVC (x64-windows-static-md-release-vs2019comp triplet)
- [ ] T011 [US4] If Windows MSVC build has remaining __imp_ linker errors from OpenSSL: recreate tds_win32_compat.cpp with ONLY the OpenSSL-specific wrappers (not curl ones), update CMakeLists.txt sources list
- [ ] T012 [US4] Verify all CI platforms pass: Linux GCC, macOS Clang, Windows MSVC, Windows MinGW

**Checkpoint**: Extension builds cleanly on all platforms without libcurl.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Documentation and final cleanup

- [X] T013 [P] Update CLAUDE.md: remove libcurl references from Azure AD section, add cpp-httplib (bundled) to technology list
- [X] T014 [P] Update AZURE.md if it mentions curl or build dependencies (N/A — no curl references in AZURE.md)
- [ ] T015 Commit and push final changes, verify CI green on all platforms

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately
- **Foundational (Phase 2)**: Depends on Phase 1 (T001, T002)
- **US1+US2 (Phase 3)**: Depends on Phase 2 (build config)
- **US3 (Phase 4)**: Depends on Phase 2. Can run in parallel with Phase 3 (different file)
- **US4 (Phase 5)**: Depends on Phase 3 AND Phase 4 (all curl removed before testing Windows build)
- **Polish (Phase 6)**: Depends on Phase 5

### User Story Dependencies

- **US1+US2 (P1)**: Windows build + service principal — MUST complete first (core migration)
- **US3 (P2)**: Device code flow — can run in parallel with US1+US2 (different source file)
- **US4 (P2)**: Windows verification — depends on US1+US2 and US3 completion

### Parallel Opportunities

- T001 and T002 are sequential (header before implementation)
- T005 and T007 can run in parallel (different files: azure_token.cpp vs azure_device_code.cpp)
- T013 and T014 can run in parallel (different files)

---

## Implementation Strategy

### MVP First (Phase 1 + 2 + 3)

1. Complete Phase 1: Create HTTP wrapper
2. Complete Phase 2: Update build config
3. Complete Phase 3: Migrate azure_token.cpp
4. **STOP and VALIDATE**: `make` succeeds, service principal auth works
5. This alone fixes the primary issue (libcurl removed from build)

### Full Delivery

1. MVP above → curl removed from build
2. Phase 4: Migrate azure_device_code.cpp → all auth flows migrated
3. Phase 5: Windows CI verification → confirm fix
4. Phase 6: Polish → docs updated

---

## Notes

- Total tasks: 15
- Phase 3 (US1+US2): 2 tasks — core migration
- Phase 4 (US3): 2 tasks — device code migration
- Phase 5 (US4): 4 tasks — Windows verification (may need iteration)
- Parallel opportunities: T005 || T007 (different files), T013 || T014
- MVP scope: Phases 1-3 (7 tasks) — removes curl dependency entirely

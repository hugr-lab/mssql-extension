# Tasks: Integrated Authentication (Kerberos / SSPI)

**Input**: Design documents from `/specs/042-integrated-authentication/`
**Prerequisites**: plan.md (required), spec.md (required), research.md, quickstart.md

**Tests**: Integration tests under `test/sql/integrated_auth/` require the new containerized KDC (`make docker-kerberos-up`). Pure parser unit tests under `test/cpp/` need no infrastructure.

**Organization**: Tasks are grouped by user story and follow the migration order from `plan.md` (refactor first, parser next, POSIX, Windows, docs). Each task is ≤ ~300 LoC of diff.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1=POSIX cred cache, US2=Windows SSPI, US3=keytab, US4=raw creds, US5=regression)

---

## Phase 1: Foundational refactor (no behavior change)

**Purpose**: Extract `IAuthenticator` and route existing SQL-auth + Azure-AD through the same path. All existing tests must pass unchanged after each task in this phase.

**CRITICAL**: This phase MUST land as a standalone PR before any Kerberos code. Reviewers can confirm "no behavior change" by running the full existing test suite.

- [ ] **T001** [US5] Add `src/include/tds/auth/iauthenticator.hpp` — three-method interface (`InitialBytes`, `NextBytes`, `Free`) plus optional `SetChannelBinding`. No DuckDB headers. Empty `ChannelBindings` struct stub. Include the comment `// Mirrors microsoft/go-mssqldb integratedauth.IntegratedAuthenticator interface verbatim.`

- [ ] **T002** [US5] Add `GetAuthenticator() -> std::shared_ptr<IAuthenticator>` virtual method to `AuthenticationStrategy` with a `return nullptr;` default in `src/include/tds/auth/auth_strategy.hpp`. No other interface changes.

- [ ] **T003** [P] [US5] Verify `SqlAuthStrategy` and `FedAuthStrategy` build unchanged with the new default. No code changes expected — virtual default suffices.

- [ ] **T004** [US5] Add new `AuthMethod` enum to `src/include/mssql_storage.hpp`: `SQL`, `AZURE_AD`, `MANUAL_TOKEN`, `KRB5`, `WINSSPI`. Add `auth_method` field (default `SQL`). Existing booleans (`use_azure_auth`) remain for backwards compatibility and are kept in sync.

- [ ] **T005** [US5] Run full existing test suite (`make test` + `make integration-test` against existing SQL Server container). All tests must pass with zero changes. **GATE**: do not proceed to Phase 2 until this passes.

**Checkpoint**: Refactor merged. No user-visible change. Foundation ready for Kerberos additions.

---

## Phase 2: Connection-string surface + LOGIN7 wiring (no backend yet)

**Purpose**: Parser, secret schema, conflict validation, and LOGIN7 SSPI-field plumbing. Backends not yet implemented — `Trusted_Connection=yes` errors with "not yet implemented". This phase keeps the change small enough to review independently.

- [ ] **T006** [US1] Add `ENABLE_KRB5` option to `CMakeLists.txt` (default `ON`). Platform-conditional library discovery via `pkg-config krb5-gssapi` (Linux), `find_library(GSS_FRAMEWORK GSS)` (macOS), `target_link_libraries(secur32)` (Windows). Define `MSSQL_ENABLE_KRB5` / `MSSQL_ENABLE_SSPI` accordingly. Conditional source inclusion of authenticator `.cpp` files.

- [ ] **T007** [P] [US1] Extend `MSSQLConnectionInfo` in `src/include/mssql_storage.hpp` with fields: `string krb5_configfile`, `krb5_keytabfile`, `krb5_credcachefile`, `krb5_realm`, `service_principal_name`. Boolean `krb5_dnslookupkdc` (tri-state via `int8_t` default `-1` for "use krb5.conf default"). All optional.

- [ ] **T008** [P] [US1] Parse new keys in `ParseConnectionString` and `ParseUri` in `src/mssql_storage.cpp`. Accept `authenticator`, `krb5-configfile`, `krb5-keytabfile`, `krb5-credcachefile`, `krb5-realm`, `krb5-dnslookupkdc`, `service_principal_name`. Use the `go-mssqldb` names verbatim — do not transform to snake_case. Case-insensitive lookup.

- [ ] **T009** [US1] Parse `Trusted_Connection`, `Integrated Security`, and `Integrated_Security` as aliases in `ParseConnectionString`. Values `yes`, `true`, `sspi`, `1` map to `auth_method = KRB5` on POSIX (`#ifndef _WIN32`) and `WINSSPI` on Windows. Values `no`, `false`, `0` are a no-op (default `SQL`).

- [ ] **T010** [US5] Add conflict validation in `ValidateConnectionString`: `Trusted_Connection=yes` + `User Id` → error; `Trusted_Connection=yes` + Azure secret/parameters → error; `authenticator=krb5` on Windows + `authenticator=winsspi` → error. Error messages name the offending keys.

- [ ] **T011** [P] [US1] Extend MSSQL secret schema in `src/mssql_secret.cpp` to accept `authenticator`, `krb5_configfile`, `krb5_keytabfile`, `krb5_credcachefile`, `krb5_realm`, `service_principal_name` (note: secret field names use underscores per existing convention; ATTACH/URI use hyphens per `go-mssqldb`).

- [ ] **T012** [US1] Add LOGIN7 SSPI field support in `src/tds/tds_protocol.cpp`: new `Login7Options.use_int_security` boolean and `Login7Options.sspi_initial_blob` `vector<uint8_t>`. When set: set `OptionFlags2 |= 0x80`, write blob length into `ibSSPI`/`cbSSPI` (or `cbSSPILong` if blob > 65 535 bytes), write blob into the variable-length section. User/password fields stay empty.

- [ ] **T013** [US1] Recognize `0xED` SSPI token in `src/tds/tds_token_parser.cpp`. Format: 1 byte type + 2-byte LE length + N bytes blob. Surface as a token kind the LOGIN loop can consume.

- [ ] **T014** [US1] Add SSPI continuation loop scaffolding in `TdsConnection::Login()` in `src/tds/tds_connection.cpp`. Mirror the existing FEDAUTH continuation block. For now, throw `"MSSQL Error: Integrated authentication is not yet implemented in this build"` if the strategy provides an `IAuthenticator` — verifies the LOGIN7 path compiles cleanly but does not yet wire a backend.

- [ ] **T015** [P] [US5] Unit tests in `test/cpp/test_integrated_auth_parsing.cpp`. ~15 test cases covering each new key, each alias, each conflict case, secret-vs-connection-string parity. No SQL Server required.

**Checkpoint**: Connection-string surface stable. Validation works. LOGIN7 builder can emit Integrated-Auth packets. End-to-end ATTACH still errors with "not yet implemented" — that's expected.

---

## Phase 3: POSIX Kerberos backend (P1 + P3 stories, v1 MVP)

**Purpose**: Functional Kerberos integrated auth on Linux and macOS. Closes US1 (credential cache) and US3 (keytab) and US4 (raw credentials).

- [ ] **T016** [US1] Create `src/include/tds/auth/krb5_authenticator.hpp`. Class `Krb5Authenticator : public IAuthenticator`. Constructor takes `Krb5Config` struct (spn, configfile, keytabfile, credcachefile, realm, raw_username, raw_password). Private fields: `gss_ctx_id_t ctx = GSS_C_NO_CONTEXT;`, `gss_cred_id_t cred = GSS_C_NO_CREDENTIAL;`, `gss_name_t target_name = GSS_C_NO_NAME;`. **No DuckDB headers.**

- [ ] **T017** [US1] Implement `Krb5Authenticator::InitialBytes()` in `src/tds/auth/krb5_authenticator.cpp` (POSIX-only, guarded by `#if defined(MSSQL_ENABLE_KRB5)`). Sequence: import SPN as `GSS_C_NT_HOSTBASED_SERVICE` → acquire creds (mode dispatch: ccache, keytab, raw) → `gss_init_sec_context` with SPNEGO mech → return output blob. Error path maps GSSAPI major/minor status through `gss_display_status` per R8 of `research.md`. ~250 LoC.

- [ ] **T018** [US1] Implement `Krb5Authenticator::NextBytes(server_blob)`. Pass server blob into `gss_init_sec_context` continuation, return next output blob. Set `complete_` flag when GSS returns `GSS_S_COMPLETE`. ~50 LoC.

- [ ] **T019** [US1] Implement `Krb5Authenticator::Free()` — `gss_delete_sec_context`, `gss_release_cred`, `gss_release_name`. Idempotent.

- [ ] **T020** [US1] Create `IntegratedAuthStrategy` in `src/tds/auth/integrated_auth_strategy.cpp`. Implements `AuthenticationStrategy`. `GetLogin7Options()` returns `Login7Options{ use_int_security = true, username = "", password = "", sspi_initial_blob = authenticator_->InitialBytes() }`. `GetAuthenticator()` returns the wrapped `IAuthenticator`. `RequiresFedAuth() = false`. `GetName() = "IntegratedAuth(krb5)"` or `"IntegratedAuth(winsspi)"`.

- [ ] **T021** [US1] Wire Kerberos into `AuthStrategyFactory::Create()` in `src/tds/auth/auth_strategy_factory.cpp`. Branch on `info.auth_method == KRB5` → build a `Krb5Config` from `info` → wrap in `Krb5Authenticator` → wrap in `IntegratedAuthStrategy`. Surface "extension built without Kerberos support" error when `MSSQL_ENABLE_KRB5` is undefined (FR-012).

- [ ] **T022** [US1] Replace the "not yet implemented" stub in `TdsConnection::Login()` (T014) with the real SSPI continuation loop. Read `0xED` token → call `NextBytes` → if non-empty, send SSPI Message packet (`PacketType::SSPI = 0x11`) → repeat. Terminate on `LOGINACK` + `DONE` or `ERROR` token.

- [ ] **T023** [US1] Build the containerized KDC in `docker/kerberos/`: `Dockerfile`, `krb5.conf`, `init-kdc.sh`. Register `alice@EXAMPLE.COM` (password `alicepw`) and `MSSQLSvc/sqlserver-krb:1433@EXAMPLE.COM`. Export the SPN key to a keytab mounted into the SQL Server container.

- [ ] **T024** [US1] Add `docker/sql-server-kerberos/init.sql` and a `docker-compose.kerberos.yml` that brings up both KDC and a Kerberos-configured SQL Server. New `Makefile` target `docker-kerberos-up`. CI matrix gets a `MSSQL_KERBEROS_TEST=1` axis.

- [ ] **T025** [P] [US1] Write `test/sql/integrated_auth/krb5_basic.test`. Five cases: ATTACH with `Trusted_Connection=yes`, basic SELECT, JOIN across tables, transaction commit/rollback, `mssql_pool_stats()` after concurrent queries. Tagged `[kerberos]`.

- [ ] **T026** [P] [US3] Write `test/sql/integrated_auth/krb5_keytab.test`. Three cases: ATTACH with keytab + principal, query, error when keytab missing/unreadable. Tagged `[kerberos]`.

- [ ] **T027** [P] [US1] Write `test/sql/integrated_auth/krb5_errors.test`. Four cases: no ccache (post-`kdestroy`); expired ticket (set `KRB5_TIME_REFRESH` to backdated); wrong SPN via `service_principal_name` override; clock skew (set system clock forward). Each verifies the specific error substring from R8.

- [ ] **T028** [P] [US1] Write `test/sql/integrated_auth/trusted_connection_alias.test`. Three ATTACHes — `Trusted_Connection=yes`, `Integrated Security=SSPI`, `Integrated Security=true` — all succeed identically on POSIX.

- [ ] **T029** [P] [US1] Write `test/sql/integrated_auth/conflicts.test`. Three negative cases: `Trusted_Connection=yes;User Id=alice`; `Trusted_Connection=yes` + Azure secret reference; `authenticator=krb5;authenticator=winsspi`. Each must produce a clear ATTACH-time error.

- [ ] **T030** [US4] Write `test/sql/integrated_auth/krb5_raw.test`. Raw-credentials mode: `authenticator=krb5;User Id=alice;Password=alicepw;krb5-realm=EXAMPLE.COM`. Verify query succeeds, and that wrong-password produces the expected `"preauthentication failed"` error.

**Checkpoint**: POSIX Kerberos auth fully functional. v1 MVP shippable for Linux/macOS users.

---

## Phase 4: Windows SSPI backend (P1 story for Windows)

**Purpose**: Functional Integrated Auth on Windows via SSPI. Closes US2. May be deferred to v2 (see spec Open Questions §2).

- [ ] **T031** [US2] Create `src/include/tds/auth/winsspi_authenticator.hpp`. Class `WinSspiAuthenticator : public IAuthenticator`. Private fields: `CredHandle cred_;`, `CtxtHandle ctx_;`, `string spn_;`. **No DuckDB headers.** Guarded by `#if defined(MSSQL_ENABLE_SSPI)`.

- [ ] **T032** [US2] Implement `WinSspiAuthenticator` in `src/tds/auth/winsspi_authenticator.cpp`. Constructor: `AcquireCredentialsHandle(NULL, "Negotiate", SECPKG_CRED_OUTBOUND, ...)`. `InitialBytes()`: `InitializeSecurityContext` with `ISC_REQ_CONFIDENTIALITY | ISC_REQ_REPLAY_DETECT` and target = SPN. `NextBytes`: continuation with the server token in the input buffer. `Free()`: `DeleteSecurityContext` + `FreeCredentialsHandle`. Error path renders status via `FormatMessageA`. ~200 LoC.

- [ ] **T033** [US2] Wire Windows SSPI into `AuthStrategyFactory::Create()` parallel to T021. Branch on `info.auth_method == WINSSPI`. Document that keytab / raw-credentials modes are POSIX-only.

- [ ] **T034** [US2] Windows CI job runs a subset of `test/sql/integrated_auth/` that does not require a KDC (CI Windows runners can't easily join a domain). Run the parser tests (T015) and conflict tests (T029) at minimum. Document the manual test procedure for on-domain Windows hosts in `docs/kerberos.md`.

**Checkpoint**: Windows SSPI lights up. All four target platforms supported.

---

## Phase 5: Documentation

**Purpose**: Update README, add `docs/kerberos.md`, update `docs/architecture.md`. Required for the feature to be discoverable and supportable.

- [ ] **T035** [P] [US5] Update `README.md`:
    - Remove the `Windows Authentication: Only SQL Server authentication is supported` line from the Limitations section (around line 1499).
    - Add a new top-level section "Integrated Authentication (Kerberos / SSPI)" before "Azure AD authentication" with: prerequisite checklist, simplest ATTACH example, link to `docs/kerberos.md`.
    - Add rows to the Key Aliases table for `Trusted_Connection` / `Integrated Security` / `authenticator`.
    - Add rows to the Secret Fields table for `authenticator`, `krb5_keytabfile`, `krb5_realm`, `service_principal_name`.

- [ ] **T036** [P] [US5] Create `docs/kerberos.md`. Mirror the depth of `AZURE.md`. Sections: Table of Contents, Quick Start, Prerequisites (POSIX vs Windows), Authentication Modes (ccache, keytab, raw), Connection Examples (all three forms × all three modes), Using MSSQL Secrets, Troubleshooting (one entry per row of R8's error taxonomy), SPN verification with `setspn -L`, Reference.

- [ ] **T037** [US5] Update `docs/architecture.md` Authentication Strategy Pattern section: add `IAuthenticator` to the diagram, document the layering (existing `AuthenticationStrategy` wraps an optional `IAuthenticator`), list the two new strategies / authenticators. Update the "Authentication Methods" table.

- [ ] **T038** [US5] Add `docs/architecture.md` cross-reference to `docs/kerberos.md` at the bottom of the file.

---

## Phase 6: Polish & Regression

- [ ] **T039** [US5] Run full existing test suite on all four target platforms in CI. **GATE**: all pre-existing tests pass unchanged on all platforms.

- [ ] **T040** [US5] Run `clang-format` on all new and modified `.cpp` and `.hpp` files.

- [ ] **T041** [US5] Manual smoke test on a real on-prem AD environment if available — document the result in the PR description.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 1 (refactor)** has no dependencies — can start immediately. Must merge before Phase 2 begins.
- **Phase 2 (parser + LOGIN7)** depends on Phase 1. Must merge before Phase 3 begins.
- **Phase 3 (POSIX Kerberos)** depends on Phase 2. Self-contained; closes US1, US3, US4.
- **Phase 4 (Windows SSPI)** depends on Phase 2, independent of Phase 3.
- **Phase 5 (Docs)** can begin after Phase 3 in parallel with Phase 4.
- **Phase 6 (Polish)** depends on whichever feature phases are scoped into the release.

### Within Each Phase

- Phase 1: T001 → T002 → T003 → T004 → T005 (serial; T005 is the gate)
- Phase 2: T006 first (build system), then T007–T011 in parallel, then T012–T014, then T015
- Phase 3: T016 → (T017, T018, T019 serial in same file) → T020 → T021 → T022; T023+T024 (Docker) in parallel; then T025–T030 tests in parallel
- Phase 4: T031 → T032 → T033 → T034

### Parallel Opportunities (within a developer or across developers)

- T007, T008, T011 (different files, all touch parsing) can be split across two engineers.
- T025, T026, T027, T028, T029, T030 (six test files) are all independent — full fan-out.
- T035, T036 (README + docs/kerberos.md) are independent docs writers.

---

## Implementation Strategy

### MVP-1 (POSIX Kerberos only — minimum shippable)

1. Land Phase 1 as PR 1 (refactor).
2. Land Phase 2 as PR 2 (parser + LOGIN7 wiring).
3. Land Phase 3 as PR 3 (POSIX Kerberos backend + tests).
4. Land Phase 5's T035 + T036 as PR 4 (docs).
5. **Stop here for v1.** Windows users still see the "not yet implemented" error on `Trusted_Connection=yes`; document this in the README.

### MVP-2 (POSIX + Windows)

6. Add Phase 4 as PR 5.
7. Update README/docs.

### Incremental delivery

Each PR is independently mergeable. The connection-string parser changes in Phase 2 are useful on their own (validation prevents footguns) even if backends aren't ready. The refactor in Phase 1 is a pure structural improvement and could merge regardless.

---

## Notes

- [P] tasks = different files, no dependencies
- [Story] label maps task to user story (US1 POSIX cred-cache, US2 Windows SSPI, US3 keytab, US4 raw, US5 regression/docs)
- Total: 41 tasks (5 refactor + 10 parser/wiring + 15 POSIX + 4 Windows + 4 docs + 3 polish)
- The single largest task is T017 (`Krb5Authenticator::InitialBytes` — ~250 LoC). All others are ≤200 LoC.
- Commit after each numbered task. Each PR groups tasks per the migration order in `plan.md`.

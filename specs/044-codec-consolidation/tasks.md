---

description: "Task list for spec 044 UTF-16 Codec Consolidation"
---

# Tasks: UTF-16 Codec Consolidation

**Input**: Design documents from `/specs/044-codec-consolidation/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md (all present)

**Tests**: A new SQLLogicTest regression
(`test/sql/copy/copy_to_nvarchar_unicode.test`) and a new C++
microbenchmark (`test/cpp/bench_utf16.cpp`) are explicitly required
by the spec (FR-020..FR-024, FR-041..FR-043). They are written
alongside their associated implementation tasks (not TDD-style
write-tests-first), since the migration's correctness contract is
bit-identity inherited from spec 043 rather than new behavior.

**Organization**: Tasks are grouped by user story to enable
independent implementation and review. US3 (legacy retirement +
rename pass) has an explicit dependency on US1, US2, US4 being
complete first — documented below.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to ([US1]..[US5])
- File paths are exact and absolute to the repository root

## Path Conventions

- DuckDB extension: source under `src/`, headers under `src/include/`
  mirroring the `src/` layout, tests under `test/cpp/` (C++) and
  `test/sql/` (SQLLogicTest)
- This spec adds one new directory `test/bench/` for the
  shell-driven end-to-end benchmark

---

## Phase 1: Setup

**Purpose**: Repository preparation; no source code changes.

- [X] T001 Create new directory `test/bench/` at repository root (will hold `bench_codec_e2e.sh` in US5). Verify `test/cpp/` and `test/sql/copy/` already exist (they do).
- [X] T002 Verify pre-migration build green on the current branch base: run `GEN=ninja make`, `GEN=ninja make test`, and `GEN=ninja make test-login7-encoding`; all three MUST pass before any source-code task starts.
- [ ] T003 [P] **DEFERRED to a separate US5 session.** Kick off a long-running background build of the **spec-043 baseline binary** in a separate worktree at commit `5db82e3` (per quickstart.md Checkpoint 3.2). This binary is needed later for US5; the build takes ~10-20 minutes including vcpkg fetch, so start it now and let it complete while implementation proceeds. Command: `git worktree add ../mssql-043-baseline 5db82e3 && cd ../mssql-043-baseline && GEN=ninja make vcpkg-setup && GEN=ninja make`. Record the resulting binary path for later use as `$BASELINE_DUCKDB`.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: There are no blocking foundational changes — every user story is one or a few file edits on top of the existing simdutf wrapper from spec 043. This phase intentionally has no tasks.

**Checkpoint**: Foundation already in place (spec 043 merged). User-story work can start immediately after Setup.

---

## Phase 3: User Story 1 — NVARCHAR scan decode unified on the SIMD path (Priority: P1) 🎯 MVP

**Goal**: The hottest UTF-16 consumer in the extension (per-row NVARCHAR scan decode in `TypeConverter::ConvertString`) routes through the simdutf-backed wrapper. Every `SELECT` over an NVARCHAR/NCHAR/XML column flows through one unified decode path.

**Independent Test**: Existing SQL scan tests in `test/sql/query/`, `test/sql/catalog/`, and `test/sql/integration/` all pass against the post-task build; output strings byte-identical to a pre-task build for any mixed-Unicode payload.

### Implementation for User Story 1

- [X] T004 [US1] Migrate the NVARCHAR/NCHAR/XML scan decode call site: in `src/tds/encoding/type_converter.cpp` change `Utf16LEDecode(value.data(), value.size())` (line 424) to `SimdutfUtf16LEDecode(value.data(), value.size())`. Update the file's `#include` directives: replace or add `#include "tds/encoding/simdutf_wrappers.hpp"` (keep `utf16.hpp` if other symbols are still needed; spec 044 Phase B will sweep them later).
- [X] T005 [US1] Build verification: run `GEN=ninja make` and confirm the build is green on the current platform. No new compiler warnings introduced by the change.
- [X] T006 [US1] Regression verification: run `GEN=ninja make test-login7-encoding` (in-memory bit-identity check) and `GEN=ninja make test` (full SQL test suite). All previously-green tests MUST remain green.
- [X] T007 [US1] Integration verification: with `make docker-up` running, execute `GEN=ninja make integration-test`. The existing scan / catalog / query test suites all exercise NVARCHAR cells; all MUST remain green.

**Checkpoint**: User Story 1 complete. The single hottest UTF-16 hot path is unified on simdutf. Buildable, testable, deployable as an incremental win on its own.

---

## Phase 4: User Story 2 — NVARCHAR BCP write encode unified on the SIMD path (Priority: P1)

**Goal**: The write-side counterpart of US1 — every BCP per-cell encode of NVARCHAR values goes through simdutf. Plus a new SQLLogicTest regression that round-trips mixed-Unicode NVARCHAR via `COPY TO MSSQL` and asserts byte-identical retrieval.

**Independent Test**: New `test/sql/copy/copy_to_nvarchar_unicode.test` passes against the post-task build; existing `test/sql/copy/` and `test/sql/ctas/` suites pass unchanged.

### Implementation for User Story 2

- [X] T008 [US2] Migrate the BCP row encoder's three call sites: in `src/tds/encoding/bcp_row_encoder.cpp` change `Utf16LEEncodeDirect(...)` → `SimdutfUtf16LEEncodeDirect(...)` at lines 407 and 477, and `Utf16LEEncode(str.GetString())` → `SimdutfUtf16LEEncode(str.GetString())` at line 696. Update `#include` to add `"tds/encoding/simdutf_wrappers.hpp"` if not already present.
- [X] T009 [US2] Migrate the BCP writer's encode call site: in `src/copy/bcp_writer.cpp` change `tds::encoding::Utf16LEEncode(str)` → `tds::encoding::SimdutfUtf16LEEncode(str)` at line 615. (The decode call site at line 208 is handled in US3.) Update `#include` to add `"tds/encoding/simdutf_wrappers.hpp"`.
- [X] T010 [US2] Build verification: run `GEN=ninja make`; green build required.
- [X] T011 [P] [US2] Write the new regression test `test/sql/copy/copy_to_nvarchar_unicode.test` per data-model.md F2: ATTACH the MSSQL test database, CREATE TABLE with NVARCHAR(200) column, INSERT (or COPY FROM duckdb-source) rows containing ASCII, Cyrillic, accented Latin, CJK ideograph, and emoji (surrogate pair) values, then `SELECT` and assert byte-identical retrieval ordered by id. Use the project's standard ENV-driven secret pattern (look at `test/sql/integration/non_ascii_password.test` from spec 043 as the template). File group tags: `# group: [copy] [integration]`.
- [X] T012 [US2] Integration verification: with `make docker-up` running, execute `GEN=ninja make integration-test`. The new `copy_to_nvarchar_unicode.test` MUST pass; existing `test/sql/copy/`, `test/sql/ctas/`, and `test/sql/insert/` tests MUST remain green.

**Checkpoint**: User Story 2 complete. BCP write hot path unified. New regression test in place. P1 user-visible work done.

---

## Phase 5: User Story 4 — Azure / FedAuth token encoding through the SIMD wrapper (Priority: P2)

**Goal**: The three Azure-AD-related FedAuth call sites go through simdutf. JWT access tokens are ASCII so no behavior change is expected; the migration's value here is **single-source consolidation** and **a clean rebase surface against spec 042**.

**Independent Test**: Existing `test/sql/azure/` suite passes unchanged; `mssql_azure_auth_test()` continues to acquire tokens against the configured tenant.

**Phase ordering**: We place US4 before US3 because (a) US4 is fully independent of every other story, and (b) US3 (legacy retirement + rename pass) MUST come after all migration tasks are done — including US4's three. Running US4 before US3 keeps US3's rename pass strictly mechanical with no still-open migration work.

### Implementation for User Story 4

- [X] T013 [P] [US4] Migrate `src/azure/azure_fedauth.cpp`: change `tds::encoding::Utf16LEEncode(token_utf8)` → `tds::encoding::SimdutfUtf16LEEncode(token_utf8)` at line 28. Update `#include` to add `"tds/encoding/simdutf_wrappers.hpp"`.
- [X] T014 [P] [US4] Migrate `src/tds/auth/fedauth_strategy.cpp`: change `encoding::Utf16LEEncode(access_token)` → `encoding::SimdutfUtf16LEEncode(access_token)` at line 54. Update `#include`. **Coordinate with spec 042**: if spec 042 has restructured this file by the time this task runs, the one-line substitution still applies on top — rebase, re-apply, no surrounding refactor.
- [X] T015 [P] [US4] Migrate `src/tds/auth/manual_token_strategy.cpp`: change `encoding::Utf16LEEncode(access_token)` → `encoding::SimdutfUtf16LEEncode(access_token)` at line 34. Update `#include`. Same spec-042 coordination as T014.
- [X] T016 [US4] Build verification: run `GEN=ninja make`; green build required.
- [X] T017 [US4] Integration verification: with `make docker-up` running (or against a real Azure tenant if available), execute the Azure auth test suite (`test/sql/azure/`). All previously-green tests MUST remain green.

**Checkpoint**: User Story 4 complete. Auth-layer migration done. Spec 042 rebase surface is one substituted line per file across three files — strictly mechanical.

---

## Phase 6: User Story 3 — Tail call sites unified + legacy retirement (Priority: P2)

**Goal**: Migrate the remaining tail of UTF-16 call sites (BCP error decode, simple-query, ENVCHANGE/INFO/ERROR token decode, COLMETADATA decode, EncodePassword helper, SQL_BATCH text encode), then execute the **Phase B rename**: fold the simdutf wrapper into `utf16.{hpp,cpp}`, retire the legacy hand-rolled converter to a private anonymous-namespace fallback, drop the `Simdutf` prefix from public symbols.

**Dependencies**: US1 (T004-T007), US2 (T008-T012), US4 (T013-T017) MUST all be complete before T018 starts. The rename pass (T024-T029) consolidates *every* migrated call site at once.

**Independent Test**: After all rename tasks: `grep -rn 'SimdutfUtf16LE' src/` returns zero matches; `find src -name 'simdutf_wrappers*'` returns empty; full test suite green; `src/include/tds/encoding/utf16.hpp` exposes `Utf16LE*` symbols backed by the simdutf implementation.

### Implementation for User Story 3 — Phase A (tail call-site migration)

- [X] T018 [US3] Migrate `src/copy/bcp_writer.cpp` decode side: change `tds::encoding::Utf16LEDecode(&response[pos], msg_len * 2)` → `tds::encoding::SimdutfUtf16LEDecode(...)` at line 208 (BCP error message decode).
- [X] T019 [P] [US3] Migrate `src/tds/tds_protocol.cpp` at three call sites: line 197 (standalone `EncodePassword` helper — still in the file even though the LOGIN7 builder uses the spec-043 `EncodeLogin7VarField` helper; this legacy helper has its own consumers), line 751 and line 762 (SQL_BATCH text encoding). All three change `encoding::Utf16LEEncode(...)` → `encoding::SimdutfUtf16LEEncode(...)`. Update `#include`.
- [X] T020 [P] [US3] Migrate `src/tds/tds_column_metadata.cpp`: change `encoding::Utf16LEDecode(data + offset, byte_length)` → `encoding::SimdutfUtf16LEDecode(...)` at line 406 (COLMETADATA column name decode). Update `#include`.
- [X] T021 [P] [US3] Migrate `src/tds/tds_token_parser.cpp` at two call sites: line 337 and line 350 (ENVCHANGE/INFO/ERROR UTF-16LE field decode). Both change `encoding::Utf16LEDecode(...)` → `encoding::SimdutfUtf16LEDecode(...)`. Update `#include`.
- [X] T022 [P] [US3] Migrate `src/query/mssql_simple_query.cpp`: change `tds::encoding::Utf16LEDecode(value.data(), value.size())` → `tds::encoding::SimdutfUtf16LEDecode(...)` at line 43. Update `#include`.
- [X] T023 [US3] Build + integration verification after tail migration: run `GEN=ninja make` and `GEN=ninja make integration-test`. All MUST be green before the rename pass begins.

### Implementation for User Story 3 — Phase B (rename pass)

T024-T029 are a single logical commit, executed in order. After every step in this group, the codebase must be in a buildable state — but conventionally the commit is squashed/landed as one mechanical rename.

- [X] T024 [US3] Fold the simdutf wrapper into `utf16.{hpp,cpp}`. Concrete steps: (a) replace the contents of `src/include/tds/encoding/utf16.hpp` with the post-rename header per `contracts/utf16_post_rename.hpp` (signature-identical to the legacy public API; symbols `Utf16LE*` without prefix; optional `tds::encoding::testing::LegacyUtf16LE*` re-export gated by `#ifdef MSSQL_BENCH_BUILD`). (b) Replace the contents of `src/tds/encoding/utf16.cpp` with the simdutf-backed implementations (move the body of `simdutf_wrappers.cpp` here, drop the `Simdutf` prefix on public symbols, rename the legacy hand-rolled functions to `LegacyUtf16LE*` inside an anonymous namespace, wire the public functions' invalid-input fallback to call the private legacy helpers, add the `#ifdef MSSQL_BENCH_BUILD` re-export block per research R3).
- [X] T025 [US3] Delete the wrapper files: `rm src/include/tds/encoding/simdutf_wrappers.hpp src/tds/encoding/simdutf_wrappers.cpp`.
- [X] T026 [US3] Update `CMakeLists.txt`: remove `src/tds/encoding/simdutf_wrappers.cpp` from `EXTENSION_SOURCES` (currently line 61). The `src/tds/encoding/utf16.cpp` entry stays (line 60 of the pre-rename file) — same path, new content.
- [X] T027 [US3] Update `Makefile` `LOGIN7_TEST_SOURCES` (lines 175-180): remove the `src/tds/encoding/simdutf_wrappers.cpp` entry; `src/tds/encoding/utf16.cpp` stays.
- [X] T028 [US3] Sweep every call site back to `Utf16LE*` (no `Simdutf` prefix). Affected files: all migrated in T004, T008, T009, T013, T014, T015, T018, T019, T020, T021, T022 (16 call sites total across 10 files) plus the two pre-existing spec-043 consumers `src/tds/tds_packet.cpp:72` (`SimdutfUtf16LEEncode(str)` → `Utf16LEEncode(str)`) and `src/tds/tds_protocol.cpp:68` (`encoding::SimdutfUtf16LEEncode(utf8_text)` → `encoding::Utf16LEEncode(utf8_text)`). Also sweep `#include "tds/encoding/simdutf_wrappers.hpp"` lines back to `#include "tds/encoding/utf16.hpp"` where files no longer need both.
- [X] T029 [US3] Update `test/cpp/test_login7_encoding.cpp` if it references `encoding::SimdutfUtf16LE*` symbols directly (it likely does, since spec 043 added the bit-identity assertions): sweep those to `encoding::Utf16LE*` (the renamed public symbols still validate the same simdutf implementation).
- [X] T030 [US3] Build verification post-rename: run `GEN=ninja make`. Build MUST be green.
- [X] T031 [US3] Test verification post-rename: run `GEN=ninja make test`, `GEN=ninja make test-login7-encoding`, and `GEN=ninja make integration-test`. All MUST be green.
- [X] T032 [US3] Spec-compliance verification: from the repo root, run the three audit commands and verify expected results:
  - `grep -rn 'SimdutfUtf16LE' src/` → zero matches
  - `find src -name 'simdutf_wrappers*'` → zero matches
  - `grep -rn '\bUtf16LE\(Encode\|Decode\|EncodeDirect\|ByteLength\)' src/ | grep -v 'src/tds/encoding/utf16.cpp' | grep -v 'src/include/tds/encoding/utf16.hpp'` → exactly 18 call-site matches (the 16 migrated + 2 spec-043 consumers from T028)

**Checkpoint**: User Story 3 complete. Legacy converter retired from the public surface; all call sites flow through one unified simdutf-backed wrapper; spec compliance audit clean.

---

## Phase 7: User Story 5 — End-to-end before/after measurement on the integration SQL Server (Priority: P2)

**Goal**: Capture real before/after numbers for the migration on the Docker SQL Server image used by `make integration-test`. Produce both raw output files and the human-readable `bench_results.md` summary.

**Dependencies**: T033 (microbenchmark code + Makefile target) depends on T024 (rename pass) because the bench's `tds::encoding::testing::LegacyUtf16LE*` re-export only exists post-rename. T037/T038 (e2e benchmark script) is build-independent — can be written any time. T034/T039/T040 (running the benchmarks) depend on the rest of US3 being merged into the spec-044 PR branch and the baseline binary from T003 being available.

**Independent Test**: `make bench-utf16` reports 13/13 fixtures byte-identical and within the 1.10× perf floor. The e2e script runs end-to-end on both binaries; ratios in `bench_results.md` show no per-step regression > 10%.

### Implementation for User Story 5

- [ ] T033 [P] [US5] Write the codec microbenchmark `test/cpp/bench_utf16.cpp` per data-model.md F1 and quickstart.md Checkpoint 2. Includes the 13-fixture set (ASCII × 3 sizes, BMP × 3, CJK × 3, emoji × 3, mixed × 1), warm-up of 100 iterations, measured run of 1000 iterations for ≤256-byte fixtures and 100 iterations for 64-KB fixtures, median-of-iterations timing, per-fixture byte-identity check (`Utf16LE*` simdutf output vs `tds::encoding::testing::LegacyUtf16LE*` legacy output), per-fixture ratio assertion (simdutf time ≤ 1.10× legacy time). Output formatted per quickstart Checkpoint 2's example.
- [ ] T034 [US5] Add the `bench-utf16` Makefile target. Pattern follows the existing `test-login7-encoding` target (Makefile:189-203). Key differences: compile `bench_utf16.cpp` with `-DMSSQL_BENCH_BUILD` so the test-only `tds::encoding::testing::` re-export is visible; link against `src/tds/encoding/utf16.cpp` directly (the post-rename TU that contains both simdutf-backed publics and the private legacy fallback). The target MUST NOT run as part of `make test` or `make test-all` (FR-024 / FR-056).
- [ ] T035 [US5] Run `GEN=ninja make bench-utf16`. Verify the binary builds cleanly. Run `./build/test/bench_utf16` and verify 13/13 fixtures pass (byte-identical + ratio ≤ 1.10×). Record local-host numbers as informational evidence; do not commit them.
- [ ] T036 [P] [US5] Write `test/bench/bench_codec_e2e.sh` per research.md R5 and spec User Story 5. Bash script. Reads env vars `MSSQL_BENCH_DUCKDB_BIN`, `MSSQL_TEST_HOST`/`PORT`/`USER`/`PASS`/`DB`, `MSSQL_BENCH_ROW_COUNT` (default 100000000), `MSSQL_BENCH_OUTPUT` (default `/tmp/bench_codec_e2e_$(date +%s).txt`). Pre-flight: connect via the binary, run `SELECT 1`, abort with clear message if SQL Server unreachable. Idempotent cleanup of target tables on entry and exit (FR-053). Six steps, each bracketed by `date +%s.%N` reads. Output one `step_name<TAB>seconds<TAB>row_count<TAB>notes` line per step. The script MUST be 100 LOC or less. Make it executable (`chmod +x`).
- [ ] T037 [US5] Smoke-test the e2e script against the spec-044 PR-head build with `MSSQL_BENCH_ROW_COUNT=1000000` (~30-60 second run). Verify all six steps complete and emit timing lines. This is developer verification only; output is NOT committed.
- [ ] T038 [US5] Capture the **baseline** end-to-end run. Prerequisite: T003's `BASELINE_DUCKDB` binary built and ready. Command: `MSSQL_BENCH_DUCKDB_BIN="$BASELINE_DUCKDB" MSSQL_BENCH_OUTPUT=specs/044-codec-consolidation/bench_results_baseline.txt bash test/bench/bench_codec_e2e.sh`. Wall-clock 10-30 minutes. The container MUST have ≥25 GB free storage (per research R6); operator verifies before kickoff. Output file committed at the listed path.
- [ ] T039 [US5] Capture the **spec-044** end-to-end run **on the same host, within minutes of T038**, against the spec-044 PR-head binary. Command: `MSSQL_BENCH_DUCKDB_BIN=$(pwd)/build/release/duckdb MSSQL_BENCH_OUTPUT=specs/044-codec-consolidation/bench_results_spec044.txt bash test/bench/bench_codec_e2e.sh`. Re-run T038 if T039 has to wait > 1 hour (host-state drift; per FR-054 "single contiguous capture window").
- [ ] T040 [US5] Write `specs/044-codec-consolidation/bench_results.md` per data-model.md A3 and FR-055. Required sections: Host (CPU, cores, OS, RAM, Docker version, SQL Server image tag), Container budget (CPU/RAM/storage limits), Binaries (commit SHAs for both, vcpkg triplet, build flags), Per-step results (markdown table with step name, baseline seconds, spec-044 seconds, ratio, row count, notes), Prose summary (which steps improved, regressions if any, magnitude of largest delta), Reproducibility (exact env vars and CLI for both runs). Commit alongside the two `.txt` files.
- [ ] T041 [US5] Verify SC-009 / SC-010 / SC-011: all six workflow steps completed on both binaries (SC-009); every per-step ratio is ≤ 1.10× (SC-010); the markdown artifact `bench_results.md` is present with all required sections (SC-011). If any step ratio exceeds 1.10×, INVESTIGATE before merging — investigate root cause, document in `bench_results.md` prose section, and decide whether to re-run T038+T039 or accept the regression with rationale.

**Checkpoint**: User Story 5 complete. The performance story for spec 044 is captured as a falsifiable artifact in the spec directory. Spec 044's PR description can cite specific numbers; the v0.2.0 release notes can cite the same.

---

## Phase 8: Polish & Cross-Cutting Concerns

**Purpose**: Final hygiene, documentation, PR preparation.

- [ ] T042 [P] Final full-suite test sweep: from a clean repo state on the spec-044 branch, run `GEN=ninja make clean && GEN=ninja make && GEN=ninja make test-all`. All tests green.
- [ ] T043 [P] Verify the existing `test/cpp/test_login7_encoding.cpp` (spec 043) still bit-identity-passes against the post-rename `utf16.hpp`. Already covered by T031 + T042; this is a final sign-off check.
- [ ] T044 [P] Walk through `quickstart.md` end-to-end on a fresh worktree to verify every documented command works as written. Update `quickstart.md` if any command is stale or wrong.
- [ ] T044a [P] Format every modified or new file with `clang-format-14` (NOT a higher version — CI Lint fails on diffs). Targets: the 10 modified production source files (`src/azure/azure_fedauth.cpp`, `src/copy/bcp_writer.cpp`, `src/query/mssql_simple_query.cpp`, `src/tds/auth/fedauth_strategy.cpp`, `src/tds/auth/manual_token_strategy.cpp`, `src/tds/encoding/bcp_row_encoder.cpp`, `src/tds/encoding/type_converter.cpp`, `src/tds/tds_column_metadata.cpp`, `src/tds/tds_protocol.cpp`, `src/tds/tds_token_parser.cpp`), the post-rename wrapper files (`src/include/tds/encoding/utf16.hpp`, `src/tds/encoding/utf16.cpp`), and the new test files (`test/cpp/bench_utf16.cpp`, `test/sql/copy/copy_to_nvarchar_unicode.test` — note: `.test` files are SQLLogicTest, not C++, so skip those). On macOS use `/opt/homebrew/opt/llvm@14/bin/clang-format -i <file>` (install via `brew install llvm@14`). Confirm the resulting diff is whitespace-only.
- [ ] T045 [P] Update `README.md` if it has a "Recent Changes" or "Performance" section that references the codec layer (it likely does not at the spec-043 baseline; check first). No change is expected to the README's body or the Third-Party Licenses section (simdutf attribution already lives there from spec 043).
- [ ] T046 Update `CLAUDE.md` "Active Technologies" section: confirm the spec-044 entry that was added by `/speckit-plan` is accurate. No new tech added beyond what 043 introduced — the line should read approximately "044-codec-consolidation: Completes the simdutf migration started in 043; consolidates UTF-16 conversion behind one symbol set. No new vcpkg deps."
- [ ] T047 Draft the spec-044 PR description. Required content: (a) one-paragraph summary of the migration scope, (b) explicit flag for spec-042 (collaborator) coordination — list the three auth files affected and the rebase strategy from research R9, (c) link to `specs/044-codec-consolidation/bench_results.md`, (d) list of the 16 migrated call sites + the rename pass, (e) acknowledgment that no `vcpkg.json` or extension-version changes are included (version bump is the v0.2.0 release engineer's call), (f) one-line confirmation that all modified files passed `clang-format-14` per T044a. Acknowledge that merge is gated on CI green across all four platforms (Linux GCC, macOS Clang, Windows MSVC, Windows MinGW) per SC-007.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No external dependencies. T003 kicks off in background and continues through later phases.
- **Foundational (Phase 2)**: empty — no foundational tasks.
- **US1 (Phase 3)**: No dependencies. Can start immediately after Phase 1.
- **US2 (Phase 4)**: No dependencies on US1 (different files). Can run in parallel with US1 if staffed.
- **US4 (Phase 5)**: No dependencies on US1/US2. Can run in parallel.
- **US3 (Phase 6)**: **Depends on US1, US2, US4 being complete** — the rename pass (T024-T029) consolidates every migrated call site at once and must come after all migrations are done. T018-T023 (tail call-site migrations) are independent of US1/US2/US4 and could in principle start earlier, but the spec-compliance audit T032 still requires the rename pass at the end.
- **US5 (Phase 7)**: T033-T035 (microbenchmark) depends on US3's rename pass (T024) because the `tds::encoding::testing::LegacyUtf16LE*` re-export only exists post-rename. T036-T037 (e2e script) is build-independent. T038-T041 (runs + summary) depend on the spec-044 build being at PR-head state (i.e., post-US3-merge into the spec-044 PR branch).
- **Polish (Phase 8)**: Depends on US1-US5 complete.

### Within Each User Story

- Within US1, US2, US4: migration → build verify → test verify. Strict sequence.
- Within US3: tail migration → tail verify → rename pass (T024-T029 as one atomic commit) → post-rename verify → audit.
- Within US5: microbenchmark code → bench Makefile target → bench smoke-run; in parallel, e2e script → smoke; then baseline run → spec-044 run → summary artifact → SC verification.

### Parallel Opportunities

- US1 and US2 and US4 can be developed in parallel (three different file clusters; no shared state). If staffed by 1 person, sequence is fine; if 2+ people, parallelize.
- T013, T014, T015 (the three US4 auth-file edits) are independent and can be done in any order.
- T018, T019, T020, T021, T022 (US3 Phase A tail-call-site migrations) are independent of each other (five different files) and can be parallelized.
- T024-T029 (US3 Phase B rename) is one atomic logical commit and must NOT be parallelized.
- T033 (microbench code) and T036 (e2e script code) can be written in parallel — different files, different languages.
- Polish tasks T042-T045 are independent and can be parallelized.

---

## Parallel Example: User Stories 1, 2, 4 (P1/P1/P2 parallel start)

```bash
# After Phase 1 setup, three independent migration streams can start simultaneously:

# Stream A (US1): NVARCHAR scan decode
Task: "Migrate Utf16LEDecode → SimdutfUtf16LEDecode in src/tds/encoding/type_converter.cpp:424 plus #include"

# Stream B (US2): NVARCHAR BCP encode
Task: "Migrate three call sites in src/tds/encoding/bcp_row_encoder.cpp (lines 407, 477, 696) and one in src/copy/bcp_writer.cpp:615; add #include"

# Stream C (US4): Azure / FedAuth
Task: "Migrate src/azure/azure_fedauth.cpp:28, src/tds/auth/fedauth_strategy.cpp:54, src/tds/auth/manual_token_strategy.cpp:34 — single-line substitutions plus #include"
```

After all three streams reach their verification tasks (T007, T012, T017), US3 (Phase 6) becomes unblocked.

## Parallel Example: User Story 3 Phase A tail-migration

```bash
# T018 covers bcp_writer.cpp:208 (the one site there that US2 didn't touch).
# T019-T022 are independent. Five streams in parallel after T018:

Task: "Migrate src/tds/tds_protocol.cpp at lines 197, 751, 762"
Task: "Migrate src/tds/tds_column_metadata.cpp:406"
Task: "Migrate src/tds/tds_token_parser.cpp at lines 337, 350"
Task: "Migrate src/query/mssql_simple_query.cpp:43"
```

All five complete before T023 (verification) and T024 (rename pass).

---

## Implementation Strategy

### MVP First (User Story 1 Only)

The minimal landable slice is User Story 1: the NVARCHAR scan decode hot path. After T001-T007 complete, the extension already has a measurable win — the single hottest UTF-16 consumer flows through simdutf, observable on any non-trivial `SELECT` over an NVARCHAR table. This slice would NOT be a viable shipping spec 044 PR (the spec contract requires every call site to migrate plus the rename pass), but it IS a viable MVP smoke and a natural break point if the spec needs to be split.

### Incremental Delivery within One PR

1. Phase 1 (Setup) + Phase 2 (no tasks).
2. Phase 3 (US1) → commit → verify scan path.
3. Phase 4 (US2) → commit → verify BCP write path + new regression test.
4. Phase 5 (US4) → commit → verify Azure auth.
5. Phase 6 (US3) Phase A (T018-T023) → commit → verify tail.
6. Phase 6 (US3) Phase B (T024-T032) → single rename commit → verify post-rename state.
7. Phase 7 (US5) → write benchmarks → run captures → commit `bench_results*` artifacts.
8. Phase 8 (Polish) → final sweep + PR description draft.

Each step leaves the repo in a buildable, test-green state. The PR head at the end of Phase 8 is the final shipping state.

### Parallel Team Strategy

With two developers:

- Dev A: US1 (Phase 3) → US3 Phase A start (T018) → US3 Phase B (T024-T029 as atomic commit, coordinated with Dev B)
- Dev B: US2 (Phase 4) + new regression test → US4 (Phase 5) → US5 benchmark code (T033, T036)
- Joint: T038-T040 (e2e capture) on one host
- Joint: Phase 8 polish + PR description

With one developer: sequential as above; each user story is a one-or-two-evening unit.

---

## Notes

- The spec deliberately combines code migration AND benchmark capture in one PR. Splitting the e2e benchmark into a follow-up PR was rejected during specification because (a) the benchmark needs both binaries available simultaneously and (b) leaving the perf claim unverified in a follow-up risks indefinite slip.
- T038-T040 produce committed artifacts (`bench_results_baseline.txt`, `bench_results_spec044.txt`, `bench_results.md`). Reviewers can interrogate these directly; the spec's performance claim is falsifiable from the diff.
- The two-commit Phase B rename strategy (research R8) keeps the rename atomic; reviewers should expect one large mechanical commit at the PR head plus per-file migration commits before it.
- Spec 042 (collaborator) coordination: the three auth-layer files (T013, T014, T015) are one-line substitutions per file by design. If spec 042 lands first, rebase spec 044 on top — the substitutions re-apply on whatever structure 042 leaves behind. If spec 044 lands first, spec 042 inherits the migrated and renamed wrapper transparently.
- Avoid: bundling unrelated refactors into any migration commit (every commit must be a one-line-per-file substitution or an atomic rename — nothing else); skipping the spec-compliance audit (T032) because "the build is green"; capturing baseline + spec-044 e2e runs on different hosts (host-state drift would invalidate the ratio).

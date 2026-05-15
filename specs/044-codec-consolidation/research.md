# Research: UTF-16 Codec Consolidation (spec 044)

Resolved questions and design decisions for the migration. All
items below are decision-locked; no `NEEDS CLARIFICATION` markers
remain in the plan or spec.

## R1. Migration order: one PR or split?

**Decision**: One PR, two visible commit phases inside it.

- **Phase A** (multi-commit, one commit per file or small file
  cluster): every call site of `encoding::Utf16LE*` migrated to
  `encoding::SimdutfUtf16LE*`. After each commit the build is
  green and the test suite passes (the two implementations
  coexist via distinct symbol names per spec 043's design).
- **Phase B** (single mechanical commit at PR head): rename the
  wrapper files (`simdutf_wrappers.{hpp,cpp}` →
  `utf16.{hpp,cpp}`), delete the old `utf16.{hpp,cpp}`, fold the
  legacy hand-rolled implementations into the new `utf16.cpp` as
  anonymous-namespace private helpers, drop the `Simdutf` prefix
  from public symbols, sweep call sites back to `Utf16LE*` names.

**Rationale**:
- Each Phase-A commit is reviewable as a one-file/one-symbol
  change. Reviewers can verify "this call site now uses the
  simdutf-backed wrapper" without scrolling.
- The Phase-B commit is mechanical (search-and-replace,
  file rename, source-list shuffle). Splitting it across files
  would leave the codebase in a transitional half-renamed state
  for the duration of multiple reviews — strictly worse than one
  atomic switch.
- One PR (not two) keeps the migration coherent. Spec 044's
  contract — "every call site flows through one
  simdutf-backed wrapper, named `Utf16LE*`" — only holds at PR
  HEAD, not at any intermediate commit. Splitting into two PRs
  would leave the symbol prefix in a public-facing
  half-state on `main` between merges.

**Alternatives considered**:
- *Two separate PRs* (PR1: migrate call sites; PR2: rename).
  Rejected: leaves `main` with `SimdutfUtf16LE*` as the public
  encoding API for the duration of PR2's review, which would
  invite consumers to depend on the transitional name.
- *Single squashed commit*. Rejected: loses the per-file
  reviewability of Phase A. The migration has sixteen call sites
  in ten files; a single commit's diff would be hard to audit.

## R2. Migration order within Phase A

**Decision**: Migrate in this order (each step independently
green; tests pass after every commit):

1. `src/tds/encoding/type_converter.cpp` (NVARCHAR scan decode —
   single largest hot path, single line change)
2. `src/tds/encoding/bcp_row_encoder.cpp` (BCP per-cell encode —
   three lines)
3. `src/copy/bcp_writer.cpp` (BCP header/control: encode + error
   decode — two lines)
4. `src/tds/tds_protocol.cpp` (SQL_BATCH encode + standalone
   EncodePassword — three lines)
5. `src/tds/tds_column_metadata.cpp` (COLMETADATA name decode —
   one line)
6. `src/tds/tds_token_parser.cpp` (ENVCHANGE / INFO / ERROR
   field decode — two lines)
7. `src/query/mssql_simple_query.cpp` (simple query result
   decode — one line)
8. `src/azure/azure_fedauth.cpp` (Azure FedAuth token encode —
   one line)
9. `src/tds/auth/fedauth_strategy.cpp` (FedAuth strategy token
   encode — one line)
10. `src/tds/auth/manual_token_strategy.cpp` (manual token
    strategy encode — one line)

**Rationale**: Order is "biggest impact first, smallest blast
radius last." If a regression surfaces during Phase A, the
implementer narrows it to a small recent commit. Auth-layer
files come last because they coordinate with spec 042
(collaborator) and we want them merged as one cluster against
042's rebase target.

**Alternatives considered**: alphabetical-by-file. Rejected as
review-unhelpful.

## R3. Test-only visibility for the private legacy fallback

**Decision**: Use an anonymous namespace plus a
`#ifdef MSSQL_BENCH_BUILD` test-only header that re-exposes the
private helpers under a `tds::encoding::testing::` namespace, gated
by a CMake-controlled compile-flag.

```cpp
// src/tds/encoding/utf16.cpp (post-rename)
namespace duckdb {
namespace tds {
namespace encoding {
namespace {  // anonymous

std::vector<uint8_t> LegacyUtf16LEEncode(const std::string& input);
std::string LegacyUtf16LEDecode(const uint8_t* data, size_t byte_length);
size_t LegacyUtf16LEEncodeDirect(const char*, size_t, uint8_t*);
size_t LegacyUtf16LEByteLength(const std::string&);

}  // anonymous namespace

// Public simdutf-backed functions...

#ifdef MSSQL_BENCH_BUILD
namespace testing {
// Re-exposed for the bench_utf16.cpp microbenchmark only.
std::vector<uint8_t> LegacyUtf16LEEncode(const std::string &input) {
    return ::duckdb::tds::encoding::LegacyUtf16LEEncode(input);
}
// ... and the other three.
}
#endif

}}}  // namespaces
```

The microbenchmark TU compiles with `-DMSSQL_BENCH_BUILD`, links
against `utf16.cpp` directly (not the full extension `.so`), and
imports both the public `encoding::Utf16LE*` symbols and the
test-only `encoding::testing::LegacyUtf16LE*` symbols. The
production extension is built without `MSSQL_BENCH_BUILD`, so the
test-only namespace is invisible to consumers.

**Rationale**:
- Keeps the private implementation private in production.
- Avoids `friend` declarations leaking out of the wrapper TU.
- The `make bench-utf16` Makefile target sets
  `CXXFLAGS += -DMSSQL_BENCH_BUILD` and compiles the bench
  binary as a standalone (like the existing
  `make test-login7-encoding` does — see `Makefile:189` in the
  pre-044 tree).

**Alternatives considered**:
- *Build the legacy helpers as a separate static lib*
  (`libutf16_legacy.a`) linked only by the bench binary.
  Rejected as over-engineered for four small functions.
- *Expose the legacy helpers permanently in a public
  `_internal_` namespace*. Rejected as a half-hearted "private"
  that future consumers could grab.

## R4. PLP NVARCHAR(MAX) chunk reassembly — confirmed safe

**Decision**: No codec-layer change required. PLP chunks are
already reassembled into a single buffer at
`src/tds/tds_row_reader.cpp:507` (`RowReader::ReadPLPType`)
before `TypeConverter::ConvertString` is invoked. The codec sees
one fully-assembled UTF-16LE byte buffer per cell; surrogate pairs
spanning PLP chunks are glued back together by the row reader.

**Evidence**:
```
RowReader::ReadPLPType(data, length, value, is_null):
    reads 8-byte total length header
    loops over chunks, appending each chunk's bytes into `value`
    returns when chunk-length-zero terminator is reached
    `value` (a std::vector<uint8_t>) is then passed to ConvertString
ConvertString(value, ...):
    str = Utf16LEDecode(value.data(), value.size())
```

The migration's one-line `Utf16LEDecode` → `SimdutfUtf16LEDecode`
substitution in `type_converter.cpp:424` inherits this assembled
buffer; the simdutf wrapper sees the same bytes the legacy
converter saw.

**Alternatives considered**: per-chunk streaming decode (skip
reassembly, decode each PLP chunk as it arrives). Rejected: would
require a new streaming-decode wrapper API, surfaces the
surrogate-pair boundary problem at the codec layer, and gives no
performance win (simdutf is fastest on contiguous buffers anyway).
Pre-existing design is correct; the spec rides on it.

## R5. End-to-end benchmark — script structure and timing

**Decision**: `test/bench/bench_codec_e2e.sh` is a bash script
that drives the DuckDB CLI (`./build/release/duckdb`) with the
mssql extension preloaded, executes each of the six workflow
steps via heredoc'd SQL, and times each step with bash's
`date +%s.%N`. Per-step timings go to stdout in a fixed
machine-readable form:

```text
step_name<TAB>seconds<TAB>row_count<TAB>notes
ddl_create_tables    0.123     -    -
generate_source      45.321    100000000  duckdb temp table
insert_values        12.456    100000     LIMIT 100000
ctas_bcp             180.123   100000000  default mssql_ctas_use_bcp=true
copy_bcp             175.456   100000000  COPY TO MSSQL
select_count         0.234     1     smoke
select_full          240.789   100000000  COPY (SELECT * ...) TO '/dev/null'
```

The script accepts these environment variables (defaulting to
`make integration-test`'s values):
- `MSSQL_BENCH_DUCKDB_BIN` (path to a built DuckDB CLI with the
  mssql extension preloaded; required)
- `MSSQL_TEST_HOST`, `MSSQL_TEST_PORT`, `MSSQL_TEST_USER`,
  `MSSQL_TEST_PASS`, `MSSQL_TEST_DB` (same names the integration
  test suite uses; see `Makefile:71-75`)
- `MSSQL_BENCH_ROW_COUNT` (optional override; default 100000000)
- `MSSQL_BENCH_OUTPUT` (output file path; default
  `/tmp/bench_codec_e2e_$(date +%s).txt`)

The script:
1. Reads the env vars.
2. Pre-flight: connects via the binary, executes a `SELECT 1`
   smoke test against the SQL Server. Aborts with a clear
   message if SQL Server is unreachable.
3. Drops the two target tables if they exist (FR-053 idempotency).
4. Runs each of the six workflow steps in sequence, measuring
   wall-clock between two `date +%s.%N` reads bracketing each
   step's DuckDB invocation.
5. Emits per-step timing lines to `MSSQL_BENCH_OUTPUT`.
6. Drops the target tables on exit (cleanup).

**Rationale**:
- Bash + DuckDB CLI is the lowest-overhead harness with no extra
  language dependencies. The script is auditable in 100 LOC.
- `date +%s.%N` is portable (GNU/macOS coreutils both support
  nanosecond resolution); the bench's resolution requirement is
  ~milliseconds, well above what bash's process-spawn jitter
  introduces.
- Per-step DuckDB invocations (rather than one long-lived
  session) keep each step's timing self-contained — if step N
  hangs or crashes, the script still emits timings for steps 1..N-1.

**Alternatives considered**:
- *Python pytest with `pyodbc` or `duckdb-python`*. Rejected:
  adds a Python interpreter dependency for a script that runs
  six SQL statements.
- *C++ harness like `test/cpp/test_simple_query.cpp`*. Rejected:
  every code change forces a rebuild; bash + heredoc'd SQL is
  zero-rebuild iteration.
- *DuckDB-native `.timer on` in one session*. Considered, but
  needs careful parsing of CLI output; the bracketed
  `date +%s.%N` approach is more robust.

## R6. Docker SQL Server storage budget for the 100M-row workload

**Decision**: The benchmark operator MUST ensure the Docker
container has at least **25 GB usable storage** (target tables
+ tempdb + log) before kicking off the back-to-back baseline +
spec-044 runs. The default `make docker-up` does not specify a
volume size cap; on most developer setups (host filesystem-backed
named volume) the limit is host-disk free space. The benchmark
operator is responsible for verifying this and noting the
container's effective storage budget in `bench_results.md`.

**Sizing math**: 100M rows × ~150 B UTF-8/row payload × 2 byte
expansion to UTF-16LE on disk × 2 target tables (`bench_target_ctas`
+ `bench_target_copy`) ≈ 60 GB worst case; with row compression
(default SQL Server behavior) typically observed as ~20 GB
combined. tempdb usage during BCP load: another ~5 GB. 25 GB is
the floor; comfortable headroom is 40 GB.

**Rationale for not auto-detecting**: detecting effective
container storage cap requires Docker inspect + filesystem stat;
adds complexity for a one-time manual benchmark. Operator
responsibility documented in `quickstart.md`.

**Alternatives considered**:
- *Smaller default workload (1M or 10M rows)*. Rejected per user
  request: signal is the point of the benchmark, and 1-10M is
  inside the noise floor for SQL Server INSERT/BCP throughput
  on a local Docker.
- *Two-table-collapsed workload (one table, CTAS-then-DROP)*.
  Rejected: makes the COPY step depend on CTAS having torn down
  first, complicating the script and breaking the
  step-independence property.

## R7. `MSSQL_BENCH_ROW_COUNT` default and override behavior

**Decision**: Default `MSSQL_BENCH_ROW_COUNT=100000000` (100M).
When the caller exports a smaller value (e.g.,
`MSSQL_BENCH_ROW_COUNT=1000000` for a smoke iteration):
- The source-table generation uses the override directly.
- The INSERT-via-VALUES step still caps at
  `min(100000, $MSSQL_BENCH_ROW_COUNT)` — see FR-051a.
- The CTAS, COPY, and SELECT * steps use the override directly.
- The timing output retains the same per-row format; the row
  count column reflects the actual run.

The committed `bench_results_baseline.txt` and
`bench_results_spec044.txt` MUST be from runs with
`MSSQL_BENCH_ROW_COUNT=100000000` (the spec's authoritative
size); smaller runs are for developer iteration only and are
NOT committed.

**Rationale**: keeps the spec's recorded numbers comparable
across reviewers and across future re-captures, while letting
day-to-day implementer iteration use a fast smoke (1M rows
completes in ~30 seconds vs 100M's 10-30 minutes).

## R8. Symbol rename pass — exact mechanical steps

**Decision**: The rename commit (Phase B) performs these steps in
one atomic commit, in this order:

1. Fold the body of `src/tds/encoding/simdutf_wrappers.cpp` into
   `src/tds/encoding/utf16.cpp` (replacing utf16.cpp's existing
   contents): the simdutf-backed implementations become the
   public functions; the existing hand-rolled functions move
   into an anonymous namespace renamed
   `LegacyUtf16LE{Encode,Decode,EncodeDirect,ByteLength}`.
2. Rename `src/include/tds/encoding/simdutf_wrappers.hpp` →
   `src/include/tds/encoding/utf16.hpp` (replacing the existing
   utf16.hpp): the public declarations now have `Utf16LE*` names
   (no `Simdutf` prefix), unchanged signatures.
3. Update `CMakeLists.txt`: drop `simdutf_wrappers.cpp` from
   `EXTENSION_SOURCES`; `utf16.cpp` stays in the list (same path
   it occupied pre-044, now backed by simdutf).
4. Update `Makefile` `LOGIN7_TEST_SOURCES`
   (`Makefile:175-180`): drop `simdutf_wrappers.cpp`, keep
   `utf16.cpp`.
5. Sweep every Phase-A-migrated call site: change
   `encoding::SimdutfUtf16LE*` → `encoding::Utf16LE*` (16 call
   sites, mechanical). Drop the `#include
   "tds/encoding/simdutf_wrappers.hpp"` lines and add (or keep)
   `#include "tds/encoding/utf16.hpp"`.
6. Update the spec-043 microbenchmark / test
   (`test/cpp/test_login7_encoding.cpp`) if it references
   `SimdutfUtf16LE*` directly — switch to `Utf16LE*`. (The
   test's existing equivalence assertions were written against
   the spec-043 transitional names; this is a trivial rename.)
7. Re-run the full test suite locally before pushing the commit.

**Rationale**: One commit captures the rename atomically; no
intermediate state where some files reference `SimdutfUtf16LE*`
and others reference `Utf16LE*`. Reviewers can read the commit
top-to-bottom and verify each step.

## R9. Spec 042 coordination

**Decision**: Spec 044 changes three auth-layer files
(`src/azure/azure_fedauth.cpp`,
`src/tds/auth/fedauth_strategy.cpp`,
`src/tds/auth/manual_token_strategy.cpp`) with strictly one-line
substitutions each. Spec 042 (collaborator, Integrated
Authentication) restructures `src/tds/auth/` more broadly. The
two PRs coordinate by:

- If spec 044 merges first: spec 042's rebase notices the
  one-line `encoding::SimdutfUtf16LEEncode` calls and inherits
  them; after spec 044's Phase-B rename merges, those calls
  become `encoding::Utf16LEEncode` again. Spec 042 simply
  rebases on the post-rename `main`.
- If spec 042 merges first: spec 044 rebases the auth-layer
  changes onto the spec-042 restructured files. The
  one-line-per-file edit is preserved as a one-line-per-file
  edit on the new structure.

The spec 044 PR description MUST flag this coordination so the
reviewer is aware. No automated conflict-resolution required;
the conflict surface is intentionally minimal (FR-030 keeps
edits to one line each precisely so rebase stays mechanical).

**Alternatives considered**: rebase together as a paired PR.
Rejected: violates independence-of-delivery. Each spec must be
mergeable on its own.

## R10. Microbenchmark fixture sizing and iteration count

**Decision**: Per-fixture warm-up of 100 iterations, then a
measured run of **1000 iterations** for short fixtures (16 B,
256 B) and **100 iterations** for 64-KB fixtures. Total wall-clock
per fixture: tens of milliseconds for short fixtures, hundreds
of milliseconds for long fixtures. Aggregate microbenchmark run
time: under 30 seconds, suitable for `make bench-utf16` as a
"run on a whim" tool.

The microbenchmark records the **median** of the iteration
times (not mean) to defuse outliers from scheduler / interrupt
noise.

**Rationale**: enough iterations to dwarf single-call overhead,
not so many that the benchmark blocks developer iteration.

**Alternatives considered**: Google Benchmark / nanobench
integration. Rejected: adds a third-party dependency for a
one-file microbenchmark; spec 044's goal is migration, not
benchmark infrastructure.

## R11. Cleanup of legacy `utf16.cpp` test references

**Decision**: After Phase B's rename commit, the
`test/cpp/test_login7_encoding.cpp` (spec 043) requires no
changes if it already uses `encoding::Utf16LE*` (the legacy
public names) for equivalence assertions. If it uses
`encoding::SimdutfUtf16LE*` symbols (which spec 043 introduced),
they MUST be swept to `encoding::Utf16LE*` in the rename commit
per R8 step 6.

Verified by re-reading `test/cpp/test_login7_encoding.cpp`
during implementation; the file is small.

## R12. README / docs / version bump

**Decision**: This spec does NOT bump `MSSQL_EXTENSION_VERSION`
in `CMakeLists.txt` and does NOT change `vcpkg.json`'s version
field. Versioning is the release engineer's call when v0.2.0
is cut; the migration lands on `main` as a non-versioned
landing.

The README's "Third-Party Licenses" section already attributes
simdutf (added in spec 043) — no change needed.

`CLAUDE.md` is updated by `/speckit-plan` (per the workflow) to
reference this plan and add 044 to "Recent Changes."

**Rationale**: keep this spec narrow. Version bumps and release
notes are a separate workflow.

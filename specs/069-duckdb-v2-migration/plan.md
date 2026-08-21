# Implementation Plan: DuckDB 2.0 migration

**Branch**: `spec/069-duckdb-v2-migration` | **Date**: 2026-08-20 | **Spec**: [spec.md](./spec.md)

## Order of work

The clusters are ordered so that every step shrinks the error log and no step
depends on a later one. After each step: rebuild, record the new error count,
commit.

**Step 0 — submodule pins.** Commit `duckdb` @ `d7a4366603` and
`extension-ci-tools` @ `35759fd`. The branch now declares its world.

**Step 1 — Cluster B (vector const-correctness).** Purely mechanical and
self-contained: write sites `GetData` → `GetDataMutable`, read sites get
`const` locals, `ToUnifiedFormat` drops the count, const `ValidityMask`
fixes. 12 files, no design decisions. Rule: accessors stay hoisted out of row
loops (they now carry a release-build check).

**Step 2 — Cluster E (scatter).** Include fixes, one-for-one renames
(`SetAlias`→`WithAlias`, `StringVector` relocation, `dtime_t` accessor,
`TableIndex` conversions, `TableFunction` ctor). Mechanical; each item is
local.

**Step 3 — Cluster A (Identifier).** The structural one. Decide the boundary
ONCE: our internal metadata keeps `std::string` (TDS, cache, SQL generation
are string-native); conversion to/from `Identifier` happens at the DuckDB API
boundary only — entry points of catalog overrides and calls into
`Catalog::*`. No `Identifier` inside `src/tds`, `src/codec`, the cache, or
SQL text builders.

**Step 4 — Cluster D (privatized members).** Swap direct member reads for
accessors in the optimizer / filter encoder / copy function.

**Step 5 — Cluster C phase 1 (Legacy filter renames).** Rename to `Legacy*`,
fix `TableFilterSet` includes and accessor use. Pushdown behavior must not
change — the existing filter-pushdown test files are the gate.

**Step 6 — compat retirement + docs.** Remove `mssql_compat.hpp` usage on
main (file deleted or reduced to a tombstone comment pointing at
`duckdb-v1.5.5`), update `CLAUDE.md` (DuckDB API compat section) and
`DATAMODEL.md` if any layer description names the shim.

**Step 7 — verification.** Unit suite, integration suite, bench sanity
(read + write vs 1.5.5 baseline, interleaved A/B), CI green on the branch.

## Working method

- `ninja -k 0` error log is the progress meter; keep the per-step counts in
  commit messages.
- Where a duckdb rename is ambiguous (e.g. `ElapsedMs`, `TimePoint`), read the
  duckdb header/commit that changed it before picking the replacement — do not
  guess from the compiler suggestion alone.
- Sources compile twice (static + loadable): halve raw error counts when
  comparing.

## Risks

- **duckdb main moves under us.** Pins are fixed SHAs; a deliberate re-bump is
  its own commit with its own build check.
- **"Remove default database" (#24837) and catalog semantics.** The tip commit
  of the pin touches attach/catalog defaults; if ATTACH behavior shifts, it
  surfaces in the attach/catalog test groups at step 7, not silently.
- **Filter phase 1 hides capability changes.** Accepted: phase 2 spec owns the
  new filter representation.

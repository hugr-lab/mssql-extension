# Spec 070 — DuckDB 2.0 follow-ups: expression pushdown, writer ramp-up, test-var syntax

**Status**: spec 069 merged (main is 2.0). **W1 DONE** (this branch:
`pushdown_expression` + temporal-cast passthrough + the #242 mapping removal +
`expression_pushdown.test`). W2/W3 ready to start.
**Follows**: spec 069, which migrated main to the 2.0 API and deliberately
left three items out of scope. Each is an independent workstream; they share
a spec because all three exist for the same reason — 2.0 changed the ground
under a subsystem — but they can land as separate PRs in any order.

---

## W1 — Adopt the non-Legacy filter representation (and the capability it unlocks) — DONE

### What 2.0 actually did to filters

Reconnaissance against duckdb `d7a4366603`:

- `TableFilterType` keeps the whole 1.x hierarchy as `LEGACY_*`; the ONE
  non-legacy type is `EXPRESSION_FILTER` — a pushed predicate as a real
  `Expression` whose column reference is `BoundReference(0)` (the column
  travels in the filter's slot). Spec 069 already taught the encoder that
  form and added the client-filter net for refusals.
- The optimizer's `FilterCombiner::TryPushdownGenericExpression` pushes an
  **arbitrary single-column expression** into a scan's TableFilterSet — but
  ONLY if the scan implements the `pushdown_expression` callback
  (`bool(ClientContext&, const LogicalGet&, Expression&)`). We do not, so
  today every `year(col) = 2024`, `octet_length(v) > 2`, `col LIKE 'x%'`
  ... stays a plan-level Filter above the scan and the whole column is
  fetched. duckdb's own table scan answers a flat `return true` (it can
  execute anything); parquet is selective.
- The combiner pre-checks single-column-ness (all bindings equal) BEFORE
  calling the callback, and does the BoundReference rewrite AFTER it — the
  callback sees the original `BoundColumnRefExpression`s, which is exactly
  what `FilterEncoder::EncodeExpression` already encodes.
- New filter kinds exist beyond expressions (`LEGACY_PREFIX_RANGE_FILTER`
  wrapping a `PrefixRangeFilter` — LIKE-prefix ranges; `DynamicTableFilterSet`
  on `LogicalGet` — join-produced runtime filters). Dynamic filters cannot
  reach our SQL: the WHERE clause is built at InitGlobal, before a hash
  join's build side materializes them. Out of scope here; noted for a
  future delayed-init design.

### The work

1. **Implement `pushdown_expression`**: dry-run `FilterEncoder::
   EncodeExpression` over the offered expression (context built from
   `get.GetColumnIds()` + bind data, same as `ComplexFilterPushdown` does)
   and accept iff it encodes. Accepted expressions arrive later as
   EXPRESSION_FILTERs and reach the server in the WHERE clause; anything
   the runtime encode still refuses is caught by the 069 client net, so a
   dry-run/runtime disagreement degrades to client-side filtering, never to
   wrong rows.
2. **Decide the relationship with `pushdown_complex_filter`**: both paths
   now feed the same encoder. Expectation: `pushdown_expression` subsumes
   most of what the complex-filter callback catches, with better plan
   integration (the filter is accounted for in the scan, not re-planned).
   Measure, then either retire the complex-filter path or document why both
   stay.
3. **Encoder gaps that become visible once expressions flow**: LIKE
   patterns beyond the prefix/suffix/contains functions, `IN` with mixed
   types, prefix-range (`LIKE 'x%'` as `>= 'x' AND < 'y'` — SQL Server can
   use an index for either form, but the range form survives non-SARGable
   LIKE handling). The function-mapping audit from the spec-065 recon
   (issue #242: length→LEN wrong rows; dayofweek/week and '/' mappings)
   gates which functions may be accepted — a mapping that changes semantics
   must NOT pass the dry-run.

### Acceptance

- `WHERE year(ts) = 2024` and `WHERE col LIKE 'abc%'` on an attached table
  produce a server-side WHERE (visible via MSSQL_DEBUG=1) and correct rows.
- A deliberately unencodable expression filter returns correct rows via the
  net (test pins both the rows and the `needs client re-filter` debug line).
- Filter-pushdown test files extended; no existing pushdown test regresses.

**Outcome (implemented).** `pushdown_expression` dry-runs the encoder and
accepts what it can render; `year/month/day(datetime2_col) op const` now push
(`WHERE (YEAR([dt]) = 2024)`) via a temporal→temporal cast passthrough (DuckDB
models DATETIME2 as TIMESTAMP_NS; the implicit precision cast over the column is
a view, so the server applies the part function natively). **#242 closed here,
not deferred**: W1 widens what pushes, so the semantically-diverging mappings
had to go with it — `length`/`len` (LEN drops trailing spaces / counts UTF-16),
`/` (server integer division vs DuckDB float), and `dayofweek`/`week`
(@@DATEFIRST / non-ISO). Removed, not rewritten: an unmapped function falls to
the spec-069 client net, correct by construction. `expression_pushdown.test`
pins both — year pushes, `length(name)=3` returns the row LEN would have lost.
**`pushdown_complex_filter` currently SHADOWS `pushdown_expression`** (PR #269
review — recorded here rather than fixed, because fixing it is a trade-off, not
a bug). In `duckdb/src/optimizer/pushdown/pushdown_get.cpp` the complex-filter
callback runs FIRST, over every pending filter, using the same
`EncodeSearchCondition` and the same `BuildEncodeContext`; it erases everything
it can render. `TryPushdownGenericExpression` — and therefore
`MSSQLPushdownExpression` — only sees what is left, i.e. exactly what the
identical encoder just refused, so the dry-run answers `false` by construction.
What actually delivers `year(dt) = 2024` today is the temporal-cast strip in
`EncodeFunctionExpression`, reached through `ComplexFilterPushdown`; deleting
the `func.pushdown_expression = ...` line leaves every test passing.

Making the callback reachable means restricting `ComplexFilterPushdown` to the
shapes the combiner will not offer (it offers only expressions referencing
exactly ONE column binding). That is not free: the combiner applies gates the
complex-filter path does not — it skips volatile expressions, `COMPARE_IN`
above `InClauseRewriter::IN_CLAUSE_REWRITE_THRESHOLD`, and **any** expression
where `CanThrow() && filters.size() > 1`. A single-column expression that
`ComplexFilterPushdown` declines under those conditions is then offered to
nobody and runs client-side — a silent pushdown REGRESSION against what ships
today (e.g. a cast-bearing `year(dt) = 2024` alongside a second predicate).
Deciding between the two paths therefore needs a measurement, not a patch: the
follow-up should either restrict `ComplexFilterPushdown` and replicate the
combiner's gates, or drop `pushdown_expression` until it can be the only path.
Until then the registration is a no-op kept for the API surface, and the
encoder-level behaviour is pinned by `test/cpp/test_filter_encoder.cpp` — which
asserts the T-SQL STRING, the thing a row-comparing sqllogictest cannot see
(rows are identical whether the predicate ran on the server or in the spec-069
client net).

---

## W2 — Lazy writer ramp-up for parallel bulk loads — DONE

### The problem (measured in 069)

2.0 parallelizes small scans across sink threads. A 110k-row COPY split
over 8 writers puts every writer below `mssql_copy_flush_rows` (102400 —
SQL Server's own compressed-rowgroup threshold), so a columnstore target
gets zero COMPRESSED rowgroups: the parallelism that speeds big loads
silently defeats compression on small ones. Spec 069 pinned the threshold
TEST to one writer; the product behavior is unchanged.

### Design sketch

Writer claiming today (spec 057): each sink thread claims its own writer on
first chunk, up to `parallel_writer_limit`. Change the claim policy to
volume-gated ramp-up:

- Thread wants a writer: if `rows_sunk_so_far >= active_writers *
  flush_rows`, claim a new one (up to the limit); otherwise append to an
  existing writer under its lock.
- Cost model: the ramp serializes only the first `flush_rows` rows per
  active writer — ~102k rows through one connection is ~0.1–0.2 s, noise on
  a load big enough to want 8 writers, decisive on a load small enough to
  want one.
- The shared-append path adds a lock the current design deliberately does
  not have; the gate must be checked against the spec-057 bench matrix
  (wide-write, threads=4 cells) to show the big-load numbers do not move.
- Interactions that must not change: `mssql_copy_parallel_writers` stays
  the cap; transaction pinning still forces one writer; the spec-063
  single-writer-per-#temp-target rule unaffected.

### Acceptance

- The 069 columnstore test drops its `SET mssql_copy_parallel_writers = 1`
  pin and still sees COMPRESSED rowgroups (that removal IS the test).
- Wide-write bench: threads=4 cells within noise of pre-change numbers.

**Outcome (implemented).** A per-chunk claim replaces the one-shot claim: a
thread that shares the writer keeps re-asking, so a large load reaches the full
writer count even when every thread arrived early. Two dead ends, both measured
and recorded so they are not re-tried:
1. Gate `rows_sunk >= active * flush_rows` (the spec's first sketch) measured
   **1.3-1.5x slower at threads=4** — it kept ~active*flush_rows rows serialized
   on the shared writer, far past the point of compression return.
2. A single warm-up batch (`rows_sunk >= flush_rows`) still measured **1.2x**:
   serializing even one 102400-row batch on a large load costs ~30% by the
   arithmetic (100k/rate warm-up vs 25k/rate parallel), not the "~0.1-0.2 s
   noise" the cost model assumed. The penalty is inherent to serializing, not
   mutex contention.

The fix is to pay the warm-up **only where it buys compression**: the gate is
active for a COLUMNSTORE target and OFF for heap/rowstore, which fan out
immediately as before. `total_rows_expected` is never populated, so a small-vs-
large size test is not available — but a heap load has no compression to protect
either way, so gating on target shape is both sufficient and free on the common
path. Final A/B vs main (heap fixture, 1M rows, threads 1 and 4): **geomean
0.971, threads=4 within noise (0.92-1.01x)**. Columnstore acceptance
(`columnstore_batch_threshold.test`, default writers) green; a 1M heap CTAS
reaches 4/4 writers with ~38 ms of ramp overhead. Applied symmetrically to COPY
and CTAS.

---

## W3 — `${VAR}` → `{VAR}` in .test files

The 2.0 sqllogictest runner deprecates the `${VAR}` substitution form and
prints a warning per occurrence per run ("please replace with {VAR}") while
still substituting. Mechanical sweep:

- `grep -rl '\${' test/sql/ | xargs sed` over every occurrence; the
  `require-env` gating semantics are unchanged (still: only DECLARED
  variables substitute — the memory-recorded trap about undeclared ones
  arriving literally still holds).
- Verify: full integration suite green with ZERO "Replacing deprecated"
  lines in the output (grep the suite log for it — that absence is the
  assertion; a floor on it would rot).
- `docs/TESTING.md` and any `.test`-authoring docs updated to the new form.

Do this LAST among the three — it touches every .test file and would sit
noisily inside any other PR's diff.

**Result.** `${VAR}` → `{VAR}` across 159 `.test` files (334 occurrences).
The zero-"Replacing deprecated" assertion caught a *second* deprecated form the
title did not name: `__TEST_DIR__`, which the 2.0 runner deprecates in favour of
`{TEST_DIR}` and which surfaced the identical warning line. One occurrence pair
in `copy/vector_encodings_bcp.test` swept the same way — the assertion is on the
suite log, not on `${`, so both had to go for it to hold. `docs/TESTING.md` and
`test/README.md` examples converted, with an authoring note added naming the
brace-only form as canonical. `test/TLS_TESTING.md` left untouched: its `${VAR}`
are real shell variables in `bash`/`curl` examples, not runner substitutions.

---

## Out of scope (all three workstreams)

- Dynamic/join-produced filters into the server WHERE (needs delayed query
  construction — its own design).
- The new filter kinds as SERVER capabilities (bloom, selectivity-optional):
  client-net execution is enough until a use case shows otherwise.
- Any change to the scan's MaxThreads=1 single-connection streaming model.

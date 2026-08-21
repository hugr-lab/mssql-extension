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
The `pushdown_complex_filter` path is kept (it still catches multi-expression
shapes the combiner does not route through `pushdown_expression`); retiring it
is a measurement left to a follow-up.

---

## W2 — Lazy writer ramp-up for parallel bulk loads

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

---

## Out of scope (all three workstreams)

- Dynamic/join-produced filters into the server WHERE (needs delayed query
  construction — its own design).
- The new filter kinds as SERVER capabilities (bloom, selectivity-optional):
  client-net execution is enough until a use case shows otherwise.
- Any change to the scan's MaxThreads=1 single-connection streaming model.

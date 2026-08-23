# v0.3.0 roadmap — the pushdown release

**Status**: working document for splitting the work (VGSML / oluies).
Owners below are PROPOSALS — claim or swap by editing this file.

## What v0.3.0 is

Pushing the boundary between the engines: UPDATE / DELETE / MERGE without
requiring primary keys (staged via bulk load + set-based joins), and JOIN /
aggregation pushdown so small DuckDB tables travel to the server instead of
dragging big tables out of it. Announced in the v0.2.3 post; the recon is
complete and lives on branch `spec/065-dml-pushdown-recon`.

## The platform decision (DECIDED 2026-08-20)

**Everything is built directly on DuckDB 2.0 (main).** Main is the 2.0 track
since spec 069; v0.3.0 releases from it, timed with DuckDB 2.0's release —
features land on main as they finish and wait there. `duckdb-v1.5.5` gets
0.2.x maintenance only: bug fixes, **no feature backports** (062 included).
The first 2.0-based release is 0.3.0 regardless of which features made the
cut by then.

## Workstreams

| Spec | Title | State | Depends on | Size | Proposed owner |
| --- | --- | --- | --- | --- | --- |
| — (#242 fix) | Filter-mapping correctness: STOP pushing `length()` (no exact T-SQL form on non-`_SC` collations — `LEN` loses rows TODAY) and audit `dayofweek`/`week` (`@@DATEFIRST`-dependent) and `'/'`. Cheap on 2.0: an unmapped function falls to the client net / plan filter, correct by construction | **DONE** — shipped in #269 with 070 W1 | 069 merged (the net) | S | oluies |
| — (step 0) | Cardinality callback: MSSQL scans estimate 1 row today, poisoning every join order around them | **DONE** (#274) — plus filter selectivity and a TOP N hard bound | nothing | S | oluies |
| 062 | INSERT via BCP (replaces batched VALUES; 2–10× on multi-row INSERT) | spec on main | single-writer seam (below) | M | VGSML |
| 066 | Materialize-own-scan (#239): a DML/JOIN plan that reads the target table materializes through its own scan, not a second query | spec on recon branch | step 0 | M | oluies |
| 067 | DML staging: UPDATE/DELETE via #temp bulk load + set-based JOIN; match-key ladder makes rowid/PK optional; closes #140 | spec on recon branch | 062 (bulk-load path), 066 | L | VGSML |
| — | MERGE pushdown (T-SQL MERGE from DuckDB MERGE INTO; semantics mapped in 065 research) | recon only | 067 | M | VGSML |
| 061 | Collation-aware ORDER BY pushdown — makes the spec-039 ORDER BY/TOP pushdown default-safe (today experimental, opt-in `mssql_order_pushdown`); the remaining half of #58 / discussion #59 | spec on main (spun off 060) | nothing | M | oluies |
| — | JOIN / aggregation pushdown (`join-agg-pushdown.md` on the recon branch): reduction-vs-relocation ladder, materialize-then-decide; the community ask in discussion #75 | recon only | 066; #242 fixed | L | pair — design review together, then split |
| 070 | 2.0 follow-ups: `pushdown_expression` (W1), lazy writer ramp-up (W2), `${VAR}`→`{VAR}` (W3) | **DONE** — W1 (#269), W2 (#270), W3 (#271) all merged | 069 merged | W1 M / W2 S / W3 S | W1 VGSML, W2+W3 oluies |

Blocking prerequisite shared by 062 / 066 / 067 / join-relocation:
**the single-writer-on-a-given-connection seam** — one place that owns "this
connection has exactly one bulk-load writer" (today enforced ad hoc; spec 063
tests it for COPY only). Whoever starts first (062) builds it; the others
consume it.

## Settled during recon — do NOT re-litigate (details in `specs/065-dml-pushdown-recon/research.md`)

- Semantic contract: **NATIVE server semantics by default**, strict semantics
  via annotation. Padding applies even under `_BIN2` — verified live.
- AVG pushes ONLY by SUM + COUNT_BIG decomposition.
- Reduction-vs-relocation ladder with materialize-then-decide; relocation of
  a small DuckDB table into #temp is a lever, not a default.
- Issue **#242 gated function pushdown**, and is now CLOSED (#269) — but read
  how. The audit was resolved by **removal, not by finding exact T-SQL forms**:
  `length`/`len`, `'/'` and `date_diff`/`date_add`/`date_part` are gone from the
  mapping table, and `dayofweek`/`week` were already out. So "#242 fixed" does
  NOT mean those functions push — it means they fall to the client net, which is
  correct by construction and is the answer for group keys and join conditions
  too. Anything wanting `length()` server-side needs an exact form first, and
  there isn't one on non-`_SC` collations. (`%` was audited with them and kept,
  gated to exact-numeric operands: T-SQL modulo rejects float/real.)

## Learned while doing step 0 (#274) — affects the join/agg workstream

The planner **cannot see a single predicate** on an MSSQL scan.
`ComplexFilterPushdown` encodes each pushed filter into a WHERE-clause STRING
and erases the expression from the plan, so `get.table_filters` is empty and
`RelationStatisticsHelper::ExtractGetStats` — which would apply either a
stats-derived selectivity or DEFAULT_SELECTIVITY — has nothing to walk.
Measured live: `WHERE id < 100` over 200000 rows estimated 200000, inside a
join, with the join-order optimizer running.

#274 works around it by applying DuckDB's own 0.2 inside the cardinality
callback, and **labels that a stopgap**.

### The W1 measurement has been run — here is the answer

Spec 070's W1 outcome asked whether `ComplexFilterPushdown` can be restricted
so single-column predicates fall through to `pushdown_expression` and survive
as EXPRESSION_FILTERs. Measured on branch `spec/070-w1-measure-shadowing`
against a live server.

**Most of it worked.** No pushdown was lost — every shape DuckDB's combiner
gates still reached the server (`IN` at the 6-value threshold,
`year(dt) = 2024` beside a second predicate, `LIKE`). The planner could finally
see predicates: `EXPLAIN` showed `Filters: id < 100` on the scan with real
selectivity. Full suite green. **The objection recorded in #269 — that the
combiner's gates (volatile, oversized `IN`, `CanThrow() && filters.size() > 1`)
would silently lose pushdown — DID NOT REPRODUCE.** That reasoning was wrong
and should not be carried forward.

**What blocks it is unrelated to any of that.** DuckDB rewrites a prefix
predicate into a range — `prefix(col,'n')` becomes `col >= 'n' AND col < 'o'`
(`FilterCombiner::TryPushdownPrefixFilter`). Exact for DuckDB's binary string
comparison; **not** exact for SQL Server, where the collation decides ordering.
On `SQL_Latin1_General_CP1_CI_AS` — SQL Server's DEFAULT collation — `'ñu'`
sorts between `'n'` and `'o'`, so the server's `LIKE N'n%'` returns 2 rows and
the range returns 3. `main` returns 2; the restricted build returns 3. Silent
wrong rows, and the entire suite passed in that state (#276 adds the missing
coverage).

Blast radius is narrower than it first looks: a column reported as
`MSSQL_NVARCHAR(n)` (spec 060) is **accidentally immune**, because DuckDB
reaches the VARCHAR overload through a no-op cast and
`TryPushdownPrefixFilter` requires a bare `BOUND_COLUMN_REF`. It is the
plain-`VARCHAR` columns — `NVARCHAR(MAX)`, i.e. what COPY and CTAS create by
default — that are exposed.

### OPEN DECISION — does 061 now gate W1? (needs VGSML)

The restriction is not dead, but it has acquired a prerequisite: **the encoder
must refuse, or correctly translate, DuckDB's range-rewritten prefix filters
before those predicates may become TableFilters.** That is collation-aware
pushdown, i.e. spec **061**, which this table still lists as *independent, any
time*.

If that reading is accepted, the dependency chain becomes:

> **061 → W1 restriction → planner-visible filters → join/agg pushdown**

and 061 stops being optional: it moves onto the critical path for the
release's headline feature, and the order-of-battle line below should change
with it. **This is not yet decided** — the alternative is to leave the
restriction unbuilt, keep #274's 0.2 stopgap, and design join/agg against
estimates the planner cannot form, which is the worse of the two but is a
legitimate choice about sequencing.

Either way: do not design relocation-vs-reduction before settling this.

## Order of battle (dependency-honest)

```text
069 (2.0 migration, #267) ✔ ──► 070 W1 (#269) ✔ W2 (#270) ✔ W3 (#271) ✔   ← spec 070 COMPLETE
step 0 (cardinality, #274) ✔ ──► 066 ──► 067 ──► MERGE   ← 066 is now the head of the critical path
062 (single-writer seam) ──► 067
061 — independent today; PROPOSED to gate W1 (see OPEN DECISION above), UNDECIDED
join/agg — after 066 (+ #242, now closed); and after the 061/W1 decision, which
           determines whether it can rely on planner-visible filters at all
```

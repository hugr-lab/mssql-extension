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
| — (step 0) | Cardinality callback: MSSQL scans estimate 1 row today, poisoning every join order around them | **DONE** (#274) — plus filter selectivity (stopgap) and a TOP N estimate clamp | nothing | S | oluies |
| 062 | INSERT via BCP (replaces batched VALUES; 2–10× on multi-row INSERT) | spec on main | single-writer seam (below) | M | VGSML |
| 066 | Materialize-own-scan (#239): a DML/JOIN plan that reads the target table materializes through its own scan, not a second query | spec on recon branch | step 0 | M | oluies |
| 067 | DML staging: UPDATE/DELETE via #temp bulk load + set-based JOIN; match-key ladder makes rowid/PK optional; closes #140 | spec on recon branch | 062 (bulk-load path), 066 | L | VGSML |
| — | MERGE pushdown (T-SQL MERGE from DuckDB MERGE INTO; semantics mapped in 065 research) | recon only | 067 | M | VGSML |
| 061 | **Collation-faithful pushdown** (widened in #277 from ORDER BY only). Makes the spec-039 ORDER BY/TOP pushdown default-safe (today experimental, opt-in `mssql_order_pushdown`) — the remaining half of #58 / discussion #59 — AND supplies the exactness that server-side DML requires: `native AND … COLLATE …_BIN2` plus a trailing-space sentinel, added beside the native predicate so the Index Seek survives. **Now a prerequisite, not an independent item** | spec on main; widened in #277 | nothing | M | oluies |
| — (W1 restriction) | Stop `ComplexFilterPushdown` shadowing `pushdown_expression`, so pushed predicates survive as EXPRESSION_FILTERs the planner can estimate. **Measured (see below): no pushdown lost. Gated on 061** — DuckDB's prefix→range rewrite is inexact against SQL Server collations, and the pushed predicate is not re-checked. Removing it is what deletes #274's selectivity stopgap (`table_scan.cpp` "DELETE THIS when the filters stay visible", `DATAMODEL.md` likewise) | measured, gated on 061 | **061** | M | unassigned |
| — | JOIN / aggregation pushdown (`join-agg-pushdown.md` on the recon branch): reduction-vs-relocation ladder, materialize-then-decide; the community ask in discussion #75 | recon only | 066; #242 fixed; **and the W1-restriction row above, which decides whether it can rely on planner-visible filters** | L | pair — design review together, then split |
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

The planner cannot see any predicate **that our pushdown encodes** — what the
encoder REFUSES is regenerated by `FilterPushdown::PushdownGet` and survives as
a table filter or a `LogicalFilter`, and is estimated normally. So the blind
spot is exactly the successful pushdowns, which is the wrong way round: the
better we push, the less the planner knows. Measured live — `WHERE id < 100`
over 200000 rows estimated 200000, inside a join, with the join-order optimizer
running.

The mechanism is documented once, in `DATAMODEL.md`'s statistics-layer section
(the `MSSQLStatisticsProvider` bullets) — it is expected to be DELETED when this
is fixed, so it is not restated here. What belongs here is the consequence for
sequencing.

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

### DECIDED (VGSML, #275 / #277) — yes, 061 gates W1. The reason is DML, not SELECT.

> **061 → W1 restriction → planner-visible filters → DML-collapse + join/agg**

I proposed this edge as a SELECT-correctness story. That undersells it, and the
real reason is worth carrying:

**A server-side `UPDATE`/`DELETE` has nothing downstream to re-check it.** A
predicate matching one row too many does not return an extra row — it
**modifies data DuckDB would never have touched**. So the DML-collapse
workstream (067, MERGE) needs the pushed predicate to be *exactly* equivalent,
and on the default `_CI_AS` collation a string predicate is not. That is 061's
job. For SELECT the same looseness is survivable, because a client pass can
still fix it; for DML it is not survivable at all.

Corollary from #275 worth not forgetting: **rowid is not automatically safe
either.** It is safe today only because a PK is unique *under the server's own
comparison*, so `pk = value` cannot hit two rows. Anything that moves a rowid
comparison to binary semantics client-side breaks that quietly.

Also settled: my #276 conclusion that the restriction is "blocked" was too
strong. DuckDB's range rewrite returns a **superset** of DuckDB's own answer,
so the restriction was not wrong — it was **missing the re-check**. DuckDB 2.0
does not re-apply a `TableFilter` behind a `filter_pushdown` scan, so turning
predicates into EXPRESSION_FILTERs left nothing to apply them; the spec-069
client net already does this for *refused* filters and needs arming for
"pushed, but not authoritative".

**SELECT semantics — DECIDED (oluies): native stays the default.** #277 § 4.1
proposed pushing the native predicate and taking exactness from the client net,
but that net applies DuckDB's *binary* comparison, so `WHERE name = 'abc'`
would stop matching `'ABC'` on a CI collation — contradicting the first line of
the recon list below and the user-facing promise in
`website/docs/reading/queries.md`. Superset-and-re-check and native-by-default
are mutually exclusive for SELECT (native answer 2, range 3, re-check 1), so
the contract wins and § 4.1 changes:

- SELECT pushes the **exact** predicate (`= N'abc'`, `LIKE N'n%'`, `IN (…)`).
- DuckDB's prefix→range rewrite is **refused**, not pushed-and-repaired.
- The client net is **not** armed for "pushed but not authoritative" — arming
  it is what would break native. It stays for *refused* filters only.

**This costs nothing in plan shape**, which is worth recording because the
opposite was assumed while the question was open: measured on an indexed
`NVARCHAR(100) COLLATE Latin1_General_CI_AS` column, `LIKE N'n1%'` and
`>= N'n1' AND < N'n2'` BOTH produce an Index Seek — SQL Server turns a prefix
LIKE into a seek range itself. Refusing the rewrite keeps the seek AND the
semantics.

Still open on #277: whether DML is an exception. If native is the default
everywhere, a server-side DELETE matching `'ABC'` for `name = 'abc'` is the
documented behaviour rather than a violation, and the `_BIN2` pair is needed
only under the strict annotation — which narrows § 4.2. If DML is deliberately
strict, that must be stated outright, because a default that differs between
SELECT and DML is the kind of thing discovered through someone's deleted rows.

## Order of battle (dependency-honest)

```text
069 (2.0 migration, #267) ✔ ──► 070 W1 (#269) ✔ W2 (#270) ✔ W3 (#271) ✔   ← spec 070 COMPLETE
step 0 (cardinality, #274) ✔ ──► 066 ──► 067 ──► MERGE   ← 066 is now the head of the critical path
062 (single-writer seam) ──► 067
061 (collation-faithful, #277) ──► W1 restriction ──► planner-visible filters ──► DML-collapse + join/agg
join/agg — after 066 (+ #242, now closed) and after 061 → W1, which is what
           makes planner-visible filters available to it at all
```

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
| 061 | **Collation-faithful pushdown** (widened in #277 from ORDER BY only). Makes the spec-039 ORDER BY/TOP pushdown default-safe (today experimental, opt-in `mssql_order_pushdown`) — the remaining half of #58 / discussion #59 — AND supplies the exactness that server-side DML requires: `native AND … COLLATE …_BIN2` plus a trailing-space sentinel, added beside the native predicate so the Index Seek survives. **Now a prerequisite, not an independent item** | spec on main (ORDER BY only); widening proposed in #277 — **OPEN**, mechanism lives on that PR branch | nothing | M | oluies |
| — (W1 restriction) | Stop `ComplexFilterPushdown` shadowing `pushdown_expression`, so pushed predicates survive as EXPRESSION_FILTERs the planner can estimate. **Measured (see below): the #269 gates objection did not reproduce, but two shapes DO lose pushdown and must be excluded from the deferral (relaxation-only filters, and `rowid`). Partially gated on 061** — available for NON-STRING predicates now; string comparisons must stay claimed by `ComplexFilterPushdown` until 061 supplies a collation-faithful form, because the combiner rewrites `prefix()` into a range that is inexact on any non-binary ordering. The re-check route is REJECTED (it breaks native semantics). Removing the string half is what deletes #274's selectivity stopgap (`table_scan.cpp` "DELETE THIS when the filters stay visible", `DATAMODEL.md` likewise) | measured; non-string half unblocked, string half gated on 061 | **061** (string predicates only) | M | unassigned |
| — | JOIN / aggregation pushdown (`join-agg-pushdown.md` on the recon branch): reduction-vs-relocation ladder, materialize-then-decide; the community ask in discussion #75 | recon only | 066; #242 fixed; **and 061 → W1, which is what makes planner-visible filters available to it — for non-string predicates now, for string comparisons after 061** | L | pair — design review together, then split |
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

The mechanism is documented once, in
[`DATAMODEL.md` — Layer 4 (Cache & registries)](../DATAMODEL.md#layer-4--cache--registries) under the
`MSSQLStatisticsProvider` bullets — it is expected to be DELETED when this is
fixed, so it is not restated here. What belongs here is the consequence for
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
`year(dt) = 2024` beside a second predicate). The planner could finally see
predicates: `EXPLAIN` showed `Filters: id < 100` on the scan with real
selectivity. Full suite green. **The objection recorded in #269 — that the
combiner's gates (volatile, oversized `IN`, `CanThrow() && filters.size() > 1`)
would silently lose pushdown — DID NOT REPRODUCE.** That specific reasoning was
wrong and should not be carried forward.

**But "no pushdown lost" was too broad a generalisation from those shapes, and
is false** (roborev job 1130, verified against the source). Two classes DO lose,
neither of them tested by that measurement:

- **Relaxation-only shapes.** `GenerateTableScanFilters` returns
  `PUSHED_DOWN_PARTIALLY` for `LIKE`, OR-chains, non-dense `IN` and
  temporal-cast filters, and `pushdown_get.cpp` then SKIPS
  `TryPushdownGenericExpression` for anything not `NO_PUSHDOWN`. So the server
  receives only the combiner's *relaxation* — prefix bounds, an optional filter,
  margin-adjusted temporal bounds — and the exact predicate stays above the
  scan. The `LIKE` result recorded above as "reached the server" was in fact
  `[name] >= N'n' AND [name] < N'o'`: the relaxation, not the predicate.
  `WHERE name LIKE 'ab%cd'` over 200k rows streams the whole `ab` prefix range
  to the client.
- **`rowid` predicates lose pushdown entirely.** A rowid filter is
  single-column, so it defers — but `FilterEncoder::Encode` refuses every
  virtual column (`table_col_idx >= VIRTUAL_COL_START`) and routes it to the
  client net, whereas `ComplexFilterPushdown` reached `EncodeColumnRef`, which
  rewrites rowid to the scalar PK column and pushes real T-SQL. So
  `WHERE rowid > 100` goes from `WHERE [id] > 100` to a full table scan. Rows
  stay correct; the wire cost does not.

Neither is a reason the restriction cannot be done — both are shapes the
deferral must EXCLUDE. But the tracked claim has to be "the #269 gates objection
did not reproduce", not "no pushdown lost".

**What blocks it is unrelated to any of that.** DuckDB rewrites a prefix
predicate into a range — `prefix(col,'n')` becomes `col >= 'n' AND col < 'o'`
(`FilterCombiner::TryPushdownPrefixFilter`). Exact for DuckDB's binary string
comparison; **not** exact for SQL Server, where the collation decides ordering.
On any collation whose ordering is not binary — e.g. the common
`SQL_Latin1_General_CP1_CI_AS` install default — `'ñu'` sorts between `'n'` and
`'o'`, so the server's `LIKE N'n%'` returns 2 rows and
the range returns 3. `main` returns 2; the restricted build returns 3. Silent
wrong rows, and the entire suite passed in that state (#276 adds the missing
coverage).

Blast radius: a column reported as `MSSQL_NVARCHAR(n)` (spec 060) is
**accidentally immune**, because DuckDB reaches the VARCHAR overload through a
no-op cast and `TryPushdownPrefixFilter` requires a bare `BOUND_COLUMN_REF`.
Exposed is everything `MSSQLColumnInfo::NativeDuckDBType` leaves as a plain
VARCHAR:

- MAX columns (`max_length <= 0`) — including the `NVARCHAR(MAX)` COPY and CTAS
  create by default, and `varchar(max)` / `nvarchar(max)` on pre-existing tables;
- `text` / `ntext`, which reach it with a type name it does not map at all and
  fall through the final `else` — NOT via the `max_length <= 0` guard, since
  `sys.columns` reports 16 for them (the pointer size, the same 16 behind the
  known `text`→16 CAST truncation);
- cast-required and geometry columns, and lengths outside the inline limits;
- **and every string column, `NVARCHAR(100)` included, whenever
  `mssql_catalog_native_types` is `false`** — `NativeDuckDBType` is only
  consulted when that setting is on, so turning it off removes the immunity
  wholesale.

### DECIDED (VGSML, #275 / #277) — yes, 061 gates W1. The reason is DML, not SELECT.

> **061 → W1 restriction → planner-visible filters → DML-collapse + join/agg**

I proposed this edge as a SELECT-correctness story. That undersells it, and the
real reason is worth carrying:

**A server-side `UPDATE`/`DELETE` has nothing downstream to re-check it.** A
predicate matching one row too many does not return an extra row — it
**modifies data DuckDB would never have touched**. So the DML-collapse
workstream (067, MERGE) needs the pushed predicate to be *exactly* equivalent,
and on any collation whose ordering is not binary — the common `_CI_AS` install
default included — a string predicate is not. That is 061's
job. For SELECT the same looseness is survivable, because a client pass can
still fix it; for DML it is not survivable at all.

Corollary from #275 worth not forgetting: **rowid is not automatically safe
either.** It is safe today only because a PK is unique *under the server's own
comparison*, so `pk = value` cannot hit two rows. Anything that moves a rowid
comparison to binary semantics client-side breaks that quietly.

An intermediate conclusion that is now SUPERSEDED, recorded because it was
briefly the plan: the range rewrite returns a **superset** of DuckDB's own
answer, so it looked as though the restriction merely needed the spec-069
client net armed for "pushed, but not authoritative". It does not — arming the
net is what breaks the semantic contract. See the decision below.

**SELECT semantics — DECIDED (oluies): native stays the default.** #277 § 4.1
proposed pushing the native predicate and taking exactness from the client net,
but that net applies DuckDB's *binary* comparison, so `WHERE name = 'abc'`
would stop matching `'ABC'` on a CI collation — contradicting the first bullet
of the recon list ABOVE and the user-facing promise in
`website/docs/reading/queries.md`. Measured on a `_CI_AS` column: the same
predicate returns **2 rows pushed and 1 row client-side**. So SELECT pushes the
**exact** predicate (`= N'abc'`, `LIKE N'n%'`, `IN (…)`) and native semantics
are preserved by the predicate actually reaching the server.

**Which mechanism delivers that, and the trade it carries.** Two readings, and
only one of them honours the decision:

- **(a) `ComplexFilterPushdown` keeps claiming string-pattern expressions.**
  This is today's behaviour — verified live, `prefix()` is consumed there and
  emitted as `[name] LIKE N'n%'`, so DuckDB's `TryPushdownPrefixFilter` never
  sees it. Native semantics kept, Index Seek kept. **Cost: those predicates
  stay planner-invisible**, which is exactly the blind spot W1 exists to close.
  So W1 can make non-string predicates visible; string comparisons cannot join
  them without 061.
- **(b) Decline the range at the scan.** Planner-visible, but the original LIKE
  has already been pruned by the combiner, so nothing is pushed at all: the
  predicate runs in the client net under DuckDB's **binary** comparison. That
  loses the Index Seek *and* violates the contract in the same move — refusal
  is NOT semantics-neutral.

The decision therefore means **(a)**, and W1's reach is narrower than it looked.

**Correction to an earlier version of this section**, since it was briefly the
stated basis for the decision: it claimed the choice "costs nothing in plan
shape", citing a measurement that `LIKE N'n1%'` and `>= N'n1' AND < N'n2'` both
produce an Index Seek. That measurement is real but answers a different
question — it compares two forms that would both be *pushed*, whereas under the
restriction the LIKE form is not on the table. The trade is the one stated in
(a) above: planner visibility, not plan shape.

Still open on #277: whether DML is an exception. If native is the default
everywhere, a server-side DELETE matching `'ABC'` for `name = 'abc'` is the
documented behaviour rather than a violation, and the `_BIN2` pair is needed
only under the strict annotation — which narrows § 4.2. If DML is deliberately
strict, that must be stated outright, because a default that differs between
SELECT and DML is the kind of thing discovered through someone's deleted rows.

**This open question can withdraw one of the two reasons for the `061 → W1`
edge, so do not treat that edge as fully settled.** The edge currently rests on
DML exactness — "nothing downstream can re-check a server-side DML". If DML
inherits native semantics by default, that requirement lapses and the `_BIN2`
pair is needed only under the strict annotation. What survives either way is
the mechanism-(a) reason: string comparisons cannot become planner-visible
without a collation-faithful form. If the DML answer comes back "native
everywhere", the W1 row, the order-of-battle edge, and the matching notes in
`specs/070-duckdb-v2-followups/spec.md` and `src/table_scan/table_scan.cpp`
must all be revisited together — and `website/docs/writing/dml.md`, which is
the only one USERS read. Four surfaces now encode this dependency.

## Order of battle (dependency-honest)

```text
069 (2.0 migration, #267) ✔ ──► 070 W1 (#269) ✔ W2 (#270) ✔ W3 (#271) ✔   ← spec 070 COMPLETE
step 0 (cardinality, #274) ✔ ──► 066 ──► 067 ──► MERGE   ← 066 is now the head of the critical path
062 (single-writer seam) ──► 067
061 (collation-faithful, #277) ──► W1 restriction, STRING half only ──► planner-visible filters ──► DML-collapse + join/agg
    (W1's non-string half needs no gate and can be done today)
join/agg — after 066 (+ #242, now closed) and after 061 → W1, which is what
           makes planner-visible filters available to it at all
```

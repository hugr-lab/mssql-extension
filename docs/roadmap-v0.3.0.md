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
| — (step 0) | Cardinality callback: MSSQL scans estimate 1 row today, poisoning every join order around them | not started; named in 065 research | nothing | S | oluies |
| 062 | INSERT via BCP (replaces batched VALUES; 2–10× on multi-row INSERT) | spec on main | single-writer seam (below) | M | VGSML |
| 066 | Materialize-own-scan (#239): a DML/JOIN plan that reads the target table materializes through its own scan, not a second query | spec on recon branch | step 0 | M | oluies |
| 067 | DML staging: UPDATE/DELETE via #temp bulk load + set-based JOIN; match-key ladder makes rowid/PK optional; closes #140 | spec on recon branch | 062 (bulk-load path), 066 | L | VGSML |
| — | MERGE pushdown (T-SQL MERGE from DuckDB MERGE INTO; semantics mapped in 065 research) | recon only | 067 | M | VGSML |
| 061 | Collation-aware ORDER BY pushdown — makes the spec-039 ORDER BY/TOP pushdown default-safe (today experimental, opt-in `mssql_order_pushdown`); the remaining half of #58 / discussion #59 | spec on main (spun off 060) | nothing | M | oluies |
| — | JOIN / aggregation pushdown (`join-agg-pushdown.md` on the recon branch): reduction-vs-relocation ladder, materialize-then-decide; the community ask in discussion #75 | recon only | 066; #242 fixed | L | pair — design review together, then split |
| 070 | 2.0 follow-ups: `pushdown_expression` (W1), lazy writer ramp-up (W2), `${VAR}`→`{VAR}` (W3) | **W1 MERGED** (#269); **W2 in review** (#270); W3 done ahead of the spec (#271) | 069 merged | W1 M / W2 S / W3 S | W1 VGSML, W2+W3 oluies |

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

## Order of battle (dependency-honest)

```text
069 (2.0 migration, #267) ✔ ──► 070.W1 (#269) ✔ + W3 (#271) ✔ ──► 070.W2 (#270) in review
step 0 (cardinality) ──► 066 ──► 067 ──► MERGE          ← step 0 NOT STARTED, blocks most of the release
062 (single-writer seam) ──► 067
061 — independent, any time
join/agg — after 066 (+ #242, now closed)
```

# Spec 067 — UPDATE/DELETE via staged JOIN: retiring rowid as a requirement

**Status:** Draft (step 4 of the v0.3.0 order; design settled during the 066
discussion, 2026-08-06)
**Goal (maintainer directive):** move away from rowid as far as possible —
filter pushdown where the statement allows it (spec 065), staged JOIN
everywhere else — **so that updates work without a primary key**. #140
closes FULLY here (065 closes it for pushable statements only).

---

## 1. The insight that makes PK-less correct, not approximate

For a DETERMINISTIC predicate and SET list, matching rows BY VALUE is
semantically equivalent to matching them by the predicate: two
indistinguishable rows either both satisfy the WHERE or neither does (the
predicate is a function of column values), and their computed SET values are
identical. So a stage JOIN on column values updates exactly the set DuckDB
would update — duplicates move together, which is the only consistent
meaning "that row" HAS on a keyless table. Row identity is not required;
a MATCH KEY is, and it has a ladder.

## 2. D1 — the match-key ladder

Resolved per table at plan time:

1. **Primary key** — today's join key, narrowest and indexed.
2. **Any UNIQUE index over NOT NULL columns** — same properties, widens
   coverage to PK-less-but-keyed tables for free.
3. **ALL columns, NULL-safe** — the keyless base case. Join condition per
   column: `IS NOT DISTINCT FROM` on SQL Server 2022+/Azure/Fabric; the
   `EXISTS (SELECT s.c INTERSECT SELECT t.c)` rewrite on older versions.
   Comparison semantics are the SERVER's (collation, padding) — consistent
   with the native-semantics contract (research.md §7); the strict
   annotation story applies if ever needed.

Guard: a VOLATILE function anywhere in WHERE or SET (random(), now()-class)
breaks the equivalence argument on rung 3 — refuse with a message naming
the function and the ladder ("add a unique index, or make the expression
deterministic"). Rungs 1–2 are unaffected (identity is real there).

LOB-typed columns (nvarchar(max)/varbinary(max)/xml) in a rung-3 key: legal
in the EXISTS/INTERSECT form (not in a plain JOIN predicate on some
versions) — the rewrite already covers it; measure, and if pathological,
exclude LOB columns from the key when the remaining columns are unique in
the STAGED set (cheap client-side check at stage build).

## 3. D2 — what the stage carries and the statement shapes

- UPDATE: match-key columns + computed SET values;
  `UPDATE t SET t.c = s.c__new, ... FROM target t JOIN ##stage s ON <ladder condition>`
- DELETE: match-key columns only;
  `DELETE t FROM target t JOIN ##stage s ON <ladder condition>`
- Rung 3 note: the JOIN naturally updates ALL duplicates of a staged value
  row — which is the correct set (§1). The stage itself is DISTINCT over
  the key (duplicates staged once).

## 4. D3 — delivery: everything from spec 066 D5

The vehicle and modes are decided in 066 and reused wholesale:
`##stage_<uuid>` scratch tables (their rows never need to roll back),
pipelined incremental as the autocommit default (scan ∥ stage-fill ∥ DML on
separate connections, ping-pong stages, ~100k-row statement batches),
streaming in transactions (stage fills on a pool connection, the JOIN runs
pinned), single-statement as the transaction shape / autocommit opt-in,
client CDC + VALUES join below the small-result threshold and as the
fallback. The columnar chunk buffering replaces `vector<vector<Value>>`
(the no-per-value-path rule), and the defer machinery retires.

## 5. Consequences

- `BindUpdateConstraints` / `GetRowIdColumns`: the PK requirement is gone
  entirely; rowid columns bind from the ladder (multi-column row
  identifiers are first-class — recon A4). The user-visible `rowid`
  pseudo-column stays PK-based as today; DML simply no longer requires it.
- Spec 065's D4 plan-time error ("requires a primary key or a fully
  pushable WHERE") is INTERIM: once this spec lands, that error path
  becomes the rung-3 staged path, and the only refusal left is the
  volatile-function guard.
- `%%physloc%%` (SQL Server's undocumented physical RID, the ctid
  analogue) is deliberately NOT used: undocumented, and rows move (heap
  forwarding, page splits) between scan and mutation — the value-match
  ladder is documented semantics with no reuse hazard. Recorded so it is
  not re-proposed.

## 6. Tests (beyond the 066/065 suites)

1. Rung ladder selection: PK table → PK key; PK-less with unique index →
   that index; keyless → all columns (assert via plan/counters).
2. Keyless UPDATE/DELETE with duplicates: both duplicates move, count
   matches DuckDB-local semantics on the same data.
3. NULL-bearing keys: NULL-safe equality both rewrite forms (2022 native /
   EXISTS-INTERSECT), including all-NULL rows.
4. Volatile guard: `WHERE random() < 0.5` on a keyless table → the named
   refusal; same statement on a PK table → works.
5. #140's original reproduction — green end to end.

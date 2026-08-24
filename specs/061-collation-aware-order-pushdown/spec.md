# Spec 061 — Collation-faithful pushdown (ORDER BY, filters, DML)

**Status:** Draft — scope widened 2026-08-23, see § 3
**Date:** 2026-08-01
**Depends on:** spec 039 (ORDER BY pushdown, shipped disabled)
**Related:** issue #225 (the `_UTF8` suffix test this reuses), spec 060 (catalog
now carries per-column collation), issue #272 (the same divergence in string
`=`/`IN`/`LIKE`), PR #276 (the range-rewritten prefix that finds it), the v0.3.0
DML-collapse workstream (server-side UPDATE/DELETE without a rowid round trip)

> **Scope note (2026-08-23).** This spec started as "ORDER BY only" and is now
> the single home for one question asked in three places: *when may a string
> comparison run on SQL Server, given that the server compares by the column's
> collation and DuckDB compares by bytes?* ORDER BY (§ 1–2, § 5) was the first
> consumer; filters and DML-collapse (§ 3–4) are the two that followed. They
> share the evidence and the collation predicate, but — importantly — they do
> **not** share the same rule, because only one of them can re-check on the
> client.

---

## 1. Why the existing setting is off, measured

`mssql_order_pushdown` has shipped `false` since spec 039, and the reason is
not caution — it changes results. Seven rows in a `nvarchar(20) NOT NULL`
column on a `SQL_Latin1_General_CP1_CI_AS` database, `ORDER BY s LIMIT 4`:

```text
off:  Apple | BANANA | _x    | apple
on:   _x    | Ähre   | Apple | apple
```

Not a different order of the same rows — a **different set**. `BANANA` against
`Ähre`. DuckDB sorts by code point; SQL Server sorts linguistically by the
column's collation, which on a default install is case-insensitive and
accent-sensitive. With `LIMIT`, that decides which rows exist.

So the setting is not a performance toggle a user can flip blind, and turning
it on by default would silently change query results for anyone on a default
SQL Server collation. That is the whole of the problem.

Worth noting the existing pushdown is already conservative in one dimension: it
declines when NULL ordering disagrees, which is visible in the debug log as
`ORDER BY[0]: NULL order mismatch for nullable column s`. The collation
dimension has no such check because nothing carried the collation to the
optimizer.

## 2. The one case where the orders provably agree

A **binary** collation — `_BIN` or `_BIN2` — orders by code point, which is what
DuckDB does. `_BIN2` compares full code points; `_BIN` is the older form that
compares the first character by code point and the rest bytewise, so only
`_BIN2` is exactly DuckDB's order for multi-code-unit strings.

That gives a rule with no judgement in it: push the ORDER BY when every ordering
key is a string column under a `_BIN2` collation, or is not a string at all.
Numbers, dates and UUIDs have no collation and already agree.

**This is not a niche case.** A Microsoft Fabric warehouse's default collation is
`Latin1_General_100_BIN2_UTF8`, so every string column there qualifies and the
pushdown would be automatic on the platform where the round trip costs most.
The same is true of any database created `_BIN2` deliberately, which is common
for case-sensitive keys.

## 3. The same divergence in filters — and the mechanism that closes it

§ 2's rule is *"push only when the collation happens to already agree."* It is
correct but passive: on a default `_CI_AS` install nothing qualifies. There is
an active form — **force the comparison's collation in the emitted T-SQL** — and
it is what makes filters and DML tractable.

### 3.1 What `COLLATE` fixes, measured

Live SQL Server 2025, column `varchar(20) COLLATE SQL_Latin1_General_CP1_CI_AS`
holding `ab`, `ab␣`, `AB`, `nero`, `Nero`, `ñu`:

| predicate | server default | `COLLATE Latin1_General_BIN2` | DuckDB's own answer |
|---|---|---|---|
| `= 'Nero'` | 2 | **1** ✅ | 1 |
| `LIKE 'n%'` | 2 | **1** ✅ | 1 |
| range `>= 'n' AND < 'o'` (PR #276) | 3 (catches `ñu`) | **1** ✅ | 1 |
| `= 'ab'` (case) | 3 | 2 | 1 |
| `'ab' = 'ab '` (trailing space) | true | **true — NOT fixed** ❌ | false |
| `(a+'~') = ('ab'+'~') COLLATE …BIN2` | — | **1** ✅ | 1 |

Two findings carry the design:

1. **`COLLATE …_BIN2` reproduces DuckDB's binary semantics** for case, accents,
   ordering, `LIKE`, and — the case PR #276 found — the range DuckDB rewrites a
   prefix predicate into. That last row is the one that matters: `ñu` sorts
   between `n` and `o` linguistically, so the rewritten range silently gains a
   row on the *default* collation.
2. **Trailing spaces are not a collation property.** SQL Server ignores them in
   comparison as an operator rule; no collation turns that off. A sentinel —
   comparing `a + '~'` against `value + '~'` — does, because the space stops
   being trailing.

### 3.2 What forcing it costs: SARGability

Forcing the collation on the *column side* changes the expression the index is
built on, so the seek is gone. Measured on a 20 006-row table with an index on
`a` (`SET SHOWPLAN_TEXT`):

| emitted predicate | plan |
|---|---|
| `a = 'nero'` (today) | **Index Seek** |
| `a = 'nero' COLLATE …BIN2` alone | **Index Scan** ⛔ |
| `a = 'nero' AND a = 'nero' COLLATE …BIN2` | **Index Seek** + residual `WHERE` ✅ |
| `a LIKE 'n%' AND a LIKE 'n%' COLLATE …BIN2` | **Index Seek** (`>= 'Mþ' AND < 'O'`) + residual ✅ |
| `a = 'ab' AND (a+'~') = ('ab'+'~') COLLATE …BIN2` | **Index Seek** + residual ✅ |

So the collation-forced predicate must be **added**, never substituted: the
native predicate keeps the seek, the forced one filters what the seek returned.
Note the server's own seek range for `LIKE` (`>= 'Mþ'`) is deliberately *wider*
than the matches — the optimizer already guarantees the seek is a superset, which
is the same guarantee this design leans on.

### 3.3 The precondition, and where it fails

Adding a predicate can only ever *remove* rows, so the pair is correct exactly
when the native predicate returns a **superset** of DuckDB's answer.

- **Equality** — provable. `_BIN2` equality is byte equality, the strictest
  there is; any linguistic collation calls byte-equal strings equal. Native ⊇
  binary, always.
- **Prefix / `LIKE 'x%'`** — holds for the Latin collations measured (CI adds
  `Nero`, the rewritten range adds `ñu`; both are *extra* rows). It is an
  argument, not a theorem, for collations with expansions or ignorable
  characters, so it is gated per collation family and defaults to "not proven".
- **`<>`, `NOT LIKE`** — **fails**. Negation inverts the inclusion: under `_CI_AS`
  the server *excludes* `'AB'` from `a <> 'ab'` while DuckDB keeps it, so the
  server returns a **subset** and no later filtering can put the row back.

## 4. Three consumers, three different rules

The rule differs by whether anything downstream can re-check the server's answer.

### 4.1 SELECT filters — one pass on the server, one in DuckDB

Push **only the native predicate** (SARGable, superset) and let DuckDB apply the
exact one. Nothing is forced into the T-SQL, so the seek, the plan shape and the
cardinality estimate are all untouched; the price is transferring the superset
delta, which for a selective predicate is a handful of rows.

This needs no new machinery: the spec-069 **client filter net** already re-applies
filters the encoder refused. The change is to arm it for a filter that *was*
pushed — "pushed, but not authoritative" — because DuckDB 2.0 does **not**
re-check a `TableFilter` behind a `filter_pushdown` scan (the contract recorded
in `CLAUDE.md`).

**This is also what unblocks the W1 restriction** measured on
`spec/070-w1-measure-shadowing` (PR #275/#276). That branch made single-column
predicates survive as `EXPRESSION_FILTER`s so the planner could see them — and
found the rewritten range returning 3 rows where the server's own `LIKE` returns
2. Under this rule that is not a bug but the expected superset: the client net
drops `Nero` and `ñu` and the answer is DuckDB's. The missing piece was the
re-check, not the restriction.

### 4.2 DML-collapse — both predicates, on the server

A server-side `UPDATE`/`DELETE` executes where nothing can re-check it: a
predicate matching one row too many **modifies data that DuckDB would not have
touched**. Superset-and-re-check is structurally unavailable, so here the pair
from § 3.2 is emitted — native (seek) **AND** `COLLATE …_BIN2` (exact), plus the
sentinel on equality.

The duplication's usual cost — a confused cardinality estimate from two
predicates on one column — is close to free in exactly this context: a
single-table `UPDATE`/`DELETE` has no join order to get wrong. That is why the
expensive form is confined here.

Forms whose superset property is unproven (§ 3.3) do not collapse: they fall
back to the existing rowid round trip, which targets rows by identity rather
than by predicate and is correct under any collation.

### 4.3 ORDER BY — § 2's passive rule, plus an option

Sorting has no superset: `TOP k` under the wrong order returns the wrong *set*,
and a client re-sort of truncated rows cannot recover it. So § 2 stands — push
when the key is already `_BIN2` or non-string. `ORDER BY a COLLATE …_BIN2` is
available and correct, but it forces a server-side sort (the index order no
longer applies), so it is worth it only when `TOP k` avoids transferring the
table. Offered as an opt-in, not a default.

### 4.4 Summary

| Path | Sent to the server | What makes it exact |
|---|---|---|
| SELECT, superset forms (`=`, `IN`, prefix/`LIKE`) | native predicate only | DuckDB client net |
| SELECT, `<>` / `NOT LIKE` on strings | nothing | DuckDB |
| DML-collapse, superset forms | native **+** `COLLATE …BIN2` (+ sentinel on `=`) | the T-SQL itself |
| DML, everything else | — | rowid round trip |
| Column already `_BIN2`, or non-string | native predicate only | already exact |
| ORDER BY | only when `_BIN2` / non-string | § 2 |

## 5. What has to be built

The optimizer decides pushdown from `BoundOrderByNode`s and the scan's bind
data. The collation is already available and unused:
`MSSQLColumnInfo::collation_name` is populated for every column, and spec 060
put the declared type — including the collation — on the LogicalType the catalog
reports. Nothing new has to be fetched.

- **D1 A collation predicate.** `OrderIsCodePointStable(column)`: true for a
  non-string column, true for a string column whose collation name ends in
  `_BIN2`, false otherwise. `_BIN` is deliberately excluded — see § 2.
  The suffix test rather than `COLLATIONPROPERTY`, for the reason #225 recorded:
  Fabric closes the connection on that function.
- **D2 Feed it into the existing gate**, beside the NULL-order check that is
  already there. An ORDER BY is pushed only if every key passes.
- **D3 A third setting value.** `mssql_order_pushdown` becomes
  `off` (today's default) / `safe` / `always`, or stays BOOLEAN with a companion
  — to be decided in review. `safe` is the one that should eventually become the
  default; `always` preserves today's opt-in for users who know their collation
  matches or do not care about tie order.
- **D4 Tests that assert the ORDER, not the row count.** The regression this
  guards is a reordering, so a test that counts rows cannot see it. The shape is
  the § 1 table: the same seven rows under a `_CI_AS` column and under a `_BIN2`
  column, with the plan asserted too (`EXPLAIN` shows no ORDER_BY operator above
  the scan when it was pushed).

- **D5 One collation predicate, two questions.** `OrderIsCodePointStable` (D1)
  answers *"does this column already agree?"*. Filters need a second,
  `SupersetUnderNativeCollation(column, comparison_type)`: true for equality and
  prefix forms on any collation, **false for `<>` / `NOT LIKE`**, true trivially
  for non-strings. § 3.3 is its justification and § 4.4 its truth table. Both
  read `MSSQLColumnInfo::collation_name`; neither needs a round trip.
- **D6 Emit-side helper for the DML pair.** One place builds
  `native AND forced [AND sentinel]` so COPY-style call sites cannot each invent
  their own; the sentinel is added only for equality, and only when the column's
  collation is not already `_BIN2`.
- **D7 Nothing is forced on the SELECT path.** The encoder emits exactly what it
  emits today; the change is that a pushed string predicate is *also* registered
  in the spec-069 client net. Stated as a decision because the tempting
  alternative — forcing `COLLATE` everywhere — is what § 3.2 measures as an
  index scan.

## 6. Acceptance criteria

**Filters (§ 3–4):**

F1. On a `_CI_AS` column, `WHERE a = 'ab'`, `a IN (…)` and `a LIKE 'n%'` return
    DuckDB's exact rows, and return the **same** rows whether or not the filter
    was pushed — the push/no-push disagreement of issue #272 is the regression
    this guards (block pushdown with a `LIMIT` to get the other side). PR #276's
    `ñu` case is included.
F2. A collapsed `UPDATE`/`DELETE` on a `_CI_AS` column affects exactly the rows
    DuckDB's predicate selects — asserted by row identity, not by count —
    including a value with a trailing space and a case variant. The plan check
    shows the seek survived (§ 3.2).
F3. A string `<>` predicate does **not** collapse: it falls back (client-side for
    SELECT, rowid for DML) and returns DuckDB's rows.

**ORDER BY (§ 1–2):**

O1. With the default setting, generated SQL is byte-identical to today's.
O2. Under `safe`, a `_CI_AS` string key is **not** pushed and results match
    plain DuckDB exactly, including with `LIMIT`.
O3. Under `safe`, a `_BIN2` string key **is** pushed, and the result is identical
    to the un-pushed one — asserted row by row, not by count.
O4. A mixed ORDER BY (one `_BIN2` key, one `_CI_AS` key) is not pushed at all.
O5. On Fabric, `safe` pushes — the warehouse collation qualifies.

## 7. Risks

- **`_BIN2` is code-point order, but is DuckDB's?** DuckDB compares strings by
  UTF-8 bytes. UTF-8 byte order and code-point order coincide, so the two agree
  — but this deserves an explicit test with characters above U+FFFF rather than
  an argument, because it is the whole premise.
- **The collation is per column, and an expression is not a column.** `ORDER BY
  upper(s)` has no collation of its own; it inherits the argument's. The first
  cut should push only bare column references and treat anything else as
  unsafe, which is narrower than what spec 039 already supports.
- **Making `safe` the default is still a behaviour change** for anyone with a
  `_BIN2` database who was relying on DuckDB doing the sort — the results are
  identical, but the *plan* and the memory profile change. Worth a release note
  rather than a silent flip.

Risks specific to the widened scope:

- **This changes a documented contract.** `test/sql/catalog/collation_filter.test`
  asserts *server* semantics today — `WHERE col_default = 'Apple'` returns
  `Apple`, `apple`, `APPLE`. Under § 4.1 the answer becomes DuckDB's (`Apple`
  only). That is the point — it is what makes pushed and un-pushed agree — but it
  must be a **decision recorded and the test rewritten**, never a test quietly
  fitted to new behaviour. Users relying on case-insensitive matching get it back
  by writing `lower(a) = 'apple'`, which is explicit.
- **The superset property for prefixes is argued, not proven** (§ 3.3). A
  collation with expansions or ignorable characters could in principle order a
  byte-in-range value outside the linguistic range, which would *lose* rows —
  the one failure mode the client net cannot catch. Mitigation: gate on
  collation family, default unproven families to "do not push", and test against
  a non-Latin collation before widening.
- **`NVARCHAR` under `_BIN2` compares UTF-16 code units**, DuckDB compares UTF-8
  bytes. They agree across the BMP and disagree above U+FFFF (supplementary
  plane: emoji, rare CJK). § 5's D1 risk note already flags this for ordering;
  it applies identically to the forced-collation equality of § 4.2 and needs the
  same above-U+FFFF test.
- **The sentinel changes the expression's type and length.** `a + '~'` on a
  `varchar(8000)`/`MAX` column risks truncation or an implicit conversion the
  plan shows as `CONVERT_IMPLICIT` (visible in the § 3.2 measurement). Pick the
  sentinel and the cast deliberately, and keep it off any column where the
  concatenation could overflow.
- **Two predicates on one column skew the estimate.** The optimizer multiplies
  their selectivities and under-counts. Confined to DML (§ 4.2) precisely
  because a single-table `UPDATE`/`DELETE` has no join order to lose — but if
  the pair ever reaches a `SELECT`, that is the cost to measure first.

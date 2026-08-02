# Spec 061 — Collation-aware ORDER BY pushdown

**Status:** Draft
**Date:** 2026-08-01
**Depends on:** spec 039 (ORDER BY pushdown, shipped disabled)
**Related:** issue #225 (the `_UTF8` suffix test this reuses), spec 060 (catalog now carries per-column collation)

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

## 3. What has to be built

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

## 4. Acceptance criteria

1. With the default setting, generated SQL is byte-identical to today's.
2. Under `safe`, a `_CI_AS` string key is **not** pushed and results match
   plain DuckDB exactly, including with `LIMIT`.
3. Under `safe`, a `_BIN2` string key **is** pushed, and the result is identical
   to the un-pushed one — asserted row by row, not by count.
4. A mixed ORDER BY (one `_BIN2` key, one `_CI_AS` key) is not pushed at all.
5. On Fabric, `safe` pushes — the warehouse collation qualifies.

## 5. Risks

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

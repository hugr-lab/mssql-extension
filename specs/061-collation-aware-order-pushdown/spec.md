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
it is what makes the DML path tractable. § 4 is where it applies and where it
deliberately does not.

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

## 4. Three consumers, three different rules — and one independent widening

§ 4.1–4.4 are the collation question: the rule differs by whether anything
downstream can re-check the server's answer. § 4.5 is separate — exact T-SQL
forms for functions #242 removed, which needs no collation machinery and could
ship on its own.

### 4.1 SELECT filters — push the exact predicate, refuse the inexact rewrite

**Decided: native server semantics stay the default** (@oluies on #275, merged to
`main` as `3579f63`). A pushed string predicate must return *the server's own
answer* — `WHERE name = 'abc'` keeps matching `'ABC'` on a case-insensitive
collation, which is what `website/docs/reading/queries.md` promises users today
and what `collation_filter.test` asserts.

An earlier draft of this section proposed the opposite: push a superset and
re-check exactly in DuckDB. It is recorded here because the two are **mutually
exclusive**, and the arithmetic is the clearest statement of why. For `LIKE 'n%'`
over `nero`, `Nero`, `ñu`:

| | rows |
|---|---|
| server's own `LIKE N'n%'` — the contract's answer | **2** |
| DuckDB's prefix→range rewrite, pushed | 3 |
| that range, re-checked in DuckDB | 1 |

Nothing in the superset-plus-re-check shape produces 2. So under native
semantics:

- **Push the exact predicate** — `= N'abc'`, `LIKE N'n%'`, `IN (…)`. The server
  evaluates it under the column's collation, which is the answer the contract
  names. `LIKE` prefix stays SARGable: the optimizer derives a seek range from
  it itself (measured in § 3.2, `>= 'Mþ' AND < 'O'`).
- **Refuse DuckDB's prefix→range rewrite.** It is exact for DuckDB's binary
  comparison and wrong for the server's — the `ñu` row of § 3.1. This is the
  "the encoder must refuse or correctly translate" half, and it is far less than
  the full `_BIN2` machinery.
- **Do NOT arm the client net for pushed filters.** The net applies DuckDB's
  binary comparison, so it would return 1 where the contract requires 2. It
  stays what it is: the backstop for filters the encoder *refused*.

**What this means for the W1 restriction.** Not one blanket change. String-pattern
expressions must stay claimed by `ComplexFilterPushdown` until this spec supplies
a collation-faithful form — that is what keeps the exact `LIKE`, the seek and the
semantics together; they stay planner-invisible meanwhile, which is the accepted
cost. Non-string predicates carry no collation and can defer to
`pushdown_expression` today. `specs/070-duckdb-v2-followups/spec.md` and
`src/table_scan/table_scan.cpp` carry that split on `main`.

**Shapes any deferral must exclude** (verified against source, roborev job 1130).
These lose pushdown entirely rather than degrading, and no client re-check
recovers the wire cost:

- **`rowid` predicates.** The two encoder paths disagree by construction:
  `EncodeColumnRef` tests `COLUMN_IDENTIFIER_ROW_ID` *before* the virtual-column
  gate and rewrites it to the scalar PK, while `FilterEncoder::Encode` refuses
  everything `>= VIRTUAL_COL_START` into `unhandled`. Defer a rowid predicate and
  `WHERE rowid > 100` goes from `WHERE [id] > 100` to a full table scan. Rows stay
  correct; the scan is unbounded.
- **OR-chains, non-dense `IN`, and temporal casts**, which arrive through
  `CreateOptionalExpressionFilter` as an `optional_filter` scalar function the
  encoder has no mapping for. It refuses, so the scan pushes *nothing at all*.

**Consequence, stated plainly:** native-by-default does **not** close issue #272.
A pushed `=` still answers differently from an un-pushed one, because the server
compares by collation and DuckDB by bytes. That divergence is the price of the
contract, and #272 stays open to track it rather than being marked resolved here.

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

### 4.3 ORDER BY and TOP — the widening the same trick buys

Sorting has no superset to fall back on: `TOP k` under the wrong order returns
the wrong **set**, and no client re-sort of already-truncated rows recovers it.
§ 1 measures exactly that — four rows, two disjoint answers.

Note this is **not** in tension with § 4.1's native-by-default. Both apply the
same principle — *preserve what ships* — to surfaces where what ships differs.
Filters push today, so their shipped behaviour is the server's semantics. ORDER
BY does **not** push today (`mssql_order_pushdown` has been `false` since spec
039), so its shipped behaviour is **DuckDB's** order. Preserving each therefore
points in opposite directions, and that is a consequence of history, not an
inconsistency.

**§ 2's rule (push only a `_BIN2` or non-string key) is correct but empty on a
default install.** The active form of § 3 fixes that, and here it is genuinely
cheap. Measured, 200 000-row `nvarchar(100) COLLATE SQL_Latin1_General_CP1_CI_AS`
with an index on the key:

| pushed | plan | 5-run elapsed |
|---|---|---|
| `TOP 10 … ORDER BY s` (native) | `Top` → **Index Scan ORDERED FORWARD** — no sort at all | ~0–1 ms |
| `TOP 10 … ORDER BY s COLLATE …BIN2` | `Top` → **`Sort(TOP 10)`** → Compute Scalar → Index Scan | ~6–7 ms |

Two things follow.

**It is correct.** On § 1's own seven values the collation-forced order is
DuckDB's, element for element:

```text
DuckDB           ORDER BY s LIMIT 4 :  Apple | BANANA | Zebra | _x
server, native CI_AS              :  _x    | Ähre   | apple | Apple
server, COLLATE …_BIN2            :  Apple | BANANA | Zebra | _x   ← identical
```

Including `_x` sorting *after* the uppercase letters, which is where the
linguistic collation departs most sharply.

**It is affordable.** The forced order costs the ordered-index fast path, but
SQL Server answers with a **bounded `Sort(TOP 10)`** — a priority queue over the
scan, not a full sort — so the cost is one pass, ~6 ms per 200 k rows. Set that
against the alternative for a non-`_BIN2` column, which today is *no pushdown at
all*: transferring the whole column to DuckDB to find ten rows.

And there is no regression to trade away. The ordered-index path exists only
when the column's own collation already matches the requested order — precisely
the `_BIN2` case § 2 already allows for free. So:

- **`_BIN2` or non-string key** → push as today, ordered index scan, no sort.
- **Any other string key** → push `ORDER BY key COLLATE Latin1_General_BIN2`
  with the `TOP`, taking the bounded sort. Widens TopN pushdown from "nothing on
  a default install" to every string column.
- **Mixed keys** → the rule is per key; a forced key and a native key can appear
  in one `ORDER BY` since each is independent.

Unchanged: the existing NULL-ordering check still gates, ties remain
tie-broken by the server (already true for non-string keys today, so not new
here), and the above-BMP caveat of § 5's D1 applies to the forced key exactly as
it does to a naturally-`_BIN2` one.

**Which is why the pushdown can now default to on** (D3). It shipped disabled
because it changed results; forcing the key removes that, so the remaining
question is only where the sort runs. A second setting,
`mssql_order_pushdown_native_collation`, keeps the *other* behaviour reachable —
push without forcing and take the server's linguistic order, which is faster
still (no sort at any collation) and is what a user comparing against SSMS
actually wants. One default that matches DuckDB, one opt-in that matches the
server; nobody has to choose between correct and fast without being told which
they picked.

### 4.4 Summary

**The default differs between reads and writes. That is deliberate, and it is the
single most surprising thing in this spec:** the same predicate on the same
column selects four rows and deletes one. A destructive statement being the
conservative one is defensible, but it must be documented where someone about to
run a `DELETE` will see it (`docs/dml-collation-semantics`, merged as #286).

| Path | Sent to the server | Semantics | Whose answer |
|---|---|---|---|
| SELECT, strings (`=`, `IN`, `LIKE`) | the **exact** predicate; inexact rewrites refused | native | the server's |
| SELECT, `<>` / `NOT LIKE` on strings | nothing | — | DuckDB's |
| DML-collapse, superset forms | native **+** `COLLATE …BIN2` (+ sentinel on `=`) | strict | DuckDB's |
| DML, everything else | — | — | rowid round trip |
| Column already `_BIN2`, or non-string | native predicate only | identical either way | both |
| ORDER BY / TOP, `_BIN2` or non-string key | native predicate only | ordered index scan | both agree |
| ORDER BY / TOP, any other string key | `ORDER BY key COLLATE …_BIN2` | strict | DuckDB's (§ 4.3) |

### 4.5 Extending what pushes — the simple exact forms

Native semantics settle *comparison*. They say nothing about a scalar function's
**value**: `length('ab ')` is 3 in DuckDB whatever the collation, so a mapping to
`LEN` that answers 2 is a plain value bug — which is what issue #242 found and
#269 fixed by removing four mappings.

**Take (measured exact, direct 1:1 forms):** `abs` → `ABS`, `floor` → `FLOOR`,
`ceil`/`ceiling` → `CEILING`, `round` → `ROUND`, `substring` → `SUBSTRING`.
Verified DuckDB-vs-server on the cases that usually separate implementations:
`round` agrees on half-away-from-zero, on negatives (`round(-2.5)` = −3 both
sides) and in the 2-argument form; `ceil(-2.3)` = −2 and `floor(-2.3)` = −3 both
sides; `substring` agrees **including** the `start = 0` edge, where both answer
`ab` for `substring('abcdef', 0, 3)`. No collation is involved in any of them,
so native-vs-strict does not arise.

**Leave to DuckDB (decision, 2026-08-29):** `/`, `week`, `dayofweek`, `length`.
Exact T-SQL forms *do* exist for all four and are recorded below so the next
reader of #242 does not re-derive them — but they are **not worth pushing**:
each needs a compound rewrite, and a compound expression over a column defeats
the index just as surely as the naive mapping did, while the queries that use
them are narrow. DuckDB computes them on the rows the other predicates already
selected.

| DuckDB | exact form, for the record | why it stays unpushed |
|---|---|---|
| `/` | `({0} * 1.0 / NULLIF({1}, 0))` | `5/2`→2.5 both sides; `NULLIF` is not optional — raw `5*1.0/0` **aborts the query** server-side while DuckDB yields `inf` (NULL and `inf` behave alike in predicate position, verified). Two wrappers to fix one operator. |
| `week` | `DATEPART(ISO_WEEK, {0})` | Genuinely exact — DuckDB's `week()` *is* the ISO week and T-SQL has the datepart; the original mapping just used the wrong one (1, 1, 53 on both sides for 2024-01-04 / 2024-12-30 / 2021-01-01). Cheap to restore later if a real query wants it. |
| `dayofweek` | `((DATEDIFF(day,'19000107',{0}) % 7) + 7) % 7` | Anchored on a known Sunday, so `@@DATEFIRST`-independent — the property that disqualified `DATEPART(weekday, …)`. The `+7 %7` is load-bearing: without it T-SQL's sign-preserving `%` answers −4 for pre-anchor dates. Correct, and unreadable in a WHERE clause. |
| `length` | `LEN({0} + N'~') - 1` | Rejected on correctness as well as cost: `LEN` counts **UTF-16 code units**, DuckDB counts code points, so `'a😀b'` is 3 in DuckDB and **4** on the server. Exact only for BMP-only data — not a guarantee worth shipping. |

SARGability is unchanged by the five we take: a function wrapping a column
defeats a seek whether it is `ABS(x)` or the `YEAR(x)` already pushed. The gain
is not transferring the column.

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
- **D3 Two settings, and the default flips ON.** § 4.3 removes the reason the
  pushdown shipped disabled: with the key's collation forced, the pushed order
  *is* DuckDB's, so there is nothing left for a user to get wrong by leaving it
  on. The three-valued `off`/`safe`/`always` sketched in the first draft is
  dropped — it made the user choose between correctness and speed, and the
  measurement says they no longer have to.

  - **`mssql_order_pushdown`** stays BOOLEAN — so a script that already says
    `SET mssql_order_pushdown = false` keeps working — and its **default becomes
    `true`**. On means: push `ORDER BY`/`TOP` and *preserve DuckDB's order* —
    the native key for a `_BIN2` or non-string key (ordered index scan, free),
    `COLLATE Latin1_General_BIN2` on any other string key (bounded `Sort(TOP k)`,
    ~6 ms/200 k rows). Results are identical to not pushing at all; only the plan
    and the memory profile change.
  - **`mssql_order_pushdown_native_collation`** is new, BOOLEAN, default
    `false`. On means: push the key **without** forcing — the server orders by
    its own collation. This *changes results* (that is the point: it is the
    SSMS-like ordering, and it makes ORDER BY agree with the native filter
    semantics of § 4.1), and it buys the ordered-index path on any collation, no
    sort at all. It is the knob for someone who wants the server's linguistic
    order deliberately.

  The two compose in one direction only: the native flag chooses **how** a key
  is pushed, never **whether**. With `mssql_order_pushdown = false` nothing is
  pushed no matter what the native flag says, and that must be asserted — an
  opt-in that silently re-enables a disabled pushdown is the worst of both.

- **D3a NULL ordering: split it server-side, do not put it in the sort key.**
  The existing check (§ 1) declines a nullable key outright because SQL Server
  sorts NULLs first in ASC and DuckDB sorts them last. An earlier draft of this
  decision said the fix — moving the NULL test into the ORDER BY as
  `CASE WHEN k IS NULL THEN 1 ELSE 0 END` — would cost the ordered-index path.
  **Measured, that is wrong**, and the working form is a `UNION ALL` of two
  bounded `TOP`s rather than a compound sort key:

  ```sql
  SELECT TOP k … FROM (
      SELECT TOP k …, 0 AS g FROM t WHERE key IS NOT NULL ORDER BY key
      UNION ALL
      SELECT TOP k …, 1 AS g FROM t WHERE key IS NULL
  ) x ORDER BY g, key
  ```

  On a nullable `_BIN2` key over 200 000 rows the plan contains **no Sort
  operator at all** —

  ```text
  Top(10)
    └─ Merge Join(Concatenation)
         ├─ Top(10) → Index Seek(key IsNotNull) ORDERED FORWARD
         └─ Top(10) → Index Seek(key = NULL)    ORDERED FORWARD
  ```

  — because both branches arrive already ordered and the server concatenates
  them by merge. **~0 ms, against ~6–7 ms for the `CASE`-in-ORDER-BY form**,
  which degrades to `Sort(TOP k)` over an unordered Index Scan. `IS NOT NULL`
  becomes a *seek predicate*; the `CASE` expression cannot, which is the whole
  difference.

  **The metadata for this already exists and is already read.**
  `MSSQLColumnInfo::is_nullable` is populated by all three catalog query shapes
  (`mssql_metadata_cache.cpp`) and the gate consults it today —
  `mssql_optimizer.cpp:306` feeds it to `IsNullOrderCompatible`. So the two-branch
  form needs no new metadata and no extra round trip; it needs the gate to emit a
  different query instead of giving up.

  **And the gate is narrower than "nullable declines".** Reading it against
  DuckDB's default (`default_null_order = NULLS_LAST`, verified, for ASC *and*
  DESC):

  | key | DuckDB asks | SQL Server's default | today |
  |---|---|---|---|
  | `NOT NULL` | — | — | **pushes** (the check returns early) |
  | nullable, `DESC` | NULLS LAST | DESC → NULLs last | **pushes** — they agree |
  | nullable, `ASC` | NULLS LAST | ASC → NULLs first | **declined** |
  | nullable, `ASC NULLS FIRST` (explicit) | NULLS FIRST | NULLs first | **pushes** |

  So the only shape that loses is **a nullable key sorted plain ascending** —
  which is `ORDER BY col`, the most common ORDER BY there is. That is what the
  two-branch form buys back, and why it is worth more than the collation work it
  is bundled beside.

  Two consequences:

  - **A nullable key stops being a blanket decline.** For a `_BIN2` or
    non-string key it now pushes on the free path — no sort — where today it
    does not push at all.
  - **For a forced-collation key the NULL split is free anyway**, because that
    query is already paying a `Sort(TOP k)` for the collation: measured, the
    non-NULL branch keeps its `Index Seek(IsNotNull) ORDERED FORWARD` feeding
    that same bounded sort.

  The naive shapes both fail and are recorded so they are not retried: putting
  the `CASE` in the sort key loses the seek (above), and wrapping an
  index-ordered subquery in an outer sort — `SELECT … FROM (SELECT TOP 100
  PERCENT … ORDER BY key) x ORDER BY CASE …` — is **flattened by the optimizer
  into exactly the same plan as the plain `CASE` sort**, measured identical.
  T-SQL does not preserve a subquery's order without a bound, so the ordering
  has to be re-expressed as something the optimizer can turn into a seek, which
  is what the two-branch form does.

  Still deliberately a **separate change from the collation work**: it is a
  second, independent widening, and landing it in the same commit would make an
  ORDER BY regression impossible to bisect.
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
    the **server's** rows — `'AB'` matches — and the emitted T-SQL contains the
    exact predicate, never DuckDB's prefix→range rewrite. `like_pushdown_collation.test`
    (PR #276, on `main`) already pins the `LIKE`-vs-range half; the `=` and `IN`
    halves join it. Note this does NOT assert push/no-push agreement — under
    native semantics they legitimately differ, which is why #272 stays open.
F2. A collapsed `UPDATE`/`DELETE` on a `_CI_AS` column affects exactly the rows
    DuckDB's predicate selects — asserted by row identity, not by count —
    including a value with a trailing space and a case variant. The plan check
    shows the seek survived (§ 3.2).
F3. A string `<>` predicate does **not** collapse: it falls back (client-side for
    SELECT, rowid for DML) and returns DuckDB's rows.
F4. (§ 4.5, independent) Each newly mapped function returns the same rows pushed
    as un-pushed, asserted on the values that separate implementations:
    `round(-2.5)`, a 2-argument `round`, `ceil`/`floor` of a negative, and
    `substring(s, 0, 3)`. The four left to DuckDB stay unmapped — a test that
    `length`/`week`/`dayofweek`/`/` are still absent from the mapping table keeps
    § 4.5's decision from being undone by someone reading only the exact forms.
F5. (§ 4.3) On a `_CI_AS` string key, `ORDER BY s LIMIT k` returns DuckDB's rows
    in DuckDB's order — asserted element by element on § 1's seven values, the
    set that motivated this spec — and the plan shows the sort happened on the
    server (nothing transferred beyond `k` rows).

**ORDER BY (§ 1–2):**

O1. **The default now pushes.** With no settings touched, `ORDER BY s LIMIT k` on
    a `_CI_AS` string key reaches the server (visible at `MSSQL_DEBUG=1`) and
    returns DuckDB's rows in DuckDB's order — asserted element by element on § 1's
    seven values, not by count. This is the criterion that replaces the old
    "generated SQL is byte-identical to today's": the SQL deliberately is not.
O2. A `_BIN2` string key and a non-string key are pushed **without** a `COLLATE`
    clause — the free path — and the emitted SQL is asserted, not just the rows,
    so a regression that forces the collation everywhere is caught by cost rather
    than by correctness (it would pass O1).
O3. `SET mssql_order_pushdown = false` restores today's behaviour exactly: no
    ORDER BY reaches the server, including when
    `mssql_order_pushdown_native_collation` is `true`. An opt-in must not
    re-enable a disabled pushdown.
O4. `SET mssql_order_pushdown_native_collation = true` on a `_CI_AS` key emits no
    `COLLATE`, and the result is the **server's** order — asserted as the § 1
    "on" row (`_x | Ähre | Apple | apple`), which is a different *set* under
    `LIMIT`. The divergence is the feature; it is pinned so it cannot regress
    into the faithful path unnoticed.
O5. A nullable key pushes via the two-branch form (D3a) and returns DuckDB's
    NULLS-LAST order; on a `_BIN2` key the plan is asserted to contain **no Sort
    operator**, since the whole point is that the seek survives. Both naive
    shapes are pinned as rejected: the `CASE`-in-sort-key form and the
    `TOP 100 PERCENT` subquery form produce a sort, so the assertion is on the
    plan, not the rows — the rows are identical either way.
O6. A mixed ORDER BY (one `_BIN2` key, one `_CI_AS` key) pushes both — each key is
    judged independently, the `_BIN2` one bare and the other forced.
O7. On Fabric, the default pushes with no `COLLATE` clause — the warehouse
    collation is already `_BIN2`, so it takes the free path.

## 7. Risks

- **`_BIN2` is code-point order, but is DuckDB's?** DuckDB compares strings by
  UTF-8 bytes. UTF-8 byte order and code-point order coincide, so the two agree
  — but this deserves an explicit test with characters above U+FFFF rather than
  an argument, because it is the whole premise.
- **The collation is per column, and an expression is not a column.** `ORDER BY
  upper(s)` has no collation of its own; it inherits the argument's. The first
  cut should push only bare column references and treat anything else as
  unsafe, which is narrower than what spec 039 already supports.
- **Flipping `mssql_order_pushdown` on by default is a behaviour change even
  though results do not move.** The sort relocates to SQL Server: plans, memory
  profile and where a slow query shows up all change, and a server under load now
  absorbs work DuckDB used to do. Results being identical is what makes it
  *safe*, not what makes it invisible — it needs a release note, and
  `mssql_order_pushdown = false` is the documented way back.
- **`mssql_order_pushdown_native_collation` changes results by design.** It is
  the one setting in this spec that makes a query return different rows, so its
  documentation has to say so in the first sentence rather than describe it as a
  performance option.

Risks specific to the widened scope:

- **The read/write split is the risk this spec ships.** SELECT keeps server
  semantics (`collation_filter.test` stands as written); DML-collapse is strict.
  So `SELECT … WHERE name = 'abc'` matches `'ABC'` and `DELETE … WHERE name =
  'abc'` does not. Defensible — the destructive statement is the careful one —
  but it is exactly the shape that gets discovered through someone's deleted
  rows, so it is documented user-side (#286) rather than left to this spec.
- **#272 is not closed by this design.** Pushed and un-pushed `=` still disagree,
  because the server compares by collation and DuckDB by bytes. An earlier draft
  of § 4.1 closed it by making both answers DuckDB's; that was rejected because
  it inverts the shipped contract. The divergence is now a *known and documented*
  cost rather than a bug, and #272 tracks it.
- **The superset property for prefixes is argued, not proven** (§ 3.3). A
  collation with expansions or ignorable characters could in principle order a
  byte-in-range value outside the linguistic range, which would *lose* rows —
  the one failure mode the client net cannot catch. Mitigation: gate on
  collation family, default unproven families to "do not push", and test against
  a non-Latin collation before widening.
- **`NVARCHAR` under `_BIN2` compares UTF-16 code units**, DuckDB compares UTF-8
  bytes. They agree across the BMP and disagree above U+FFFF (supplementary
  plane: emoji, rare CJK). § 5's D1 risk note already flags this for ordering;
  it applies identically to the forced-collation equality of § 4.2 and to the
  forced ORDER BY key of § 4.3, and needs the same above-U+FFFF test in all three.
- **The sentinel changes the expression's type and length.** `a + '~'` on a
  `varchar(8000)`/`MAX` column risks truncation or an implicit conversion the
  plan shows as `CONVERT_IMPLICIT` (visible in the § 3.2 measurement). Pick the
  sentinel and the cast deliberately, and keep it off any column where the
  concatenation could overflow.
- **Two predicates on one column skew the estimate.** The optimizer multiplies
  their selectivities and under-counts. Confined to DML (§ 4.2) precisely
  because a single-table `UPDATE`/`DELETE` has no join order to lose — but if
  the pair ever reaches a `SELECT`, that is the cost to measure first.
